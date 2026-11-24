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

## Implementation and live verification (2026-11-24, Pixel 6 Pro GS101)

### What was implemented (dynamic, host-driven)
- **[include/hw_abi.h](../include/hw_abi.h)**: added `cpu_topo_count`, `cpu_max_mhz[64]`, `cpu_min_mhz[64]`, `cpu_capacity[64]`, and `cpu_eff_class[64]` to `struct hw_header` (total size remains well under `0x1000`).
- **[AvfPlan.kt](../app/src/main/kotlin/dev/hyperwin/app/AvfPlan.kt)**, **[WindowBridge.kt](../app/src/main/kotlin/dev/hyperwin/app/WindowBridge.kt)**, **[jni.c](../app/src/main/cpp/jni.c)**, **[AvfLaunch.kt](../app/src/main/kotlin/dev/hyperwin/app/AvfLaunch.kt)**: dynamically reads `/sys/devices/system/cpu/cpuN/` (`cpu_capacity`, `cpufreq/cpuinfo_max_freq`, `cpufreq/cpuinfo_min_freq`) at VM launch, sorts distinct `(capacity, maxMhz)` tiers ascending (`0` = lowest, higher = faster — no hardcoded 1+3+4 or Pixel 6 Pro layout), and populates `struct hw_header` only when `MATCH_HOST` multicore topology (`cpuCount > 1`) is active.
- **[guest/efi/cpu_acpi.c](../guest/efi/cpu_acpi.c)**: when `h->cpu_topo_count == cpus && cpus > 1`, patches each enabled MADT GICC entry's `ProcessorPowerEfficiencyClass` (ACPI 6.0+ offset 76, updating MADT checksum) and emits per-CPU `_CPC` packages with per-core `Highest/Nominal = capacity`, `Lowest = scaled min`, `NominalFrequency = max_mhz`, and `LowestFrequency = min_mhz`. Falls back to the legacy uniform `cpu_mhz` table when `cpus == 1` or counts do not match. Preserves `osc_ssdt`, `PCCT`, and `gpu_ssdt` (`ACPI\HWGP0001`).

### Live verification on Pixel 6 Pro (`1C231FDEE004NA`, GS101: 4×A55 `cap=160` 1803 MHz, 2×A76 `cap=498` 2253 MHz, 2×X1 `cap=1024` 2802 MHz)
1. **Host vCPU affinity & placement (critical finding):**
   - `crosvm_hyperwin` is launched by AVF with `--host-cpu-topology --virt-cpufreq`, **without** `--cpu-affinity=`.
   - All 8 host threads (`vcpu0`..`vcpu7`) have `Cpus_allowed_list: 0-7` and migrate freely across all 8 physical cores (confirmed via live `ps -T -o TID,PSR,COMM` sampling and across reboots where guest `MIDR_EL1` per logical CPU changed between boots: boot 1 read `0..3=A55, 4..5=A76, 6..7=X1`; boot 2 read `0..4,7=X1, 5..6=A55`).
   - Android AVF's `VirtualMachineRawConfig` / `CpuOptions` AIDL exposes only `cpuCount` / `matchHost` and `boostUclamp` (no `vcpuAffinity`), and SELinux prevents `untrusted_app_34` (`dev.hyperwin.app`) from calling `sched_setaffinity` on `untrusted_app_all_virtmgr_crosvm` (`crosvm_hyperwin`).
2. **Guest ACPI MADT & `GetLogicalProcessorInformationEx(RelationProcessorCore)`:**
   - **Before:** MADT GICC `effClass = [0,0,0,0, 0,0, 0,0]`, `_CPC` uniform (`Highest=100`, `2802 MHz`). Windows reported 8 cores with `EfficiencyClass = [0,0,0,0, 1,1, 1,1]` (masks `0x1`..`0x80`).
   - **After:** MADT GICC `effClass = [0,0,0,0, 1,1, 2,2]` (`len=82`, `rev=5`), per-CPU `_CPC` emitted (`cap=160/498/1024`, `max_mhz=1803/2253/2802`). Windows booted cleanly and reported 8 cores with `EfficiencyClass = [0,0,0,0, 1,1, 1,1]` (Windows 11's 2-class hetero bucket) **consistently even when boot-time `MIDR_EL1` landed on random host cores** (e.g. `cpu5..6` reading A55 MIDR still received `effClass=1` from ACPI).
   - Scheduling distribution shifted under foreground + background contention (`cputopo.exe bench`, 80 frames):
     - **Before (cool, clocks unthrottled):** `solo` `cpus=[0:24 1:10 2:7 3:5 4:0 5:0 6:0 7:34]` (`p50=21.52 ms`, `p95=29.66 ms`); `bg6_eco` `cpus=[0:8 1:43 2:15 3:14 4:0 5:0 6:0 7:0]` (`p50=15.63 ms`, `p95=16.14 ms`); `bg6_norm` `cpus=[0:10 1:19 2:28 3:23 4:0 5:0 6:0 7:0]` (`p50=15.98 ms`, `p95=26.94 ms`).
     - **After (warm/throttled: `BIG=86°C` capped at `984 MHz`, `MID=78°C` `1491 MHz`, `LITTLE=79°C` `738 MHz`):** `solo` `cpus=[0:34 1:0 2:0 3:0 4:10 5:15 6:7 7:14]` (`p50=40.09 ms`, `p95=106.17 ms`); `bg6_eco` `cpus=[0:57 1:0 2:0 3:0 4:8 5:8 6:2 7:5]` (`p50=33.21 ms`, `p95=56.77 ms`); `bg6_norm` `cpus=[0:30 1:0 2:0 3:0 4:16 5:16 6:10 7:8]` (`p50=37.98 ms`, `p95=127.69 ms`).
   - **Honest performance verdict:** Because AVF leaves `vcpu0..vcpu7` floating on `0-7` and the post-change run was heavily thermally throttled (`BIG` capped from 2802 to 984 MHz), **no speedup is claimed**. The firmware/ACPI tables now accurately and dynamically describe host tiers without hardcoding, and Windows places foreground work on cores 4–7 instead of exclusively cores 0–3, but hard tier-to-physical-core speedup is fundamentally bounded by AVF's unpinned vCPUs until AVF exposes vCPU pinning or a guest `virt-cpufreq` driver exists.
3. **Boot stability, display/VSync, and input:** Clean Windows boot, desktop renders normally, `ACPI\HWGP0001` and `ROOT\HWVSINPUT\0000` remain healthy. Also added `POST_NOTIFICATIONS` permission in `AndroidManifest.xml` / `MainActivity.kt` so `WindowsService`'s ongoing notification is visible and configurable in Android Settings.

## Active virtual-frequency control experiment (2026-11-24)

The subsequent live investigation found that the Pixel's existing AVF launch exposes the
**legacy** `virtual,android-v-only-cpufreq` device: guest physical address `0x1040000`,
size `0x40` for eight CPUs. These values are discovered from FDT, not assumed by the driver.
Windows already changes its per-core PCC DesiredPerformance values under both idle and load,
but host vCPU utilization minimums remained zero before the bridge.

Implemented an **opt-in, demand-start** experiment in
[hwcpufreq.c](../guest/drivers/hwcpufreq/hwcpufreq.c). Each guest worker runs on its own
logical CPU and forwards that CPU's CPPC request, scaled to kHz, through legacy MMIO.
The crosvm backend changes the calling vCPU thread's utilization clamp. This works within
the existing platform permissions without Android root or cross-process affinity changes.
Unknown protocols, absent/mismatched topology and unexpected region sizes are rejected.
It is not installed automatically or included in the normal driver bundle.

The signed read-only [cppcprobe.c](../guest/tools/cppcprobe.c) records topology,
DesiredPerformance and optional bridge telemetry. Discovery no longer uses the shared
HW_DIAG words: those were overwritten by other guest diagnostics. EFI only discovers the
region; it does not access frequency MMIO. Parser bounds and compatibility-string checks
have a host regression test at [cpufreq_discovery.c](../guest/efi/tests/cpufreq_discovery.c).

### Functionality and first OFF/ON/OFF run

- Start and stop both succeeded. During the ~11-second functionality test, host vCPU
  `uclamp.min` changed with requests (examples included 40, 248, 619 and 1024); maximums
  remained 1024. Bridge counters advanced. No backend MMIO/scheduler errors were observed.
- Stop joined workers, cleared active telemetry and restored **all eight** host vCPU
  minimums to 0, maximums to 1024. Desktop/agent remained healthy.
- Same boot, same existing `cputopo.exe bench`, 4288 MiB crosvm memory. Each pacing phase
  collected 80 samples. A and C had the driver stopped; B was enabled for ~10 seconds.

| Pacing workload | OFF A mean / p95 (ms) | ON B mean / p95 (ms) | OFF C mean / p95 (ms) |
|---|---:|---:|---:|
| solo | 26.16 / 44.91 | 8.52 / 9.31 | 23.37 / 40.43 |
| bg6_eco | 15.25 / 18.62 | 12.43 / 17.47 | 15.38 / 18.22 |
| bg6_norm | 15.17 / 17.37 | 15.92 / 21.07 | 15.55 / 18.26 |

Live cluster sensors began around 41–43°C for each run. They ended around 47°C (A),
52–53°C (B), and 48°C (C). Advertised scaling maximums were unchanged. Thermalservice also
returned an older frozen sensor set; those frozen 79/78/86°C values must not be confused
with the moving readings. The improved solo response is promising but does not establish
an equivalent game-FPS improvement, a sustained benefit, or a general background-load win.

### Limits before any default enablement

This experiment does not fix physical affinity, implement accurate delivered-performance
feedback, or preserve arbitrary custom pre-existing host clamps. Windows' existing
reference/delivered counters are still synthetic; CPU0 frequently requests its maximum
even at the desktop. Idle power and long sustained heat therefore need additional work.
Keep the service demand-start and stopped outside explicit tests. See the
[driver notes](../guest/drivers/hwcpufreq/README.md) for supported configuration and cleanup.

### Repeat attempt and release decision

A later repeat attempt must **not** be used as sustained acceptance: the device operator's
command-completion matcher launched two enabled benchmarks concurrently (05:55:03.389
and 05:55:03.981). The enabled block lasted ~10 seconds and was stopped after a live thermal
reading reported BIG 90°C (later reads changed sharply). Both overlapped processes exited;
no third enabled test was run. Clamps returned to 0/1024 and GPU, gamepad bus, touch and
vsock PnP devices remained OK. No further stress test was requested.

Solo samples were again faster, but concurrency and thermal differences invalidate any
sustained comparison. The initial sequential OFF/ON/OFF remains a short synthetic test,
not a real-game performance or battery-life result. **Implementation and host control are
verified; sustained performance and production policy are not.** The experimental service
remains STOPPED with DEMAND_START. Do not enable it by default for 0.1.1 on this evidence.

### Final idle smoke

After removing the temporary EFI MMIO reads/diagnostic-slot writes and restricting CPPC
logging to debug builds, the final debug APK was installed as an in-place update. Windows
shut down cleanly before installation and the existing VM booted afterward with its data
preserved. The read-only probe still reported topology 8, legacy protocol 1, base `0x1040000`,
size `0x40`; counters advanced. HyperWin GPU and gamepad bus PnP status were OK. The bridge
was not started on this boot (`STATE STOPPED`, `DEMAND_START`, exit 1077); all eight host
vCPU constraints were min 0/max 1024. No further benchmark or performance write was made.

Local validation: EFI build, full debug APK build, ARM64 signed driver build, AML
CPU-table disassembly, sanitizer-backed FDT parser regression test, and scoped whitespace
checks passed. No commit, push or release was performed.

## Idle-aware revision and feedback investigation (2026-11-24)

The demand-start experiment now has one accounting coordinator, not eight polling
workers. It queries Windows per-CPU idle/kernel/user times using system information
class 8, validates the returned layout and counter progression, and fails to zero
boost on accounting errors. Workers wait on per-CPU events. At <=5% guest utilization
for 200 ms a CPU releases its floor; >=10% reactivates forwarding. The coordinator
samples at 20 ms while busy and 100 ms when all CPUs are idle. Missing accounting
ticks preserve the prior decision rather than creating a false load measurement.

Telemetry now exposes guest load, sample/error counts, and the legacy device's
frequency estimate sampled on request changes. It does **not** feed that estimate
to ACPI as a measured delivered-performance counter.

### Results

- Revised signed driver started/stopped cleanly; all accounting samples reported
  zero errors. Idle CPUs frequently released their floor even while Windows kept
  nonzero DesiredPerformance values. Stop restored every host minimum to 0 and
  maximum to 1024. GPU/gamepad PnP and the desktop remained healthy.
- A 25-second idle run with guest probes showed release, but the probes themselves
  caused guest activity. A subsequent OFF/ON/OFF test therefore used only an
  identical host sampler within each quiet interval, with no guest probe or command.

| Quiet interval | Host elapsed (s) | crosvm user/system tick deltas | Host vCPU minimums |
|---|---:|---:|---|
| OFF A | 15.55 | 810 / 160 | All zero in 10 samples |
| ON B | 15.19 | 659 / 140 | All zero in 2/10 samples; CPU0=248 in 6/10; one sample had CPU6/7=1024 |
| OFF C | 15.67 | 850 / 174 | All zero in 10 samples |

These are CPU-accounting observations, **not battery-power measurements**. The
phone was charging; battery current was not suitable evidence of energy savings.
Live cluster temperatures remained roughly 38–44°C during the quiet windows.

One sequential enabled workload test then completed (80 samples per pacing phase):
solo mean/p95 9.31/15.03 ms; bg6_eco 13.03/18.41 ms; bg6_norm 15.65/18.14 ms.
Live cluster temperatures rose from 39–42°C to 48–50°C. No concurrent benchmark was
run. The service was stopped afterward and all eight floors returned to zero.

### Why default enablement is still withheld

Accurate CPPC reference/delivered counters must accumulate during active execution,
with delivered rate reflecting the performance actually provided. Windows idle
accounting can identify idle time, but cannot measure the physical performance
of a migrating host vCPU. The legacy crosvm device reads the current frequency of
its configured `pcpu`, scaled to virtual units; it does not measure where that vCPU
actually executed. Existing live measurements show unrestricted host migration.
AVF additionally passes `--no-pmu` on AArch64, and HyperWin's boot compatibility
code substitutes timer reads for PMU cycle reads. Those timer values cannot be
relabeled as measured CPU cycles.

Sources: [ACPI counter semantics](https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/08_Processor_Configuration_and_Control/declaring-processors.html),
[crosvm legacy frequency backend](https://android.googlesource.com/platform/external/crosvm/+/refs/heads/main/devices/src/virtcpufreq.rs),
[AVF launch configuration](https://android.googlesource.com/platform/packages/modules/Virtualization/+/refs/heads/main/android/virtmgr/src/crosvm.rs),
and [HyperWin's PMU compatibility code](../guest/efi/trace.c).

The idle-control improvement is implemented and tested. **Truthful delivered
feedback and default-enable acceptance remain unresolved** on this platform.
No synthetic replacement feedback was added, no power scheme was changed, and no
automatic startup/install was enabled. A host-supported per-vCPU feedback mechanism
(or another validated source) is needed before claiming that gap is closed.

## Experimental Settings toggle (2026-11-24)

Settings → Performance → **Experimental CPU performance** now exposes the idle-aware
bridge as an explicit opt-in (default off). The preference is saved in app settings
and reapplied on guest-agent connection. Windows service startup remains demand-only.
The status row reports waiting, enabling, active, off, or failure/unavailable; the
summary notes possible responsiveness benefits and increased heat/battery use.

The signed driver is bundled with a control script, staged only when enabling.
Turning off requires no guest file upload and stops the service live. The app
serializes changes, skips superseded requests, invalidates work on disconnect,
and registers command completion before sending to avoid fast-exit callback races.
No Windows power scheme or global automatic driver startup is changed.

Verified through the actual Pixel Settings UI after installing the final debug APK:
ON reported `Active (experimental)` and service RUNNING, without a Windows restart;
OFF reported success and service STOPPED, with all eight host vCPU floors restored
to min0/max1024. A saved ON preference reapplied after app update/agent reconnect.
The status text and switch alignment were also corrected. Final state was OFF and
STOPPED. This opt-in does not claim the unresolved delivered-feedback limitation
has been solved. Debug APK build, packaging shell syntax and scoped diff checks passed.
