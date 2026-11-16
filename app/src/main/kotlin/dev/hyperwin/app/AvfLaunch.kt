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
    private const val NAME = "hyperwin"
    private val keyboardLock = Any()
    private val letterEvdevCodes = intArrayOf(
        30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
        49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,
    )
    private val digitEvdevCodes = intArrayOf(11, 2, 3, 4, 5, 6, 7, 8, 9, 10)

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
        private set
    @Volatile var sharedWindow: AvfPlan.SharedWindow? = null
        private set

    fun touch(event: android.view.MotionEvent): Boolean = send("sendMultiTouchEvent", event)

    fun mouse(event: android.view.MotionEvent): Boolean = send("sendMouseEvent", event)

    fun key(event: KeyEvent): Boolean = synchronized(keyboardLock) {
        val vm = machine ?: return false
        val guestEvent = event.withGuestScanCode() ?: return false
        runCatching {
            // AVF accepts the Android KeyEvent and reads its Linux scan code.
            vm.javaClass.getMethod("sendKeyEvent", KeyEvent::class.java).invoke(vm, guestEvent) as Boolean
        }.getOrDefault(false)
    }

    /** Sends basic Android IME text as guest key presses for touch-only phones. */
    fun typeText(text: CharSequence): TextSendResult = synchronized(keyboardLock) {
        val normalized = text.toString().replace("\r\n", "\n").replace('\r', '\n')
        if (machine == null) {
            return TextSendResult(0, normalized.length, TextSendFailure.GUEST_UNAVAILABLE)
        }
        val strokes = normalized.map {
            guestStroke(it) ?: return TextSendResult(0, normalized.length, TextSendFailure.UNSUPPORTED_CHARACTER)
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

    /** Software and some virtual keyboards report scanCode=0; provide their evdev equivalent. */
    private fun KeyEvent.withGuestScanCode(): KeyEvent? {
        if (scanCode != 0) return this
        val code = evdevCode(keyCode) ?: return null
        return KeyEvent(
            downTime, eventTime, action, keyCode, repeatCount, metaState,
            deviceId, code, flags, source,
        )
    }

    private fun evdevCode(keyCode: Int): Int? {
        if (keyCode in KeyEvent.KEYCODE_A..KeyEvent.KEYCODE_Z) {
            return letterEvdevCodes[keyCode - KeyEvent.KEYCODE_A]
        }
        if (keyCode in KeyEvent.KEYCODE_0..KeyEvent.KEYCODE_9) {
            return digitEvdevCodes[keyCode - KeyEvent.KEYCODE_0]
        }
        if (keyCode in KeyEvent.KEYCODE_F1..KeyEvent.KEYCODE_F10) {
            return 59 + keyCode - KeyEvent.KEYCODE_F1
        }
        return when (keyCode) {
            KeyEvent.KEYCODE_ESCAPE -> 1
            KeyEvent.KEYCODE_MINUS -> 12
            KeyEvent.KEYCODE_EQUALS -> 13
            KeyEvent.KEYCODE_DEL -> 14
            KeyEvent.KEYCODE_TAB -> 15
            KeyEvent.KEYCODE_LEFT_BRACKET -> 26
            KeyEvent.KEYCODE_RIGHT_BRACKET -> 27
            KeyEvent.KEYCODE_ENTER -> 28
            KeyEvent.KEYCODE_CTRL_LEFT -> 29
            KeyEvent.KEYCODE_SEMICOLON -> 39
            KeyEvent.KEYCODE_APOSTROPHE -> 40
            KeyEvent.KEYCODE_GRAVE -> 41
            KeyEvent.KEYCODE_SHIFT_LEFT -> 42
            KeyEvent.KEYCODE_BACKSLASH -> 43
            KeyEvent.KEYCODE_COMMA -> 51
            KeyEvent.KEYCODE_PERIOD -> 52
            KeyEvent.KEYCODE_SLASH -> 53
            KeyEvent.KEYCODE_SHIFT_RIGHT -> 54
            KeyEvent.KEYCODE_ALT_LEFT -> 56
            KeyEvent.KEYCODE_SPACE -> 57
            KeyEvent.KEYCODE_CAPS_LOCK -> 58
            KeyEvent.KEYCODE_F11 -> 87
            KeyEvent.KEYCODE_F12 -> 88
            KeyEvent.KEYCODE_NUMPAD_ENTER -> 96
            KeyEvent.KEYCODE_CTRL_RIGHT -> 97
            KeyEvent.KEYCODE_ALT_RIGHT -> 100
            KeyEvent.KEYCODE_MOVE_HOME, KeyEvent.KEYCODE_HOME -> 102
            KeyEvent.KEYCODE_DPAD_UP -> 103
            KeyEvent.KEYCODE_PAGE_UP -> 104
            KeyEvent.KEYCODE_DPAD_LEFT -> 105
            KeyEvent.KEYCODE_DPAD_RIGHT -> 106
            KeyEvent.KEYCODE_MOVE_END -> 107
            KeyEvent.KEYCODE_DPAD_DOWN -> 108
            KeyEvent.KEYCODE_PAGE_DOWN -> 109
            KeyEvent.KEYCODE_INSERT -> 110
            KeyEvent.KEYCODE_FORWARD_DEL -> 111
            KeyEvent.KEYCODE_META_LEFT -> 125
            KeyEvent.KEYCODE_META_RIGHT -> 126
            KeyEvent.KEYCODE_MENU -> 127
            else -> null
        }
    }

    private data class GuestStroke(val keyCode: Int, val shift: Boolean = false)

    private fun send(method: String, event: android.view.MotionEvent): Boolean {
        val vm = machine ?: return false
        return runCatching {
            vm.javaClass.getMethod(method, android.view.MotionEvent::class.java).invoke(vm, event) as Boolean
        }.getOrDefault(false)
    }

    /** edk2-gunyah, the setup ESP, and the tiny11 ISO. Same shape as the QEMU boot that reached Windows Setup. */
    fun bootWindows(
        ctx: Context,
        onVmStarted: () -> Unit = {},
        onVmStopped: () -> Unit = {},
        onConsole: (String) -> Unit,
    ): String {
        val dir = File(ctx.filesDir, "win").apply { mkdirs() }
        val firmware = File(dir, "edk2-gunyah.fd")
        val esp = File(dir, "setup-esp.img")
        val iso = File(dir, "tiny11.iso")
        copyAsset(ctx, "edk2-gunyah.fd", firmware)
        copyAsset(ctx, "setup-esp.img", esp)
        if (!iso.isFile || iso.length() == 0L) {
            val staged = File("/data/local/tmp/tiny11-noprompt.iso")
            if (!staged.isFile) {
                return fail("Windows ISO is not staged at ${staged.absolutePath}")
            }
            staged.copyTo(iso, overwrite = true)
        }
        GuestBoot.requireAligned(esp)
        GuestBoot.requireAligned(iso)
        val window = AvfPlan.createWindow(ByteArray(0))
        sharedWindow?.close()
        sharedWindow = window
        startDebugTap(window, File(dir, "frame.bgra"))
        val report = boot(
            ctx,
            GuestBoot.setupImage(firmware, listOf(esp, iso)),
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

    /** Guest debug channel: the guest writes a PC ring at window offset 0x7f00000; log it when it changes. */
    private fun startDebugTap(window: AvfPlan.SharedWindow, frameDump: File? = null) {
        Thread({
            var ticks = 0
            val b = window.buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN)
            var lastCount = -1L
            var lastMarks = ""
            var lastWindow = ""
            while (sharedWindow === window) {
                runCatching {
                    val magic = ByteArray(8).also { for (i in 0 until 8) it[i] = b.get(i) }.decodeToString()
                    val flags = b.getInt(52)
                    val seq = b.getInt(44)
                    val pixel = b.getInt(0x10000)
                    val windowState = "$magic flags=${flags.toUInt().toString(16)} seq=$seq pixel=${pixel.toUInt().toString(16)}"
                    if (windowState != lastWindow) {
                        lastWindow = windowState
                        Log.i("HyperWinDbg", windowState)
                    }
                    val base = 0x7f00000
                    val marks = StringBuilder()
                    for (i in 0 until 20) marks.append(java.lang.Long.toHexString(b.getLong(base + 0x100 + 8 * i))).append(' ')
                    if (marks.toString() != lastMarks) { lastMarks = marks.toString(); Log.i("HyperWinDbg", "marks " + lastMarks) }
                    if (b.getLong(base) == 0x1111222233334444L && lastCount != 1L) {
                        lastCount = 1L
                        fun h(o: Int) = java.lang.Long.toHexString(b.getLong(base + o))
                        Log.i("HyperWinDbg", "GUEST EXCEPTION vector=" + h(8) + " esr=" + h(16) + " elr=" + h(24) +
                            " far=" + h(32) + " lr=" + h(40) + " sp=" + h(48) + " spsr=" + h(56))
                    }
                }
                /* Debug builds: save the guest frame (1280x720 BGRA) every few seconds so it can
                 * be pulled with adb when the app's own controls cover part of it. */
                if (frameDump != null && ++ticks % 8 == 0) runCatching {
                    val frame = ByteArray(1280 * 720 * 4)
                    val view = window.buffer.duplicate()
                    view.position(0x10000)
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
            cfgClass.getMethod("setDebugLevel", Int::class.javaPrimitiveType).invoke(cfg, 1)
            cfgClass.getMethod("setProtectedVm", Boolean::class.javaPrimitiveType).invoke(cfg, false)
            cfgClass.getMethod("setVmOutputCaptured", Boolean::class.javaPrimitiveType).invoke(cfg, true)
            cfgClass.getMethod("setConnectVmConsole", Boolean::class.javaPrimitiveType).invoke(cfg, false)
            val config = cfgClass.getMethod("build").invoke(cfg)
            runCatching { vmmClass.getMethod("delete", String::class.java).invoke(vmm, NAME) }
            val vm = vmmClass.getMethod("create", String::class.java, config.javaClass).invoke(vmm, NAME, config)
            val events = LinkedBlockingQueue<String>()
            val cbClass = Class.forName(PKG + "VirtualMachineCallback")
            val callback = java.lang.reflect.Proxy.newProxyInstance(cbClass.classLoader, arrayOf(cbClass)) { proxy, m, args ->
                when (m.name) {
                    "onPayloadStarted", "onPayloadReady" -> { events.add("started"); null }
                    "onError" -> {
                        events.add("error: ${args?.getOrNull(1)} ${args?.getOrNull(2)}")
                        handleVmStopped(sharedWindow, onVmStopped)
                        null
                    }
                    "onStopped" -> {
                        events.add("stopped: ${args?.getOrNull(1)}")
                        handleVmStopped(sharedWindow, onVmStopped)
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
                val event = events.poll(250, TimeUnit.MILLISECONDS) ?: continue
                Log.i(TAG, event)
                if (event.startsWith("error") || event.startsWith("stopped")) done.set(event)
            }
            val tail = synchronized(console) { console.toString().takeLast(2000) }
            if (!keep || done.get() != null) {
                machine = null
                runCatching { vm.javaClass.getMethod("stop").invoke(vm) }
                runCatching { vmmClass.getMethod("delete", String::class.java).invoke(vmm, NAME) }
            } else {
                machine = vm
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
        runCatching { onVmStopped() }
        if (window != null && sharedWindow === window) {
            sharedWindow = null
            window.close()
        }
    }
}
