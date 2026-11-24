# Experimental Windows CPPC to AVF frequency bridge

This is an opt-in demand-start driver. Its signed binary is bundled with the app,
but the default setting is off and Windows never auto-starts the service.
It supports only the legacy `virtual,android-v-only-cpufreq` MMIO protocol discovered
from the live EFI device tree. Unknown/upstream protocols and mismatched CPU counts
are rejected. No guest physical address is hardcoded for the frequency device.

Windows already writes DesiredPerformance to HyperWin's PCC shared page. The driver
uses one accounting coordinator (20 ms while busy, 100 ms while idle) and scales
capacity units to kHz. Idle CPUs release their floor after 200 ms at <=5%
utilization; >=10% reactivates control. Accounting errors release all boosts.
Workers sleep on events instead of polling. One worker is bound to each Windows
logical CPU so the MMIO write traps on the corresponding
crosvm vCPU thread: the legacy backend uses `sched_setattr(0, ...)` on its caller.
This requests utilization constraints; it does not pin Android threads or guarantee
a physical frequency/core, and thermal policy still applies.

Stopping the service joins every worker before unmapping and unloading. Each worker
first requests zero utilization minimum, restoring the baseline observed on the
Pixel (min 0, max 1024). **Do not use this driver on configurations with pre-existing
custom utilization clamps**: it cannot recover an arbitrary prior host constraint.
It samples the backend-reported frequency on request changes only for diagnostics; this is a
configured-core estimate, not execution feedback for a migrating vCPU. It does
not provide accurate delivered-performance feedback; the existing
shared reference/delivered counters remain synthetic. These limitations must be
resolved before promoting this experiment to a default feature.

Build: `sh guest/drivers/hwcpufreq/build.sh`. Uses the existing ARM64 driver toolchain
and local test certificate. A SYSTEM test session can create a kernel service with
`start= demand`, start it, and stop it. Never configure automatic start while testing.
`guest/tools/cppcprobe.c` reads requests and bridge telemetry without touching MMIO.

Acceptance: prove per-vCPU host clamp changes, clean unload/restoration, normal
boot/display/input, then compare OFF/ON/OFF performance at matched temperatures.
A changing telemetry counter alone does not prove host acceptance or a speedup.

## App setting

Settings → Performance → **Experimental CPU performance** stores the global
`experimentalCpuPerformance` preference (default false). The app stages the signed
driver only when enabling, then runs `toggle.ps1` through the SYSTEM guest agent.
The script stops any existing instance before replacing the driver and always keeps
the service demand-start. Disabling needs no file upload to the guest. The app
reapplies the preference on guest-agent connection; a disconnected VM shows a waiting
status. Failures/unsupported VMs show an unavailable status instead of claiming active.
Turning the preference off stops the bridge; it does not alter the Windows power plan.
