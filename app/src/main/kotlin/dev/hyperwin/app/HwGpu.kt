package dev.hyperwin.app

import android.util.Log

/**
 * Host side of the GPU rings: loads libhwgpu.so and starts the gfxstream renderer thread that
 * serves every guest channel in the shared window.  The app calls [start] before the VM boots
 * and [stop] when the VM exits.
 */
object HwGpu {
    private const val TAG = "HwGpu"
    private var loaded = false
    @Volatile var running = false
        private set

    private fun ensureLoaded() {
        if (loaded) return
        System.loadLibrary("gfxstream_backend")
        System.loadLibrary("hwgpu")
        loaded = true
    }

    fun start(window: AvfPlan.SharedWindow, width: Int, height: Int) {
        ensureLoaded()
        val flags = 0x1 or 0x4  // USE_VK_BIT | USE_SURFACELESS_BIT
        val rc = nativeStart(window.buffer, flags, width, height)
        if (rc != 0) {
            Log.e(TAG, "nativeStart failed ($rc)")
            return
        }
        running = true
        Log.i(TAG, "renderer started ${width}x${height}")
    }

    fun stop() {
        if (!running) return
        nativeStop()
        running = false
        Log.i(TAG, "renderer stopped")
    }

    private external fun nativeStart(window: java.nio.ByteBuffer, flags: Int, width: Int, height: Int): Int
    private external fun nativeStop()
}
