# Memory balloon: give Windows' unused RAM back to Android

Why: with a 5.5 GB guest the Pixel 6 Pro runs with about 300 MB available, swap full (3 GB) and the
Mali CMA pool used up. That spoils speed numbers and makes Android kill apps. Checked 2026-11-23.

## Feasibility check (before any build)

1. **The phone's AVF has the API.** `framework-virtualization.jar` (Android 17) has
   `VirtualMachineCustomImageConfig.Builder.useAutoMemoryBalloon(boolean)`, which sets
   `VirtualMachineRawConfig.balloon`, and `VirtualMachine.setMemoryBalloon(bytes)`,
   `setMemoryBalloonByPercent(...)`, `getActualMemoryBalloonBytes()`. `GuestBoot.uefiImage` now calls
   `useAutoMemoryBalloon(true)` when `adb shell setprop debug.hyperwin.balloon 1` is set before the start.
2. **virtmgr refuses it on this phone.** With that property on, virtmgr logs
   `Memory balloon not enabled: config.balloon=false, hypervisor.memory_reclaim.supported=false` and runs
   crosvm with `--no-balloon`. No `ro.boot.hypervisor.memory_reclaim.supported` is set: the hypervisor is plain
   `kvm.arm-nvhe` with `protected_vm.supported=0`. Both facts come from the platform, which we cannot change
   (see the `avf-stock-limits` memory note). So no balloon device exists in the VM: Windows lists virtio
   console, block, rng and vsock, and no device 0x1045.
3. **Windows would take it.** The stock virtio-win ISO has `Balloon/w11/ARM64/balloon.sys` and `blnsvr.exe`. Our
   PCI bus driver (`hwpci`) already lets the stock `viosock.sys` and `viostor.sys` bind to their devices, so the
   same route should bind `balloon.sys`. It is not in `guest/drivers/drivers.json` yet. This only matters if
   point 2 changes (a phone whose AVF allows the balloon, such as one with memory reclaim support).

## What a guest RAM size change does (the cheap stopgap)

Measured with Borderlands in its main menu, Pixel 6 Pro:

| Guest RAM | MemAvailable | Swap free | CMA free | Borderlands |
|---|---|---|---|---|
| 5.5 GB | about 300 MB | 21 MB of 3 GB | 0 | 3 to 4 FPS |
| 3.0 GB | 2.4 to 3.4 GB | 1.1 to 1.2 GB | 0 | 3 to 4 FPS (no change) |

So the guest's RAM size does not explain Borderlands' speed. It does give Android 2 to 3 GB back.

## If we want it anyway: a balloon of our own

Only the VM host can free guest memory, and the guest RAM belongs to crosvm, which the app cannot reach. The
shared window, GPU heap and USB region are different: the app creates them (`SharedMemory`) and hands crosvm the
file descriptors (`installWindowBackingHook`, file-backed mappings). A design that works inside that:

1. Back an extra RAM range of the guest (say 2 GB) with an app-owned memfd, as the heap is, and let Windows see it
   as RAM in the firmware's memory map.
2. A small guest driver takes pages from that range (`MmAllocatePagesForMdlEx` with the range as low and high
   address) when the app asks, and tells the app which frames through the ring.
3. The app punches holes in the memfd for those frames (`fallocate` PUNCH_HOLE): Android gets the memory back.
   Deflating gives the pages back to Windows, and the host faults in zero pages.
4. The app asks to inflate when `onTrimMemory` says Android is short, and to deflate when Windows is short.

Cost: a kernel driver (the build tree has the pieces: hwusbraw maps a region the same way), an EFI memory map
change, the app policy, and testing for hangs. Roughly a week of careful work, with a real risk of hanging
Windows when memory is short. Not started. Needs a yes.
