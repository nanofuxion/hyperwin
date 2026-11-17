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

    private fun ensureLoaded(): Boolean {
        if (loaded) return true
        // A build without gpu/work/android (gpu/host/build-android.sh) still runs Windows, without a GPU.
        loaded = runCatching {
            System.loadLibrary("gfxstream_backend")
            System.loadLibrary("hwgpu")
        }.onFailure { Log.e(TAG, "GPU libraries missing from this build", it) }.isSuccess
        return loaded
    }

    /** STREAM_RENDERER_FLAGS_* (gfxstream virtio-gpu-gfxstream-renderer.h), as ring_test uses. */
    private const val USE_SURFACELESS = 1 shl 3
    private const val USE_VK = 1 shl 5

    fun start(window: AvfPlan.SharedWindow, width: Int, height: Int) {
        if (!ensureLoaded()) return
        // adb shell setprop log.tag.hwgpu DEBUG: the server logs every guest request.
        if (Log.isLoggable("hwgpu", Log.DEBUG)) android.system.Os.setenv("HWGPU_TRACE", "1", true)
        else android.system.Os.unsetenv("HWGPU_TRACE")
        val rc = nativeStart(window.buffer, USE_VK or USE_SURFACELESS, width, height)
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
