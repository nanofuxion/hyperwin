# Native Direct3D over AVF gfxstream: research decision

2026-11-15. Research only; no boot/device changes. Main thread history and current guest code inspected.

## Finding

The main thread selected a Vulkan ICD but did not establish a native Direct3D driver route. guest/icd/vulkan_gfxstream.c unconditionally fails instance creation. Registering even a working ICD would not make Microsoft's Direct3D runtime use Vulkan automatically.

Recommended architecture: unchanged Microsoft Direct3D/DXGI -> installed HyperWin D3D user-mode driver (DDI) -> Vulkan/gfxstream guest encoder -> ARM64 WDDM kernel driver managing virtio-gpu transport -> AVF gfxstream -> Android GPU driver. This is a real virtual graphics adapter, not physical GPU passthrough. Keep the Vulkan ICD as a second frontend over shared transport.

The KMD must implement allocation ownership, mappings, contexts, submission, fences, scheduling/reset and resource lifetime. The UMD must implement the D3D DDI, shader translation, accurate feature reporting, and resource semantics. Presentation/shared allocations and DWM integration are additional work. Neither an ICD nor a display-only driver supplies this complete stack.

## Reuse candidates

- virtio-win viogpu: transport/display reference. viogpudo uses DxgkInitializeDisplayOnlyDriver; it is not an accelerated D3D UMD. https://github.com/virtio-win/kvm-guest-drivers-windows/tree/master/viogpu
- Microsoft render-only sample: useful KMD/UMD reference, not a production or ready ARM64/gfxstream implementation. https://github.com/microsoft/graphics-driver-samples
- Experimental wddm-mali-g52 reports ARM64 Windows desktop rendering via an adapted Mesa D3D10 frontend, with feature level 10_0 and x86/x64 process support. This is an upstream claim, not independently reproduced here. It targets RK3576 physical hardware, not AVF. Its GPL-2.0 code and Mesa patches need license review before incorporation. https://github.com/gahingwoo/wddm-mali-g52
- Most promising reuse experiment: adapted Mesa D3D frontend -> Gallium Zink -> Vulkan/gfxstream. Zink emits Vulkan from Gallium, but this exact WDDM combination is UNPROVEN. Validate resource sharing, shader semantics, feature requirements, and the frontend's limitations before choosing it. https://docs.mesa3d.org/drivers/zink.html
- Dozen translates Vulkan onto D3D12, the opposite direction. DXVK/vkd3d API replacements are not drop-in WDDM UMDs. Venus uses a distinct protocol and is not a drop-in gfxstream client.

## Independent milestones

1. Keep firmware, Windows boot, and software display work independent. Use a separate test VM/image for driver experiments; never overwrite the main thread's running disk.
2. On the actual target AVF build, establish device transport (PCI or MMIO), capsets, renderer version, Vulkan features, blob/shared-memory and fence support. Configuration strings alone prove none of these. Fix host renderer availability before interpreting guest failures.
3. Port the gfxstream guest transport to Windows ARM64 and prove an offscreen Vulkan render/readback using the actual host GPU.
4. Prototype the Mesa frontend/Zink route and a render-capable WDDM adapter. First acceptance: untouched system d3d11.dll creates the selected non-software adapter, draws an offscreen triangle, and returns correct pixels with host GPU activity. Do not silently fall back to WARP or overclaim feature level.
5. Implement presentation/DWM sharing; initially correctness before zero-copy. Exercise allocation cleanup, multiple processes, fences, resize, reset/TDR and memory pressure.
6. Validate native ARM64 and emulated x86/x64 separately, including architecture-appropriate UMD loading. Kernel driver must be ARM64. Add driver-package signing/conformance for distribution. A correctly installed graphics adapter does not itself guarantee anti-cheat acceptance.

D3D12 requires its own DDI/feature work; do not promise it from the D3D11 prototype. Hyper-V GPU-P is not an Android gfxstream switch: it relies on a compatible virtualization and vendor-driver stack.

## Primary references

https://learn.microsoft.com/en-us/windows-hardware/drivers/display/user-mode-display-drivers
https://learn.microsoft.com/en-us/windows-hardware/drivers/display/initializing-communication-with-the-direct3d-user-mode-display-driver
https://learn.microsoft.com/en-us/windows-hardware/drivers/display/wddm-v1-2-features
https://learn.microsoft.com/en-us/windows/arm/faq
https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/plan/plan-for-gpu-acceleration-in-windows-server
https://github.com/google/gfxstream

Conclusion: WDDM KMD + D3D UMD is the appropriate architecture. No turnkey driver established. Mesa frontend/Zink reuse is the first feasibility experiment, not a completed implementation or guarantee of gaming compatibility.
