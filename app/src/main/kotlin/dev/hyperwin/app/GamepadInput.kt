package dev.hyperwin.app

import android.content.Context
import android.hardware.input.InputManager
import android.os.CombinedVibration
import android.os.Handler
import android.os.Looper
import android.os.VibrationAttributes
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent

/**
 * Forwards Android gamepads to Windows over the input vsock, where hwvsinput.sys shows each one as
 * an Xbox controller to XInput (gamepad/README.md). Up to four players: each controller gets a slot,
 * kept by its descriptor, so one that reconnects is the same player again. Rumble comes back from
 * Windows (message 6) and goes to the controller's motors, and for player 1 to the phone too.
 */
object GamepadInput : InputManager.InputDeviceListener {
    private const val TAG = "HyperWin"
    private const val SLOTS = 4
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

    /** One player's controller, as XInput sees it. */
    private class Pad(val slot: Int) {
        var deviceId = -1
        var descriptor = ""
        var keyButtons = 0
        var hatButtons = 0
        var lx = 0; var ly = 0; var rx = 0; var ry = 0
        var axisLt = 0; var axisRt = 0; var keyLt = 0; var keyRt = 0
        var connected = false
        fun clear() {
            keyButtons = 0; hatButtons = 0; lx = 0; ly = 0; rx = 0; ry = 0
            axisLt = 0; axisRt = 0; keyLt = 0; keyRt = 0
        }
    }

    private val pads = Array(SLOTS) { Pad(it) }
    private var appContext: Context? = null
    private val main = Handler(Looper.getMainLooper())

    // Settings (gamepad/README.md): rumble on the controllers, and player 1's on the phone.
    const val RUMBLE = "rumble"
    const val PHONE_RUMBLE = "phoneRumble"
    val PHONE_RUMBLE_MODES = listOf("auto", "phone", "controller")

    fun register(ctx: Context) {
        appContext = ctx.applicationContext
        ctx.getSystemService(InputManager::class.java).registerInputDeviceListener(this, null)
        InputDevice.getDeviceIds().forEach { onInputDeviceAdded(it) }
    }

    /** Sends every player again, for a link that just came up. */
    fun resend() = pads.forEach { send(it) }

    fun isGamepad(source: Int): Boolean =
        (source and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
            (source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK

    /** The player for an Android device: its slot if it has one, else the slot it had before, else a free one. */
    private fun padFor(deviceId: Int): Pad? {
        pads.firstOrNull { it.deviceId == deviceId }?.let { return it }
        val device = InputDevice.getDevice(deviceId) ?: return null
        if (!isGamepad(device.sources) || device.isVirtual) return null
        val pad = pads.firstOrNull { !it.connected && it.descriptor == device.descriptor }
            ?: pads.firstOrNull { !it.connected && it.descriptor.isEmpty() }
            ?: pads.firstOrNull { !it.connected }
            ?: return null
        pad.deviceId = deviceId
        pad.descriptor = device.descriptor
        pad.connected = true
        pad.clear()
        Log.i(TAG, "controller ${device.name} is player ${pad.slot + 1}")
        return pad
    }

    fun onKey(event: KeyEvent): Boolean {
        if (!isGamepad(event.source)) return false
        val pad = padFor(event.deviceId) ?: return false
        val down = event.action == KeyEvent.ACTION_DOWN
        val bit = keyMap[event.keyCode]
        when {
            bit != null -> pad.keyButtons = if (down) pad.keyButtons or bit else pad.keyButtons and bit.inv()
            event.keyCode == KeyEvent.KEYCODE_BUTTON_L2 -> pad.keyLt = if (down) TRIGGER_MAX else 0
            event.keyCode == KeyEvent.KEYCODE_BUTTON_R2 -> pad.keyRt = if (down) TRIGGER_MAX else 0
            else -> return false
        }
        send(pad)
        return true
    }

    fun onMotion(event: MotionEvent): Boolean {
        if (!isGamepad(event.source)) return false
        val pad = padFor(event.deviceId) ?: return false
        pad.lx = stick(event.getAxisValue(MotionEvent.AXIS_X))
        pad.ly = stick(-event.getAxisValue(MotionEvent.AXIS_Y))
        pad.rx = stick(event.getAxisValue(MotionEvent.AXIS_Z))
        pad.ry = stick(-event.getAxisValue(MotionEvent.AXIS_RZ))
        pad.axisLt = trigger(maxOf(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE)))
        pad.axisRt = trigger(maxOf(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS)))
        var hat = 0
        val hx = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hy = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        if (hx < -0.5f) hat = hat or XINPUT_DPAD_LEFT else if (hx > 0.5f) hat = hat or XINPUT_DPAD_RIGHT
        if (hy < -0.5f) hat = hat or XINPUT_DPAD_UP else if (hy > 0.5f) hat = hat or XINPUT_DPAD_DOWN
        pad.hatButtons = hat
        send(pad)
        return true
    }

    override fun onInputDeviceAdded(deviceId: Int) {
        padFor(deviceId)?.let { send(it) }
    }

    override fun onInputDeviceChanged(deviceId: Int) {}

    override fun onInputDeviceRemoved(deviceId: Int) {
        val pad = pads.firstOrNull { it.deviceId == deviceId } ?: return
        stopRumble(pad.slot)
        pad.deviceId = -1
        pad.connected = false
        pad.clear()
        send(pad)                 // the descriptor stays, so the slot waits for this controller
        Log.i(TAG, "player ${pad.slot + 1} disconnected")
    }

    private fun send(pad: Pad) {
        VsockInput.pad(pad.slot, pad.connected, pad.keyButtons or pad.hatButtons, maxOf(pad.axisLt, pad.keyLt),
            maxOf(pad.axisRt, pad.keyRt), pad.lx, pad.ly, pad.rx, pad.ry)
    }

    /** Debug builds: a made-up controller in a slot (GuestAgent's PAD broadcast). */
    fun fake(slot: Int, connected: Boolean, buttons: Int, lx: Int) {
        val pad = pads.getOrNull(slot) ?: return
        pad.connected = connected
        pad.keyButtons = buttons
        pad.lx = lx
        send(pad)
    }

    private fun stick(v: Float): Int = (v.coerceIn(-1f, 1f) * STICK_MAX).toInt()
    private fun trigger(v: Float): Int = (v.coerceIn(0f, 1f) * TRIGGER_MAX).toInt()

    // ---- rumble ----

    private val phoneLevel = IntArray(1)      // what the phone plays for player 1 now, 0..255
    private var phoneSince = 0L

    /** Message 6 from Windows: the two XInput motors of a player, 0..255 each. */
    fun rumble(slot: Int, left: Int, right: Int) {
        main.post { applyRumble(slot, left, right) }
    }

    private fun applyRumble(slot: Int, left: Int, right: Int) {
        val ctx = appContext ?: return
        val prefs = ctx.getSharedPreferences("hyperwin", Context.MODE_PRIVATE)
        if (!prefs.getBoolean(RUMBLE, true)) return
        val pad = pads.getOrNull(slot) ?: return
        val device = InputDevice.getDevice(pad.deviceId)
        val motors = device?.vibratorManager
        val ids = motors?.vibratorIds ?: IntArray(0)
        val mode = prefs.getString(PHONE_RUMBLE, "auto")
        if (ids.isNotEmpty()) controllerRumble(motors!!, ids, left, right)
        if (slot == 0 && (mode == "phone" || (mode == "auto" && ids.isEmpty()))) phoneRumble(ctx, left, right)
    }

    private fun effect(strength: Int): VibrationEffect? =
        if (strength <= 0) null else VibrationEffect.createOneShot(1000, strength.coerceIn(1, 255))

    /** XInput rumble lasts until it changes; Android's needs a length, so a 1 s shot restarts on each change. */
    private fun controllerRumble(motors: VibratorManager, ids: IntArray, left: Int, right: Int) {
        if (left == 0 && right == 0) { motors.cancel(); return }
        val combined = if (ids.size >= 2) {
            CombinedVibration.startParallel().apply {
                effect(left)?.let { addVibrator(ids[0], it) }
                effect(right)?.let { addVibrator(ids[1], it) }
            }.combine()
        } else {
            CombinedVibration.createParallel(effect(maxOf(left, right))!!)
        }
        motors.vibrate(combined)
    }

    /** The phone's own motor for player 1: the heavy motor counts fully, the light one less. */
    private fun phoneRumble(ctx: Context, left: Int, right: Int) {
        val vibrator: Vibrator = ctx.getSystemService(VibratorManager::class.java).defaultVibrator
        val level = maxOf(left, right * 6 / 10)
        if (level == phoneLevel[0]) return
        phoneLevel[0] = level
        phoneSince = System.currentTimeMillis()
        if (level == 0) { vibrator.cancel(); return }
        val amp = if (vibrator.hasAmplitudeControl()) level else 255
        // On for 1 s at a time, repeated, until the next change, a stop, or 10 s unchanged.
        val wave = VibrationEffect.createWaveform(longArrayOf(1000), intArrayOf(amp), 0)
        vibrator.vibrate(wave, VibrationAttributes.createForUsage(VibrationAttributes.USAGE_MEDIA))
        val started = phoneSince
        main.postDelayed({ if (phoneSince == started && phoneLevel[0] != 0) stopPhone() }, 10_000)
    }

    private fun stopPhone() {
        val ctx = appContext ?: return
        phoneLevel[0] = 0
        ctx.getSystemService(VibratorManager::class.java).defaultVibrator.cancel()
    }

    private fun stopRumble(slot: Int) {
        InputDevice.getDevice(pads[slot].deviceId)?.vibratorManager?.cancel()
        if (slot == 0) stopPhone()
    }

    /** When Windows goes out of sight, the app goes to the background, or the VM stops. */
    fun stopAllRumble() = main.post { for (i in 0 until SLOTS) stopRumble(i) }
}
