package dev.hyperwin.app

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
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

    internal const val XINPUT_DPAD_UP = 0x0001
    internal const val XINPUT_DPAD_DOWN = 0x0002
    internal const val XINPUT_DPAD_LEFT = 0x0004
    internal const val XINPUT_DPAD_RIGHT = 0x0008
    internal const val XINPUT_START = 0x0010
    internal const val XINPUT_BACK = 0x0020
    internal const val XINPUT_LEFT_THUMB = 0x0040
    internal const val XINPUT_RIGHT_THUMB = 0x0080
    internal const val XINPUT_LEFT_SHOULDER = 0x0100
    internal const val XINPUT_RIGHT_SHOULDER = 0x0200
    internal const val XINPUT_GUIDE = 0x0400
    internal const val XINPUT_A = 0x1000
    internal const val XINPUT_B = 0x2000
    internal const val XINPUT_X = 0x4000
    internal const val XINPUT_Y = 0x8000

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
    private var touchActive = false
    private var touchButtons = 0
    private var touchLt = 0
    private var touchRt = 0
    private var touchLx = 0
    private var touchLy = 0
    private var touchRx = 0
    private var touchRy = 0
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

    // ---- motion: the phone's gyroscope and accelerometer as player 1's DualShock 4 twin's ----

    /** Player 1 as "xbox" (an Xbox 360 controller, for the most games) or "ds4" (a DualShock 4, with gyro). */
    const val PLAYER1 = "player1Pad"

    private fun ds4(ctx: Context) = ctx.getSharedPreferences("hyperwin", Context.MODE_PRIVATE).getString(PLAYER1, "xbox") == "ds4"
    private var sensing = false
    private var paused = false
    private val accel = FloatArray(3)
    @Volatile private var rotation = android.view.Surface.ROTATION_90
    /** Sensor events arrive here, off the main thread: 250 a second. */
    private val sensorThread by lazy { android.os.HandlerThread("hw-motion").apply { start() } }

    private val motionListener = object : SensorEventListener {
        override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}
        override fun onSensorChanged(e: SensorEvent) {
            if (e.sensor.type == Sensor.TYPE_ACCELEROMETER) { System.arraycopy(e.values, 0, accel, 0, 3); return }
            // The phone held sideways like a controller, screen to the player: the DualShock 4's X is
            // the screen's right, Y the screen's normal, Z the screen's down.
            val turned = rotation == android.view.Surface.ROTATION_270
            val s = if (turned) 1f else -1f
            val g = 180f / Math.PI.toFloat() * 16f
            val a = 8192f / SensorManager.GRAVITY_EARTH
            fun clamp(v: Float) = v.coerceIn(-32767f, 32767f).toInt().toShort()
            VsockInput.motion(0, shortArrayOf(
                clamp(s * e.values[1] * g), clamp(e.values[2] * g), clamp(s * e.values[0] * g),
                clamp(s * accel[1] * a), clamp(accel[2] * a), clamp(s * accel[0] * a),
            ))
        }
    }

    /** Streams the phone's motion while player 1 is connected, the setting is on and the app is in front. */
    private fun updateSensing() {
        val ctx = appContext ?: return
        val want = (pads[0].connected || touchActive) && !paused && ds4(ctx)
        if (want == sensing) return
        val sm = ctx.getSystemService(SensorManager::class.java)
        if (want) {
            val gyro = sm.getDefaultSensor(Sensor.TYPE_GYROSCOPE) ?: return
            runCatching {
                rotation = ctx.getSystemService(android.hardware.display.DisplayManager::class.java)
                    .getDisplay(android.view.Display.DEFAULT_DISPLAY)?.rotation ?: rotation
                val h = Handler(sensorThread.looper)
                sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)?.let { sm.registerListener(motionListener, it, 4000, h) }
                sm.registerListener(motionListener, gyro, 4000, h)
            }.onFailure { Log.w(TAG, "phone motion unavailable", it); return }
        } else {
            sm.unregisterListener(motionListener)
        }
        sensing = want
        Log.i(TAG, "phone motion for player 1 ${if (want) "on" else "off"}")
    }

    fun pause() { paused = true; updateSensing(); stopAllRumble() }
    fun resume() { paused = false; updateSensing() }
    /** After the Player 1 setting changed: Windows swaps the controller, then gets the state again. */
    fun player1Changed() {
        val ctx = appContext ?: return
        VsockInput.padKind(ds4(ctx))
        updateSensing()
        main.postDelayed({ resend() }, 1500)
    }

    /** Sends every player again, for a link that just came up. */
    fun resend() {
        appContext?.let { VsockInput.padKind(ds4(it)) }
        pads.forEach { send(it) }
    }

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
        if (pad.slot == 0) updateSensing()
        val useTouch = pad.slot == 0 && touchActive
        val connected = pad.connected || useTouch
        val buttons = pad.keyButtons or pad.hatButtons or (if (useTouch) touchButtons else 0)
        val lt = maxOf(pad.axisLt, pad.keyLt, if (useTouch) touchLt else 0)
        val rt = maxOf(pad.axisRt, pad.keyRt, if (useTouch) touchRt else 0)
        val lx = if (useTouch && touchLx != 0) touchLx else pad.lx
        val ly = if (useTouch && touchLy != 0) touchLy else pad.ly
        val rx = if (useTouch && touchRx != 0) touchRx else pad.rx
        val ry = if (useTouch && touchRy != 0) touchRy else pad.ry
        VsockInput.pad(pad.slot, connected, buttons, lt, rt, lx, ly, rx, ry)
    }

    /** Virtual touch gamepad (player 1): connects or disconnects slot 0 when no physical pad is attached. */
    fun setTouchActive(active: Boolean) {
        if (touchActive == active) return
        touchActive = active
        if (!active) {
            touchButtons = 0; touchLt = 0; touchRt = 0
            touchLx = 0; touchLy = 0; touchRx = 0; touchRy = 0
        }
        send(pads[0])
    }

    fun setTouchButton(bit: Int, down: Boolean) {
        val next = if (down) touchButtons or bit else touchButtons and bit.inv()
        if (next == touchButtons) return
        touchButtons = next
        if (!touchActive) touchActive = true
        send(pads[0])
    }

    fun setTouchTrigger(right: Boolean, value: Int) {
        val clamped = value.coerceIn(0, TRIGGER_MAX)
        if (right) {
            if (touchRt == clamped) return
            touchRt = clamped
        } else {
            if (touchLt == clamped) return
            touchLt = clamped
        }
        if (!touchActive) touchActive = true
        send(pads[0])
    }

    /** Analog stick for virtual touch pad: x in -1..1 (right positive), y in -1..1 (up positive). */
    fun setTouchStick(right: Boolean, x: Float, y: Float) {
        val sx = stick(x)
        val sy = stick(y)
        if (right) {
            if (touchRx == sx && touchRy == sy) return
            touchRx = sx; touchRy = sy
        } else {
            if (touchLx == sx && touchLy == sy) return
            touchLx = sx; touchLy = sy
        }
        if (!touchActive) touchActive = true
        send(pads[0])
    }

    fun clearTouchState() {
        val had = touchButtons != 0 || touchLt != 0 || touchRt != 0 ||
            touchLx != 0 || touchLy != 0 || touchRx != 0 || touchRy != 0
        touchButtons = 0; touchLt = 0; touchRt = 0
        touchLx = 0; touchLy = 0; touchRx = 0; touchRy = 0
        if (had && touchActive) send(pads[0])
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
