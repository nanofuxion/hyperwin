# panvk-lab

Tests Mali Vulkan drivers on the Pixel 6 Pro (Mali-G78 MP20, gs101) from the ADB shell only.
No screen, no taps, no app start. The goal is a PanVK build that suits Mali best, with DXVK 3.1.1
(what HyperWin uses) as the measuring stick.

```
lab.sh status                       is the main thread's Windows VM running? how hot is the phone?
lab.sh probe [driver.so] [VAR=1]    what DXVK 3.1.1 needs, checked on the real GPU
lab.sh clean                        remove everything it put on the phone
```

How it reaches the GPU: the plain shell may not open `/dev/mali0`, so the script uses
`run-as dev.hyperwin.app` (the app is debuggable). Binaries run from the app's `cache/panvk-lab`,
which is allowed there and not from `/data/local/tmp`. `status` exits 1 while `crosvm_hyperwin` is
running, so timing runs wait for the main thread to be idle. `probe` is light and can run any time.

## What DXVK 3.1.1 asks of the driver

Read from `gpu/work/dxvk/src/dxvk/dxvk_device_info.cpp` (the `true` rows). Vulkan 1.3 is the
baseline in this source (`DxvkVulkanApiVersion`), although news posts say 3.0 needs 1.4. Hard
requirements beyond 1.3 core: `geometryShader`, `multiViewport`, `shaderClipDistance`,
`shaderCullDistance`, `textureCompressionBC`, `robustBufferAccess2`, `nullDescriptor`,
`depthClipEnable`, `maintenance5`, `maintenance6`, `VK_KHR_load_store_op_none`.

## Findings, 2026-11-18, FristOneRR PanVK 3.7.0 on the Pixel 6 Pro

- Loads and creates a device. Vulkan 1.3.366, 174 extensions, one 2887 MB heap, subgroup 16.
- Reports the GPU as `Mali unknown 0x92020010 MC20`. The public (stale) source lists G78 as
  product 9.0.2, so this Pixel's id is probably missing from its model table. Not yet confirmed
  against 3.7.0, whose source is not public.
- With no environment variables, 5 required features are off: `geometryShader`, `multiViewport`,
  `shaderClipDistance`, `shaderCullDistance`, `textureCompressionBC`. DXVK 3.1.1 refuses to start.
- With `PANVK_DXVK_COMPAT=1`, all required features are on, `VK_EXT_transform_feedback` appears,
  and device creation passes. In the stale source that variable gates exactly those features, and
  its table of documented variables leaves it out. So the shipped default is wrong for DXVK 3.
- Missing either way: `VK_EXT_conservative_rasterization`, `VK_KHR_present_wait`,
  `VK_EXT_fragment_shader_interlock`, `VK_EXT_descriptor_buffer`, `VK_EXT_device_fault`,
  `VK_EXT_swapchain_maintenance1`, `shaderFloat64`, `depthBounds`.
- The driver hides ~150 `FRR_*` / `PANVK_*` switches (see `strings` on the `.so`), among them
  `FRR_GSEMU`, `FRR_XFB`, `FRR_CLIPDIST`, `FRR_BCEMU`, `PANVK_GS_PASSTHROUGH`, `PANVK_KBASE_DVFS`.
  Their meaning needs reading code. Only the ones in the project's README are safe to rely on.

## Where the source stands

- Public source (`FristOneRR-Admin/FristOneRR-Panvk-Source`) stopped at beta 1.2.0 on 2026-09-30.
  Releases reached 3.7.0 on 2026-11-02, so what is on the phone cannot be rebuilt from it.
- The README lists no Pixel. Closest listed: Galaxy S21 (G78 MP14, needs `PANVK_ATOM_STRIDE=56`).
  Valhall CSF parts (G610 and up) are unsupported. The Pixel 6 Pro is Valhall Job Manager.

## Next, not done yet

1. Add a GPU workload to `probe` (compute and draw with correctness readback) so changes can be
   compared, not only listed.
2. Run DXVK's own D3D11 on the phone against PanVK, using `gpu/work/dxvk-android`, and compare
   frame times with the phone's stock driver under the same Vortek wrapper.
3. Decide the source base: rebuild from the stale tree, or move to upstream Mesa with a kbase
   backend. Needs a decision before any driver code is written.
4. Check Pixel kernel `kbase` version and ioctl differences (the Pixel's driver is Google's own
   fork, unlike the Exynos and MediaTek phones on the list).
