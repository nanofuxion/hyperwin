package dev.hyperwin.app

import android.app.Service
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.os.Process
import android.os.SharedMemory
import android.view.Surface
import android.view.SurfaceControl
import android.util.Log
import java.io.File
import java.util.zip.ZipInputStream

/**
 * Host side of the GPU rings. The gfxstream renderer, and the Vulkan driver it loads, run in a
 * process of their own ([GpuService], ":gpu"), which maps the same shared window. gfxstream loads
 * its driver once per process, so changing the driver means a new renderer process. Windows keeps
 * running across that: programs using the GPU at that moment lose it, and ones started after get
 * the new driver. A renderer crash does not take Windows down either; the system restarts the
 * service and [connection] hands it the window again.
 */
object HwGpu {
    private const val TAG = "HwGpu"
    private const val STACK = "gpuStack"   // preference: one "1|id" or "0|id" line per entry, top first
    private const val OLD_DRIVER = "gpuDriver"
    const val PHONE = "phone"

    /**
     * What the app carries. Fixes are wrappers (gpu/host/icd) that sit on top of a driver and can
     * be stacked; a driver is what the GPU work finally reaches, the phone's own or one added from
     * a file, and exactly one is on.
     */
    val BUILT_IN = linkedMapOf(
        "vortek" to "Vortek",
        "bcn" to "BC textures",
        PHONE to "Phone default",
    )
    private val FIX_LIBS = mapOf("vortek" to "libvulkan_vortek.so", "bcn" to "libvulkan_bcn.so")

    data class Entry(val id: String, val on: Boolean) {
        val fix get() = id in FIX_LIBS
        val builtIn get() = id in BUILT_IN
        val label get() = BUILT_IN[id] ?: id.removeSuffix(".so")
    }

    private fun prefs(ctx: Context) = ctx.getSharedPreferences("hyperwin", Context.MODE_PRIVATE)

    /** The stack, top first: saved order, then anything new (an added file, a new built-in). */
    fun stack(ctx: Context): List<Entry> {
        val p = prefs(ctx)
        val saved = p.getString(STACK, null)?.lines()?.filter { it.length > 2 }?.map { Entry(it.substring(2), it[0] == '1') }
            ?: when (val old = p.getString(OLD_DRIVER, null)) {  // the single-choice setting this replaced
                null -> listOf(Entry("vortek", true))
                "vortek", "bcn" -> listOf(Entry(old, true))
                "" -> listOf(Entry(PHONE, true))
                else -> listOf(Entry(old, true))
            }
        val files = drivers(ctx).map { it.name }.toSet()
        val list = saved.filter { it.builtIn || it.id in files }.distinctBy { it.id }.toMutableList()
        for (id in BUILT_IN.keys + files) if (list.none { it.id == id }) list += Entry(id, false)
        // Exactly one driver on: the first one switched on, else the phone's.
        val driver = list.firstOrNull { !it.fix && it.on }?.id ?: PHONE
        // Fixes always sit above the drivers, as they run.
        return list.filter { it.fix } + list.filter { !it.fix }.map { it.copy(on = it.id == driver) }
    }

    fun saveStack(ctx: Context, list: List<Entry>) {
        prefs(ctx).edit().putString(STACK, list.joinToString("\n") { (if (it.on) "1|" else "0|") + it.id }).apply()
    }

    /** Switches [id] on as the driver; the other drivers go off. */
    fun useDriver(ctx: Context, id: String) {
        saveStack(ctx, stack(ctx).map { if (it.fix) it else it.copy(on = it.id == id) })
    }

    /** Drops a driver added from a file. The phone's driver takes over if it was the one on. */
    fun deleteDriver(ctx: Context, id: String) {
        File(driverDir(ctx), id).delete()
        saveStack(ctx, stack(ctx))
    }

    private var ctx: Context? = null
    private var window: AvfPlan.SharedWindow? = null
    private var width = 0
    private var height = 0
    private var bound = false
    @Volatile var running = false
        private set

    // The compositor layer GPU frames go on (SGPU_CMD_SCANOUT), above the CPU-drawn desktop. This
    // process owns it and lends it to each renderer, so it outlives a renderer that is replaced.
    private var layer: SurfaceControl? = null
    private var d3dLayer: SurfaceControl? = null
    private var layerWidth = 0
    private var layerHeight = 0
    private var service: Messenger? = null

    const val VSYNC = "scanout_vsync"

    /** Whether GPU frames wait for the phone's screen ("Limit to screen refresh"). On by default. */
    fun vsync(ctx: Context) = prefs(ctx).getBoolean(VSYNC, true)

    fun setVsync(ctx: Context, on: Boolean) {
        prefs(ctx).edit().putBoolean(VSYNC, on).apply()
        val msg = Message.obtain(null, GpuService.SET_VSYNC).apply { data = Bundle().apply { putBoolean("vsync", on) } }
        service?.let { m -> runCatching { m.send(msg) } }
    }

    private fun layerBundle() = Bundle().apply {
        putParcelable("layer", layer)
        putParcelable("d3dLayer", d3dLayer)
        putInt("layerWidth", layerWidth)
        putInt("layerHeight", layerHeight)
    }

    /** The layers scanout and HwD3D frames go on and the size they cover, or nulls when the view
     *  has none. */
    fun setLayer(sc: SurfaceControl?, d3d: SurfaceControl?, width: Int, height: Int) {
        val sameLayer = sc != null && sc === layer
        layer = sc
        d3dLayer = d3d
        layerWidth = width
        layerHeight = height
        val msg = Message.obtain(null, if (sameLayer) GpuService.LAYER_SIZE else GpuService.LAYER).apply { data = layerBundle() }
        service?.let { m -> runCatching { m.send(msg) } }
    }

    /** Takes the GPU's frame off the screen, for when nobody will replace it (renderer or VM gone). */
    fun hideLayer() {
        layer?.let { sc -> runCatching { SurfaceControl.Transaction().setVisibility(sc, false).apply() } }
        d3dLayer?.let { sc -> runCatching { SurfaceControl.Transaction().setVisibility(sc, false).apply() } }
    }

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, binder: IBinder) {
            val w = window ?: return
            val c = ctx ?: return
            val msg = Message.obtain(null, GpuService.START).apply {
                data = layerBundle().apply {
                    putParcelable("window", w.memory)
                    putParcelable("heap", w.heap)
                    putInt("width", width)
                    putInt("height", height)
                    putBundle("env", env(c))
                    putBoolean("vsync", vsync(c))
                }
            }
            service = Messenger(binder)
            runCatching { service?.send(msg) }.onFailure { Log.e(TAG, "renderer did not take the window", it) }
            running = true
            Log.i(TAG, "renderer process connected: ${describe(c)}")
        }

        override fun onServiceDisconnected(name: ComponentName) {
            // Died, most likely in the driver. The system would start it again only after a growing
            // delay, so bind afresh at once; Windows programs waiting on the GPU then get errors
            // instead of hanging.
            running = false
            service = null
            // Its last frame would stay up over the desktop; the next renderer shows the layer again
            // with its first frame.
            hideLayer()
            Log.w(TAG, "renderer process ended, starting another")
            Handler(Looper.getMainLooper()).postDelayed({ restart() }, 500)
        }

        override fun onBindingDied(name: ComponentName) = restart()
    }

    fun start(context: Context, window: AvfPlan.SharedWindow, width: Int, height: Int) {
        ctx = context.applicationContext
        this.window = window
        this.width = width
        this.height = height
        bind()
    }

    fun stop() {
        hideLayer()
        service = null
        window = null
        unbind()
        running = false
    }

    /** A new renderer with the current driver settings, under the running Windows. */
    fun restart() {
        if (window == null) return
        unbind()
        bind()
    }

    private fun bind() {
        val c = ctx ?: return
        if (bound) return
        bound = c.bindService(Intent(c, GpuService::class.java), connection, Context.BIND_AUTO_CREATE or Context.BIND_IMPORTANT)
        if (!bound) Log.e(TAG, "could not start the renderer process")
    }

    private fun unbind() {
        if (!bound) return
        bound = false
        runCatching { ctx?.unbindService(connection) }
    }

    fun driverDir(ctx: Context) = File(ctx.filesDir, "drivers").apply { mkdirs() }

    /** Vulkan drivers added from files, such as Turnip builds for Adreno GPUs. */
    fun drivers(ctx: Context) = driverDir(ctx).listFiles { f -> f.name.endsWith(".so") }.orEmpty().sortedBy { it.name }

    /**
     * Copies a driver in: a .so, or a .zip holding one (the AdrenoTools layout, where meta.json's
     * libraryName names it). Returns the file name, or null when there was no driver in it.
     */
    fun importDriver(ctx: Context, uri: Uri): String? {
        val name = ctx.contentResolver.query(uri, null, null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(c.getColumnIndexOrThrow(android.provider.OpenableColumns.DISPLAY_NAME)) else null
        } ?: uri.lastPathSegment ?: "driver"
        val dir = driverDir(ctx)
        if (name.endsWith(".so")) {
            val out = File(dir, File(name).name)
            ctx.contentResolver.openInputStream(uri)!!.use { i -> out.outputStream().use { i.copyTo(it) } }
            return out.name
        }
        // A zip: read meta.json first if present, else take the first .so.
        var wanted: String? = null
        ZipInputStream(ctx.contentResolver.openInputStream(uri)!!).use { z ->
            generateSequence { z.nextEntry }.firstOrNull { it.name.endsWith("meta.json") }?.let {
                wanted = Regex("\"libraryName\"\\s*:\\s*\"([^\"]+)\"").find(z.readBytes().decodeToString())?.groupValues?.get(1)
            }
        }
        ZipInputStream(ctx.contentResolver.openInputStream(uri)!!).use { z ->
            val entry = generateSequence { z.nextEntry }.firstOrNull { e ->
                !e.isDirectory && e.name.endsWith(".so") && (wanted == null || File(e.name).name == wanted)
            } ?: return null
            val base = name.removeSuffix(".zip").replace(Regex("[^A-Za-z0-9._-]"), "_")
            val out = File(dir, "$base.so")
            out.outputStream().use { z.copyTo(it) }
            return out.name
        }
    }

    /** "Vortek over Phone default": the fixes that are on, top first, then the driver. */
    fun describe(ctx: Context): String {
        val on = stack(ctx).filter { it.on }
        val fixes = on.filter { it.fix }.joinToString(" + ") { it.label }
        val driver = on.first { !it.fix }.label
        return if (fixes.isEmpty()) driver else "$fixes over $driver"
    }

    /** Where the driver wrapper keeps compiled shaders, one file a driver, and stats.txt. */
    fun shaderCacheDir(ctx: Context) = File(ctx.cacheDir, "shaders")

    /** "312 MB, 4,120 pipelines made, 97% from the cache", or null when there is no cache yet. */
    fun shaderCacheSummary(ctx: Context): String? {
        val files = shaderCacheDir(ctx).listFiles { f -> f.name.endsWith(".bin") }?.takeIf { it.isNotEmpty() } ?: return null
        val mb = files.sumOf { it.length() } / (1 shl 20)
        val stats = runCatching { File(shaderCacheDir(ctx), "stats.txt").readText().trim().split(" ").map { it.toLong() } }.getOrNull()
        val size = if (mb > 0) "$mb MB" else "under 1 MB"
        if (stats == null || stats.size < 2 || stats[0] == 0L) return size
        return "$size. Last run: %,d pipelines, %d%% from the cache".format(stats[0], stats[1] * 100 / stats[0])
    }

    /** Deletes the cache. Only while the renderer is stopped: it has the files open. */
    fun clearShaderCache(ctx: Context) = shaderCacheDir(ctx).deleteRecursively()

    /** What the renderer process sets before gfxstream loads its driver. */
    private fun env(ctx: Context): Bundle {
        val on = stack(ctx).filter { it.on }
        val fixes = on.filter { it.fix }.map { FIX_LIBS.getValue(it.id) }
        val driver = on.first { !it.fix }.id.let { if (it == PHONE) null else File(driverDir(ctx), it) }
        return Bundle().apply {
            // gfxstream opens the top wrapper as its Vulkan loader. Each wrapper opens the next one
            // in HWGPU_VK_CHAIN, and the last opens HWGPU_VK_DRIVER, or the phone's libvulkan.so.
            // With no fixes the plain wrapper still goes in, for the shader cache it keeps.
            if (fixes.isNotEmpty()) {
                putString("ANDROID_EMU_VK_LOADER_PATH", fixes.first())
                putString("HWGPU_VK_CHAIN", fixes.joinToString(":"))
            } else {
                putString("ANDROID_EMU_VK_LOADER_PATH", "libvulkan_passthrough.so")
            }
            // Compiled shaders, kept between runs by the bottom wrapper (shader-cache/README.md).
            putString("HWGPU_VK_CACHE_DIR", shaderCacheDir(ctx).absolutePath)
            if (driver != null) putString("HWGPU_VK_DRIVER", driver.absolutePath)
            // adb shell setprop log.tag.hwgpu DEBUG: the server logs every guest request.
            if (Log.isLoggable("hwgpu", Log.DEBUG)) putString("HWGPU_TRACE", "1")
        }
    }
}

/** The renderer process. It lives while the app is bound to it, and exits when unbound. */
class GpuService : Service() {
    companion object {
        const val START = 1
        const val LAYER = 2       // a new scanout layer, or none
        const val LAYER_SIZE = 3  // the same layer, another size
        const val SET_VSYNC = 4
        private const val TAG = "HwGpu"
        /** STREAM_RENDERER_FLAGS_* (gfxstream virtio-gpu-gfxstream-renderer.h), as ring_test uses. */
        private const val USE_SURFACELESS = 1 shl 3
        private const val USE_VK = 1 shl 5
    }

    private var memory: SharedMemory? = null
    // Held for the life of the process: the mapping goes away when its buffer is collected.
    private var mapped: java.nio.ByteBuffer? = null
    private var heapMemory: SharedMemory? = null
    private var mappedHeap: java.nio.ByteBuffer? = null
    private var started = false

    private val messenger = Messenger(Handler(Looper.getMainLooper()) { msg ->
        when (msg.what) {
            START -> if (!started) start(msg.data)
            LAYER -> if (started) setLayer(msg.data)
            LAYER_SIZE -> if (started) nativeSetLayerSize(msg.data.getInt("layerWidth"), msg.data.getInt("layerHeight"))
            SET_VSYNC -> if (started) nativeSetVsync(msg.data.getBoolean("vsync", true))
        }
        true
    })

    private fun start(data: Bundle) {
        val shm = data.getParcelable("window", SharedMemory::class.java) ?: return
        data.getBundle("env")?.let { env -> env.keySet().forEach { android.system.Os.setenv(it, env.getString(it), true) } }
        val loaded = runCatching {
            System.loadLibrary("gfxstream_backend")
            System.loadLibrary("hwgpu")
        }.onFailure { Log.e(TAG, "GPU libraries missing from this build", it) }.isSuccess
        if (!loaded) return
        memory = shm
        val map = shm.mapReadWrite().also { mapped = it }
        val heap = data.getParcelable("heap", SharedMemory::class.java)
        heapMemory = heap
        val heapMap = heap?.let { runCatching { it.mapReadWrite() }.onFailure { e -> Log.e(TAG, "GPU heap not mapped", e) }.getOrNull() }
        mappedHeap = heapMap
        val rc = nativeStart(map, heapMap, USE_VK or USE_SURFACELESS, data.getInt("width"), data.getInt("height"))
        if (rc != 0) {
            Log.e(TAG, "nativeStart failed ($rc)")
            return
        }
        started = true
        nativeSetVsync(data.getBoolean("vsync", true))
        setLayer(data)
        Log.i(TAG, "renderer started ${data.getInt("width")}x${data.getInt("height")}, " +
            "loader ${System.getenv("ANDROID_EMU_VK_LOADER_PATH") ?: "phone"}, driver ${System.getenv("HWGPU_VK_DRIVER") ?: "phone"}")
    }

    // HwD3D's layer and the Surface its swap chains draw into, made here so the buffers come from this process.
    private var d3dSurface: Surface? = null

    private fun setLayer(data: Bundle) {
        val sc = data.getParcelable("layer", SurfaceControl::class.java)
        nativeSetLayer(sc, data.getInt("layerWidth"), data.getInt("layerHeight"))
        val d3d = data.getParcelable("d3dLayer", SurfaceControl::class.java)
        val surface = d3d?.let { runCatching { Surface(it) }.onFailure { e -> Log.e(TAG, "no HwD3D surface", e) }.getOrNull() }
        nativeSetD3dLayer(d3d, surface)
        d3dSurface?.release()
        d3dSurface = surface
    }

    override fun onBind(intent: Intent): IBinder = messenger.binder

    override fun onDestroy() {
        if (started) nativeStop()
        Log.i(TAG, "renderer stopped")
        // The driver cannot be unloaded and gfxstream cannot start twice in one process.
        Process.killProcess(Process.myPid())
    }

    private external fun nativeStart(window: java.nio.ByteBuffer, heap: java.nio.ByteBuffer?, flags: Int, width: Int, height: Int): Int
    private external fun nativeStop()
    private external fun nativeSetLayer(layer: SurfaceControl?, width: Int, height: Int)
    private external fun nativeSetLayerSize(width: Int, height: Int)
    private external fun nativeSetVsync(on: Boolean)
    private external fun nativeSetD3dLayer(layer: SurfaceControl?, surface: Surface?)
}
