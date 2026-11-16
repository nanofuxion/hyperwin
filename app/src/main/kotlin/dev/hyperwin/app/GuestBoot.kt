package dev.hyperwin.app

import android.content.Context
import java.io.File

/**
 * Same boot shape as SweetCider's [dev.sweetcider.probe.VmSmokeTest]:
 * a Linux ARM64 Image from setKernelPath, and a private raw disk from
 * VirtualMachineCustomImageConfig.Disk.RWDisk. The disk must be a non-empty
 * multiple of 4096 bytes. EDK2 is only the QEMU stand-in that boots the
 * tiny11 ISO; AVF itself enters the Image, it does not run a UEFI shell.
 */
object GuestBoot {
    private const val PKG = "android.system.virtualmachine."

    fun requireAligned(disk: File) {
        require(disk.isFile && disk.length() > 0 && disk.length() % 4096L == 0L) {
            "AVF disk image must be non-empty and 4096-byte aligned: ${disk.absolutePath} (${disk.length()} bytes)"
        }
    }

    /** Working copy of the Windows disk, the way SweetCider copies root.img into filesDir. */
    fun workingDisk(ctx: Context, seed: File): File {
        requireAligned(seed)
        val dir = File(ctx.filesDir, "win").apply { mkdirs() }
        val working = File(dir, "system.img")
        if (!working.isFile || working.length() != seed.length()) seed.copyTo(working, overwrite = true)
        requireAligned(working)
        return working
    }

    fun addDisk(cic: Any, disk: File, id: String? = null) {
        requireAligned(disk)
        val builder = cic.javaClass
        val diskClass = Class.forName(PKG + "VirtualMachineCustomImageConfig\$Disk")
        val rw = if (id == null) {
            diskClass.getDeclaredMethod("RWDisk", String::class.java).apply { isAccessible = true }
                .invoke(null, disk.absolutePath)
        } else {
            diskClass.getDeclaredMethod("RWDisk", String::class.java, String::class.java)
                .apply { isAccessible = true }
                .invoke(null, disk.absolutePath, id)
        }
        builder.getDeclaredMethod("addDisk", diskClass).apply { isAccessible = true }.invoke(cic, rw)
    }

    fun customImage(ctx: Context, kernel: File, disk: File): Any =
        installImage(ctx, kernel, listOf(workingDisk(ctx, disk)))

    /**
     * UEFI boot. The firmware and every disk are files this app owns.
     * Host files are not mounted; the guest reads those through the vsock file port.
     */
    fun uefiImage(pflash: List<File>, disks: List<File>, bootloader: File? = null): Any {
        pflash.forEach { requireAligned(it) }
        val cicClass = Class.forName(PKG + "VirtualMachineCustomImageConfig\$Builder")
        val cic = cicClass.getDeclaredConstructor().newInstance()
        cicClass.getMethod("setName", String::class.java).invoke(cic, "hyperwin")
        if (bootloader != null) {
            cicClass.getMethod("setBootloaderPath", String::class.java)
                .invoke(cic, bootloader.absolutePath)
        }
        if (pflash.isNotEmpty()) {
            val paths = pflash.map { it.absolutePath }.toTypedArray()
            cicClass.getMethod("setPflashPaths", Array<String>::class.java)
                .invoke(cic, paths as Any)
        }
        val displayClass = Class.forName(PKG + "VirtualMachineCustomImageConfig\$DisplayConfig\$Builder")
        val display = displayClass.getDeclaredConstructor().newInstance()
        displayClass.getMethod("setWidth", Int::class.javaPrimitiveType).invoke(display, 1280)
        displayClass.getMethod("setHeight", Int::class.javaPrimitiveType).invoke(display, 720)
        displayClass.getMethod("setHorizontalDpi", Int::class.javaPrimitiveType).invoke(display, 160)
        displayClass.getMethod("setVerticalDpi", Int::class.javaPrimitiveType).invoke(display, 160)
        displayClass.getMethod("setRefreshRate", Int::class.javaPrimitiveType).invoke(display, 60)
        cicClass.getMethod(
            "setDisplayConfig",
            Class.forName(PKG + "VirtualMachineCustomImageConfig\$DisplayConfig"),
        ).invoke(cic, displayClass.getMethod("build").invoke(display))
        cicClass.getMethod("useTouch", Boolean::class.javaPrimitiveType).invoke(cic, true)
        cicClass.getMethod("useKeyboard", Boolean::class.javaPrimitiveType).invoke(cic, true)
        cicClass.getMethod("useMouse", Boolean::class.javaPrimitiveType).invoke(cic, true)
        disks.forEach { addDisk(cic, it) }
        val gpu = cicClass.getMethod(
            "setGpuConfig",
            Class.forName(PKG + "VirtualMachineCustomImageConfig\$GpuConfig"),
        )
        gpu.invoke(cic, AvfPlan.gpuBuilder())
        return cicClass.getMethod("build").invoke(cic)!!
    }

    /** Payload plus every install disk: system, OEM drivers, and the ISO. */
    fun installImage(ctx: Context, kernel: File, disks: List<File>): Any {
        require(ctx.filesDir.isDirectory)
        require(kernel.isFile) { "missing AVF payload ${kernel.absolutePath}" }
        val cicClass = Class.forName(PKG + "VirtualMachineCustomImageConfig\$Builder")
        val cic = cicClass.getDeclaredConstructor().newInstance()
        cicClass.getMethod("setName", String::class.java).invoke(cic, "hyperwin")
        cicClass.getMethod("setKernelPath", String::class.java).invoke(cic, kernel.absolutePath)
        disks.forEach { addDisk(cic, it) }
        val gpu = cicClass.getMethod("setGpuConfig", Class.forName(PKG + "VirtualMachineCustomImageConfig\$GpuConfig"))
        gpu.invoke(cic, AvfPlan.gpuBuilder())
        return cicClass.getMethod("build").invoke(cic)!!
    }

    /**
     * The QEMU boot that reached Windows Setup: edk2-gunyah as the ARM64 Image,
     * the setup ESP, then the installer. No virtio-gpu, so Windows keeps the shared GOP.
     */
    fun setupImage(kernel: File, disks: List<File>): Any {
        require(kernel.isFile) { "missing firmware ${kernel.absolutePath}" }
        disks.forEach { requireAligned(it) }
        val cicClass = Class.forName(PKG + "VirtualMachineCustomImageConfig\$Builder")
        val cic = cicClass.getDeclaredConstructor().newInstance()
        cicClass.getMethod("setName", String::class.java).invoke(cic, "hyperwin")
        cicClass.getMethod("setKernelPath", String::class.java).invoke(cic, kernel.absolutePath)
        cicClass.getMethod("useKeyboard", Boolean::class.javaPrimitiveType).invoke(cic, true)
        cicClass.getMethod("useMouse", Boolean::class.javaPrimitiveType).invoke(cic, true)
        disks.forEach { addDisk(cic, it) }
        return cicClass.getMethod("build").invoke(cic)!!
    }
}
