# DirectX recovery and performance plan · 23 November 2026

## Starting point after the Windows reinstall

The fresh guest's DxDiag report, saved at `C:\HyperWin\dxdiag.txt`, lists HyperWin GPU (Mali-G78) as a render-only WDDM 1.3 adapter with DDI 11.2 and `hwd3d_umd.dll` 1.0.61325.19667. Windows also lists HyperWin screen using `hwidd.dll` 1.0.7505.56559 and Microsoft Basic Display Driver. The installed HyperWin GPU and screen package versions match the app bundle. These are adapter and driver entries. The number of active monitors and which adapter owns each active desktop path still need a live Windows display topology query.

AIO 2.2.0 drew an animated D3D11 cube after reinstall. Its Vulkan inventory found Mali-G78. This establishes hardware availability. The test's specific D3D11 device and backend still need to be correlated. Its roughly 27 FPS observation has no recorded uncapped setting. The earlier full-desktop blackout occurred when Borderlands began rendering on the previous install. Its cause and its reproducibility on the fresh guest are open.

## Overnight results on the previous install

| Work | Evidence and remaining work |
|---|---|
| Full AIO GUI, D3D11, windowed | 700–745 FPS uncapped with 3 GB guest RAM. Vsync on gave 28–30 FPS. DXGI waited about 30 ms for DWM. Retest both modes on the new guest. |
| Windowed vsync | An experimental driver setting produced DWM acknowledgements and 31–55 FPS, but window bitmap pixels were unavailable. A proposed Render DDI experiment is unbuilt. Correct desktop pixels and pacing at the phone's selected refresh rate remain open. |
| D3D9 query waits | Borderlands previously spent 190–290 ms per frame waiting on a query at 3–4 FPS. The host query gate was built and exercised with qtest9, which did not reproduce that stall. Verify the running gate on/off and measure Borderlands itself. |
| Fullscreen and black desktop | An earlier Android buffer layout/copy fix was reported working on Mali. A later launch blacked out the whole desktop. Recheck desktop, AIO, then Borderlands on this install, including recovery after exit. |
| Windowed cube | Its separate FPS window blocks the fast layer. A debug override hides the HUD. The proposed cover layers that preserve window order are design only. |
| Frames in flight and copies | Guest and host counter code compiled behind HWD3D_FIF, default off. The rebuilt APK was installed before the Windows reinstall. Verify guest DLL identity before testing. The roughly 1.4 ms overlay copy still needs a measured improvement. |

Historical measurements and switch details are in [the speed plan](README.md#speed-plan-state-of-play-2026-11-23-written-for-whoever-reads-this-first). Treat the older HwD3D bring-up phase numbers separately from the speed phases below.

## Execution order and acceptance

1. **Recover the baseline (speed phase 0).** Inventory the running APK, guest DLL hashes, driver packages, switches, resolution, 3 GB RAM setting and Windows active display paths (source adapter LUID, target, mode and primary display). Record the visible desktop. Run the full AIO GUI D3D11 test with vsync on and off. Correlate the AIO PID with its D3D11 adapter/UMD, host game channel, physical GPU and successful draws/presents. Check for WARP or other fallback. Record the loaded guest D3D/DXGI module paths. Repeat for Borderlands D3D9 windowed, then fullscreen. Check the menu, Alt-Tab and desktop after exit. If the desktop goes black, capture the last good and first bad frames, shared desktop frame, layer state and device-lost logs before changing anything. Recover visible output before recording performance.
2. **Finish the phase 0 comparison.** Use the same Pixel, game build, scene, resolution, settings and vsync off in Winlator and HyperWin. Record frame times, FPS, query waits, CPU/GPU work, present cost, clock, memory and thermal state. Obtain the missing fullscreen samples. The first justified optimization is the D3D9 query wait. Retest gate off/on under matched conditions.
3. **Speed phase 1 — present without a CPU wait.** Preserve clipping, HUDs and overlapping windows. Test cover layers, frames in flight, direct rendering to the window layer, and exported completion fences with fallback. Measure copy cost and check frame integrity, pacing and desktop recovery. Track windowed vsync separately through a correct 60/120 Hz test.
4. **Speed phase 2 — fewer synchronous replies.** Test guest-assigned handles, cached capabilities, guest-side writes for writable D3D9 locks, and single-write replies. Preserve exact answers for reads and failure handling. Measure waits per frame and their cost.
5. **Speed phase 3 — lower ring overhead.** Test direct batched writes, per-context streams, wakeups and game-thread placement. Check ordering at flush/present and before waits. Measure lock time, ring time and frame time against the old path.
6. **Speed phase 4 — routing and game profiles.** Verify desktop sharing on the stock driver and per-game isolation, backend selection, shader cache identity and invalidation. Test Edge/shared surfaces as well as games.
7. **Speed phase 5 — Android-side D3D12.** Exercise the Windows driver path into vkd3d on Android with a real D3D12 workload and prove the loaded module paths. Validate visible frames, correctness and before/after performance.

Phases 2–5 have no recorded end-to-end Pixel acceptance in the overnight speed report. Inventory partial implementations before editing. Phase 0 evidence may reorder speed phases 1–3. For each phase keep the old path selectable, record the change, test conditions, before/after numbers and discrepancies in d3d/README.md, report to the side thread and wait for its reply. The main thread owns implementation and Pixel testing. Install builds only with user authorization in that thread. Keep DXVK and vkd3d on Android and preserve normal Windows/game code in releases. Completion requires all six tested phases, the documented matched Winlator comparison, and Borderlands reaching at least 30 FPS with visible correct output.
