package dev.hyperwin.app

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ApplicationInfo
import android.os.ParcelFileDescriptor
import android.util.Log
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicInteger

/**
 * Link to hwagent.exe in the guest over virtio-vsock (HW_VSOCK_PORT_AGENT). Carries the guest's
 * network packets to [GuestNet], runs commands in Windows and copies files in.
 * Frames: u32 payload length, u8 type, payload (include/hw_abi.h).
 */
object GuestAgent {
    private const val TAG = "HyperWinAgent"
    private const val PORT = 5651L
    private const val PACKET = 1
    private const val EXEC = 2
    private const val EXEC_OUT = 3
    private const val EXEC_DONE = 4
    private const val FILE_OPEN = 5
    private const val FILE_DATA = 6
    private const val FILE_CLOSE = 7
    private const val FILE_RESULT = 8
    private const val HELLO = 9
    private const val NET_CONFIG = 10
    private const val PORTS = 11
    private const val TOUCH = 12
    private const val CPU_MHZ = 13
    private const val CURSOR = 14
    private const val VERSION = 5
    const val ACTION_EXEC = "dev.hyperwin.app.EXEC"
    const val ACTION_PUSH = "dev.hyperwin.app.PUSH"
    /** Debug builds: --ei dx, dy, buttons, wheel, n (repeats, 2 ms apart) as relative mouse input. */
    const val ACTION_MOUSE = "dev.hyperwin.app.MOUSE"
    /** Debug builds: --ei slot, buttons, lx, --ez connected: a made-up controller; --es kind xbox|ds4: player 1's kind. */
    const val ACTION_PAD = "dev.hyperwin.app.PAD"
    /** Debug builds: --es zip /sdcard/Download/x.zip adds a DXVK and switches to it; --es use shipped|<id> switches. */
    const val ACTION_DXVK = "dev.hyperwin.app.DXVK"

    private val writeLock = Any()
    @Volatile private var vm: Any? = null
    @Volatile private var out: FileOutputStream? = null
    @Volatile private var net: GuestNet? = null
    @Volatile private var lan = false
    private var appContext: Context? = null
    private var receiver: BroadcastReceiver? = null
    private val nextId = AtomicInteger(1)

    @Volatile var connected = false
        private set
    /** Told when a game in Windows starts or stops wanting mouse-look (HW_AGENT_CURSOR). */
    @Volatile var onCursor: ((Boolean) -> Unit)? = null
    /** Told when a post-patch PnP driver update needs a Windows restart to take effect. */
    @Volatile var onGuestRebootNeeded: (() -> Unit)? = null
    @Volatile var guestRebootNeeded = false
        private set
    @Volatile var agentVersion = 0
        private set
    private var updateSent = false
    /** True when Windows takes touch screen contacts ([touch]). */
    val touchReady get() = connected && agentVersion >= 2

    /** One HW_AGENT_TOUCH frame: the contacts on the screen right now. */
    fun touch(frame: ByteArray): Boolean {
        if (!touchReady) return false
        send(TOUCH, frame, frame.size)
        return true
    }
    /** True when the guest carries the phone's own LAN address. */
    val lanActive get() = net?.lanActive == true

    fun start(ctx: Context, machine: Any, lanMode: Boolean) {
        appContext = ctx.applicationContext
        lan = lanMode
        vm = machine
        updateSent = false
        guestRebootNeeded = false
        registerDebugCommands(ctx.applicationContext)
        Thread({ connectLoop(machine) }, "hw-agent").apply { isDaemon = true; start() }
    }

    fun stop() {
        vm = null
        connected = false
        ExperimentalCpuPerformance.disconnected()
        guestRebootNeeded = false
        doneWatchers.keys.toList().forEach { doneWatchers.remove(it)?.invoke(-1) }
        fileResultOk = false
        fileResultLatch?.countDown()
        runCatching { out?.close() }
        out = null
        net?.close()
        net = null
    }

    /** Switches between the NAT address and the phone's LAN address while Windows runs. */
    fun setLanMode(on: Boolean) {
        lan = on
        if (connected) startNet()
    }

    private fun startNet() {
        val ctx = appContext ?: return
        net?.close()
        val n = GuestNet(ctx, lan) { p, len -> send(PACKET, p, len) }
        net = n
        val cfg = n.config().toByteArray()
        send(NET_CONFIG, cfg, cfg.size)
        Log.i(TAG, "network ${if (n.lanActive) "LAN" else "NAT"} ${n.config()}")
    }

    private fun connectLoop(machine: Any) {
        while (vm === machine) {
            val pfd = runCatching {
                machine.javaClass.getMethod("connectVsock", Long::class.javaPrimitiveType).invoke(machine, PORT) as ParcelFileDescriptor
            }.getOrNull()
            if (pfd == null) { Thread.sleep(1000); continue }
            try {
                out = FileOutputStream(pfd.fileDescriptor)
                connected = true
                Log.i(TAG, "connected")
                startNet()
                read(FileInputStream(pfd.fileDescriptor))
            } catch (e: Exception) {
                Log.i(TAG, "link dropped: ${e.message}")
            } finally {
                connected = false
                ExperimentalCpuPerformance.disconnected()
                agentVersion = 0
                out = null
                doneWatchers.keys.toList().forEach { doneWatchers.remove(it)?.invoke(-1) }
                fileResultOk = false
                fileResultLatch?.countDown()
                net?.close()
                net = null
                runCatching { pfd.close() }
            }
            Thread.sleep(1000)
        }
    }

    private fun read(input: FileInputStream) {
        // Own buffering: BufferedInputStream asks available(), which a vsock socket refuses.
        val buf = ByteArray((1 shl 20) + (1 shl 16))
        var have = 0
        while (true) {
            val n = input.read(buf, have, buf.size - have)
            if (n < 0) throw IllegalStateException("closed by guest")
            have += n
            var pos = 0
            while (have - pos >= 5) {
                val len = le32(buf, pos)
                if (len < 0 || len > 1 shl 20) throw IllegalStateException("bad frame $len")
                if (have - pos - 5 < len) break
                frame(buf[pos + 4].toInt(), buf.copyOfRange(pos + 5, pos + 5 + len), len)
                pos += 5 + len
            }
            if (pos > 0) { System.arraycopy(buf, pos, buf, 0, have - pos); have -= pos }
        }
    }

    private fun frame(type: Int, buf: ByteArray, len: Int) {
        when (type) {
            PACKET -> net?.fromGuest(buf, len)
            EXEC_OUT -> String(buf, 4, len - 4).lines().forEach { if (it.isNotBlank()) Log.i(TAG, "out| ${it.trimEnd()}") }
            EXEC_DONE -> {
                val cmdId = le32(buf, 0)
                val code = le32(buf, 4)
                Log.i(TAG, "exit $code (command $cmdId)")
                doneWatchers.remove(cmdId)?.invoke(code)
            }
            FILE_RESULT -> {
                val ok = le32(buf, 0) == 1
                Log.i(TAG, if (ok) "file written" else "file write failed")
                fileResultOk = ok
                fileResultLatch?.countDown()
            }
            HELLO -> {
                agentVersion = buf[0].toInt()
                Log.i(TAG, "agent version ${buf[0]}, network adapter ${if (buf[1].toInt() == 1) "ready" else "missing"}")
                if (agentVersion < VERSION) updateAgent()
                appContext?.let { DisplayWatchdog.agentUp(it); ExperimentalCpuPerformance.sync(it) }
                // A boot cut short (the app closed, the phone low on memory) must not send Windows
                // into its recovery screens, which this VM cannot run. Cheap to repeat each start.
                exec("cmd /c bcdedit /set {current} recoveryenabled no & bcdedit /set {current} bootstatuspolicy ignoreallfailures")
                val mhz = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(AvfPlan.hostCpuMhz()).array()
                if (agentVersion >= 3 && AvfPlan.hostCpuMhz() > 0) send(CPU_MHZ, mhz, 4)
                // If an offline pre-boot patch staged new driver packages, finish PnP registration now.
                if (agentVersion >= VERSION) {
                    val id = exec(
                        "cmd /c if exist C:\\HyperWin\\gpu\\pending.install (" +
                            "del /q C:\\HyperWin\\gpu\\reboot.needed 2>nul & " +
                            "C:\\HyperWin\\gpu-install.cmd && del /q C:\\HyperWin\\gpu\\pending.install)",
                    )
                    onceDone(id) { rc ->
                        if (rc == 0) {
                            val rId = exec("cmd /c if exist C:\\HyperWin\\gpu\\reboot.needed (del /q C:\\HyperWin\\gpu\\reboot.needed 2>nul & exit 0) else (exit 1)")
                            onceDone(rId) { rRc ->
                                if (rRc == 0) {
                                    guestRebootNeeded = true
                                    onGuestRebootNeeded?.invoke()
                                }
                            }
                        }
                    }
                }
            }
            CURSOR -> if (len >= 1) onCursor?.invoke(buf[0].toInt() != 0)
            PORTS -> {
                val b = ByteBuffer.wrap(buf, 0, len).order(ByteOrder.LITTLE_ENDIAN)
                val tcp = (0 until (b.short.toInt() and 0xffff)).map { b.short.toInt() and 0xffff }.toSet()
                val udp = (0 until (b.short.toInt() and 0xffff)).map { b.short.toInt() and 0xffff }.toSet()
                net?.mirrorPorts(tcp, udp)
            }
        }
    }

    private fun le32(b: ByteArray, o: Int) = ByteBuffer.wrap(b, o, 4).order(ByteOrder.LITTLE_ENDIAN).int

    private fun send(type: Int, payload: ByteArray, len: Int) {
        val o = out ?: return
        val frame = ByteArray(5 + len)
        ByteBuffer.wrap(frame).order(ByteOrder.LITTLE_ENDIAN).putInt(len).put(type.toByte())
        System.arraycopy(payload, 0, frame, 5, len)
        synchronized(writeLock) {
            try { o.write(frame) } catch (e: Exception) { runCatching { o.close() } }
        }
    }

    /** Runs a command line in Windows: as SYSTEM with output in logcat, or on the user's desktop. */
    fun exec(command: String, desktop: Boolean = false, hidden: Boolean = false, onExit: ((Int) -> Unit)? = null): Int {
        val id = nextId.getAndIncrement()
        val cmd = command.toByteArray()
        val mode = if (hidden) 2 else if (desktop) 1 else 0     // HW_AGENT_EXEC_*
        val p = ByteBuffer.allocate(5 + cmd.size).order(ByteOrder.LITTLE_ENDIAN).putInt(id).put(mode.toByte()).put(cmd).array()
        if (onExit != null) doneWatchers[id] = onExit
        send(EXEC, p, p.size)
        return id
    }

    private val doneWatchers = java.util.concurrent.ConcurrentHashMap<Int, (Int) -> Unit>()
    @Volatile private var fileResultLatch: java.util.concurrent.CountDownLatch? = null
    @Volatile private var fileResultOk = false

    fun forgetCompletion(id: Int) { doneWatchers.remove(id) }

    /** Calls [onExit] with the exit code when command [id] (from [exec]) finishes. */
    fun onceDone(id: Int, onExit: (Int) -> Unit) { doneWatchers[id] = onExit }
    private val pushLock = Any()

    fun clearRebootNeeded() { guestRebootNeeded = false }

    /** A machine made by an older app has an older agent: send ours and swap it in. Once per boot. */
    private fun updateAgent() {
        val ctx = appContext ?: return
        if (updateSent) return
        updateSent = true
        Thread {
            runCatching {
                push(ctx.assets.open("machine/guest/hwagent.exe"), "C:\\HyperWin\\hwagent.new.exe")
                exec("cmd /c start /b cmd /c \"ping -n 3 127.0.0.1 >nul & sc stop HyperWinAgent & ping -n 3 127.0.0.1 >nul & " +
                    "copy /y C:\\HyperWin\\hwagent.new.exe C:\\HyperWin\\hwagent.exe & sc start HyperWinAgent\"")
                Log.i(TAG, "sent agent version $VERSION to replace $agentVersion")
            }.onFailure { Log.e(TAG, "agent update failed", it) }
        }.start()
    }

    fun push(local: File, guestPath: String): Boolean = push(local.inputStream(), guestPath)

    fun push(source: java.io.InputStream, guestPath: String): Boolean = synchronized(pushLock) {
        val latch = java.util.concurrent.CountDownLatch(1)
        fileResultOk = false
        fileResultLatch = latch
        val path = guestPath.toByteArray()
        send(FILE_OPEN, path, path.size)
        source.use { input ->
            val buf = ByteArray(1 shl 16)
            while (true) {
                val n = input.read(buf)
                if (n <= 0) break
                send(FILE_DATA, buf, n)
            }
        }
        send(FILE_CLOSE, ByteArray(0), 0)
        val got = latch.await(120, java.util.concurrent.TimeUnit.SECONDS)
        fileResultLatch = null
        got && fileResultOk
    }

    /**
     * Debug builds take commands from adb:
     *   am broadcast -a dev.hyperwin.app.EXEC --es cmd "ipconfig /all" [--ez desktop true]
     *   am broadcast -a dev.hyperwin.app.PUSH --es src /data/local/tmp/x.exe --es dst "C:\\HyperWin\\x.exe"
     */
    private fun registerDebugCommands(ctx: Context) {
        if (receiver != null || ctx.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE == 0) return
        receiver = object : BroadcastReceiver() {
            override fun onReceive(c: Context, intent: Intent) {
                Thread {
                    when (intent.action) {
                        ACTION_EXEC -> intent.getStringExtra("cmd")?.let {
                            Log.i(TAG, "command ${exec(it, intent.getBooleanExtra("desktop", false))}: $it")
                        }
                        ACTION_PUSH -> runCatching {
                            push(File(intent.getStringExtra("src")!!), intent.getStringExtra("dst")!!)
                        }.onFailure { Log.e(TAG, "push failed", it) }
                        ACTION_DXVK -> {
                            val id = intent.getStringExtra("zip")?.let { path ->
                                val (id, error) = HwGpu.importDxvk(c, android.net.Uri.fromFile(File(path)))
                                Log.i(TAG, "dxvk zip $path: ${id ?: error}")
                                id
                            } ?: intent.getStringExtra("use")
                            if (id != null) {
                                HwGpu.useDxvk(c, id)
                                HwGpu.restart()
                                Log.i(TAG, "dxvk now ${HwGpu.dxvkName(c)}; have ${HwGpu.dxvkList(c).map { it.id }}")
                            }
                        }
                        ACTION_PAD -> if (intent.hasExtra("kind")) {
                            c.getSharedPreferences("hyperwin", Context.MODE_PRIVATE).edit()
                                .putString(GamepadInput.PLAYER1, intent.getStringExtra("kind")).apply()
                            GamepadInput.player1Changed()
                        } else GamepadInput.fake(intent.getIntExtra("slot", 0), intent.getBooleanExtra("connected", true),
                            intent.getIntExtra("buttons", 0), intent.getIntExtra("lx", 0))
                        ACTION_MOUSE -> repeat(intent.getIntExtra("n", 1)) {
                            VsockInput.relative(intent.getIntExtra("dx", 0), intent.getIntExtra("dy", 0),
                                intent.getIntExtra("buttons", 0), intent.getIntExtra("wheel", 0), intent.getIntExtra("hwheel", 0))
                            Thread.sleep(2)
                        }
                    }
                }.start()
            }
        }
        ctx.registerReceiver(receiver, IntentFilter().apply { addAction(ACTION_EXEC); addAction(ACTION_PUSH); addAction(ACTION_MOUSE); addAction(ACTION_PAD); addAction(ACTION_DXVK) }, Context.RECEIVER_EXPORTED)
    }
}
