package dev.hyperwin.app

import android.app.Activity
import android.net.Uri
import java.io.File

/** Builds a machine's system.img from the user's ISO without running Windows Setup. */
object MachinePrep {
    fun interface Progress {
        fun step(percent: Int, message: String)
    }

    private external fun nativePrepare(json: String, isoFd: Int, assets: String, progress: Progress): String
    private external fun nativeReplaceBootFile(disk: String, path: String, data: ByteArray): Boolean
    private external fun nativeHardenBoot(disk: String, scratch: String): Int
    private external fun nativeNtfsDelete(disk: String, paths: Array<String>): Int
    private external fun nativePatchGuest(disk: String, assets: String, progress: Progress): String

    /**
     * Repair hook: when /data/local/tmp/hyperwin/ntfsdel.txt exists, each line is a path inside the
     * Windows partition (like /Windows/System32/x.sys) that is deleted before the machine starts.
     * For a driver that keeps Windows from booting. It is applied once per file time; remove the file afterwards.
     */
    fun applyOfflineDeletes(machine: Machine) {
        val list = File("/data/local/tmp/hyperwin/ntfsdel.txt")
        runCatching {
            if (!list.canRead()) return
            val paths = list.readLines().map { it.trim() }.filter { it.startsWith("/") }
            // The list is shell-owned, so it cannot be renamed from here: it is applied once per file
            // time, and whoever wrote it removes it.
            val seen = File(machine.disk.parentFile, "ntfsdel.applied")
            if (seen.exists() && seen.readText() == list.lastModified().toString()) return
            val n = nativeNtfsDelete(machine.disk.absolutePath, paths.toTypedArray())
            android.util.Log.i("HyperWin", "offline deletes on ${machine.name}: $n of ${paths.size}")
            seen.writeText(list.lastModified().toString())
        }
    }

    /** Keeps Windows out of its recovery screens after a cut-short boot. Cheap when already so. */
    fun hardenBoot(ctx: android.content.Context, machine: Machine) {
        val rc = runCatching { nativeHardenBoot(machine.disk.absolutePath, File(ctx.cacheDir, "bcd.tmp").absolutePath) }.getOrDefault(-1)
        if (rc != 0) android.util.Log.i("HyperWin", if (rc == 1) "boot settings fixed on ${machine.name}" else "boot settings check failed")
    }

    /**
     * Our boot files (loader, display, compatibility) live in the machine's boot partition. Brings
     * them up to this app's copies; false means the disk still has older ones.
     */
    fun refreshLoader(ctx: android.content.Context, machine: Machine): Boolean = runCatching {
        val names = listOf("BOOTAA64.EFI", "GOP.EFI", "COMPAT.EFI")
        val files = names.map { name -> ctx.assets.open("machine/efi/$name").use { it.readBytes() } }
        val stamp = java.util.zip.CRC32().apply { files.forEach(::update) }.value
        if (machine.loaderStamp == stamp) return true
        val ok = names.indices.all { nativeReplaceBootFile(machine.disk.absolutePath, "EFI/BOOT/${names[it]}", files[it]) }
        if (ok) { machine.loaderStamp = stamp; machine.save() }
        android.util.Log.i("HyperWin", if (ok) "boot files updated on ${machine.name}" else "boot file update failed")
        ok
    }.getOrDefault(false)

    init { System.loadLibrary("hyperwin") }

    fun prepare(activity: Activity, machine: Machine, progress: Progress): String {
        val uri = Uri.parse(machine.source)
        val assets = stageAssets(activity)
        machine.prepared = false
        machine.save()
        val report = activity.contentResolver.openFileDescriptor(uri, "r").use { pfd ->
            requireNotNull(pfd) { "Could not open ${machine.sourceName}" }
            nativePrepare(machine.json.absolutePath, pfd.fd, assets.absolutePath, progress)
        }
        if (report == "ok") {
            GuestBoot.requireAligned(machine.disk)
            machine.guestStamp = gpuStamp(activity)
            machine.prepared = true
            machine.save()
            Machine.setActive(activity, machine)
            return "${machine.name} is ready. Boot Windows to start it."
        }
        return report
    }

    fun guestNeedsPatch(ctx: android.content.Context, machine: Machine): Boolean =
        machine.prepared && machine.guestStamp != gpuStamp(ctx)

    /** Copies updated HyperWin drivers and tools into system.img offline before Windows boots. */
    fun patchGuest(ctx: android.content.Context, machine: Machine, progress: Progress): String {
        val dir = File(ctx.cacheDir, "patch-assets").apply { deleteRecursively(); mkdirs() }
        try {
            progress.step(0, "Preparing update files…")
            copyAssetTree(ctx, "machine/guest", File(dir, "guest"))
            copyAssetTree(ctx, "machine/drivers", File(dir, "drivers"))
            val report = nativePatchGuest(machine.disk.absolutePath, dir.absolutePath, progress)
            android.util.Log.i("HyperWin", "patchGuest(${machine.name}): $report")
            if (report == "ok") {
                machine.guestStamp = gpuStamp(ctx)
                machine.save()
            }
            return report
        } finally {
            dir.deleteRecursively()
        }
    }

    /** GPU files under machine/guest/gpu (tools/machine_assets.sh), installed by gpu-install.cmd. */
    fun gpuFiles(ctx: android.content.Context): List<String> {
        fun walk(path: String): List<String> = ctx.assets.list("machine/guest/gpu$path").orEmpty().let { names ->
            if (names.isEmpty()) listOf(path.removePrefix("/")) else names.flatMap { walk("$path/$it") }
        }
        return walk("").filter { it != "stamp.txt" }
    }

    /** Fingerprint of this app's GPU files; Windows keeps the one it has in C:\HyperWin\gpu\stamp.txt. */
    fun gpuStamp(ctx: android.content.Context): String =
        ctx.assets.open("machine/guest/gpu/stamp.txt").use { it.readBytes().decodeToString().trim() }

    private fun copyAssetTree(ctx: android.content.Context, path: String, dest: File) {
        val children = ctx.assets.list(path).orEmpty()
        if (children.isEmpty()) {
            dest.parentFile?.mkdirs()
            ctx.assets.open(path).use { input -> dest.outputStream().use { input.copyTo(it) } }
        } else children.forEach { copyAssetTree(ctx, "$path/$it", File(dest, it)) }
    }

    private fun stageAssets(activity: Activity): File {
        val dir = File(activity.cacheDir, "prep-assets").apply { deleteRecursively(); mkdirs() }
        // A new machine gets the GPU at first boot (firstboot.cmd); its stamp.txt says it is current.
        copyAssetTree(activity, "machine", dir)
        return dir
    }
}
