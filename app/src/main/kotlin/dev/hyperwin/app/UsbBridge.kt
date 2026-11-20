package dev.hyperwin.app

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.util.Log
import java.nio.ByteBuffer

/**
 * USB passthrough, phone side (usb/README.md): finds devices, asks permission, opens and claims them, and
 * gives the usbfs file descriptor to native code, which serves Windows' requests through the shared USB region.
 * Off until switched on: `am broadcast -a dev.hyperwin.app.USB_PASS --ez enable true` (Settings comes with phase 3).
 * Devices the phone's own input paths already carry (keyboards, mice, gamepads) and hubs stay with Android.
 */
object UsbBridge {
    private const val TAG = "HyperWinUsb"
    private const val ACTION_PERMISSION = "dev.hyperwin.app.USB_PERMISSION"
    private const val ACTION_SWITCH = "dev.hyperwin.app.USB_PASS"
    private const val ACTION_DUMP = "dev.hyperwin.app.USB_DUMP"   // debug: log 48 bytes of the region at --ei offset
    init { System.loadLibrary("hyperwin") }

    private class Open(val device: UsbDevice, val connection: UsbDeviceConnection, val slot: Int)

    private var ctx: Context? = null
    private var manager: UsbManager? = null
    private val open = HashMap<String, Open>()
    @Volatile private var enabled = false
    private var receiver: BroadcastReceiver? = null
    private var regionBuffer: ByteBuffer? = null

    fun start(context: Context, region: ByteBuffer) {
        stop()
        ctx = context.applicationContext
        manager = ctx!!.getSystemService(Context.USB_SERVICE) as UsbManager
        nativeStart(region)
        regionBuffer = region
        enabled = ctx!!.getSharedPreferences("hyperwin", Context.MODE_PRIVATE).getBoolean("usb_pass", false)
        val r = object : BroadcastReceiver() {
            override fun onReceive(c: Context, i: Intent) {
                val dev: UsbDevice? = i.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                when (i.action) {
                    UsbManager.ACTION_USB_DEVICE_ATTACHED -> if (dev != null) offer(dev)
                    UsbManager.ACTION_USB_DEVICE_DETACHED -> if (dev != null) release(dev)
                    ACTION_PERMISSION -> if (dev != null && i.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) attach(dev)
                    ACTION_DUMP -> regionBuffer?.let { b ->
                        val n = i.getIntExtra("offset", 0)
                        Log.i(TAG, "region[$n..]: " + (0 until 48).joinToString("") { "%02x".format(b.get(n + it)) })
                    }
                    ACTION_SWITCH -> {
                        enabled = i.getBooleanExtra("enable", false)
                        c.getSharedPreferences("hyperwin", Context.MODE_PRIVATE).edit().putBoolean("usb_pass", enabled).apply()
                        Log.i(TAG, "passthrough ${if (enabled) "on" else "off"}")
                        if (enabled) manager?.deviceList?.values?.forEach { offer(it) } else open.values.toList().forEach { release(it.device) }
                    }
                }
            }
        }
        val f = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED); addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
            addAction(ACTION_PERMISSION); addAction(ACTION_SWITCH); addAction(ACTION_DUMP)
        }
        ctx!!.registerReceiver(r, f, Context.RECEIVER_EXPORTED)
        receiver = r
        if (enabled) manager!!.deviceList.values.forEach { offer(it) }
    }

    fun stop() {
        receiver?.let { runCatching { ctx?.unregisterReceiver(it) } }
        receiver = null
        open.values.toList().forEach { release(it.device) }
    }

    /** Input devices and hubs stay with Android. */
    private fun passes(d: UsbDevice): Boolean {
        if (d.deviceClass == UsbConstants.USB_CLASS_HUB) return false
        for (n in 0 until d.interfaceCount) if (d.getInterface(n).interfaceClass == UsbConstants.USB_CLASS_HID) return false
        return true
    }

    private fun offer(d: UsbDevice) {
        if (!enabled || !passes(d) || open.containsKey(d.deviceName)) return
        val m = manager ?: return
        if (m.hasPermission(d)) attach(d)
        else m.requestPermission(d, PendingIntent.getBroadcast(ctx, 0, Intent(ACTION_PERMISSION).setPackage(ctx!!.packageName), PendingIntent.FLAG_MUTABLE))
    }

    private fun attach(d: UsbDevice) {
        val m = manager ?: return
        if (open.containsKey(d.deviceName)) return
        val c = m.openDevice(d) ?: return Unit.also { Log.w(TAG, "cannot open ${d.deviceName}") }
        for (n in 0 until d.interfaceCount) c.claimInterface(d.getInterface(n), true)   // takes it from Android's drivers
        val raw = c.rawDescriptors
        val slot = nativeAttach(c.fileDescriptor, d.deviceId, d.vendorId, d.productId, 3, raw)   // native reads the real speed
        if (slot < 0) { Log.w(TAG, "no slot for ${d.deviceName}: $slot"); c.close(); return }
        open[d.deviceName] = Open(d, c, slot)
        Log.i(TAG, "${d.deviceName} ${"%04x:%04x".format(d.vendorId, d.productId)} in slot $slot")
    }

    private fun release(d: UsbDevice) {
        val o = open.remove(d.deviceName) ?: return
        nativeDetach(o.slot)
        for (n in 0 until d.interfaceCount) runCatching { o.connection.releaseInterface(d.getInterface(n)) }
        o.connection.close()
        Log.i(TAG, "${d.deviceName} released")
    }

    @JvmStatic private external fun nativeStart(region: ByteBuffer)
    @JvmStatic private external fun nativeAttach(fd: Int, busId: Int, vid: Int, pid: Int, speed: Int, desc: ByteArray): Int
    @JvmStatic private external fun nativeDetach(slot: Int)
    @JvmStatic external fun nativeSlotState(slot: Int): Int
}
