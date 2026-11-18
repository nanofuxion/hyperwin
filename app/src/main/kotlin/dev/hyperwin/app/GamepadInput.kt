package dev.hyperwin.app

import android.hardware.input.InputManager
import android.content.Context
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent

/**
 * Forwards any Android gamepad to Windows over the input vsock, where hwvsinput.sys shows it as an
 * Xbox controller to XInput. Works for USB, Bluetooth, and anything else Android exposes as
 * SOURCE_GAMEPAD/JOYSTICK.
 */
object GamepadInput : InputManager.InputDeviceListener {
    private const val STICK_MAX = 32767
    private const val TRIGGER_MAX = 255

    private const val XINPUT_DPAD_UP = 0x0001
    private const val XINPUT_DPAD_DOWN = 0x0002
    private const val XINPUT_DPAD_LEFT = 0x0004
    private const val XINPUT_DPAD_RIGHT = 0x0008
    private const val XINPUT_START = 0x0010
    private const val XINPUT_BACK = 0x0020
    private const val XINPUT_LEFT_THUMB = 0x0040
    private const val XINPUT_RIGHT_THUMB = 0x0080
    private const val XINPUT_LEFT_SHOULDER = 0x0100
    private const val XINPUT_RIGHT_SHOULDER = 0x0200
    private const val XINPUT_GUIDE = 0x0400
    private const val XINPUT_A = 0x1000
    private const val XINPUT_B = 0x2000
    private const val XINPUT_X = 0x4000
    private const val XINPUT_Y = 0x8000

    private val keyMap = mapOf(
        KeyEvent.KEYCODE_BUTTON_A to XINPUT_A,
        KeyEvent.KEYCODE_BUTTON_B to XINPUT_B,
        KeyEvent.KEYCODE_BUTTON_X to XINPUT_X,
        KeyEvent.KEYCODE_BUTTON_Y to XINPUT_Y,
        KeyEvent.KEYCODE_BUTTON_L1 to XINPUT_LEFT_SHOULDER,
        KeyEvent.KEYCODE_BUTTON_R1 to XINPUT_RIGHT_SHOULDER,
        KeyEvent.KEYCODE_BUTTON_THUMBL to XINPUT_LEFT_THUMB,
        KeyEvent.KEYCODE_BUTTON_THUMBR to XINPUT_RIGHT_THUMB,
        KeyEvent.KEYCODE_BUTTON_START to XINPUT_START,
        KeyEvent.KEYCODE_BUTTON_SELECT to XINPUT_BACK,
        KeyEvent.KEYCODE_BUTTON_MODE to XINPUT_GUIDE,
        KeyEvent.KEYCODE_DPAD_UP to XINPUT_DPAD_UP,
        KeyEvent.KEYCODE_DPAD_DOWN to XINPUT_DPAD_DOWN,
        KeyEvent.KEYCODE_DPAD_LEFT to XINPUT_DPAD_LEFT,
        KeyEvent.KEYCODE_DPAD_RIGHT to XINPUT_DPAD_RIGHT,
    )

    private var keyButtons = 0
    private var hatButtons = 0
    private var lx = 0; private var ly = 0
    private var rx = 0; private var ry = 0
    private var axisLt = 0; private var axisRt = 0
    private var keyLt = 0; private var keyRt = 0
    private var connected = false

    fun register(ctx: Context) {
        ctx.getSystemService(InputManager::class.java).registerInputDeviceListener(this, null)
        connected = anyGamepad()
    }

    private fun anyGamepad() = InputDevice.getDeviceIds().any { id -> InputDevice.getDevice(id)?.let { isGamepad(it.sources) } == true }

    /** Sends the current state again, for a link that just came up. */
    fun resend() = update()

    fun isGamepad(source: Int): Boolean =
        (source and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
            (source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK

    fun onKey(event: KeyEvent): Boolean {
        if (!isGamepad(event.source)) return false
        val bit = keyMap[event.keyCode]
        if (bit != null) {
            keyButtons = if (event.action == KeyEvent.ACTION_DOWN) keyButtons or bit else keyButtons and bit.inv()
            connected = true
            update()
            return true
        }
        when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_L2 -> { keyLt = if (event.action == KeyEvent.ACTION_DOWN) TRIGGER_MAX else 0 }
            KeyEvent.KEYCODE_BUTTON_R2 -> { keyRt = if (event.action == KeyEvent.ACTION_DOWN) TRIGGER_MAX else 0 }
            else -> return false
        }
        connected = true
        update()
        return true
    }

    fun onMotion(event: MotionEvent): Boolean {
        if (!isGamepad(event.source)) return false
        lx = stick(event.getAxisValue(MotionEvent.AXIS_X))
        ly = stick(-event.getAxisValue(MotionEvent.AXIS_Y))
        rx = stick(event.getAxisValue(MotionEvent.AXIS_Z))
        ry = stick(-event.getAxisValue(MotionEvent.AXIS_RZ))
        axisLt = trigger(maxOf(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE)))
        axisRt = trigger(maxOf(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS)))
        hatButtons = 0
        val hx = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hy = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        if (hx < -0.5f) hatButtons = hatButtons or XINPUT_DPAD_LEFT
        else if (hx > 0.5f) hatButtons = hatButtons or XINPUT_DPAD_RIGHT
        if (hy < -0.5f) hatButtons = hatButtons or XINPUT_DPAD_UP
        else if (hy > 0.5f) hatButtons = hatButtons or XINPUT_DPAD_DOWN
        connected = true
        update()
        return true
    }

    override fun onInputDeviceAdded(deviceId: Int) {
        if (!connected && InputDevice.getDevice(deviceId)?.let { isGamepad(it.sources) } == true) {
            connected = true
            update()
        }
    }
    override fun onInputDeviceChanged(deviceId: Int) {}
    override fun onInputDeviceRemoved(deviceId: Int) {
        if (!anyGamepad() && connected) {
            connected = false
            keyButtons = 0; hatButtons = 0
            lx = 0; ly = 0; rx = 0; ry = 0
            axisLt = 0; axisRt = 0; keyLt = 0; keyRt = 0
            update()
        }
    }

    private fun update() {
        VsockInput.pad(connected, keyButtons or hatButtons, maxOf(axisLt, keyLt), maxOf(axisRt, keyRt), lx, ly, rx, ry)
    }

    private fun stick(v: Float): Int = (v.coerceIn(-1f, 1f) * STICK_MAX).toInt()
    private fun trigger(v: Float): Int = (v.coerceIn(0f, 1f) * TRIGGER_MAX).toInt()
}
