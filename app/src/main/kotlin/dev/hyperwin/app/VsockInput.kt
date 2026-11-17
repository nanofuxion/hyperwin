package dev.hyperwin.app

import android.os.ParcelFileDescriptor
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import java.io.FileOutputStream
import java.io.IOException

/**
 * Keyboard and absolute pointer for the guest over virtio-vsock. The guest's hwvsinput.sys listens
 * on HW_VSOCK_PORT_INPUT. Each message is one length byte, then:
 *   1, make code (u16), flags (1 break, 2 E0)           a set 1 scan code
 *   2, x (u16), y (u16), buttons, wheel                  absolute pointer, 0..65535
 */
object VsockInput {
    private const val TAG = "HyperWin"
    private const val PORT = 5650L
    private const val KEYBOARD = 1
    private const val POINTER = 2
    private const val AXIS_MAX = 65535

    private val lock = Any()
    @Volatile private var vm: Any? = null
    private var pfd: ParcelFileDescriptor? = null
    private var out: FileOutputStream? = null
    private var thread: Thread? = null
    private var buttons = 0
    private var lastX = AXIS_MAX / 2
    private var lastY = AXIS_MAX / 2

    @Volatile var connected = false
        private set

    fun start(machine: Any) {
        synchronized(lock) {
            vm = machine
            if (thread?.isAlive == true) return
            thread = Thread({ connectLoop(machine) }, "hw-vsock-input").apply { isDaemon = true; start() }
        }
    }

    fun stop() {
        synchronized(lock) {
            vm = null
            closeLocked()
        }
    }

    private fun connectLoop(machine: Any) {
        var attempts = 0
        while (vm === machine) {
            if (!connected) {
                val socket = runCatching {
                    machine.javaClass.getMethod("connectVsock", Long::class.javaPrimitiveType)
                        .invoke(machine, PORT) as ParcelFileDescriptor
                }.getOrNull()
                if (socket != null) {
                    synchronized(lock) {
                        pfd = socket
                        out = FileOutputStream(socket.fileDescriptor)
                        connected = true
                        buttons = 0
                    }
                    Log.i(TAG, "vsock input connected after $attempts attempts")
                } else {
                    attempts++
                }
            }
            Thread.sleep(1000)
        }
    }

    private fun closeLocked() {
        connected = false
        runCatching { out?.close() }
        runCatching { pfd?.close() }
        out = null
        pfd = null
    }

    private fun send(report: ByteArray): Boolean = synchronized(lock) {
        val o = out ?: return false
        try {
            val msg = ByteArray(report.size + 1)
            msg[0] = report.size.toByte()
            System.arraycopy(report, 0, msg, 1, report.size)
            o.write(msg)
            true
        } catch (e: IOException) {
            Log.i(TAG, "vsock input dropped: ${e.message}")
            closeLocked()
            false
        }
    }

    fun key(event: KeyEvent): Boolean = synchronized(lock) {
        if (!connected) return false
        val up = when (event.action) {
            KeyEvent.ACTION_DOWN -> false
            KeyEvent.ACTION_UP -> true
            else -> return false
        }
        val evdev = if (event.scanCode != 0) event.scanCode else evdevCode(event.keyCode) ?: return false
        val (make, e0) = setOne(evdev) ?: return false
        send(byteArrayOf(KEYBOARD.toByte(), (make and 0xff).toByte(), (make shr 8).toByte(),
            ((if (up) 1 else 0) or (if (e0) 2 else 0)).toByte()))
    }

    /** Finger on the guest view: absolute position, primary button while touching, two fingers right-click. */
    fun touch(event: MotionEvent, viewWidth: Int, viewHeight: Int): Boolean {
        val ending = event.actionMasked == MotionEvent.ACTION_UP || event.actionMasked == MotionEvent.ACTION_CANCEL ||
            event.actionMasked == MotionEvent.ACTION_POINTER_UP
        val pos = guestPosition(event.getX(0), event.getY(0), viewWidth, viewHeight)
            ?: if (ending) lastX to lastY else return true
        return when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> pointer(pos, 1)
            MotionEvent.ACTION_MOVE -> pointer(pos, if (event.pointerCount > 1) buttons else 1)
            MotionEvent.ACTION_POINTER_DOWN -> pointer(pos, 2)
            MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> pointer(pos, 0)
            else -> true
        }
    }

    /** Hardware mouse: hover moves, buttons follow the Android button state, wheel scrolls. */
    fun mouse(event: MotionEvent, viewWidth: Int, viewHeight: Int): Boolean {
        val pos = guestPosition(event.x, event.y, viewWidth, viewHeight)
        if (event.actionMasked == MotionEvent.ACTION_SCROLL) {
            val wheel = event.getAxisValue(MotionEvent.AXIS_VSCROLL).toInt().coerceIn(-127, 127)
            return wheel == 0 || pointer(pos ?: (lastX to lastY), buttons, wheel)
        }
        pos ?: return true
        var state = 0
        if (event.buttonState and MotionEvent.BUTTON_PRIMARY != 0) state = state or 1
        if (event.buttonState and MotionEvent.BUTTON_SECONDARY != 0) state = state or 2
        if (event.buttonState and MotionEvent.BUTTON_TERTIARY != 0) state = state or 4
        return pointer(pos, state)
    }

    private fun pointer(pos: Pair<Int, Int>, state: Int, wheel: Int = 0): Boolean = synchronized(lock) {
        if (!connected) return false
        buttons = state
        lastX = pos.first
        lastY = pos.second
        send(byteArrayOf(
            POINTER.toByte(),
            (lastX and 0xff).toByte(), (lastX shr 8).toByte(),
            (lastY and 0xff).toByte(), (lastY shr 8).toByte(),
            state.toByte(), wheel.toByte(),
        ))
    }

    /** Same letterboxing as GuestDisplay.fitRect. Touches on the black bars are ignored. */
    private fun guestPosition(x: Float, y: Float, width: Int, height: Int): Pair<Int, Int>? {
        if (width <= 0 || height <= 0) return null
        val scale = minOf(width.toFloat() / AvfPlan.FRAME_WIDTH, height.toFloat() / AvfPlan.FRAME_HEIGHT)
        val w = AvfPlan.FRAME_WIDTH * scale
        val h = AvfPlan.FRAME_HEIGHT * scale
        val fx = (x - (width - w) / 2f) / w
        val fy = (y - (height - h) / 2f) / h
        if (fx < -0.02f || fx > 1.02f || fy < -0.02f || fy > 1.02f) return null
        return (fx.coerceIn(0f, 1f) * AXIS_MAX).toInt() to (fy.coerceIn(0f, 1f) * AXIS_MAX).toInt()
    }

    /** Linux evdev codes below 89 are the PC set 1 make codes; the navigation block needs E0. */
    private fun setOne(evdev: Int): Pair<Int, Boolean>? = when (evdev) {
        in 1..88 -> evdev to false
        96 -> 0x1C to true
        97 -> 0x1D to true
        98 -> 0x35 to true
        100 -> 0x38 to true
        102 -> 0x47 to true
        103 -> 0x48 to true
        104 -> 0x49 to true
        105 -> 0x4B to true
        106 -> 0x4D to true
        107 -> 0x4F to true
        108 -> 0x50 to true
        109 -> 0x51 to true
        110 -> 0x52 to true
        111 -> 0x53 to true
        125 -> 0x5B to true
        126 -> 0x5C to true
        127 -> 0x5D to true
        else -> null
    }

    private val letterEvdev = intArrayOf(
        30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
        49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,
    )
    private val digitEvdev = intArrayOf(11, 2, 3, 4, 5, 6, 7, 8, 9, 10)

    /** Soft keyboards report scanCode 0; map the Android key code to its evdev code. */
    private fun evdevCode(keyCode: Int): Int? {
        if (keyCode in KeyEvent.KEYCODE_A..KeyEvent.KEYCODE_Z) return letterEvdev[keyCode - KeyEvent.KEYCODE_A]
        if (keyCode in KeyEvent.KEYCODE_0..KeyEvent.KEYCODE_9) return digitEvdev[keyCode - KeyEvent.KEYCODE_0]
        if (keyCode in KeyEvent.KEYCODE_F1..KeyEvent.KEYCODE_F10) return 59 + keyCode - KeyEvent.KEYCODE_F1
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
            KeyEvent.KEYCODE_MOVE_HOME -> 102
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
}
