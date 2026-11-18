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
 *   2, x (u16), y (u16), buttons, wheel, wheel (s16), hwheel (s16)   absolute pointer, 0..65535
 *   3, buttons (u16), lt, rt, lx ly rx ry (s16), connected   gamepad, XInput layout
 *   4, dx (s16), dy (s16), buttons, wheel (s16), hwheel (s16)       relative pointer, mouse counts
 * Buttons: 1 left, 2 right, 4 middle, 8 back, 16 forward. Wheels in 1/120 steps; the u8 wheel in
 * message 2 is whole steps, for drivers from before the s16 ones.
 */
object VsockInput {
    private const val TAG = "HyperWin"
    private const val PORT = 5650L
    private const val KEYBOARD = 1
    private const val POINTER = 2
    private const val RELATIVE = 4
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
    /** Where the user last pointed, as a fraction of the guest's height. The keyboard pans to keep it in view. */
    @Volatile var focusY = 0.5f
        private set

    fun start(machine: Any) {
        synchronized(lock) {
            // Always a new thread: the previous VM's loop may still be in its sleep and exits on its own.
            vm = machine
            closeLocked()
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
                    GamepadInput.resend()  // a controller already plugged in shows up at once
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

    /** The controller's whole state, in XInput's terms; the guest shows it as an Xbox controller. */
    fun pad(connected: Boolean, buttons: Int, lt: Int, rt: Int, lx: Int, ly: Int, rx: Int, ry: Int): Boolean {
        val b = java.nio.ByteBuffer.allocate(14).order(java.nio.ByteOrder.LITTLE_ENDIAN)
        b.put(3).putShort(buttons.toShort()).put(lt.toByte()).put(rt.toByte())
            .putShort(lx.toShort()).putShort(ly.toShort()).putShort(rx.toShort()).putShort(ry.toShort())
            .put(if (connected) 1 else 0)
        return send(b.array())
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

    /** What fingers on the guest view do. */
    enum class TouchMode { TOUCH, TRACKPAD }
    @Volatile var touchMode = TouchMode.TOUCH

    private const val TAP_MS = 250L
    private const val TAP_SLOP_PX = 24f
    private const val SCROLL_STEP_PX = 36f
    private const val PAD_SPEED = 1.4f
    private var gesture = 0                 // which path took this gesture: 0 pointer, 1 touch, 2 trackpad
    private var padX = 0f
    private var padY = 0f
    private var padTravel = 0f
    private var padFingers = 0
    private var padDownAt = 0L
    private var padDragging = false
    private var padLastTap = 0L
    private var padScroll = 0f

    /**
     * Finger on the guest view. Touch mode sends every finger to Windows as a touch screen contact
     * when the guest agent is up, else one finger as the pointer. Trackpad mode moves the pointer.
     */
    fun touch(event: MotionEvent, viewWidth: Int, viewHeight: Int): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_DOWN) gesture = when {
            touchMode == TouchMode.TRACKPAD -> 2
            HidInput.connected || GuestAgent.touchReady -> 1
            else -> 0
        }
        return when (gesture) {
            // Windows' own touch screen and Precision Touchpad when hwhid.sys is there (HidInput).
            1 -> {
                guestPosition(event.getX(0), event.getY(0), viewWidth, viewHeight, clamp = true)?.let { focusY = it.second / AXIS_MAX.toFloat() }
                HidInput.screen(event) { x, y -> guestPosition(x, y, viewWidth, viewHeight, clamp = true) } ||
                    contacts(event, viewWidth, viewHeight)
            }
            2 -> HidInput.pad(event, viewWidth, viewHeight) || trackpad(event, viewWidth, viewHeight)
            else -> finger(event, viewWidth, viewHeight)
        }
    }

    /** All fingers still down, as HW_AGENT_TOUCH: count, then id, x, y per contact. */
    private fun contacts(event: MotionEvent, viewWidth: Int, viewHeight: Int): Boolean {
        val gone = when (event.actionMasked) {
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> -1
            MotionEvent.ACTION_POINTER_UP -> event.actionIndex
            else -> -2
        }
        val out = java.io.ByteArrayOutputStream()
        out.write(0)
        var n = 0
        for (i in 0 until event.pointerCount) {
            if (gone == -1 || i == gone || n == 10) continue
            val (x, y) = guestPosition(event.getX(i), event.getY(i), viewWidth, viewHeight, clamp = true) ?: continue
            if (n == 0) focusY = y / AXIS_MAX.toFloat()
            out.write(event.getPointerId(i)); out.write(x and 0xff); out.write(x shr 8); out.write(y and 0xff); out.write(y shr 8)
            n++
        }
        return GuestAgent.touch(out.toByteArray().also { it[0] = n.toByte() })
    }

    /** Laptop trackpad: drag moves the pointer, tap clicks, two fingers scroll or right-click, tap then drag holds. */
    private fun trackpad(event: MotionEvent, viewWidth: Int, viewHeight: Int): Boolean {
        val skip = if (event.actionMasked == MotionEvent.ACTION_POINTER_UP) event.actionIndex else -1
        val live = (0 until event.pointerCount).filter { it != skip }
        val x = live.map { event.getX(it) }.average().toFloat()
        val y = live.map { event.getY(it) }.average().toFloat()
        val here = lastX to lastY
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                padTravel = 0f; padScroll = 0f; padFingers = 1; padDownAt = event.eventTime
                padDragging = event.eventTime - padLastTap < TAP_MS
                if (padDragging) pointer(here, 1)
            }
            MotionEvent.ACTION_POINTER_DOWN -> padFingers = maxOf(padFingers, event.pointerCount)
            MotionEvent.ACTION_MOVE -> {
                val dx = x - padX
                val dy = y - padY
                padTravel += kotlin.math.abs(dx) + kotlin.math.abs(dy)
                if (event.pointerCount >= 2) {
                    padScroll += dy
                    val steps = (padScroll / SCROLL_STEP_PX).toInt()
                    if (steps != 0) { padScroll -= steps * SCROLL_STEP_PX; pointer(here, buttons, steps * 120) }
                } else if (padTravel > TAP_SLOP_PX) {
                    val scale = minOf(viewWidth.toFloat() / AvfPlan.frameWidth, viewHeight.toFloat() / AvfPlan.frameHeight)
                    val nx = lastX + dx * PAD_SPEED * AXIS_MAX / (AvfPlan.frameWidth * scale)
                    val ny = lastY + dy * PAD_SPEED * AXIS_MAX / (AvfPlan.frameHeight * scale)
                    pointer(nx.toInt().coerceIn(0, AXIS_MAX) to ny.toInt().coerceIn(0, AXIS_MAX), buttons)
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                val tapped = event.actionMasked == MotionEvent.ACTION_UP && padTravel <= TAP_SLOP_PX &&
                    event.eventTime - padDownAt < TAP_MS
                if (padDragging) {
                    pointer(here, 0)
                } else if (tapped) {
                    val button = if (padFingers >= 2) 2 else 1
                    pointer(here, button)
                    pointer(here, 0)
                    if (button == 1) padLastTap = event.eventTime
                }
                padDragging = false
            }
        }
        padX = x
        padY = y
        return true
    }

    /** One finger as the pointer: absolute position, primary button while touching, two fingers right-click. */
    private fun finger(event: MotionEvent, viewWidth: Int, viewHeight: Int): Boolean {
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

    /** Hardware mouse: hover moves, buttons follow the Android button state, wheels scroll. */
    fun mouse(event: MotionEvent, viewWidth: Int, viewHeight: Int): Boolean {
        val pos = guestPosition(event.x, event.y, viewWidth, viewHeight)
        if (event.actionMasked == MotionEvent.ACTION_SCROLL) {
            val (v, h) = wheels(event)
            return (v == 0 && h == 0) || pointer(pos ?: (lastX to lastY), buttons, v, h)
        }
        pos ?: return true
        return pointer(pos, mouseButtons(event))
    }

    private fun mouseButtons(event: MotionEvent): Int {
        var state = 0
        if (event.buttonState and MotionEvent.BUTTON_PRIMARY != 0) state = state or 1
        if (event.buttonState and MotionEvent.BUTTON_SECONDARY != 0) state = state or 2
        if (event.buttonState and MotionEvent.BUTTON_TERTIARY != 0) state = state or 4
        if (event.buttonState and MotionEvent.BUTTON_BACK != 0) state = state or 8
        if (event.buttonState and MotionEvent.BUTTON_FORWARD != 0) state = state or 16
        return state
    }

    // Wheel fractions left over from rounding to 1/120 steps, carried into the next event.
    private var wheelRest = 0f
    private var hwheelRest = 0f

    /** The event's wheels in 1/120 steps, history included: a smooth wheel or touchpad keeps its fractions. */
    private fun wheels(event: MotionEvent): Pair<Int, Int> {
        var v = wheelRest
        var h = hwheelRest
        for (i in 0 until event.historySize) {
            v += event.getHistoricalAxisValue(MotionEvent.AXIS_VSCROLL, i) * 120
            h += event.getHistoricalAxisValue(MotionEvent.AXIS_HSCROLL, i) * 120
        }
        v += event.getAxisValue(MotionEvent.AXIS_VSCROLL) * 120
        h += event.getAxisValue(MotionEvent.AXIS_HSCROLL) * 120
        val vi = v.toInt().coerceIn(-32767, 32767)
        val hi = h.toInt().coerceIn(-32767, 32767)
        wheelRest = v - vi
        hwheelRest = h - hi
        return vi to hi
    }

    /** "Mouse speed in games", 0.25 to 4. */
    @Volatile var gameSpeed = 1f
    private var restX = 0f
    private var restY = 0f

    /**
     * A captured mouse (View.requestPointerCapture): movement since the last event, every sample in
     * the event's history added up, fractions carried over, and sent as relative mouse counts.
     */
    fun captured(event: MotionEvent): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_SCROLL) {
            val (v, h) = wheels(event)
            return relative(0, 0, buttons, v, h)
        }
        var dx = restX
        var dy = restY
        for (i in 0 until event.historySize) {
            dx += event.getHistoricalAxisValue(MotionEvent.AXIS_X, i) * gameSpeed
            dy += event.getHistoricalAxisValue(MotionEvent.AXIS_Y, i) * gameSpeed
        }
        dx += event.getAxisValue(MotionEvent.AXIS_X) * gameSpeed
        dy += event.getAxisValue(MotionEvent.AXIS_Y) * gameSpeed
        val ix = dx.toInt()
        val iy = dy.toInt()
        restX = dx - ix
        restY = dy - iy
        return relative(ix, iy, mouseButtons(event))
    }

    /** Relative movement in mouse counts, split into messages that fit 16 bits. */
    fun relative(dx: Int, dy: Int, state: Int, wheel: Int = 0, hwheel: Int = 0): Boolean = synchronized(lock) {
        if (!connected) return false
        if (dx == 0 && dy == 0 && wheel == 0 && hwheel == 0 && state == buttons) return true
        buttons = state
        var x = dx
        var y = dy
        do {
            val sx = x.coerceIn(-32767, 32767)
            val sy = y.coerceIn(-32767, 32767)
            x -= sx
            y -= sy
            val last = x == 0 && y == 0
            val w = if (last) wheel else 0
            val h = if (last) hwheel else 0
            if (!send(byteArrayOf(RELATIVE.toByte(), sx.toByte(), (sx shr 8).toByte(), sy.toByte(), (sy shr 8).toByte(),
                    state.toByte(), w.toByte(), (w shr 8).toByte(), h.toByte(), (h shr 8).toByte()))) return false
        } while (!last)
        true
    }

    private fun pointer(pos: Pair<Int, Int>, state: Int, wheel: Int = 0, hwheel: Int = 0): Boolean = synchronized(lock) {
        if (!connected) return false
        buttons = state
        lastX = pos.first
        lastY = pos.second
        focusY = lastY / AXIS_MAX.toFloat()
        send(byteArrayOf(
            POINTER.toByte(),
            (lastX and 0xff).toByte(), (lastX shr 8).toByte(),
            (lastY and 0xff).toByte(), (lastY shr 8).toByte(),
            state.toByte(), (wheel / 120).coerceIn(-127, 127).toByte(),
            wheel.toByte(), (wheel shr 8).toByte(), hwheel.toByte(), (hwheel shr 8).toByte(),
        ))
    }

    /** Same letterboxing as GuestDisplay.fitRect. Touches on the black bars are ignored. */
    private fun guestPosition(x: Float, y: Float, width: Int, height: Int, clamp: Boolean = false): Pair<Int, Int>? {
        if (width <= 0 || height <= 0) return null
        val scale = minOf(width.toFloat() / AvfPlan.frameWidth, height.toFloat() / AvfPlan.frameHeight)
        val w = AvfPlan.frameWidth * scale
        val h = AvfPlan.frameHeight * scale
        val fx = (x - (width - w) / 2f) / w
        val fy = (y - (height - h) / 2f) / h
        if (!clamp && (fx < -0.02f || fx > 1.02f || fy < -0.02f || fy > 1.02f)) return null
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
