package dev.hyperwin.app

import android.content.Context
import java.io.File
import java.io.RandomAccessFile

/**
 * Install media for a Windows ARM64 guest, laid out the way SweetCider keeps a private
 * root image and then asks the guest installer to place files.
 *
 * The ISO is the user's own setup disc. It is copied into private storage unchanged.
 * system.img is the writable target (4096-byte aligned, which AVF requires of every
 * RWDisk). oem.img is a FAT volume whose root holds autounattend.xml and whose hyperwin
 * directory holds the driver DLLs and the gfxstream ICD. First logon copies that
 * directory to C:\HyperWin and registers the ICD. System d3d11.dll stays put.
 *
 * Test signing is not written into the ISO. Before each boot of an installed system
 * disk, [enableTestSigning] sets BCD element 16000049 on that disk's Windows loaders.
 */
object WindowsInstall {
    const val SYSTEM_BYTES = 40L shl 30

    private external fun nativePack(dir: String, image: String): Int
    private external fun nativeEnableTestSigning(disk: String): String

    /** Sets test signing on the installed system disk. A stock ISO is left untouched. */
    fun enableTestSigning(disk: File): String {
        if (!disk.isFile) return "No Windows BCD on this disk yet."
        return nativeEnableTestSigning(disk.absolutePath)
    }

    init { System.loadLibrary("hyperwin") }

    data class Media(
        val iso: File,
        val system: File,
        val oem: File,
        val detail: String,
    )

    fun prepare(ctx: Context, iso: File, systemBytes: Long = SYSTEM_BYTES): Media {
        require(iso.isFile && iso.length() > 0L) { "ISO is missing: ${iso.absolutePath}" }
        val win = File(ctx.filesDir, "win").apply { mkdirs() }
        val stagedIso = File(win, "install.iso")
        if (iso.absolutePath != stagedIso.absolutePath) {
            iso.inputStream().use { input -> stagedIso.outputStream().use { input.copyTo(it) } }
        }
        val system = File(win, "system.img")
        val bytes = (systemBytes + 4095L) and -4096L
        RandomAccessFile(system, "rw").use { it.setLength(bytes) }
        GuestBoot.requireAligned(system)

        val tree = File(win, "oem-tree").apply { deleteRecursively(); mkdirs() }
        ctx.assets.open("autounattend.xml").use { input ->
            File(tree, "autounattend.xml").outputStream().use { input.copyTo(it) }
        }
        File(tree, "hyperwin.id").writeText("hyperwin\n")
        val bin = File(tree, "hyperwin").apply { mkdirs() }
        ctx.assets.list("hyperwin")?.forEach { name ->
            ctx.assets.open("hyperwin/$name").use { input ->
                File(bin, name).outputStream().use { input.copyTo(it) }
            }
        }
        val oem = File(win, "oem.img")
        val packed = nativePack(tree.absolutePath, oem.absolutePath)
        require(packed == 0 && oem.isFile) { "OEM driver disk was not packed ($packed)" }
        GuestBoot.requireAligned(oem)

        val detail = """
            ISO ${stagedIso.absolutePath} (${stagedIso.length()} bytes)
            system disk ${system.absolutePath} (${system.length()} bytes, 4096-aligned)
            driver disk ${oem.absolutePath} (${oem.length()} bytes)
            ISO copied unchanged. Test signing is set on the installed disk's BCD before boot.
            Boot: setKernelPath(hyperwin.img), RWDisk(system), RWDisk(oem), RWDisk(iso), gfxstream Vulkan.
            Setup reads autounattend.xml from the driver disk and copies hyperwin\\ to C:\HyperWin.
        """.trimIndent()
        return Media(stagedIso, system, oem, detail)
    }
}
