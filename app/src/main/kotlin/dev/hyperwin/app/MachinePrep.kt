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
            machine.prepared = true
            machine.save()
            Machine.setActive(activity, machine)
            return "${machine.name} is ready. Boot Windows to start it."
        }
        return report
    }

    /** GPU files in machine/guest/gpu (tools/machine_assets.sh), installed by gpu-install.cmd. */
    val GPU_FILES = listOf("hwgpu.sys", "vulkan_hwgpu.dll", "vulkan_hwgpu.json", "vktest.exe", "vulkan-1.dll")

    /** Checksum of this app's GPU files; Windows keeps the one it has in C:\HyperWin\gpu\stamp.txt. */
    fun gpuStamp(ctx: android.content.Context): Long = gpuStampCache ?: java.util.zip.CRC32().apply {
        for (name in GPU_FILES) ctx.assets.open("machine/guest/gpu/$name").use { update(it.readBytes()) }
    }.value.also { gpuStampCache = it }
    private var gpuStampCache: Long? = null

    private fun stageAssets(activity: Activity): File {
        val dir = File(activity.cacheDir, "prep-assets").apply { deleteRecursively(); mkdirs() }
        fun copy(path: String, dest: File) {
            val children = activity.assets.list(path).orEmpty()
            if (children.isEmpty()) {
                dest.parentFile?.mkdirs()
                activity.assets.open(path).use { input -> dest.outputStream().use { input.copyTo(it) } }
            } else children.forEach { copy("$path/$it", File(dest, it)) }
        }
        copy("machine", dir)
        // A new machine gets the GPU at first boot (firstboot.cmd); the stamp says it is current.
        File(dir, "guest/gpu/stamp.txt").writeText("${gpuStamp(activity)}\r\n")
        return dir
    }
}
