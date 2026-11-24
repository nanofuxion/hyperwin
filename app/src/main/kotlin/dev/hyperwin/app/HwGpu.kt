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
import java.io.BufferedInputStream
import java.io.File
import java.io.InputStream
import java.util.zip.GZIPInputStream
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
        "malivk" to "MaliVK",
        PHONE to "Phone default",
    )
    private val FIX_LIBS = mapOf(
        "vortek" to "libvulkan_vortek.so",
        "bcn" to "libvulkan_bcn.so",
        "malivk" to "libvulkan_malivk.so",
    )

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

    // ---- Direct3D: which DXVK the phone runs (d3d/README.md, "Choosing the DXVK version") ----

    const val DXVK_SHIPPED = "shipped"
    /** The HwD3D ABI a DXVK zip must have been built for: dxvk-android.patch's hwd3d_abi_version. */
    const val DXVK_ABI = 1
    private const val DXVK_CHOICE = "dxvkChoice"
    const val DXVK_SHIPPED_NAME = "DXVK-Sarek 1.14.1"

    data class Dxvk(val id: String, val name: String, val builtIn: Boolean)

    fun dxvkDir(ctx: Context) = File(ctx.filesDir, "dxvk").apply { mkdirs() }

    private fun hasDxvkLib(dir: File, prefix: String): Boolean =
        dir.listFiles()?.any { it.isFile && it.name.startsWith(prefix) && it.name.endsWith(".so") } == true

    private fun dxvkMeta(dir: File): org.json.JSONObject? =
        runCatching { org.json.JSONObject(File(dir, "meta.json").readText()) }.getOrNull()
            ?.takeIf { it.optInt("abi") == DXVK_ABI && hasDxvkLib(dir, "libdxvk_dxgi") && hasDxvkLib(dir, "libdxvk_d3d11") }

    /** The shipped DXVK, then every one added from a zip, by name. */
    fun dxvkList(ctx: Context): List<Dxvk> = listOf(Dxvk(DXVK_SHIPPED, DXVK_SHIPPED_NAME, true)) +
        dxvkDir(ctx).listFiles { f -> f.isDirectory }.orEmpty().sortedBy { it.name }
            .mapNotNull { d -> dxvkMeta(d)?.let { Dxvk(d.name, it.optString("name", d.name), false) } }

    /** The one in use; the shipped one when the chosen one is gone. */
    fun dxvkChoice(ctx: Context): String =
        prefs(ctx).getString(DXVK_CHOICE, DXVK_SHIPPED)!!.takeIf { id -> dxvkList(ctx).any { it.id == id } } ?: DXVK_SHIPPED

    fun dxvkName(ctx: Context): String = dxvkList(ctx).first { it.id == dxvkChoice(ctx) }.name

    fun useDxvk(ctx: Context, id: String) {
        prefs(ctx).edit().putString(DXVK_CHOICE, id).apply()
    }

    fun deleteDxvk(ctx: Context, id: String) {
        if (id != DXVK_SHIPPED) File(dxvkDir(ctx), id).deleteRecursively()
    }

    /**
     * Unpacks a DXVK archive (.zip, .tar.gz, .tgz, or .tar): meta.json (name, abi) and libdxvk_*.so.
     * Returns its id, or an error to show. A build for another HwD3D ABI is refused: its libraries
     * would call into the renderer wrongly.
     */
    fun importDxvk(ctx: Context, uri: Uri): Pair<String?, String?> {
        val tmp = File(dxvkDir(ctx), ".incoming").apply { deleteRecursively(); mkdirs() }
        try {
            val raw = ctx.contentResolver.openInputStream(uri) ?: return null to "That file could not be opened."
            var libs = 0
            var hasDlls = false
            fun handleEntry(name: String, isDir: Boolean, writeTo: (File) -> Unit) {
                if (isDir) return
                val base = name.substringAfterLast('/')
                if (base.endsWith(".dll", ignoreCase = true)) hasDlls = true
                if (!(base == "meta.json" || (base.startsWith("libdxvk_") && base.endsWith(".so")))) return
                writeTo(File(tmp, base))
                if (base != "meta.json") libs++
            }
            BufferedInputStream(raw).use { buf ->
                buf.mark(4)
                val b0 = buf.read()
                val b1 = buf.read()
                buf.reset()
                if (b0 == 0x50 && b1 == 0x4b) {
                    ZipInputStream(buf).use { zip ->
                        while (true) {
                            val entry = zip.nextEntry ?: break
                            handleEntry(entry.name, entry.isDirectory) { dest ->
                                dest.outputStream().use { zip.copyTo(it) }
                            }
                        }
                    }
                } else {
                    val tarStream: InputStream = if (b0 == 0x1f && b1 == 0x8b) GZIPInputStream(buf) else buf
                    tarStream.use { tar ->
                        val hdr = ByteArray(512)
                        var longName: String? = null
                        while (readFully(tar, hdr)) {
                            if (hdr.all { it == 0.toByte() }) break
                            val type = hdr[156].toInt().toChar()
                            val sizeStr = String(hdr, 124, 12, Charsets.US_ASCII).trim { it <= ' ' || it == '\u0000' }
                            val size = if (sizeStr.isEmpty()) 0L else sizeStr.toLong(8)
                            val rawName = readCString(hdr, 0, 100)
                            val prefix = readCString(hdr, 345, 155)
                            val entryName = longName ?: if (prefix.isNotEmpty()) "$prefix/$rawName" else rawName
                            longName = null
                            if (type == 'L') {
                                val data = ByteArray(size.toInt())
                                readFully(tar, data)
                                longName = String(data, Charsets.UTF_8).trimEnd('\u0000')
                            } else {
                                var consumed = 0L
                                handleEntry(entryName, type == '5' || entryName.endsWith("/")) { dest ->
                                    dest.outputStream().use { out ->
                                        copyExact(tar, out, size)
                                        consumed = size
                                    }
                                }
                                if (consumed < size) skipExact(tar, size - consumed)
                            }
                            val pad = (512L - (size % 512L)) % 512L
                            if (pad > 0) skipExact(tar, pad)
                        }
                    }
                }
            }
            val meta = runCatching { org.json.JSONObject(File(tmp, "meta.json").readText()) }.getOrNull()
                ?: return null to if (hasDlls) {
                    "That archive contains Windows DLLs, not an Android HwD3D build (libdxvk_*.so). Build one with gpu/host/hwd3d/build.sh."
                } else {
                    "No meta.json in that archive. Make one with gpu/host/hwd3d/build.sh."
                }
            if (meta.optInt("abi") != DXVK_ABI)
                return null to "Built for HwD3D ABI ${meta.optInt("abi")}; this app speaks $DXVK_ABI. Build it again with the app's patch."
            if (libs == 0 || dxvkMeta(tmp) == null) return null to "That archive has no libdxvk_dxgi.so and libdxvk_d3d11.so."
            val id = meta.optString("name", "dxvk").lowercase().replace(Regex("[^a-z0-9.]+"), "-").trim('-').ifEmpty { "dxvk" }
            val dest = File(dxvkDir(ctx), id)
            dest.deleteRecursively()
            tmp.renameTo(dest)
            return id to null
        } finally {
            tmp.deleteRecursively()
        }
    }

    private fun readCString(buf: ByteArray, off: Int, maxLen: Int): String {
        var len = 0
        while (len < maxLen && buf[off + len] != 0.toByte()) len++
        return String(buf, off, len, Charsets.UTF_8)
    }

    private fun readFully(input: InputStream, dst: ByteArray): Boolean {
        var off = 0
        while (off < dst.size) {
            val n = input.read(dst, off, dst.size - off)
            if (n < 0) return false
            off += n
        }
        return true
    }

    private fun copyExact(input: InputStream, out: java.io.OutputStream, bytes: Long) {
        val buf = ByteArray(16384)
        var left = bytes
        while (left > 0) {
            val n = input.read(buf, 0, minOf(buf.size.toLong(), left).toInt())
            if (n < 0) break
            out.write(buf, 0, n)
            left -= n
        }
    }

    private fun skipExact(input: InputStream, bytes: Long) {
        val buf = ByteArray(8192)
        var left = bytes
        while (left > 0) {
            val n = input.read(buf, 0, minOf(buf.size.toLong(), left).toInt())
            if (n < 0) break
            left -= n
        }
    }

    private var ctx: Context? = null
    private var window: AvfPlan.SharedWindow? = null
    private var width = 0
    private var height = 0
    @Volatile var running = false
        private set

    // The compositor layers GPU frames go on (SGPU_CMD_SCANOUT), above the CPU-drawn desktop. This
    // process owns them and lends them to each renderer, so they outlive a renderer that is replaced.
    private var layer: SurfaceControl? = null
    private var d3dLayer: SurfaceControl? = null
    private var gameUnder: SurfaceControl? = null
    private var gameOver: SurfaceControl? = null
    private var pointer: SurfaceControl? = null
    private var layerWidth = 0
    private var layerHeight = 0

    const val VSYNC = "scanout_vsync"

    /** Whether GPU frames wait for the phone's screen ("Limit to screen refresh"). On by default. */
    fun vsync(ctx: Context) = prefs(ctx).getBoolean(VSYNC, true)

    fun setVsync(ctx: Context, on: Boolean) {
        prefs(ctx).edit().putBoolean(VSYNC, on).apply()
        for (r in renderers) r.send(GpuService.SET_VSYNC, Bundle().apply { putBoolean("vsync", on) })
    }

    /**
     * One renderer process. The desktop one serves the rings, DWM and the screen driver. It hands
     * every other Direct3D program to the games one, so a game that crashes its process leaves the
     * desktop drawing. Each is told the other's messenger and talks to it straight.
     */
    private class Renderer(val cls: Class<out GpuService>, val games: Boolean) {
        var service: Messenger? = null
        var bound = false

        fun send(what: Int, data: Bundle? = null) {
            val m = service ?: return
            runCatching { m.send(Message.obtain(null, what).apply { data?.let { this.data = it } }) }
        }

        fun layerBundle() = Bundle().apply {
            if (games) {
                putParcelable("layer", gameOver)
                putParcelable("underLayer", gameUnder)
                putParcelable("d3dLayer", d3dLayer)
            } else {
                putParcelable("layer", layer)
                putParcelable("pointerLayer", pointer)
            }
            putInt("layerWidth", layerWidth)
            putInt("layerHeight", layerHeight)
        }

        val connection = object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName, binder: IBinder) {
                val w = window ?: return
                val c = ctx ?: return
                service = Messenger(binder)
                send(GpuService.START, layerBundle().apply {
                    putParcelable("window", w.memory)
                    putParcelable("heap", w.heap)
                    putInt("width", width)
                    putInt("height", height)
                    putBundle("env", env(c).apply { if (games) putString("HWGPU_ROLE", "games") })
                    putBoolean("vsync", vsync(c))
                })
                introduce()
                if (!games) running = true
                Log.i(TAG, "${if (games) "games" else "desktop"} renderer process connected: ${describe(c)}")
            }

            override fun onServiceDisconnected(name: ComponentName) {
                // Died, most likely in the driver. The system would start it again only after a growing
                // delay, so bind afresh at once; Windows programs waiting on the GPU then get errors
                // instead of hanging.
                service = null
                if (games) {
                    // The desktop drops the channels it had handed over, and keeps drawing.
                    desktop.send(GpuService.GAMES_DIED)
                    introduce()
                    hideGameLayers()
                    Log.w(TAG, "games renderer process ended, starting another")
                } else {
                    running = false
                    // Its last frame would stay up over the desktop; the next renderer shows the layer
                    // again with its first frame.
                    hideLayer()
                    Log.w(TAG, "renderer process ended, starting another")
                }
                Handler(Looper.getMainLooper()).postDelayed({ if (window != null) { unbind(); bind() } }, 500)
            }

            override fun onBindingDied(name: ComponentName) {
                if (window != null) { unbind(); bind() }
            }
        }

        fun bind() {
            val c = ctx ?: return
            if (bound) return
            bound = c.bindService(Intent(c, cls), connection, Context.BIND_AUTO_CREATE or Context.BIND_IMPORTANT)
            if (!bound) Log.e(TAG, "could not start the ${if (games) "games " else ""}renderer process")
        }

        fun unbind() {
            if (!bound) return
            bound = false
            service = null
            runCatching { ctx?.unbindService(connection) }
        }
    }

    private val desktop = Renderer(GpuService::class.java, false)
    private val games = Renderer(GameGpuService::class.java, true)
    private val renderers = listOf(desktop, games)

    /** Tells each renderer where the other one is, or that it is gone. */
    private fun introduce() {
        desktop.send(GpuService.PEER, Bundle().apply { putParcelable("peer", games.service) })
        games.send(GpuService.PEER, Bundle().apply { putParcelable("peer", desktop.service) })
    }

    /** The layers scanout and HwD3D frames go on and the size they cover, or nulls when the view
     *  has none. Games draw under the desktop when windowed, and over it when full screen. */
    fun setLayer(sc: SurfaceControl?, d3d: SurfaceControl?, under: SurfaceControl?, over: SurfaceControl?,
                 pointerLayer: SurfaceControl?, width: Int, height: Int) {
        val sameLayer = sc != null && sc === layer
        pointer = pointerLayer
        layer = sc
        d3dLayer = d3d
        gameUnder = under
        gameOver = over
        layerWidth = width
        layerHeight = height
        for (r in renderers) r.send(if (sameLayer) GpuService.LAYER_SIZE else GpuService.LAYER, r.layerBundle())
    }

    private fun hideGameLayers() {
        d3dLayer?.let { sc -> runCatching { SurfaceControl.Transaction().setVisibility(sc, false).apply() } }
    }

    /** Takes the GPU's frame off the screen, for when nobody will replace it (renderer or VM gone). */
    fun hideLayer() {
        for (sc in listOfNotNull(layer, pointer)) runCatching { SurfaceControl.Transaction().setVisibility(sc, false).apply() }
        hideGameLayers()
    }

    fun start(context: Context, window: AvfPlan.SharedWindow, width: Int, height: Int) {
        ctx = context.applicationContext
        this.window = window
        this.width = width
        this.height = height
        desktop.bind()
        games.bind()
    }

    fun stop() {
        hideLayer()
        window = null
        for (r in renderers) r.unbind()
        running = false
    }

    /** New renderers with the current driver settings, under the running Windows. */
    fun restart() {
        if (window == null) return
        for (r in renderers) { r.unbind(); r.bind() }
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
        val fixes = on.filter { it.fix }.map { FIX_LIBS.getValue(it.id) }.toMutableList()
        // Triggered Vulkan capture: when debug.hwgpu.vkcap names a game (e.g. "borderlands"),
        // insert libvulkan_vkcap.so at the top of the chain (right below DXVK, above MaliVK).
        val vkcap = runCatching {
            Class.forName("android.os.SystemProperties").getMethod("get", String::class.java, String::class.java)
                .invoke(null, "debug.hwgpu.vkcap", "") as String
        }.getOrDefault("")
        if (vkcap.isNotEmpty()) fixes.add(0, "libvulkan_vkcap.so")
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
            // The DXVK HwD3D draws Direct3D with: the shipped one is found by name, an added one by folder.
            val dxvk = dxvkChoice(ctx)
            if (dxvk != DXVK_SHIPPED) putString("HWD3D_DXVK_DIR", File(dxvkDir(ctx), dxvk).absolutePath)
            // Compiled shaders, kept between runs by the bottom wrapper (shader-cache/README.md).
            putString("HWGPU_VK_CACHE_DIR", shaderCacheDir(ctx).absolutePath)
            // Touched while Windows' screen reaches the scanout layer; DisplayWatchdog reads its time.
            putString("HWGPU_SCREEN_FLAG", DisplayWatchdog.flagFile(ctx).absolutePath)
            if (driver != null) putString("HWGPU_VK_DRIVER", driver.absolutePath)
            // adb shell setprop log.tag.hwgpu DEBUG: the server logs every guest request.
            if (Log.isLoggable("hwgpu", Log.DEBUG)) putString("HWGPU_TRACE", "1")
        }
    }
}

/** The renderer process. It lives while the app is bound to it, and exits when unbound. */
open class GpuService : Service() {
    companion object {
        const val START = 1
        const val LAYER = 2       // a new scanout layer, or none
        const val LAYER_SIZE = 3  // the same layer, another size
        const val SET_VSYNC = 4
        const val PEER = 5        // the other renderer's messenger, or none
        const val ADOPT = 6       // games: serve this channel the desktop read the program of
        const val SET_HOLE = 7    // desktop: where a windowed game shows through
        const val GAMES_DIED = 8  // desktop: the games renderer ended
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
    @Volatile private var peer: Messenger? = null

    private val messenger = Messenger(Handler(Looper.getMainLooper()) { msg ->
        when (msg.what) {
            START -> if (!started) start(msg.data)
            LAYER -> if (started) setLayer(msg.data)
            LAYER_SIZE -> if (started) nativeSetLayerSize(msg.data.getInt("layerWidth"), msg.data.getInt("layerHeight"))
            SET_VSYNC -> if (started) nativeSetVsync(msg.data.getBoolean("vsync", true))
            PEER -> peer = msg.data.getParcelable("peer", Messenger::class.java)
            ADOPT -> if (started) msg.data.let { d ->
                if (!nativeAdopt(d.getInt("slot"), d.getInt("generation"), d.getString("exe") ?: "", d.getString("conf") ?: ""))
                    Log.e(TAG, "could not take channel ${d.getInt("slot")}")
            }
            SET_HOLE -> if (started) msg.data.let { d ->
                nativeSetHole(d.getBoolean("on"), d.getInt("l"), d.getInt("t"), d.getInt("r"), d.getInt("b"))
            }
            GAMES_DIED -> if (started) nativeGamesDied()
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
        nativeSetCallbacks()
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
        if (data.containsKey("underLayer")) nativeSetUnderLayer(data.getParcelable("underLayer", SurfaceControl::class.java))
        if (data.containsKey("pointerLayer")) nativeSetPointerLayer(data.getParcelable("pointerLayer", SurfaceControl::class.java))
        val d3d = data.getParcelable("d3dLayer", SurfaceControl::class.java)
        val surface = d3d?.let { runCatching { Surface(it) }.onFailure { e -> Log.e(TAG, "no HwD3D surface", e) }.getOrNull() }
        nativeSetD3dLayer(d3d, surface)
        d3dSurface?.release()
        d3dSurface = surface
    }

    override fun onBind(intent: Intent): IBinder = messenger.binder

    /** Desktop, from a channel thread: gives a Direct3D program's channel to the games renderer. */
    @Suppress("unused")  // called from hwgpu_jni.cpp
    private fun handoff(slot: Int, generation: Int, exe: String, conf: String): Boolean {
        val m = peer ?: return false
        val data = Bundle().apply {
            putInt("slot", slot)
            putInt("generation", generation)
            putString("exe", exe)
            putString("conf", conf)
        }
        return runCatching { m.send(Message.obtain(null, ADOPT).apply { this.data = data }) }.isSuccess
    }

    /** Games, from a channel thread: where the desktop must let a windowed game show through. */
    @Suppress("unused")  // called from hwgpu_jni.cpp
    private fun hole(on: Boolean, l: Int, t: Int, r: Int, b: Int) {
        val m = peer ?: return
        val data = Bundle().apply {
            putBoolean("on", on); putInt("l", l); putInt("t", t); putInt("r", r); putInt("b", b)
        }
        runCatching { m.send(Message.obtain(null, SET_HOLE).apply { this.data = data }) }
    }

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
    private external fun nativeSetCallbacks()
    private external fun nativeAdopt(slot: Int, generation: Int, exe: String, conf: String): Boolean
    private external fun nativeSetHole(on: Boolean, l: Int, t: Int, r: Int, b: Int)
    private external fun nativeGamesDied()
    private external fun nativeSetUnderLayer(layer: SurfaceControl?)
    private external fun nativeSetPointerLayer(layer: SurfaceControl?)
}

/** The games renderer: Direct3D programs that are not part of the desktop, in a process of their own. */
class GameGpuService : GpuService()
