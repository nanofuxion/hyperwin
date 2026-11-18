package dev.hyperwin.app

import android.os.ParcelFileDescriptor
import android.util.Log
import android.view.MotionEvent
import java.io.FileOutputStream
import java.io.IOException

/**
 * Fingers for Windows' own touch screen and Precision Touchpad (guest/drivers/hwhid/hwhid.c, over
 * vsock HW_VSOCK_PORT_HID). Windows does every gesture itself: taps, press and hold, tap and drag,
 * scrolling, pinch, three and four finger swipes. Each message is one length byte, then:
 *   1, count, then count x (id, flags: 1 touching, x u16, y u16)            touch screen, 0..32767
 *   2, count, buttons, then count x (id, flags: 1 touching, 2 finger, x, y)  touchpad, 0..4095
 * A finger that lifts is sent once more with touching clear.
 */
object HidInput {
    private const val TAG = "HyperWin"
    private const val PORT = 5652L
    private const val SCREEN_MAX = 32767
    private const val PAD_MAX = 4095

    private val lock = Any()
    @Volatile private var vm: Any? = null
    private var pfd: ParcelFileDescriptor? = null
    private var out: FileOutputStream? = null

    /** True once hwhid.sys in Windows has taken the connection. */
    @Volatile var connected = false
        private set

    fun start(machine: Any) {
        synchronized(lock) {
            vm = machine
            closeLocked()
        }
        Thread({ connectLoop(machine) }, "hw-hid").apply { isDaemon = true; start() }
    }

    fun stop() {
        synchronized(lock) {
            vm = null
            closeLocked()
        }
    }

    private fun connectLoop(machine: Any) {
        while (vm === machine) {
            if (!connected) {
                val socket = runCatching {
                    machine.javaClass.getMethod("connectVsock", Long::class.javaPrimitiveType)
                        .invoke(machine, PORT) as ParcelFileDescriptor
                }.getOrNull()
                if (socket != null) synchronized(lock) {
                    pfd = socket
                    out = FileOutputStream(socket.fileDescriptor)
                    connected = true
                    Log.i(TAG, "touch driver connected")
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

    private fun send(msg: ByteArray): Boolean = synchronized(lock) {
        val o = out ?: return false
        try {
            o.write(msg)
            true
        } catch (e: IOException) {
            Log.i(TAG, "touch driver dropped: ${e.message}")
            closeLocked()
            false
        }
    }

    /** The pointer that this event lifts, or -1; every pointer on ACTION_UP and ACTION_CANCEL. */
    private fun lifting(event: MotionEvent): Int = when (event.actionMasked) {
        MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> Int.MAX_VALUE
        MotionEvent.ACTION_POINTER_UP -> event.actionIndex
        else -> -1
    }

    /** Fingers on the guest view as Windows' touch screen, over the letterboxed Windows picture. */
    fun screen(event: MotionEvent, place: (Float, Float) -> Pair<Int, Int>?): Boolean {
        val lift = lifting(event)
        return frame(1, null, event, 10) { i ->
            val (x, y) = place(event.getX(i), event.getY(i)) ?: return@frame null
            Triple(if (lift == Int.MAX_VALUE || lift == i) 0 else 1, x / 2, y / 2)
        }
    }

    /** Fingers on the guest view as a touchpad the size of the view. */
    fun pad(event: MotionEvent, viewWidth: Int, viewHeight: Int): Boolean {
        if (viewWidth <= 0 || viewHeight <= 0) return false
        val lift = lifting(event)
        return frame(2, 0, event, 5) { i ->
            val x = (event.getX(i) / viewWidth * PAD_MAX).toInt().coerceIn(0, PAD_MAX)
            val y = (event.getY(i) / viewHeight * PAD_MAX).toInt().coerceIn(0, PAD_MAX)
            Triple(if (lift == Int.MAX_VALUE || lift == i) 2 else 3, x, y)
        }
    }

    private fun frame(type: Int, buttons: Int?, event: MotionEvent, most: Int, contact: (Int) -> Triple<Int, Int, Int>?): Boolean {
        if (!connected) return false
        val head = if (buttons == null) 3 else 4
        val msg = ByteArray(head + 6 * most)
        var n = 0
        for (i in 0 until event.pointerCount) {
            if (n == most) break
            val (flags, x, y) = contact(i) ?: continue
            val o = head + 6 * n
            msg[o] = event.getPointerId(i).toByte()
            msg[o + 1] = flags.toByte()
            msg[o + 2] = x.toByte(); msg[o + 3] = (x shr 8).toByte()
            msg[o + 4] = y.toByte(); msg[o + 5] = (y shr 8).toByte()
            n++
        }
        val len = head - 1 + 6 * n
        msg[0] = len.toByte()
        msg[1] = type.toByte()
        msg[2] = n.toByte()
        if (buttons != null) msg[3] = buttons.toByte()
        return send(msg.copyOf(len + 1))
    }
}
