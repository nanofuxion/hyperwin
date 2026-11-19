package dev.hyperwin.app

import android.content.Context
import android.os.ParcelFileDescriptor
import android.os.SharedMemory
import android.system.Os
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.lang.reflect.InvocationTargetException
import java.lang.reflect.Proxy

/** What an AVF launch uses. QEMU's nvdimm plus virtio-gpu-pci is the same shape. */
object AvfPlan {
    const val WINDOW_SIZE = 128 * 1024 * 1024
    const val WINDOW_SIZE_MIB = WINDOW_SIZE / (1024 * 1024)
    const val RAM_BASE_PA = 0x8000_0000L
    const val WINDOWS_MEMORY_MIB = 3072
    /**
     * GPU memory Windows programs map directly, shared with the renderer (include/hw_abi.h,
     * HW_HEAP_*): a second region right below the window. The VM gets this much more memory, so
     * Windows keeps what the machine asks for.
     */
    const val HEAP_SIZE_MIB = 512
    const val HEAP_SIZE = HEAP_SIZE_MIB * 1024 * 1024
    /** Memory the VM needs beyond what Windows itself gets: the window and the GPU heap. */
    const val SHARED_MIB = WINDOW_SIZE_MIB + HEAP_SIZE_MIB
    const val WINDOWS_VM_MEMORY_MIB = WINDOWS_MEMORY_MIB + SHARED_MIB
    const val WINDOW_GUARD_BELOW_TOP_MIB = 16
    /** Default guest frame size. A machine may ask for another; see include/hw_abi.h for the limits. */
    const val FRAME_WIDTH = 1280
    const val FRAME_HEIGHT = 720
    /** Frame size of the window made last, which is the running guest's. */
    @Volatile var frameWidth = FRAME_WIDTH
        private set
    @Volatile var frameHeight = FRAME_HEIGHT
        private set
    val frameBytes get() = frameWidth * frameHeight * 4
    /** hw_frame_off(h, 0): big frames start past the audio ring. */
    val frameOffset get() = if (2 * frameBytes > 0x800000 - 0x10000) 0x1000000 else 0x10000

    fun validFrame(width: Int, height: Int) =
        width in 640..4096 && height in 480..2304 && 0x1000000L + 2L * width * height * 4 <= 0x7f00000L
    /** Keep the ring inside UEFI conventional RAM, with room below its top boundary. */
    const val WINDOW_PA = RAM_BASE_PA + (WINDOWS_MEMORY_MIB - WINDOW_SIZE_MIB - WINDOW_GUARD_BELOW_TOP_MIB).toLong() * 1024 * 1024
    const val HEAP_PA = WINDOW_PA - HEAP_SIZE.toLong()
    const val VSOCK_PORT = 5601

    fun stageUnattended(ctx: Context): File {
        val root = File(ctx.filesDir, "oem").apply { mkdirs() }
        val bin = File(root, "hyperwin").apply { mkdirs() }
        ctx.assets.open("autounattend.xml").use { input ->
            File(root, "autounattend.xml").outputStream().use { input.copyTo(it) }
        }
        File(root, "hyperwin.id").writeText("hyperwin\n")
        ctx.assets.list("hyperwin")?.forEach { name ->
            ctx.assets.open("hyperwin/$name").use { input ->
                File(bin, name).outputStream().use { input.copyTo(it) }
            }
        }
        return root
    }

    /** ashmem region crosvm maps with --file-backed-mapping. Same hook SweetCider uses. */
    fun createWindow(seed: ByteArray, width: Int = FRAME_WIDTH, height: Int = FRAME_HEIGHT): SharedWindow {
        val shm = SharedMemory.create("hyperwin", WINDOW_SIZE)
        val map = shm.mapReadWrite().order(ByteOrder.LITTLE_ENDIAN)
        WindowBridge.nativeInit(map, seed)
        val valid = validFrame(width, height)
        frameWidth = if (valid) width else FRAME_WIDTH
        frameHeight = if (valid) height else FRAME_HEIGHT
        // Windows shows these as the computer and processor instead of the firmware's "Gunyah" text.
        val soc = listOf(android.os.Build.SOC_MANUFACTURER, android.os.Build.SOC_MODEL)
            .filter { it.isNotBlank() && it != android.os.Build.UNKNOWN }
        WindowBridge.nativeConfigure(
            map, frameWidth, frameHeight, hostCpuMhz(),
            android.os.Build.MANUFACTURER, android.os.Build.MODEL,
            soc.firstOrNull().orEmpty(), soc.joinToString(" "),
        )
        val native = nativeScreen()
        WindowBridge.nativeSetScreen(map, frameWidth, frameHeight, native.first, native.second, false)
        val pfd = fdOf(shm)
        val link = runCatching { Os.readlink("/proc/self/fd/${pfd.fd}") }.getOrDefault("?")
        // Zero until the renderer hands parts of it out; pages are only real once touched.
        val heap = SharedMemory.create("hyperwin-gpu", HEAP_SIZE)
        return SharedWindow(shm, map, pfd, link, heap, fdOf(heap))
    }

    private fun fdOf(shm: SharedMemory): ParcelFileDescriptor {
        val parcel = android.os.Parcel.obtain()
        shm.writeToParcel(parcel, 0)
        parcel.setDataPosition(0)
        val pfd = parcel.readFileDescriptor()
        parcel.recycle()
        return pfd
    }

    /** Top clock of the phone's fastest core, in MHz. */
    fun hostCpuMhz(): Int =
        (File("/sys/devices/system/cpu").listFiles { f -> f.name.matches(Regex("cpu\\d+")) } ?: emptyArray())
            .mapNotNull { runCatching { File(it, "cpufreq/cpuinfo_max_freq").readText().trim().toInt() / 1000 }.getOrNull() }
            .maxOrNull() ?: 0

    /**
     * The public Java custom-image builder currently serializes no custom memory backing files.
     * The framework still builds the normal raw AVF config internally, so intercept its
     * IVirtualizationService.createVm call and add the backing memfd immediately before parceling.
     * The hook is scoped to the synchronous VM creation call and then restored.
     */
    fun installWindowBackingHook(window: SharedWindow): AutoCloseable {
        val serviceClass = Class.forName("android.system.virtualmachine.VirtualizationService")
        val getInstance = serviceClass.getDeclaredMethod("getInstance").apply { isAccessible = true }
        val service = getInstance.invoke(null)
        val binderField = serviceClass.getDeclaredField("mBinder").apply { isAccessible = true }
        val original = requireNotNull(binderField.get(service)) { "AVF service binder is null" }
        val serviceInterface = Class.forName("android.system.virtualizationservice.IVirtualizationService")
        val proxy = Proxy.newProxyInstance(
            serviceInterface.classLoader,
            arrayOf(serviceInterface),
        ) { proxyObject, method, args ->
            when (method.name) {
                "toString" -> "HyperWin(${original})"
                "hashCode" -> System.identityHashCode(proxyObject)
                "equals" -> proxyObject === args?.firstOrNull()
                "createVm" -> {
                    val vmConfig = requireNotNull(args?.firstOrNull())
                    val rawConfig = vmConfig.javaClass.getMethod("getRawConfig").invoke(vmConfig)
                    val backingClass = Class.forName("android.system.virtualizationservice.CustomMemoryBackingFile")
                    val backings = java.lang.reflect.Array.newInstance(backingClass, 2)
                    fun backing(fd: ParcelFileDescriptor, start: Long, size: Long) = backingClass.getDeclaredConstructor().newInstance().also {
                        backingClass.getField("file").set(it, fd.dup())
                        backingClass.getField("rangeStart").setLong(it, start)
                        backingClass.getField("size").setLong(it, size)
                    }
                    java.lang.reflect.Array.set(backings, 0, backing(window.fd, WINDOW_PA, WINDOW_SIZE.toLong()))
                    java.lang.reflect.Array.set(backings, 1, backing(window.heapFd, HEAP_PA, HEAP_SIZE.toLong()))
                    rawConfig.javaClass.getField("customMemoryBackingFiles").set(rawConfig, backings)
                    invokeBinderMethod(method, original, args)
                }
                else -> invokeBinderMethod(method, original, args)
            }
        }
        binderField.set(service, proxy)
        return AutoCloseable {
            runCatching { binderField.set(service, original) }
        }
    }

    private fun invokeBinderMethod(
        method: java.lang.reflect.Method,
        target: Any,
        args: Array<out Any?>?,
    ): Any? = try {
        method.invoke(target, *(args ?: emptyArray()))
    } catch (e: InvocationTargetException) {
        throw e.targetException
    }

    fun gpuBuilder(): Any {
        val cls = Class.forName("android.system.virtualmachine.VirtualMachineCustomImageConfig\$GpuConfig\$Builder")
        val g = cls.getDeclaredConstructor().newInstance()
        fun call(name: String, arg: Any) {
            val m = cls.methods.first { it.name == name && it.parameterTypes.size == 1 }
            m.invoke(g, arg)
        }
        call("setBackend", "gfxstream")
        call("setContextTypes", arrayOf("gfxstream-vulkan"))
        call("setRendererUseVulkan", true)
        call("setRendererUseEgl", false)
        call("setRendererUseGles", false)
        call("setRendererUseSurfaceless", true)
        return cls.getMethod("build").invoke(g)!!
    }

    /** The phone's screen in pixels, landscape. */
    fun nativeScreen(): Pair<Int, Int> {
        val m = android.content.res.Resources.getSystem().displayMetrics
        return maxOf(m.widthPixels, m.heightPixels) to minOf(m.widthPixels, m.heightPixels)
    }

    class SharedWindow(
        val memory: SharedMemory,
        val buffer: ByteBuffer,
        val fd: ParcelFileDescriptor,
        val link: String,
        val heap: SharedMemory,
        val heapFd: ParcelFileDescriptor,
    ) {
        fun close() {
            FileShare.stop()   // its threads read the window
            GuestAudio.stop()  // so do its streams
            runCatching { SharedMemory.unmap(buffer) }
            runCatching { fd.close() }
            runCatching { memory.close() }
            runCatching { heapFd.close() }
            runCatching { heap.close() }
        }
    }
}
