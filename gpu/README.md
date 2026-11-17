# HyperWin GPU over shared RAM

SweetCider's gfxstream bridge, moved from vsock into the shared window and pointed at Windows.

```
Windows app ── vulkan-1.dll ── vulkan_hwgpu.dll (Mesa gfxstream encoder, windows.patch)
                                   │  sgpu requests (include/sgpu_proto.h)
                                   ▼
             channel in the shared window (include/hwgpu_ring.h, offset 0x6000000)
                                   │
Android app ── libhwgpu.so (host/hwgpu_host.cpp) ── libgfxstream_backend.so ── phone Vulkan driver
```

The wire format is SweetCider's sgpu, unchanged (`sgpu_proto.h` is a copy of
`sweetcider-gpu/tools/gfxstream-android/sgpu/sgpu_proto.h`). The host server is SweetCider's
`sgpu_server.cpp` with the socket swapped for ring channels. The phone-side renderer is the
`libgfxstream_backend.so` that `sweetcider-gpu/tools/gfxstream-android/build.sh` builds. The guest
driver is the Mesa revision that `sweetcider-gpu/tools/gfxstream-guest/build.sh` builds, with
SweetCider's sync hooks and a Windows backend instead of the Darwin one.

Because the renderer runs in the app, the app picks the Vulkan driver it renders on. Changing it
does not touch Windows.

## The ring

Eight channels, one per Windows process that opens the GPU. Each channel is a control page, a 2 MiB
guest-to-phone ring and a 1 MiB phone-to-guest ring, 24 MiB in all. It sits above the largest frame
pair and below the guest debug marks. No doorbell crosses the VM, so both sides poll with backoff.
The phone thread sleeps 2 ms at a time while no channel is open.

Each channel may only use its own context id and resource id range. The phone refuses the rest, so
one process cannot free another's objects by guessing ids. Finished fences are written into the
control page rather than sent as messages.

Mapped Vulkan memory is still a guest copy kept in step at queue submits and waits, as in
SweetCider. True zero copy means gfxstream allocating that memory inside the window. That is the
next performance step and is not done.

## Phone-side Vulkan drivers (Mali fixes)

gfxstream opens whatever library `ANDROID_EMU_VK_LOADER_PATH` names. The app points it at a wrapper
driver in `host/icd`, which loads the real driver underneath: `HWGPU_VK_DRIVER` names a driver `.so`
(Turnip, other custom builds, Android HAL modules), otherwise the phone's own `libvulkan.so`.
gfxstream loads its driver once per process, so switching drivers means a new renderer process.

`icd/wrap.cpp` is the shared core: handles pass through, and a fix module replaces only the calls
it needs. `icd/passthrough.cpp` is the empty module. On the Pixel 6 Pro, `ring_test` passes through
it the same as without it.

`HWGPU_VK_LAYERS` puts Vulkan layers between the wrapper and the driver, top first (a layer's
`.json` manifest or its `.so`). The core links them as a loader would, so stock layers run as is.

Fix modules for the Mali driver's gaps with DXVK and VKD3D:

- **`libvulkan_bcn.so`** (built). BC textures only: decoded on the GPU by
  [bcn_layer](https://github.com/Pipetto-crypto/bcn_layer) (MIT, Granite compute shaders), which
  it loads from beside itself. GameNative ships the same layer, prebuilt, on top of its own closed
  wrapper driver; nothing else here comes from GameNative. The module turns `vkCmdCopyBufferToImage2` into the plain copy, because the layer
  only watches the plain one and DXVK uses copy2. `icd/bcn_layer.patch` fixes one layer bug: the
  features it strips at device creation pointed at a variable that had gone out of scope, so
  device creation failed. `bctest` on the Pixel 6 Pro (Mali-G78): the phone's driver reports no
  BC; through this wrapper, a BC1 block reads back as the right colour with both copy calls.
  Without the copy2 hook, the copy2 upload reads back the raw block bytes.
- **`libvulkan_vortek.so`** (built). Vortek's renderer's Mali workarounds (Winlator, LGPL-2.1),
  as driver hooks instead of behind Vortek's transport:
  - features DXVK wants are reported, then dropped at device creation where the driver lacks them;
  - heaps are reported at most 2/3 of the largest;
  - scaled vertex formats are reported usable, and pipelines read them as integers;
  - extensions are hidden per engine and driver, as Vortek does;
  - vkd3d gets Vortek's subgroup and texel alignment answers.

  Vortek's own SPIR-V passes (`shader_inspector.c`) are built in unchanged, so this library is
  LGPL-2.1. Vertex and fragment modules wait as code until a pipeline needs them, and are built
  per pipeline. Graphics pipeline libraries, shader objects and module identifiers are hidden,
  since they would reach the driver without passing the inspector. BC textures go to bcn_layer, as
  in `libvulkan_bcn.so`: Vortek's CPU decoder only covers BC1 to BC5. On the Pixel 6 Pro,
  `bctest` and `ring_test` pass through it. `scaledtest` draws a triangle from SSCALED vertices
  through it: Vortek's rewrite of the input and the vertex shader runs, and all 16 pixels come
  out right.

GameNative's wrapper also fakes or hides several extensions on Mali (for example maintenance5,
push_descriptor, robustness2 faked; extended_dynamic_state 1 and 2 and line_rasterization hidden).
That binary has no source, and those changes need real games to check, so none are copied yet.

## Two Windows variants

**Direct** (`guest/direct`, built). `hwgpu.sys` is a small non-PnP driver. It does not make a GPU
appear in Windows. A process opens `\\.\HwGpu`, sends one ioctl, and gets its own channel mapped
into its address space. After that every Vulkan call is plain memory writes with no system call.
Closing the handle, or the process dying, frees the channel. Vulkan only: D3D and OpenGL apps see
no adapter, and anything that checks for a real display adapter will refuse.

**WDDM** (not started). A render-only WDDM adapter, so Windows, DXGI and Device Manager see a GPU.
The same `vulkan_hwgpu.dll` would be the adapter's Vulkan driver, with `HwgpuConnection::callLocked`
going through `D3DKMTEscape` into the kernel driver, which owns the channel. That costs a kernel
round trip per request, so it is slower than direct. Needed pieces:

1. WDDM kernel headers (`d3dkmddi.h`, `dispmprt.h`). The mingw-w64 tree here has none. The WDK
   headers come from Microsoft's WDK NuGet package, which needs your OK on its license.
2. A render-only KMD on Microsoft's sample's model: adapter start, one node, allocations as host
   resources, `DxgkDdiEscape` forwarding to the channel, fences from the control page.
3. The adapter's registry values naming `vulkan_hwgpu.dll` as its Vulkan driver.
4. D3D later: a D3D UMD, or DXVK and vkd3d-proton in each game on top of the Vulkan driver.

## Build

```
gpu/host/test-mac.sh          # ring server + gfxstream on MoltenVK, prints PASS
gpu/host/build-android.sh     # gpu/work/android/arm64-v8a/{libhwgpu,libgfxstream_backend,libc++_shared}.so
gpu/guest/direct/build.sh     # gpu/work/direct/hwgpu.sys, test-signed
gpu/guest/mesa/build.sh       # gpu/work/out/{vulkan_hwgpu.dll,vulkan_hwgpu.json,vktest.exe}
```

## Not wired up yet

The app does not load `libhwgpu.so`. It needs the three `.so` files in jniLibs, then, after
`hw_window_init` and before the VM boots:

```kotlin
object HwGpu {
    init { System.loadLibrary("hwgpu") }
    // flags: STREAM_RENDERER_FLAGS_USE_VK_BIT (0x8) | STREAM_RENDERER_FLAGS_USE_SURFACELESS_BIT (0x20)
    @JvmStatic external fun nativeStart(window: java.nio.ByteBuffer, flags: Int, width: Int, height: Int): Int
    @JvmStatic external fun nativeStop()
}
```

In Windows, as administrator:

```
copy hwgpu.sys C:\Windows\System32\drivers\
sc create hwgpu type= kernel start= auto binPath= System32\drivers\hwgpu.sys
sc start hwgpu
reg add HKLM\SOFTWARE\Khronos\Vulkan\Drivers /v C:\hwgpu\vulkan_hwgpu.json /t REG_DWORD /d 0
C:\hwgpu\vktest.exe C:\hwgpu\vulkan_hwgpu.dll
```

Nothing here has run inside Windows or on the phone yet.
