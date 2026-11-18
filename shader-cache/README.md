# A shader cache that survives restarts

Shaders a game compiled once load from disk next time. The stutter of the first
play of a level only happens once, not every time the renderer starts.

## Why it stutters every run today

When a game needs a new shader combination, the phone's driver compiles a
Vulkan pipeline. That takes tens to hundreds of milliseconds on Mali, and the
frame waits. On a PC, the driver keeps compiled pipelines on disk. Here, nothing
does:

- gfxstream passes `vkCreatePipelineCache` and `vkCreate*Pipelines` straight
  to the driver (`vk_decoder_global_state.cpp` ~5642) and saves nothing.
- DXVK 2.x has no state cache of its own anymore. It counts on the driver's
  cache, which on Android only exists if the app passes one and saves it.
- The renderer is a fresh process (`:gpu`) every boot and every driver swap. So
  whatever the driver kept in memory is gone.
- Vortek hides `VK_EXT_graphics_pipeline_library` on Mali
  (`host/icd/vortek.cpp:105`). So DXVK can't compile parts of a pipeline ahead
  of time, and everything happens at draw time.

## Scope: the wrapper only

HwD3D (`d3d/README.md`) will move DXVK from Windows to the phone. So this
phase stays in one place, `gpu/host/icd/wrap.cpp`, which every renderer uses,
now and after that move. Don't:

- add caching inside gfxstream's decoder,
- add a state cache or other tuning to DXVK or Sarek in Windows,
- change how DXVK in Windows builds its pipelines.

That work would be thrown away when DXVK moves.

## What to build: an implicit cache in the driver wrapper

Every Vulkan call passes through the wrapper in `gpu/host/icd/wrap.cpp` before
the real driver. That makes it the one place that covers DXVK, vkd3d-proton,
Zink and plain Vulkan alike.

### How it works

1. **At `vkCreateDevice`** (`wrap.cpp:289`): read
   `VkPhysicalDeviceProperties.pipelineCacheUUID` and `driverVersion`. Load
   `$HWGPU_VK_CACHE_DIR/<uuid>-<driverVersion>.bin` if it exists. Create one
   `VkPipelineCache` from it for this device. If the driver refuses the data,
   delete the file and start with an empty cache.
2. **At `vkCreateGraphicsPipelines` and `vkCreateComputePipelines`:** pass our
   cache to the driver in place of whatever the program gave. Pipeline caches
   only change compile speed, never results. They're thread-safe by default
   (unless created with `EXTERNALLY_SYNCHRONIZED`), so one cache per device
   serves every thread. Mark the cache as changed.
3. **Saving:** a background thread checks every 30 s. If the cache changed, it
   calls `vkGetPipelineCacheData`, writes a temp file, `fsync`s it, and renames
   it over the old one. It saves once more at `vkDestroyDevice` (`wrap.cpp:312`).
   The periodic save matters, because a driver swap kills the renderer without
   warning.
4. **Size cap:** over 256 MiB, start a fresh file. That's simple, and the next
   few runs refill it with what's actually used.
5. **Several devices, one file.** gfxstream already makes a device per Windows
   program, and HwD3D will too. A pipeline cache can't be shared between
   devices, so each device has its own. To save, a device makes a fresh cache
   from the file, merges its own into it with `vkMergePipelineCaches`, and
   writes that. One lock in the process keeps saves one at a time. Then one
   program's save doesn't wipe out another's pipelines.

### Where it runs

- Only in the **bottom** wrapper, the one that opens the real driver
  (`next.empty()` at `wrap.cpp:116`), so a stack of fixes doesn't cache twice.
  The driver's cache key includes the final SPIR-V, so Vortek's shader changes
  above it cache correctly.
- Today the phone's own driver with no fixes on loads with no wrapper at all
  (`HwGpu.kt`, `env()`). Always load `libvulkan_passthrough.so` at the bottom,
  so the cache is there in every setup. It costs one function call per Vulkan
  call.
- Each driver gets its own file, because `pipelineCacheUUID` differs. Swapping
  between Mali and a custom driver keeps both caches.
- The planned phone-side renderers sit on the same wrapper: DXVK for HwD3D
  (`d3d/README.md`) and Zink for OpenGL (`opengl/README.md`). So this one cache
  covers them too, with no extra work.

### The folder

The app passes `HWGPU_VK_CACHE_DIR` = `cacheDir/shaders` in the renderer's
environment, next to `HWGPU_VK_DRIVER`. `cacheDir` is right for this. Android
may clear it when storage runs low, and that's fine for a cache.

### Seeing it work

- Chain `VkPipelineCreationFeedbackCreateInfo` (Vulkan 1.3 core) onto each
  pipeline create, unless the program already did. Count
  `APPLICATION_PIPELINE_CACHE_HIT` against misses, and the time spent
  compiling.
- Log a line a minute while compiles happen: pipelines created, hits, and
  compile time.
- Settings, Graphics: "Shader cache: 312 MB, 4,120 pipelines", and a "Clear"
  button that deletes the folder when the renderer isn't running.

## First-run stutter

This cache only helps from the second time on. For the first run:

- vkd3d-proton keeps its own cache (`VKD3D_SHADER_CACHE_PATH`). Not in this
  phase: no shader-cache work for DXVK or Direct3D 12 (user, 2026-11-18).
- Graphics pipeline library for DXVK waits for HwD3D. Vortek hides it on Mali,
  and whether to show it is a question for DXVK on the phone, not in Windows.

## Phases

1. Load and save in the bottom wrapper, with logging. Test: start a DXVK
   game, play a minute, restart the renderer, and play the same part. Hits
   should be near 100%, and compile time near 0.
2. Always load the passthrough wrapper. Test the phone's own driver with no
   fixes on.
3. Settings line, Clear button, and the size cap.
4. ~~The vkd3d-proton cache path~~ dropped: no shader-cache work for DXVK or
   Direct3D 12. The graphics pipeline library question moves to HwD3D.

## As built (2026-11-18)

- All in `gpu/host/icd/wrap.cpp`, in the bottom wrapper only (`g_bottom`). The
  pipeline hooks go in front of a fix module's own (Vortek's), and call it.
- `VK_EXT_pipeline_creation_feedback` is turned on at `vkCreateDevice` when the
  driver has it, to count hits.
- Saves merge into what the file holds now, under `g_save_lock`, which also
  keeps a device from being destroyed mid-save. The 256 MiB cap writes the
  device's own cache in place of the merged one.
- `stats.txt` beside the cache files: pipelines this renderer run made, and
  how many hit. Settings, Graphics shows "Shader cache: N MB. Last run: N
  pipelines, N% from the cache" with a Clear button that waits for Windows to
  be off.
- `libvulkan_passthrough.so` now always loads under the phone's own driver.
- Test tool: `guest/tools/pipetest.c` makes distinct compute pipelines
  (`pipetest.comp`, one specialisation value each) and times them.

Measured on the Pixel 6 Pro (Mali, phone driver, no fixes):

| Run | 300 pipelines |
|---|---|
| First, empty cache | 3327 ms, 11.1 ms each, worst 60 ms |
| Renderer restarted | 26 ms, 0.09 ms each |
| Windows restarted, app reinstalled | 211 ms, all 329 from the cache |

Mali returns real cache data, about 1.2 KiB a pipeline. gfxstream's own 28
pipelines also hit the cache. Not measured: a real game, and the cache under
Vortek.

## Things to check early

- Mali returns useful `vkGetPipelineCacheData` contents. Some drivers return
  only a header. Check the size after compiling a few hundred pipelines.
- Whether gfxstream itself creates pipelines with its own cache or with
  `VK_NULL_HANDLE`, for its internal blits and compositing. Those can share our
  cache too.
