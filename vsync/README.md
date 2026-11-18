# The phone's vsync, shared with Windows

Windows knows when the phone's screen refreshes, and how fast. Games, the
desktop and DXVK's frame pacing can then line up with the real screen.

## What the main thread already covers

The scanout work in the main thread
([Windows ARM on AVF Vulkan Bridge](t3-thread://v2/096a142c-7814-413b-b52a-000bcdeb8f57/42976f0c-9077-4959-8f30-d76fd7916f36))
paces **fullscreen Vulkan scanout** with backpressure. For FIFO, or when the
"Limit to screen refresh" switch is on (`HWGPU_SCANOUT_VSYNC` in
`hwgpu_ctl.scanout_flags`), Mesa's WSI won't hand a program a new image while
an older frame still waits for the screen. The phone releases a frame once the
next one is shown, so the program draws one frame per refresh.

That's the right base, and this plan doesn't redo it. What it leaves out:

- **Timing.** Backpressure says "a frame was shown", but not when the next
  refresh comes. A program that waits for a free image starts its frame right
  after a refresh. So it holds up to a full frame of input latency and has no
  slack if a frame runs long.
- **Frame timing APIs.** `VK_KHR_present_id` and `VK_KHR_present_wait` aren't
  offered, so DXVK's frame pacing and latency control fall back to guessing.
- **Everything that isn't fullscreen scanout.** Windowed games, the desktop and
  video go through Basic Display and DWM. Windows thinks the screen runs at a
  fixed rate, and DWM composes on its own timer, so it never lines up with the
  phone's 120 Hz.
- **Matching the game's rate.** A game holding a steady 60 fps on a 120 Hz screen
  could ask the screen for 60 Hz and save power. Uneven rates like 90 fps on
  120 Hz judder.

## What to build

### 1. The vsync timeline in shared RAM

The host gets each refresh from `AChoreographer_postVsyncCallback` (API 33),
which gives the vsync time, the frame interval, and the upcoming frame
timelines (expected present time and deadline). It writes a small block into
the shared window:

| Field            | Meaning                                                   |
|------------------|-----------------------------------------------------------|
| seq              | odd while writing, even when stable                       |
| count            | refreshes since the VM started                            |
| last vsync       | host counter ticks of the last refresh                    |
| period           | ticks per refresh, which follows the display mode         |
| next present     | expected present time of the next frame timeline          |
| next deadline    | latest time a buffer can reach SurfaceFlinger for it      |
| refresh hz × 1000 | for drivers that report a rate                           |

Times are in counter ticks, not nanoseconds, so Windows can compare them with
`KeQueryPerformanceCounter` directly once it knows the offset (see 2).

It goes in the window header page, or the free space at `0xC00000`. A small
looper thread in the app process runs the choreographer callback and keeps the
block current. Choreographer needs a looper, so this can't share the CPPC
thread, which is a plain 5 ms sleep loop.

### 2. One clock on both sides

The guest's counter and the phone's counter are the same hardware counter, but
KVM gives the guest its own offset. Measure it once at start:

1. Windows writes its counter `G1` into a mailbox.
2. The host sees it and writes its own counter `H`.
3. Windows reads `G2` when the answer arrives.
4. Offset ≈ `H − (G1 + G2) / 2`. Repeat about 50 times and keep the round with
   the shortest `G2 − G1`.

Polling shared memory turns around in tens of microseconds, which is far tighter
than a refresh. The offset stays fixed for the VM's life. Measure it again
after a host suspend, in case the counter jumped.

### 3. Better pacing for scanout (on top of the main thread's work)

In Mesa's win32 WSI, scanout mode:

- **Present timing:** in FIFO, block the program's acquire until a little
  before the next deadline, not only until a release comes. The program then
  samples input as late as possible, with a frame's worth of slack. Use the
  last frame's measured render time to choose how early to wake.
- **`VK_KHR_present_id` and `VK_KHR_present_wait`:** each scanout present
  carries the program's id. The host writes "id shown at vsync N" next to the
  release words. `vkWaitForPresentKHR` waits on that. DXVK uses present wait
  for its frame pacing and its max-frame-latency handling.
- **`VK_GOOGLE_display_timing`:** report the refresh period and the actual
  present times. Some engines use this to pace themselves.

Coordinate with whoever owns `wsi_common_win32.cpp` in the main thread.
Release-based FIFO stays the fallback when the timeline block is missing.

### 4. A display driver that knows the real refresh rate

Basic Display can't report a refresh rate or vsync from the phone, and neither
can GOP. Microsoft's display-only WDDM sample, KMDOD (Windows-driver-samples,
MIT), is a Basic Display clone that a driver can build on:

- **Modes:** report the phone's refresh rates (120, 90, 60) in the VidPN target
  modes. Windows, DWM and DXGI then see a 120 Hz monitor.
- **Vsync:** a high-resolution timer in the guest, armed for each "next vsync"
  from the timeline, raises `DXGK_INTERRUPT_DISPLAYONLY_VSYNC`. DWM composes
  on the phone's refresh, and `D3DKMTWaitForVerticalBlankEvent` and DXGI's
  `WaitForVBlank` work for games that use them.
- **Dirty rectangles for free:** `DxgkDdiPresentDisplayOnly` hands the driver
  the changed rectangles and moves for each present. The driver copies just
  those into the shared framebuffer and publishes the list. The host then
  updates just those rows, with no comparing at all. That finishes the desktop
  work from the main thread's single NEON pass.
- **Taking over from GOP:** `DxgkCbAcquirePostDisplayOwnership` gives the driver
  the firmware's framebuffer, which is our GOP buffer in the shared window. So
  boot and the hand-over stay as they are.
- **Headers:** `gpu/work/reactos-ddk` already has `d3dkmddi.h` and
  `dispmprt.h`. KMDOD is C++, so port it the same way as the audio sample.

This is also the first piece of the WDDM work in `gpu/README.md`. A later
render driver can share its mode and vsync code.

### 5. Match the screen to the game

When a scanout program holds a steady rate, set the layer's frame rate to match:
`ASurfaceTransaction_setFrameRateWithChangeStrategy` with
`ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS`. 60 fps on the Pixel's LTPO
panel then runs at 60 Hz, which looks smoother than uneven doubling and uses
less power. Measure the program's rate over about a second before changing,
and go back to the fastest mode when it changes or exits.

## Phases

1. Timeline block and clock offset. Test: a guest tool prints the measured
   refresh times. Their spacing should match the phone's period within 0.1 ms.
2. Present timing, `present_id` and `present_wait` in the WSI. Test: input
   latency in `vkpresent.exe`, measured with a high-speed camera or a
   latency-test pattern, before and after. Check DXVK's frame pacing in a game.
3. Display-only driver: modes, vsync, dirty rectangles. Test: Windows display
   settings shows 120 Hz, DWM's frame rate in a windowed test matches the
   screen, and the host's present CPU time drops on a mostly still desktop.
4. Rate matching. Test: a 60 fps game holds 60 Hz on the panel. Watch for
   judder when the game's rate changes.

## Things to check early

- How much the guest's high-resolution timer drifts on AVF, and how late it
  fires when the vCPUs are busy. That decides how early the vsync timer has to
  be armed.
- Whether DWM behaves with a display-only driver that raises vsync from a timer
  instead of hardware.
- Installing a display driver: through the registry like the other drivers, or
  with an INF. That hits the same catalog question as the audio driver.
