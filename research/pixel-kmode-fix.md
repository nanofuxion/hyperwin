# Pixel KMODE_EXCEPTION_NOT_HANDLED fix — 2026-11-16

## Observed failure

On stock AVF, Windows reached its logo then bugchecked. Enabling the existing kernel bugcheck parameter renderer exposed:

- Exception: `0xffffffffc000001d` (illegal instruction).
- Address: `0xfffff861442c8830` (randomized virtual address).
- Saved processor state: `0x60000044` (EL1t, SP0).
- Instruction: `0xd53b9d08` (`mrs x8, PMCCNTR_EL0`).

Evidence: `build/pixel-debug/parameters.png` and `parameters-boot.log`.

## Cause and correction

Stock AVF has no virtual PMU. Static boot-time instruction patches cannot cover every image loaded later. The existing EL1 handler was installed at the SPx vector (`KiKernelExceptionHandler`, RVA 0x202a00), while this exception arrives at the SP0 vector (`KiKernelSp0ExceptionHandler`, RVA 0x202800).

`guest/efi/trace.c` now validates the SP0 vector prologue and patches its first instruction. `guest/efi/el1_pmu_trap.S` selects the interrupted SP0 stack, saves scratch registers, and handles only the exact PMCCNTR read, returning CNTVCT and advancing ELR by four. It preserves X18 and restores SPSel before returning. All other exceptions take the displaced original instruction and original Windows handler. The earlier broad sysreg swallowing and writes into executable stub memory were removed.

An intermediate placement after the original first two instructions lost the interrupted X18 and produced an access violation. Moving interception before that clobber resolved it. See `sp0.png` versus `sp0-preserved.png`.

The diagnostic branch enabling bugcheck parameters remains instruction- and image-stamp-checked for this specific kernel (PDB GUID E6D05F65-3C73-B225-54C8-324CE63C3E2C, age 1). It changes display only, not exception handling. Symbols downloaded from Microsoft's public symbol server are in `build/pixel-debug`.

## Verified result

Pixel 6 Pro 1C231FDEE004NA reached Windows Setup's language screen at 16:45 local, using stock AVF and the existing firmware/installer. No Windows image was applied and no firmware rebuilt. This proves Setup boot, not an installed desktop or input functionality.

- Setup ESP SHA256 (built image, APK asset and phone private file match): `4bc1fd645b86ef1c9d98e2a8cc2ba6695629f744ce90f4b019ea6355b07c69a5`.
- AVF firmware unchanged: `00922cc6875d9dd5db6da5d102fc2e4cfe6ca7005d77505a30dd702f5ef3897a`.
- QEMU firmware unchanged: `7bfb8b8452b3ee277bde3507eac0500caf7d45332a8629c2e5083fbcf26300f0`.
- Evidence: `build/pixel-debug/sp0-preserved-boot.log`, `sp0-preserved.bgra`, `sp0-preserved.png`.

Rebuild only the compatibility ESP with `sh tools/efi/build-avf-compat.sh`, then `./gradlew :app:assembleDebug`. This preserves the existing BOOTAA64.EFI/GOP.EFI and firmware. The script copies the ESP into the APK asset that bootWindows actually uses.

CNTVCT substitution does not establish PMU-equivalent frequency/performance semantics. It is the existing counter compatibility policy extended to the faulting kernel path, and needs further workload testing. The diagnostic display patch and vector signature are specific to the tested kernel.
