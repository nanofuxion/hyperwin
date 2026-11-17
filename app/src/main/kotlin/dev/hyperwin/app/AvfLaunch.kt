package dev.hyperwin.app

import android.content.Context
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.os.SystemClock
import java.io.File
import java.nio.ByteOrder
import java.io.InputStream
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

/** Starts the AVF payload and records the guest console. Used by `adb am start --ez probe true`. */
object AvfLaunch {
    private const val TAG = "HyperWin"
    private const val PKG = "android.system.virtualmachine."
    /**
     * AVF keeps one VM object per name, so with a single name the previous VM's late "stopped" can
     * land on the next one's callback and stop it. Starts alternate between two names; each name's
     * last VM has long exited by the time it is used again.
     */
    private val NAMES = arrayOf("hyperwin-a", "hyperwin-b")
    private var nextName = 0
    private val keyboardLock = Any()

    enum class TextSendFailure { GUEST_UNAVAILABLE, UNSUPPORTED_CHARACTER, INPUT_REJECTED }
    data class TextSendResult(
        val acceptedCharacters: Int,
        val totalCharacters: Int,
        val failure: TextSendFailure? = null,
    )

    fun run(ctx: Context, attachWoa: Boolean): String {
        val kernelSrc = File("/data/local/tmp/hyperwin.img")
        val kernel = File(ctx.filesDir, "hyperwin.img")
        if (kernelSrc.canRead()) kernelSrc.copyTo(kernel, overwrite = true)
        if (!kernel.isFile) return fail("no payload at ${kernel.absolutePath}")
        val system = File(ctx.filesDir, "scratch.img")
        val bytes = 64L * 1024L * 1024L
        if (system.length() != bytes) java.io.RandomAccessFile(system, "rw").use { it.setLength(bytes) }
        GuestBoot.requireAligned(system)
        if (attachWoa) {
            val woa = File("/data/local/tmp/woa_17134.img")
            Log.i(TAG, "host file ${woa.absolutePath} readable=${woa.canRead()} size=${woa.length()}; " +
                "not a VM disk. Guest reads it on vsock $HW_FILE_PORT")
        }
        return boot(ctx, GuestBoot.installImage(ctx, kernel, listOf(system)), keep = false, memoryMb = 512, expect = "HYPERWIN")
    }

    private const val HW_FILE_PORT = 5602

    @Volatile var machine: Any? = null
    /** True when the last VM exit was the guest restarting itself (Windows setup does this twice). */
    @Volatile var lastStopWasReboot = false
        private set
    @Volatile var sharedWindow: AvfPlan.SharedWindow? = null
        private set

    /** AVF drops useKeyboard/useMouse, so input goes to the guest's hwvsinput.sys over vsock. */
    fun key(event: KeyEvent): Boolean = synchronized(keyboardLock) {
        if (machine == null) return false
        VsockInput.key(event)
    }

    /**
     * Sends text as guest key presses (US layout). A character with no key stops the whole text,
     * or is left out when [skipUnknown] is set, as the on-screen keyboard pass-through wants.
     */
    fun typeText(text: CharSequence, skipUnknown: Boolean = false): TextSendResult = synchronized(keyboardLock) {
        val normalized = text.toString().replace("\r\n", "\n").replace('\r', '\n')
        if (machine == null) {
            return TextSendResult(0, normalized.length, TextSendFailure.GUEST_UNAVAILABLE)
        }
        val strokes = normalized.mapNotNull {
            guestStroke(it) ?: if (skipUnknown) null
                else return TextSendResult(0, normalized.length, TextSendFailure.UNSUPPORTED_CHARACTER)
        }
        for ((index, stroke) in strokes.withIndex()) {
            if (!sendStroke(stroke)) {
                return TextSendResult(index, strokes.size, TextSendFailure.INPUT_REJECTED)
            }
        }
        TextSendResult(strokes.size, strokes.size)
    }

    private fun sendStroke(stroke: GuestStroke): Boolean {
        val keyDownTime = SystemClock.uptimeMillis()
        val shiftDownTime = keyDownTime
        if (stroke.shift && !sendSyntheticKey(
                KeyEvent.KEYCODE_SHIFT_LEFT, KeyEvent.ACTION_DOWN, shiftDownTime, shiftDownTime, 0,
            )
        ) {
            return false
        }
        val meta = if (stroke.shift) KeyEvent.META_SHIFT_ON else 0
        val down = sendSyntheticKey(stroke.keyCode, KeyEvent.ACTION_DOWN, keyDownTime, keyDownTime, meta)
        val up = sendSyntheticKey(
            stroke.keyCode, KeyEvent.ACTION_UP, keyDownTime, SystemClock.uptimeMillis(), meta,
        )
        val shiftUp = !stroke.shift || sendSyntheticKey(
            KeyEvent.KEYCODE_SHIFT_LEFT, KeyEvent.ACTION_UP, shiftDownTime, SystemClock.uptimeMillis(), 0,
        )
        return down && up && shiftUp
    }

    private fun sendSyntheticKey(
        keyCode: Int,
        action: Int,
        downTime: Long,
        eventTime: Long,
        meta: Int,
    ): Boolean = key(
        KeyEvent(
            downTime, eventTime, action, keyCode, 0, meta,
            -1, 0, 0, InputDevice.SOURCE_KEYBOARD,
        ),
    )

    private fun guestStroke(character: Char): GuestStroke? {
        if (character in 'a'..'z') {
            return GuestStroke(KeyEvent.KEYCODE_A + character.code - 'a'.code)
        }
        if (character in 'A'..'Z') {
            return GuestStroke(KeyEvent.KEYCODE_A + character.code - 'A'.code, shift = true)
        }
        return when (character) {
            ' ' -> GuestStroke(KeyEvent.KEYCODE_SPACE)
            '\n' -> GuestStroke(KeyEvent.KEYCODE_ENTER)
            '\t' -> GuestStroke(KeyEvent.KEYCODE_TAB)
            '\b' -> GuestStroke(KeyEvent.KEYCODE_DEL)
            '-' -> GuestStroke(KeyEvent.KEYCODE_MINUS)
            '_' -> GuestStroke(KeyEvent.KEYCODE_MINUS, shift = true)
            '0', ')' -> GuestStroke(KeyEvent.KEYCODE_0, shift = character == ')')
            '1', '!' -> GuestStroke(KeyEvent.KEYCODE_1, shift = character == '!')
            '2', '@' -> GuestStroke(KeyEvent.KEYCODE_2, shift = character == '@')
            '3', '#' -> GuestStroke(KeyEvent.KEYCODE_3, shift = character == '#')
            '4', '$' -> GuestStroke(KeyEvent.KEYCODE_4, shift = character == '$')
            '5', '%' -> GuestStroke(KeyEvent.KEYCODE_5, shift = character == '%')
            '6', '^' -> GuestStroke(KeyEvent.KEYCODE_6, shift = character == '^')
            '7', '&' -> GuestStroke(KeyEvent.KEYCODE_7, shift = character == '&')
            '8', '*' -> GuestStroke(KeyEvent.KEYCODE_8, shift = character == '*')
            '9', '(' -> GuestStroke(KeyEvent.KEYCODE_9, shift = character == '(')
            '=' -> GuestStroke(KeyEvent.KEYCODE_EQUALS)
            '+' -> GuestStroke(KeyEvent.KEYCODE_EQUALS, shift = true)
            '[' -> GuestStroke(KeyEvent.KEYCODE_LEFT_BRACKET)
            '{' -> GuestStroke(KeyEvent.KEYCODE_LEFT_BRACKET, shift = true)
            ']' -> GuestStroke(KeyEvent.KEYCODE_RIGHT_BRACKET)
            '}' -> GuestStroke(KeyEvent.KEYCODE_RIGHT_BRACKET, shift = true)
            ';' -> GuestStroke(KeyEvent.KEYCODE_SEMICOLON)
            ':' -> GuestStroke(KeyEvent.KEYCODE_SEMICOLON, shift = true)
            '\'' -> GuestStroke(KeyEvent.KEYCODE_APOSTROPHE)
            '"' -> GuestStroke(KeyEvent.KEYCODE_APOSTROPHE, shift = true)
            '`' -> GuestStroke(KeyEvent.KEYCODE_GRAVE)
            '~' -> GuestStroke(KeyEvent.KEYCODE_GRAVE, shift = true)
            '\\' -> GuestStroke(KeyEvent.KEYCODE_BACKSLASH)
            '|' -> GuestStroke(KeyEvent.KEYCODE_BACKSLASH, shift = true)
            ',' -> GuestStroke(KeyEvent.KEYCODE_COMMA)
            '<' -> GuestStroke(KeyEvent.KEYCODE_COMMA, shift = true)
            '.' -> GuestStroke(KeyEvent.KEYCODE_PERIOD)
            '>' -> GuestStroke(KeyEvent.KEYCODE_PERIOD, shift = true)
            '/' -> GuestStroke(KeyEvent.KEYCODE_SLASH)
            '?' -> GuestStroke(KeyEvent.KEYCODE_SLASH, shift = true)
            else -> null
        }
    }

    private data class GuestStroke(val keyCode: Int, val shift: Boolean = false)

    /** edk2-gunyah, the setup ESP, and the tiny11 ISO. Same shape as the QEMU boot that reached Windows Setup. */
    /** One start at a time for the whole app: two VMs on one disk corrupt it. */
    private val booting = java.util.concurrent.atomic.AtomicBoolean(false)
    const val BUSY = "busy: Windows is already starting or running"

    fun bootWindows(
        ctx: Context,
        onVmStarted: () -> Unit = {},
        onVmStopped: () -> Unit = {},
        machine: Machine? = Machine.active(ctx),
        onConsole: (String) -> Unit,
    ): String {
        if (this.machine != null || !booting.compareAndSet(false, true)) return BUSY
        try {
            return bootWindowsOnce(ctx, onVmStarted, onVmStopped, machine, onConsole)
        } finally {
            booting.set(false)
        }
    }

    private fun bootWindowsOnce(
        ctx: Context,
        onVmStarted: () -> Unit,
        onVmStopped: () -> Unit,
        machine: Machine?,
        onConsole: (String) -> Unit,
    ): String {
        val dir = File(ctx.filesDir, "win").apply { mkdirs() }
        val firmware = File(dir, "edk2-gunyah.fd")
        copyAsset(ctx, "edk2-gunyah.fd", firmware)
        if (machine != null && machine.prepared) {
            // A disk with older boot files only knows the default frame size.
            MachinePrep.hardenBoot(ctx, machine)
            val (width, height) = if (MachinePrep.refreshLoader(ctx, machine)) machine.frameSize()
                else AvfPlan.FRAME_WIDTH to AvfPlan.FRAME_HEIGHT
            val window = AvfPlan.createWindow(ByteArray(0), width, height)
            sharedWindow?.close()
            sharedWindow = window
                startDebugTap(window, File(dir, "frame.bgra"))
            startCppcCounter(window)
            val report = boot(
                ctx,
                GuestBoot.setupImage(firmware, listOf(machine.disk)),
                keep = true,
                memoryMb = machine.memoryMb + AvfPlan.WINDOW_SIZE_MIB,
                cpus = machine.cpuCount,
                expect = "HYPERWIN GOP",
                sharedWindow = window,
                onVmStarted = onVmStarted,
                onVmStopped = onVmStopped,
                onConsole = onConsole,
            )
            if (!report.startsWith("VM running;")) {
                sharedWindow = null
                window.close()
            }
            return report
        }
        val esp = File(dir, "setup-esp.img")
        val iso = File(dir, "tiny11.iso")
        copyAsset(ctx, "setup-esp.img", esp)
        /* Driver bring-up only: a Windows PE disk staged by adb replaces the ISO. */
        val peStaged = File("/data/local/tmp/hyperwin/pe.img")
        val pe = File(dir, "pe.img")
        if (!peStaged.isFile) pe.delete()
        else if (peStaged.length() != pe.length() || peStaged.lastModified() > pe.lastModified()) peStaged.copyTo(pe, overwrite = true)
        val media = if (pe.isFile) pe else iso
        if (media === iso && (!iso.isFile || iso.length() == 0L)) {
            val staged = File("/data/local/tmp/tiny11-noprompt.iso")
            if (!staged.isFile) {
                return fail("Windows ISO is not staged at ${staged.absolutePath}")
            }
            staged.copyTo(iso, overwrite = true)
        }
        GuestBoot.requireAligned(esp)
        GuestBoot.requireAligned(media)
        val window = AvfPlan.createWindow(ByteArray(0))
        sharedWindow?.close()
        sharedWindow = window
        startDebugTap(window, File(dir, "frame.bgra"))
            startCppcCounter(window)
        val report = boot(
            ctx,
            GuestBoot.setupImage(firmware, listOf(esp, media)),
            keep = true,
            memoryMb = AvfPlan.WINDOWS_VM_MEMORY_MIB,
            expect = "HYPERWIN GOP",
            sharedWindow = window,
            onVmStarted = onVmStarted,
            onVmStopped = onVmStopped,
            onConsole = onConsole,
        )
        if (!report.startsWith("VM running;")) {
            sharedWindow = null
            window.close()
        }
        return report
    }

    private fun copyAsset(ctx: Context, name: String, dest: File) {
        ctx.assets.open(name).use { input ->
            dest.outputStream().use { input.copyTo(it) }
        }
    }

    fun stopWindows(onVmStopped: () -> Unit = {}): String {
        val vm = machine ?: return "No running VM is attached to this app session."
        return try {
            vm.javaClass.getMethod("stop").invoke(vm)
            machine = null
            VsockInput.stop()
            GuestAgent.stop()
            runCatching { onVmStopped() }
            sharedWindow?.let { window ->
                sharedWindow = null
                window.close()
            }
            "VM stopped."
        } catch (e: Throwable) {
            val cause = generateSequence(e) { it.cause }.last()
            fail("Could not stop the VM: ${cause.message ?: cause.javaClass.simpleName}")
        }
    }

    /**
     * The processor SSDT's reference and delivered performance counters (include/hw_abi.h HW_CPPC)
     * both read one value in the window. Windows checks that they run, so it advances in step with
     * time; equal counters tell Windows the processor runs at its rated speed.
     */
    private fun startCppcCounter(window: AvfPlan.SharedWindow) {
        Thread({
            val b = window.buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN)
            while (sharedWindow === window) {
                val now = System.nanoTime() / 1000
                runCatching { b.putLong(0xB00008, now); b.putLong(0xB00010, now) }   // HW_CPPC_REGS +0, +8
                Thread.sleep(5)                     // Windows samples them every few tens of ms
            }
        }, "hw-cppc").apply { isDaemon = true }.start()
    }

    /** Guest debug channel: the guest writes a PC ring at window offset 0x7f00000; log it when it changes. */
    private fun startDebugTap(window: AvfPlan.SharedWindow, frameDump: File? = null) {
        Thread({
            var ticks = 0
            val b = window.buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN)
            var lastCount = -1L
            var lastMarks = ""
            var lastWindow = ""
            var lastDiag = ""
            while (sharedWindow === window) {
                runCatching {
                    val magic = ByteArray(8).also { for (i in 0 until 8) it[i] = b.get(i) }.decodeToString()
                    val flags = b.getInt(52)
                    val seq = b.getInt(44)
                    val pixel = b.getInt(AvfPlan.frameOffset)
                    val windowState = "$magic flags=${flags.toUInt().toString(16)} seq=$seq pixel=${pixel.toUInt().toString(16)}"
                    if (windowState != lastWindow) {
                        lastWindow = windowState
                        Log.i("HyperWinDbg", windowState)
                    }
                    val base = 0x7f00000
                    val marks = StringBuilder()
                    for (i in 0 until 20) marks.append(java.lang.Long.toHexString(b.getLong(base + 0x100 + 8 * i))).append(' ')
                    if (marks.toString() != lastMarks) {
                        lastMarks = marks.toString()
                        Log.i("HyperWinDbg", "marks " + lastMarks)
                        val images = (32 until 160).map { b.getLong(base + 0x100 + 8 * it) }.filter { it != 0L }
                        if (images.isNotEmpty()) Log.i("HyperWinDbg", "images " + images.joinToString(" ") { java.lang.Long.toHexString(it) })
                    }
                    val diag = "diag " + (0 until 128).joinToString(" ") { Integer.toHexString(b.getInt(0xA00000 + 4 * it)) }
                    if (diag != lastDiag) { lastDiag = diag; Log.i("HyperWinDbg", diag) }
                    if (b.getLong(base) == 0x1111222233334444L && lastCount != 1L) {
                        lastCount = 1L
                        fun h(o: Int) = java.lang.Long.toHexString(b.getLong(base + o))
                        Log.i("HyperWinDbg", "GUEST EXCEPTION vector=" + h(8) + " esr=" + h(16) + " elr=" + h(24) +
                            " far=" + h(32) + " lr=" + h(40) + " sp=" + h(48) + " spsr=" + h(56))
                    }
                }
                /* Debug builds: save the guest frame (BGRA) every few seconds so it can
                 * be pulled with adb when the app's own controls cover part of it. */
                if (frameDump != null && ++ticks % 8 == 0) runCatching {
                    val frame = ByteArray(AvfPlan.frameBytes)
                    val view = window.buffer.duplicate()
                    view.position(AvfPlan.frameOffset)
                    view.get(frame)
                    val tmp = File(frameDump.parentFile, frameDump.name + ".tmp")
                    tmp.writeBytes(frame)
                    tmp.renameTo(frameDump)
                }
                Thread.sleep(700)
            }
        }, "hw-debug-tap").apply { isDaemon = true }.start()
    }

    private fun boot(
        ctx: Context,
        custom: Any,
        keep: Boolean,
        memoryMb: Int,
        cpus: Int = 1,
        expect: String,
        sharedWindow: AvfPlan.SharedWindow? = null,
        onVmStarted: () -> Unit = {},
        onVmStopped: () -> Unit = {},
        onConsole: (String) -> Unit = {},
    ): String {
        val device = android.os.Build.MODEL
        return try {
            val vmmClass = Class.forName(PKG + "VirtualMachineManager")
            val vmm = ctx.getSystemService(vmmClass) ?: return fail("VirtualMachineManager null on $device")
            val cfgClass = Class.forName(PKG + "VirtualMachineConfig\$Builder")
            val cfg = cfgClass.getConstructor(Context::class.java).newInstance(ctx)
            cfgClass.getMethod("setCustomImageConfig", custom.javaClass).invoke(cfg, custom)
            cfgClass.getMethod("setMemoryBytes", Long::class.javaPrimitiveType).invoke(cfg, memoryMb.toLong() shl 20)
            if (cpus > 1) cfgClass.getMethod("setCpuTopology", Int::class.javaPrimitiveType).invoke(cfg, 1)
            cfgClass.getMethod("setDebugLevel", Int::class.javaPrimitiveType).invoke(cfg, 1)
            cfgClass.getMethod("setProtectedVm", Boolean::class.javaPrimitiveType).invoke(cfg, false)
            cfgClass.getMethod("setVmOutputCaptured", Boolean::class.javaPrimitiveType).invoke(cfg, true)
            cfgClass.getMethod("setConnectVmConsole", Boolean::class.javaPrimitiveType).invoke(cfg, false)
            val config = cfgClass.getMethod("build").invoke(cfg)
            val name = NAMES[nextName++ % NAMES.size]
            runCatching { vmmClass.getMethod("delete", String::class.java).invoke(vmm, name) }
            runCatching { vmmClass.getMethod("delete", String::class.java).invoke(vmm, "hyperwin") }   // older builds' name
            val vm = vmmClass.getMethod("create", String::class.java, config.javaClass).invoke(vmm, name, config)
            val events = LinkedBlockingQueue<String>()
            // AVF can report one exit twice (onError, then onStopped). Act on the first only, or
            // a restart would start two VMs on the same disk.
            val ended = java.util.concurrent.atomic.AtomicBoolean(false)
            val cbClass = Class.forName(PKG + "VirtualMachineCallback")
            // AVF hands back the same VM object for the same name, so a stop of the previous VM can
            // arrive after this one is running. A stop while the object says "running" is that one.
            val launched = java.util.concurrent.atomic.AtomicBoolean(false)   // nothing of ours ends before run()
            fun stale(): Boolean = !launched.get() || runCatching {
                vm.javaClass.getMethod("getStatus").invoke(vm) == vm.javaClass.getField("STATUS_RUNNING").getInt(null)
            }.getOrDefault(false)
            val callback = java.lang.reflect.Proxy.newProxyInstance(cbClass.classLoader, arrayOf(cbClass)) { proxy, m, args ->
                if ((m.name == "onError" || m.name == "onStopped") && stale()) {
                    Log.i(TAG, "ignoring a late ${m.name} from the previous VM")
                    return@newProxyInstance null
                }
                when (m.name) {
                    "onPayloadStarted", "onPayloadReady" -> { events.add("started"); null }
                    "onError" -> {
                        events.add("error: ${args?.getOrNull(1)} ${args?.getOrNull(2)}")
                        if (ended.compareAndSet(false, true)) {
                            lastStopWasReboot = false
                            handleVmStopped(sharedWindow, onVmStopped)
                        }
                        null
                    }
                    "onStopped" -> {
                        events.add("stopped: ${args?.getOrNull(1)}")
                        if (ended.compareAndSet(false, true)) {
                            lastStopWasReboot = args?.getOrNull(1) ==
                                runCatching { cbClass.getField("STOP_REASON_REBOOT").getInt(null) }.getOrNull()
                            handleVmStopped(sharedWindow, onVmStopped)
                        }
                        null
                    }
                    "hashCode" -> System.identityHashCode(proxy)
                    "equals" -> proxy === args?.get(0)
                    else -> null
                }
            }
            vm.javaClass.getMethod("setCallback", java.util.concurrent.Executor::class.java, cbClass)
                .invoke(vm, ctx.mainExecutor, callback)
            machine = null
            val backingHook = sharedWindow?.let { AvfPlan.installWindowBackingHook(it) }
            try {
                launched.set(true)
                vm.javaClass.getMethod("run").invoke(vm)
            } finally {
                backingHook?.close()
            }
            if (keep) {
                machine = vm
                runCatching { onVmStarted() }
                    .onFailure { Log.e(TAG, "VM started, but startup notification failed", it) }
            }
            val console = StringBuilder()
            val done = AtomicReference<String?>(null)
            (vm.javaClass.getMethod("getConsoleOutput").invoke(vm) as? InputStream)?.let { stream ->
                Thread {
                    val buf = ByteArray(4096)
                    runCatching {
                        while (true) {
                            val n = stream.read(buf)
                            if (n < 0) break
                            val chunk = String(buf, 0, n)
                            synchronized(console) { console.append(chunk) }
                            Log.i(TAG, chunk.trim())
                            onConsole(chunk)
                        }
                    }
                }.apply { isDaemon = true; start() }
            }
            val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(if (keep) 45 else 40)
            while (System.nanoTime() < deadline && done.get() == null &&
                synchronized(console) { !console.contains(expect) }) {
                // The firmware prints this when it faults; it then hangs, so there is nothing to wait for.
                if (keep && synchronized(console) { console.contains("Synchronous Exception") }) {
                    done.set("error: the firmware stopped while starting")
                    break
                }
                val event = events.poll(250, TimeUnit.MILLISECONDS) ?: continue
                Log.i(TAG, event)
                if (event.startsWith("error") || event.startsWith("stopped")) done.set(event)
            }
            if (keep && done.get() == null && synchronized(console) { !console.contains(expect) })
                done.set("error: the firmware did not reach the Windows loader")
            val tail = synchronized(console) { console.toString().takeLast(2000) }
            if (!keep || done.get() != null) {
                machine = null
                runCatching { vm.javaClass.getMethod("stop").invoke(vm) }
                runCatching { vmmClass.getMethod("delete", String::class.java).invoke(vmm, name) }
            } else {
                machine = vm
                VsockInput.start(vm)
                GuestAgent.start(ctx, vm, Machine.active(ctx)?.lanMode == true)
            }
            val result = done.get()?.let { "$it\n$tail" }
                ?: if (keep) "VM running; Windows desktop not yet verified\n$tail"
                else if (tail.contains(expect)) "payload banner received\n$tail"
                else "no banner in time\n$tail"
            Log.i(TAG, result)
            result
        } catch (e: Throwable) {
            machine = null
            val cause = generateSequence(e) { it.cause }.joinToString(" <- ") { "${it.javaClass.simpleName}: ${it.message}" }
            fail(cause)
        }
    }

    private fun fail(message: String): String {
        Log.e(TAG, message)
        return message
    }

    private fun handleVmStopped(window: AvfPlan.SharedWindow?, onVmStopped: () -> Unit) {
        machine = null
        VsockInput.stop()
        GuestAgent.stop()
        runCatching { onVmStopped() }
        if (window != null && sharedWindow === window) {
            sharedWindow = null
            window.close()
        }
    }
}
