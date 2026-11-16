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
    const val WINDOWS_VM_MEMORY_MIB = WINDOWS_MEMORY_MIB + WINDOW_SIZE_MIB
    const val WINDOW_GUARD_BELOW_TOP_MIB = 16
    const val FRAME_WIDTH = 1280
    const val FRAME_HEIGHT = 720
    const val FRAME_BYTES = FRAME_WIDTH * FRAME_HEIGHT * 4
    /** Keep the ring inside UEFI conventional RAM, with room below its top boundary. */
    const val WINDOW_PA = RAM_BASE_PA + (WINDOWS_MEMORY_MIB - WINDOW_SIZE_MIB - WINDOW_GUARD_BELOW_TOP_MIB).toLong() * 1024 * 1024
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
    fun createWindow(seed: ByteArray): SharedWindow {
        val shm = SharedMemory.create("hyperwin", WINDOW_SIZE)
        val map = shm.mapReadWrite().order(ByteOrder.LITTLE_ENDIAN)
        WindowBridge.nativeInit(map, seed)
        val parcel = android.os.Parcel.obtain()
        shm.writeToParcel(parcel, 0)
        parcel.setDataPosition(0)
        val pfd = parcel.readFileDescriptor()
        parcel.recycle()
        val link = runCatching { Os.readlink("/proc/self/fd/${pfd.fd}") }.getOrDefault("?")
        return SharedWindow(shm, map, pfd, link)
    }

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
                    val backings = java.lang.reflect.Array.newInstance(backingClass, 1)
                    val backing = backingClass.getDeclaredConstructor().newInstance()
                    backingClass.getField("file").set(backing, window.fd.dup())
                    backingClass.getField("rangeStart").setLong(backing, WINDOW_PA)
                    backingClass.getField("size").setLong(backing, WINDOW_SIZE.toLong())
                    java.lang.reflect.Array.set(backings, 0, backing)
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

    class SharedWindow(
        val memory: SharedMemory,
        val buffer: ByteBuffer,
        val fd: ParcelFileDescriptor,
        val link: String,
    ) {
        fun close() {
            runCatching { SharedMemory.unmap(buffer) }
            runCatching { fd.close() }
            runCatching { memory.close() }
        }
    }
}
