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

The wire format is SweetCider's sgpu plus one reply, `SGPU_RESP_OK_MAP_HEAP` (`sgpu_proto.h` began as a copy of
`sweetcider-gpu/tools/gfxstream-android/sgpu/sgpu_proto.h`). The host server is SweetCider's
`sgpu_server.cpp` with the socket swapped for ring channels. The phone-side renderer is gfxstream
with SweetCider's Android changes and HyperWin's heap hook (`host/gfxstream.patch`, built by
`host/build-gfxstream.sh`). The guest
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

Mapped Vulkan memory is shared, not copied. The app gives the VM a second region, the GPU heap
(512 MiB right below the window, `HW_HEAP_PA`; the firmware keeps it from Windows). gfxstream
allocates host-visible memory from it (`VulkanAllocateHostMemory`, imported into the phone's driver
with `VK_EXT_external_memory_host`, through the allocator hook in `host/gfxstream.patch`). When a
Windows process maps such memory, the server grants that range to its channel (`grant[]` in the
control page) and hwgpu.sys maps it into the process (`IOCTL_HWGPU_MAP`), so both sides see the same
bytes. Only memory that does not fit, or that the driver cannot import (dedicated images), is still a
guest copy kept in step at queue submits and waits, as in SweetCider. A renderer that starts under a
running Windows keeps the ranges old processes still map out of use until they let go.

Round trips. gfxstream's commands reach the phone as pipe writes and its answers come back as pipe
reads, so a program waits on a read for every Vulkan call that returns something. Three things keep
those waits short: pipe writes carry `SGPU_FLAG_NO_REPLY` and are not answered; a read the phone
cannot answer yet is retried on a real-time schedule (50 us doubling to 250 us; `Sleep()` would round
each wait up to the 15.6 ms system tick); and the phone's server loop only yields for 200 us after a
request before it sleeps, with timer slack off. `HWGPU_STATS=<file>` in a program's environment
writes, every 2 s, how its round trips were spent.

On the Pixel 6 Pro, before and after the shared heap and the round-trip changes: the AIO Vulkan cube
went from 58 to 312 FPS, and its DXVK D3D11 cube from 13 to 104 FPS (Windows' own software renderer:
282). Measure with the phone cool: once it throttles, the same cube drops to half.

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

## Frames to the screen

A fullscreen Vulkan program's frames never come back to the CPU (`SGPU_CMD_SCANOUT`). In that mode
the guest WSI copies each finished swapchain image on the GPU into one of its own scanout images,
whose memory is a gfxstream ColorBuffer. On the phone that memory is an `AHardwareBuffer`.
`hwgpu_host` takes the buffer (`stream_renderer_resource_get_ahb`, in `host/gfxstream.patch`) and
gives it to SurfaceFlinger with `ASurfaceTransaction_setBuffer`, on a layer just above the
SurfaceView that shows the CPU-drawn desktop. Frames go to the layer one at a time. A frame that
arrives while the last is still on its way replaces any frame already waiting, like mailbox. When
the screen lets go of a buffer, the host writes its count into `hwgpu_ctl.scanout_free`. The guest
draws into that buffer only after that.

- The layer covers the whole screen, so a program gets it only while its window fills the screen,
  has the focus, and no other visible window is above it on that screen. Otherwise frames go into the
  window, where Windows draws other windows over them. While Windows' own desktop is on the layer
  (the HyperWin screen, `wddm/README.md` phase 6) the phone sets `HWGPU_SCANOUT_DESKTOP` in each
  channel's `scanout_flags` and refuses program frames, so every program draws into its window.
- The app process owns the layer (`GuestDisplay`) and lends it to each renderer process in the
  start message. When a renderer dies the app hides the layer. The next renderer shows it again
  with its first frame, and the desktop underneath stays on screen throughout.
- ColorBuffers must be RGBA. The phone's driver cannot make BGRA `AHardwareBuffer` images. The
  guest's copy swaps the channels of BGRA swapchains.
- Scanout needs the driver stack to export ColorBuffers
  (`VK_ANDROID_external_memory_android_hardware_buffer`). The renderer tries one at start. The
  hello reply carries `SGPU_HELLO_SCANOUT` only when that worked. Otherwise the guest uses the GDI
  path.
- Windowed programs, and everything DWM draws, still reach the screen through the CPU. Basic
  Display writes the framebuffer, and `nativePresent` copies only the rows that changed into the
  SurfaceView in one NEON pass. `SGPU_CMD_PRESENT` stays for software-drawn frames only.

## Build

```
gpu/host/test-mac.sh          # ring server + gfxstream on MoltenVK, prints PASS
gpu/host/build-gfxstream.sh   # gpu/work/gfx-out: libgfxstream_backend.so with host/gfxstream.patch
gpu/host/build-android.sh     # gpu/work/android/arm64-v8a/{libhwgpu,libgfxstream_backend,libc++_shared}.so
gpu/guest/direct/build.sh     # gpu/work/direct/hwgpu.sys, test-signed
gpu/guest/mesa/build.sh       # gpu/work/out/{vulkan_hwgpu.dll,vulkan_hwgpu.json,vktest.exe,vktest-x64.exe}
gpu/guest/dx/build.sh         # gpu/work/out/dx/{dxvk,vkd3d,zink}
gpu/guest/dx/test/build.sh    # gpu/work/out/dx/test/{dxtest,gltest}-{arm64,x64}.exe
gpu/guest/loader/build.sh     # gpu/work/out/vulkan-1.dll, the Khronos loader (Windows has none)
```

## In the app

`app/build.gradle.kts` packs `gpu/work/android/arm64-v8a` into the APK and runs `host/build-android.sh`
first when `libhwgpu.so` is missing; that builds gfxstream itself when needed. The renderer runs in its own
process, `GpuService` (`:gpu`, `HwGpu.kt`), which maps the shared window. The app starts it before
every VM boot and stops it when the VM exits. A build without the libraries still runs Windows,
without a GPU.

Because the driver lives in that process, it can change under a running Windows: the app ends the
process and starts another. A new renderer marks the channels the old one served as dropped, so
programs using the GPU then get errors and must start again; programs started after get the new
driver. The same happens when the driver crashes the renderer: the app starts another at once.

Settings, Graphics, is the Vulkan stack, top first. Fixes (`Vortek`, `BC textures`) are wrappers
that can be switched on, stacked and dragged into order; under them is exactly one driver, the
phone's own or one added from a file (a `.so`, or a `.zip` in the AdrenoTools layout such as Turnip
or PanVK builds). Added drivers have a remove button. The stack reaches the renderer as
`ANDROID_EMU_VK_LOADER_PATH` (the top wrapper), `HWGPU_VK_CHAIN` (the wrappers, top first: each
opens the next) and `HWGPU_VK_DRIVER` (the driver under the last one). `bcn_layer` runs once per
stack, in BC textures when it is on, else in Vortek. Drivers built as Android HAL modules link to
`libhardware.so`, which apps cannot load, so the app carries a stand-in (`icd/libhardware_stub.c`).

gfxstream looks up some instance and device calls once with no instance, as from a loader, and
calls them for every handle. Wrappers answer those with stubs that find the right chain from the
first handle (`wrap.cpp`, `any_handle`); without them gfxstream crashed destroying instances.

Windows gets the guest side from the machine assets: `tools/machine_assets.sh` builds `hwgpu.sys`
and `vulkan_hwgpu.dll` when missing and stages them, with `vulkan_hwgpu.json` and `vktest.exe`, in
`machine/guest/gpu`, which machine prep copies to `C:\HyperWin\gpu`. `firstboot.cmd` runs
`gpu-install.cmd`: it loads `hwgpu.sys` as an auto-start kernel service and registers the manifest
under `HKLM\SOFTWARE\Khronos\Vulkan\Drivers`. On machines made before this, or by an older app,
the app compares `C:\HyperWin\gpu\stamp.txt` with its own files when the guest agent connects,
and sends and installs them when they differ.

Check in Windows: `C:\HyperWin\gpu\vktest.exe C:\HyperWin\gpu\vulkan_hwgpu.dll`.

Windows apps reach the driver through `vulkan-1.dll` (`guest/loader/build.sh`), which
`gpu-install.cmd` puts in System32 when Windows has none, and updates when the copy there is ours.

Every Windows DLL here is ARM64X (`guest/arm64x`): one file with ARM64 code for ARM64 programs and
ARM64EC code for x64 programs under emulation, so both run natively against the same driver and
loader. 32-bit x86 programs have their own plain i686 build of both: `vulkan_hwgpu_x86.dll` (linked
with `--kill-at`, or the loader cannot find `vk_icdGetInstanceProcAddr`) found through the adapter's
`VulkanDriverNameWow`, and `vulkan-1_x86.dll`, which `gpu-install.cmd` puts in SysWOW64. ARM64EC claims `__x86_64__`, so windows.patch also
keeps Mesa off its x86 paths there, and gives Mesa's optional entry points `/alternatename`
fallbacks in the ARM64EC half, since lld resolves weak symbols there to NULL.

Direct3D and OpenGL go over Vulkan, per program (`guest/dx/build.sh`, in `C:\HyperWin\gpu\dx`; Zink also as `zink-x86` for 32-bit programs):
DXVK 3.1.1 (Direct3D 8 to 11), DXVK-Sarek 1.14.1 (the same, DXVK 1.10 carried on for Vulkan 1.1 and
mobile GPUs; it runs on Mali without Vortek, though slower here: the D3D11 cube at 26 FPS bare, 34 with
Vortek, against DXVK's 50), vkd3d-proton 3.1 (Direct3D 12, with DXVK's `dxgi.dll`) and Mesa Zink
(OpenGL). `use.cmd dxvk|sarek|vkd3d|zink|off <program>` copies them beside a program or takes them away;
Explorer's Send To menu has the same five. Windows' own DLLs are never replaced. `dx\test` has
`dxtest` (Direct3D 11 and 12: clear, read back) and `gltest` (OpenGL), as ARM64 and x64.

On Mali, DXVK and vkd3d-proton need the Vortek fix: it reports features they insist on and the
driver lacks (`fillModeNonSolid`, `robustBufferAccess2`, a zero vertex divisor, which pipelines get
as the largest divisor instead, `geometryShader` for PanVK), drops them again at device creation,
and offers `VK_KHR_vertex_attribute_divisor` under its old EXT name for vkd3d-proton. Zink gets the
driver as it is. Windows frames reach the app's window by
Mesa's win32 WSI in software mode: the guest has no DXGI device, so presents are GDI blits, and
every present mode is offered since a blit shows at once (windows.patch).

Verified on the Pixel 6 Pro, 2026-11-17: `vktest` PASS on "Virtio-GPU GFXStream (Mali-G78)", and
the AIO Graphics Test's native Vulkan cube (`--cube vk`) spins at about 58 FPS. With Vortek over the
phone's driver, `dxtest 11`, `dxtest 12` and `gltest` PASS as ARM64 and as x64 programs. PanVK
v3.7.0 (FristOneRR, added from its zip) runs `vktest`; with Vortek over it `dxtest 11` passes, and
vkd3d-proton refuses it for lacking transform feedback.
