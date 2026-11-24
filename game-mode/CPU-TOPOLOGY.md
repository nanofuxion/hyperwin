# Pixel 6 CPU scheduling research

## Does the Windows kernel see different kinds of cores?

**SweetCider comparison.** SweetCider recorded an eight-vCPU Pixel run using AVF's `CPU_TOPOLOGY_MATCH_HOST` with each vCPU pinned to the corresponding host core. Its [probe capture](../../sweetcider/tools/avf-feature-probe-devices-20261109.txt) records crosvm's host-topology and virtual cpufreq options. SweetCider separately generates an Apple Device Tree for XNU. Its `EEEEPPPP` configuration exposes two scheduler classes, combining A76 and X1. The project documents the pinned multicore run and describes the AMP kernel build separately as awaiting a boot test at the time of that entry. [Multicore and AMP notes](../../sweetcider/tools/xnu-local/README.md) · [Guest tree generator](../../sweetcider/tools/xnu-local/avf/mkadt_avf.py)

HyperWin's [multicore launch path](../app/src/main/kotlin/dev/hyperwin/app/AvfLaunch.kt) already requests the same MATCH_HOST mode when the configured CPU count exceeds one. That narrows the engineering gap to conveying the mapped tiers to Windows through ACPI. Use the running VM's mapping to validate the firmware change; arbitrary vCPU-count mode in SweetCider is a separate, floating configuration.

**Bootloader audit.** The firmware asset selected in [AvfLaunch.kt](../app/src/main/kotlin/dev/hyperwin/app/AvfLaunch.kt) matches the local Gunyah AVF image byte for byte by SHA-256 (`00922cc6875d9dd5db6da5d102fc2e4cfe6ca7005d77505a30dd702f5ef3897a`). Its AArch64 build includes the EDK2 device-tree parser and dynamic ACPI table generators. The [GIC CPU parser](../firmware/edk2-gunyah/edk2/DynamicTablesPkg/Library/FdtHwInfoParserLib/Gic/ArmGicCParser.c) allocates zero-filled CPU records, assigns processor IDs, and leaves ProcessorPowerEfficiencyClass at zero. The [MADT generator](../firmware/edk2-gunyah/edk2/DynamicTablesPkg/Library/Acpi/Arm/AcpiMadtLibArm/MadtGenerator.c) copies that value into the ACPI entry for each CPU. HyperWin's second-stage EFI loader then adds the uniform performance descriptions described below before starting Windows Boot Manager. On Android, [AvfPlan.kt](../app/src/main/kotlin/dev/hyperwin/app/AvfPlan.kt) takes the maximum of the cores' maximum clock speeds, so the shared MHz field has already lost the per-core distinction.

The source therefore identifies a specific missing input for guest-wide scheduling. Verify the running guest's MADT CPU entries and Windows efficiency classes against the host's vCPU affinity masks. If the host preserves tier identity, carry those relative classes through the EDK2 CPU parser into the MADT, and align the per-CPU performance descriptions with the same mapping. Derive the classes from the actual VM configuration so other phones and VM topologies receive accurate descriptions.

Device Manager's processor names cannot answer this. On the Windows guest, query `GetLogicalProcessorInformationEx(RelationProcessorCore)` and record each processor mask and `EfficiencyClass`. Higher class values denote higher intrinsic performance. If all records have the same value, Windows exposes a single class through this interface. If the values form multiple groups, Windows recognizes a distinction even when the device names match. Record the actual counts and values. [Microsoft processor relationship fields](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-processor_relationship)

HyperWin's EFI boot path calls `HyperwinInstallCpuSsdt`. It enumerates enabled processor UIDs, then emits a processor performance description (ACPI `_CPC`) for each one using a single host-supplied MHz value. Highest, nominal and lowest performance use the same constants on every processor. Consequently this table communicates no differences between the three Tensor core tiers. That makes a uniform guest classification plausible. The installed guest's efficiency classes remain unmeasured. [CPU table builder](../guest/efi/cpu_acpi.c) · [EFI boot call](../guest/efi/setup_boot.c)

Windows schedules its threads onto virtual CPUs. Android schedules the corresponding host threads onto eligible physical cores. Before changing firmware, pair the Windows class query with the host affinity mask and observed placement for each vCPU. If vCPUs have stable mappings to distinct tiers but Windows reports a single class, investigate the guest firmware's processor descriptions. If a vCPU can move freely across all tiers, a permanent “fast” label in Windows would promise capacity the host does not guarantee. Verify any eventual change with ordinary foreground and background Windows workloads. [AVF architecture](https://source.android.com/docs/core/virtualization/architecture)

The Pixel 6 and 6 Pro use the first-generation Google Tensor (GS101). Its eight CPUs form three clusters. The two fastest cores matter to HyperWin because the guest vCPU running the game, the Android graphics worker and the compositor can all need CPU time during the same frame. Whether any of them is delayed by CPU scheduling is an open measurement question. [GS101 CPU map and core types](https://android.googlesource.com/kernel/common/+/18f528a553e4500993c29639d4edc7a576f4195b/arch/arm64/boot/dts/exynos/google/gs101.dtsi)

| Android logical CPUs on GS101 | Core | Role |
|---|---|---|
| 0–3 | 4 × Cortex-A55 | Efficiency |
| 4–5 | 2 × Cortex-A76 | Performance |
| 6–7 | 2 × Cortex-X1 | Highest peak performance |

## Identifying the cores on a running device

For a future trace, collect the online CPU list from `/sys/devices/system/cpu/online`. Where readable, collect `related_cpus` and `cpuinfo_max_freq` under each `/sys/devices/system/cpu/cpufreq/policy*`, plus each `cpuN/cpu_capacity`. The policy gives the CPUs sharing a frequency control domain. Capacity and the device's CPU map help distinguish the tiers. Record missing fields as unknown. A momentary clock reading varies with load and thermal limits. The GS101 numbering above is specific to this chip. [Linux CPUFreq interface](https://docs.kernel.org/admin-guide/pm/cpufreq.html)

Record each candidate Android thread's TID and `Cpus_allowed_list` from `/proc/<pid>/task/<tid>/status` where accessible. That mask describes where the thread may run. A scheduler trace shows where it actually ran. Windows logical processor numbers belong to the guest. To identify the physical core serving a guest vCPU, follow the corresponding host VM thread in the trace.

## Where HyperWin could benefit

HyperWin already opens an Android performance hint session for the calling thread using `Process.myTid()`. The call site is the guest display loop, which updates its target from the refresh rate and reports elapsed work. That session covers one thread. Android game-channel and DXVK submission workers would need separate consideration in their owning process. [GameSettings.kt](../app/src/main/kotlin/dev/hyperwin/app/GameSettings.kt) · [GuestDisplay.kt](../app/src/main/kotlin/dev/hyperwin/app/GuestDisplay.kt)

The historical Borderlands profile attributed 190–290 ms per frame to a query waiting inside the Mali driver. Moving an already sleeping thread to an X1 cannot shorten the interval it spends waiting for GPU completion. A scheduling change could help if a worker wakes ready to submit and then waits for CPU time, or if submission work on a slower core leaves the GPU idle. Those cases look different in a trace. [HyperWin frame profile](../d3d/README.md#speed-plan-phase-0-2026-11-23-where-a-frame-goes)

## A decision test for a future performance run

1. Trace the same visible, uncapped scene with default scheduling. Capture scheduler wakeups, switches and CPU frequency alongside HyperWin's frame and query timings. Follow the game-channel worker, DXVK submission worker, compositor and guest vCPU threads where visible.
2. For the slow frames, distinguish time running, time runnable but waiting for a CPU, and time blocked on a query, fence or other event. Identify the CPU tier for the intervals on the frame's critical path. A migration or a busy efficiency core alone does not establish a bottleneck.
3. If CPU scheduling repeatedly delays submission, compare default scheduling with a performance hint for the affected worker group using its measured work interval and frame budget. Android recommends hints so its scheduler can account for other work and heat. Keep manual affinity as a diagnostic experiment only if the trace leaves a specific placement question. [Android thread scheduling guidance](https://developer.android.com/android-performance-analyzer/analyze/thread-sched)
4. Compare median and 95th-percentile frame times and runnable delay across repeated runs of the same scene, including after the phone warms up. Keep guest RAM, render path, vsync and display settings fixed. Adopt a scheduling change only if the sustained result improves and desktop composition stays smooth.

This note records a source and code review. A live scheduling trace and any performance gain remain to be measured.
