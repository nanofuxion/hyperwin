# AVF ARM64 UEFI firmware

This is an isolated build configuration for EDK2's `ArmVirtKvmTool` platform.
That platform consumes the boot FDT and uses EDK2 DynamicTablesPkg to publish
ACPI tables at runtime. It is the firmware candidate for AVF's crosvm guest;
QEMU's `ArmVirtQemu` image is only a test reference because its fixed platform
map is different.

The output is written under this directory. The script does not copy anything
into the Android app, mutate the VM disk, or launch a device.

## Build

Set `EDK2_SOURCE` to a clean EDK2 checkout containing `ArmVirtPkg/ArmVirtKvmTool.dsc`,
then run `sh firmware/avf-edk2/build.sh`. It uses EDK2's `GCC5` tag with
Homebrew's `aarch64-elf-gcc`, EDK2 BaseTools, `iasl`, and `nasm`. Override
`GCC5_AARCH64_PREFIX` if the compiler prefix differs. The script copies EDK2's
three build configuration files into `firmware/avf-edk2/conf` and removes LTO
from its private GCC5 release flags. The source checkout is not edited.

The known local source checkout used for feasibility work is
`/path/to/edk2` at commit `b24306f`. Its working tree
has unrelated local changes; the build script only reads the source and sends
all build output to this directory.

Expected firmware artifact:

```
firmware/avf-edk2/Build/RELEASE_GCC5/AARCH64/ArmVirtPkg/ArmVirtKvmTool/FV/KVMTOOL_EFI.fd
```

Some EDK2 revisions name the toolchain output directory differently. The build
script locates `KVMTOOL_EFI.fd` under its private output root and copies the
result to `firmware/avf-edk2/out/avf-kvmtool-aarch64.fd`.

## Platform facts to validate before use

From the captured AVF crosvm command line: one vCPU, 3072 MiB RAM, PCI MMIO
window `0x2c000000..0x2dffffff`, PCI CAM `0x2e000000..0x2effffff`, KVM VGIC
v3 with ITS allowed, and PMU disabled. The local crosvm source derives RAM
starting at `0x80000000`, GICD at `0x3fff0000`, GICR at
`0x3ffd0000` for one vCPU (one `0x20000` redistributor frame), and ITS at
`0x40000000`. Its FDT describes the timer PPIs as 13, 14, 11, and 10.

The U-Boot `dm tree` capture is not a raw FDT. Do not infer the complete ACPI
topology from it. The next target run must inspect the EFI configuration table
and FDT with the separate `firmware_diag.c` payload before asserting that ACPI
is absent or replacing tables. EDK2 `ArmVirtKvmTool` is a candidate, not yet
accepted: the generic QEMU EDK2 image was accepted as `--bios` but crosvm later
reported `Function not implemented` while executing a vCPU.

Storage is currently a virtio-pci block device. Windows Setup and Windows Boot
Manager need an ARM64 boot-start `viostor` driver (or a different firmware-visible
boot disk such as USB/SCSI). EDK2's built-in UEFI virtio drivers only help UEFI
read the filesystem; they do not supply the Windows kernel driver after
`ExitBootServices`.

The firmware should expose a GOP mode backed by a firmware framebuffer. This is
separate from AVF's gfxstream display path: GOP does not prove that the app can
read/present the guest display. A framebuffer bridge or a Windows virtio-gpu
driver is still needed for the desktop.
