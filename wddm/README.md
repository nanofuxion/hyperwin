# One real GPU in Windows

Windows sees the phone's GPU as a real graphics adapter, with a proper WDDM
driver. Every program gets Vulkan, Direct3D and OpenGL from it with nothing
copied beside the program. The desktop is drawn on the GPU too.

Three things sit on the one adapter:

| API | Windows side | Wire | Phone side |
|---|---|---|---|
| Vulkan | `vulkan_hwgpu.dll`, unchanged | sgpu | gfxstream |
| Direct3D 10 and 11 | HwD3D as the driver's Direct3D part (`d3d/README.md`) | HwD3D requests | DXVK built for Android |
| OpenGL | Mesa's GL with a thin pipe driver (`opengl/README.md`) | pipe requests | Mesa's Zink built for Android, or the phone's GLES through VirGL, as the user picks |

gfxstream stays, but only for Vulkan. Direct3D and OpenGL skip it.

All three use the same driver stack on the phone: the wrappers and the one
driver from Settings, Graphics. So driver swaps keep working.

```mermaid
flowchart LR
  subgraph Windows
    A[Vulkan program] --> VK[vulkan_hwgpu.dll]
    B[Direct3D program] --> RT[Microsoft d3d11.dll] --> UMD[HwD3D driver part]
    C[OpenGL program] --> GL[Microsoft opengl32.dll] --> ICD[Mesa GL + hwpipe]
    KMD[hwgpu adapter<br/>kernel driver]
  end
  VK & UMD & ICD -->|plain memory writes| R[(ring channels<br/>GPU heap)]
  KMD -.->|maps channels, fences, resets| R
  R --> GS[gfxstream] & DX[DXVK] & ZK[Zink]
  GS & DX & ZK --> S[wrappers + chosen driver] --> M[Mali]
```

## Where it stands

- `hwgpu.sys` is a small non-PnP driver. Windows doesn't see a GPU. Programs
  open `\\.\HwGpu` and get a ring channel mapped into them.
- Vulkan finds our driver through the global Khronos key.
- Direct3D and OpenGL only work per program, through DXVK, vkd3d-proton or
  Zink copied beside it. 32-bit programs get nothing.
- Games that check for a real GPU see only Microsoft's Basic Render Driver,
  and some refuse to start.
- The desktop is drawn by the CPU and reaches the phone through Basic
  Display's framebuffer.

## The kernel driver

A WDDM miniport that replaces `hwgpu.sys`. It keeps the name `hwgpu`.

### Device and install

- A root device, `ROOT\HwGpuAdapter`, like Microsoft's own Basic Render
  Driver, which is also root-enumerated.
- A display-class INF, installed by `install-driver.ps1` with a catalog
  signed in Windows, like `hwaudio` and `hwhid`. Test signing is already on in
  the BCD, so the test certificate is enough.
- The adapter's registry values point Windows at our parts:
  `VulkanDriverName`, `UserModeDriverName` for Direct3D, and
  `OpenGLDriverName`, each with a `Wow` twin for 32-bit programs.
- The adapter's name says what the phone has, for example
  "HyperWin GPU (Mali-G78)".

### The fast path stays

The old note in `gpu/README.md` says WDDM costs a kernel trip per request. It
doesn't have to:

- A program's user-mode part asks the kernel driver for a channel once,
  through `D3DKMTEscape`. The driver maps the channel and its heap ranges into
  the process, as `IOCTL_HWGPU_OPEN` and `IOCTL_HWGPU_MAP` do today.
- After that, every request is a plain memory write, as now.
- `vulkan_hwgpu.dll` opens its channel through the escape when the adapter
  exists, and through `\\.\HwGpu` otherwise.

### What Windows' scheduler sees

Windows wants to schedule GPU work, track fences, and catch a GPU that hangs.
The real work never goes through it. Instead:

- A user-mode part submits a tiny marker through Windows when it needs
  Windows to know about progress: at present, at a shared-resource handoff, at
  a flush with a fence. The marker holds the channel's stream position.
- The host writes how far it has got into the channel's control page.
- The kernel driver polls that page with a high-resolution timer while
  markers are pending, and reports them done with `DxgkCbNotifyInterrupt`.
  The timer stops when nothing is pending.

So Windows' fences, `Present` throttling and cross-process sync work, with no
cost per draw.

### Memory

Allocations are host objects. Windows only needs the CPU-visible ones to have
memory:

- One aperture segment over the 512 MiB GPU heap, for allocations the CPU maps:
  upload, readback, dynamic buffers.
- One large "video memory" segment with no backing in Windows, for everything
  that only the GPU touches. Paging operations on it become no-ops or host
  copies.

### Shared resources and DWM

A windowed program's swap chain image is read by DWM, which is another
process. On the phone, each channel has its own state. Shared allocations are
the exception: the host keeps a global table of them, as Android hardware
buffers each side can import. Windows' own sync for shared resources rides
on the markers above.

### Driver swaps

A swap restarts the `:gpu` process, and every host object is gone. The kernel
driver sees the new renderer in the control page and forces a GPU reset.
Windows then does what it does after a driver crash on a PC: DWM rebuilds
itself, and programs get `DXGI_ERROR_DEVICE_REMOVED` or
`VK_ERROR_DEVICE_LOST`. Programs started afterwards get the new driver.

The swap has to finish inside Windows' reset timeout, about 2 seconds. Raise
`TdrDelay` if a driver takes longer to start.

### The display, later

The first driver is render-only. Basic Display keeps the screen.

Then it becomes a full display and render driver:

- One screen whose modes are the phone's refresh rates (`vsync/README.md`).
- The desktop's primary images are host buffers. A flip
  (`DxgkDdiSetVidPnSourceAddress`) hands the buffer to the phone's screen with
  `ASurfaceTransaction_setBuffer`, like scanout today. Nothing comes back
  through the CPU.
- Vsync interrupts come from the phone's vsync timeline.
- `DxgkCbAcquirePostDisplayOwnership` takes over the GOP framebuffer, so boot
  looks the same.

This replaces the display-only driver in `vsync/README.md` section 4. Basic
Display and the CPU copy in `nativePresent` then only serve boot and the
recovery screen.

## Each API

**Vulkan.** Same ICD, same gfxstream, same speed. Only how it's found and how
it opens a channel changes.

**Direct3D 10 and 11.** Microsoft's `d3d11.dll` and `dxgi.dll` load our
user-mode driver. The driver interface is close to the API, so HwD3D's
requests barely change. Before this phase, HwD3D runs per program as thin
DLLs, which needs no WDK (`d3d/README.md`).

**Direct3D 9, 8, 7 and DirectDraw.** Microsoft's runtime talks to a different,
lower-level driver interface for these. Keep HwD3D's per-program DLLs for them
first. A Direct3D 9 driver part comes later, if it proves worth it.

**Direct3D 12.** Not in this plan. vkd3d-proton per program keeps working.

**OpenGL.** Microsoft's `opengl32.dll` loads our ICD from `OpenGLDriverName`.
It's the same build as the per-program one in `opengl/README.md`, with Mesa's
ICD entry points.

## Headers and the WDK

- The kernel driver only needs `d3dkmddi.h`, `dispmprt.h` and their friends.
  `gpu/work/reactos-ddk` already has them. **No WDK.**
- The Direct3D driver part needs `d3d10umddi.h`, which only ships in the WDK.
  That phase waits for your OK on the WDK license.
- HwD3D's per-program DLLs and the OpenGL ICD need neither.

## Prior art to read first

- Microsoft's Basic Render Driver: root-enumerated and render-only, the same
  shape as phase 1.
- virtio-win's `viogpu` drivers, and their 3D work for virtio-gpu with Mesa.
  A paravirtual GPU with a WDDM kernel driver is the same problem. Check how
  far that work got and what license it has.
- Mesa's `d3d10umd` frontend, a Direct3D 10 driver part on Gallium. It isn't
  used here, since it stops at Direct3D 10, but it shows the driver interface.

## Phases

1. **Render-only adapter, Vulkan only.** No WDK. The device, INF, escape
   channel mapping, markers and fences, the reset path. Vulkan found through
   `VulkanDriverName`. Test: Device Manager and `dxdiag` show the adapter,
   `vktest` passes with the Khronos key removed, and a driver swap mid-run
   resets cleanly.
2. **HwD3D per program** (`d3d/README.md` phases 0 to 3). Runs alongside.
3. **OpenGL on the phone's Zink, per program** (`opengl/README.md`). Runs
   alongside.
4. **The Direct3D 11 driver part.** Waits on the WDK. Test: `dxtest 11` with
   nothing copied beside it, DXGI lists a hardware adapter, and a game that
   refused Basic Render now starts.
5. **The OpenGL ICD on the adapter.** Test: `gltest` with nothing beside it,
   as ARM64, x64 and x86.
6. **Display and render.** Modes, flips, vsync, GOP takeover. Test: DWM draws
   on the GPU, the host's CPU use on a still desktop is near zero, and boot
   and recovery still show.
7. **Direct3D 9 driver part,** decided after HwD3D's per-program DLLs have
   been used for a while.

## Things to check early

- Which DXGK interface version Windows 11 on ARM accepts from a render-only
  driver, and what it requires of WDDM 2.0 memory handling.
- Whether a render-only adapter with no Direct3D part yet starts at all. If
  Windows insists on `UserModeDriverName` working, phase 1 needs a stub.
- How a kernel driver forces a reset by itself. Waiting for the timeout always
  works, but it's slow.
- Whether DWM composes on a render-only adapter while Basic Display shows the
  screen, and what that costs before phase 6.
- Whether anti-cheat that refuses test signing matters for the games you
  want. If so, the old direct path stays available beside the adapter.
