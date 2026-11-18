# Android's game settings

The phone treats HyperWin as a game: it boosts the right threads when frames
are due, warns before it overheats, and follows the user's Game Dashboard
choice. The result is steadier frame times, not higher peaks.

## Where it stands

- The manifest has no `appCategory`, and there's no game mode config.
- Nothing tells Android how long frames take, so CPU speeds follow load slowly.
- Thermal state isn't checked. When the phone gets hot, it throttles without
  warning and frame times jump.
- The app has no foreground service. The audio plan needs one for the
  microphone anyway.

The biggest CPU users, Windows' vCPU threads, belong to crosvm, not to the app.
Android's hint APIs only accept the app's own threads, so they can't boost the
vCPUs. The levers below are what an app can actually use.

## What to build

### 1. Say it's a game

- `android:appCategory="game"` on `<application>`.
- `<meta-data android:name="android.game_mode_config"
  android:resource="@xml/game_mode_config" />` with:

  ```xml
  <game-mode-config xmlns:android="http://schemas.android.com/apk/res/android"
      android:supportsPerformanceGameMode="true"
      android:supportsBatteryGameMode="true"
      android:allowGameDownscaling="false"
      android:allowGameFpsOverride="false" />
  ```

  Downscaling and FPS override stay off. Android's own versions of those would
  fight the app's surface size and frame pacing. HyperWin does both itself.
- Read `GameManager.getGameMode()` when Windows starts and on each resume:
  - **Performance:** fastest display mode, nothing capped.
  - **Battery:** cap scanout games to 60 fps through the "Limit to screen
    refresh" path and a 60 Hz `setFrameRate` on the layer.
  - **Standard or unsupported:** the user's own settings.
- `GameManager.setGameState(GameState(isLoading = true, ...))` while Windows
  boots, and `false` once the desktop shows (`GuestDisplay`'s `onLive`).
  Android may boost while loading.

### 2. Performance hints for our own hot threads (ADPF)

`APerformanceHint` (NDK, API 33) lets an app say "this work must finish in N
ns" for its threads, and report how long it really took. The CPU governor then
ramps before a frame is late, instead of after.

| Thread                          | Process | Target                     | Work reported              |
|---------------------------------|---------|----------------------------|----------------------------|
| gfxstream render thread         | `:gpu`  | one refresh (8.3 ms at 120 Hz) | time from a guest submit to its fence |
| frame present thread (`GuestDisplay`) | app | one refresh            | each `nativePresent` pass  |
| scanout thread (`hwgpu_host`)   | `:gpu`  | one refresh                | fence wait to `setBuffer`  |

- Create each session in the process that owns the threads, since sessions
  only take the caller's own thread ids.
- Update the target when the display rate changes.
- On API 35, report GPU time too (`AWorkDuration` with GPU duration). The
  governor can then tell whether a frame was late because of CPU or GPU.
- Use `setPreferPowerEfficiency(true)` in Battery mode.

The gfxstream render thread is the important one. The scanout test showed it
at 26 to 43% of a core.

### 3. Heat

- `PowerManager.getThermalHeadroom(10)` every few seconds while Windows runs,
  and `addThermalStatusListener`.
- A "Keep the phone cool" setting, on by default:
  - headroom over 0.85: cap scanout games to 60 fps;
  - status `SEVERE` or worse: also drop the display to 60 Hz, and show a small
    notice on the toolbar handle ("Phone is hot, limiting to 60 fps");
  - back below 0.7: lift the caps.
- Without the setting, only show the notice.
- `Window.setSustainedPerformanceMode` is the old API for this. Check
  `PowerManager.isSustainedPerformanceModeSupported()`. Pixels usually say no,
  so use it only where it's supported.

### 4. A foreground service while Windows runs

A `VmService` of type `mediaPlayback|microphone`, started with the VM and
stopped with it. It shows a "Windows is running" notification with a Stop
button. Android is then less likely to kill the app in the background, and the
microphone keeps working (see `audio/README.md`).

## Phases

1. Manifest category, game mode config, game mode reading, and loading state.
   Test: HyperWin shows up in Pixel's Game Dashboard, and switching to Battery
   caps a fullscreen test at 60.
2. ADPF sessions on the three threads. Test: frame time spread of the
   scanout benchmark, `vkpresent.exe`, and a DXVK game, before and after. Look
   at the 99th percentile, not the average.
3. Thermal caps and notice. Test: 20 minutes of a heavy game with the setting
   on and off. Watch frame time over time and the headroom readings.
4. Foreground service.

## Things to check early

- Whether Game Dashboard picks up a sideloaded app from `appCategory` alone,
  or needs the Play Store's category.
- Whether `APerformanceHint` is available on this Pixel
  (`APerformanceHint_getManager` returns null when it isn't), and the preferred
  update rate it reports.
