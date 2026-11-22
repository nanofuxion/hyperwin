<img src="assets/icon.svg" width="64" height="64" alt="">

# HyperWin

Windows ARM64 in an Android AVF virtual machine. The desktop can stay on Windows' own software compositor. Games reach the phone GPU through the virtio-gpu device AVF already creates when `GpuConfig` is set to gfxstream Vulkan. That is the same device SweetCider probed (`1af4:1050`, gfxstream-vulkan capset).

Replacing `d3d11.dll` or `xinput1_4.dll`, the way Wine and Winlator do, is a bootstrap only. Anti-cheat looks for those hooks. The path that keeps the system DLLs is:

- **Vulkan.** Windows' own `vulkan-1.dll` loads a Khronos ICD. The ICD has to speak virtio-gpu gfxstream. `guest/icd/vulkan_gfxstream.dll` is the registry slot for that driver. It does not drive the device yet, so `vkCreateInstance` returns `VK_ERROR_INCOMPATIBLE_DRIVER` and the loader skips it.
- **Picture and sound.** A 128 MiB shared window, the same idea as SweetCider's ashmem mapping. Frames are 1280×720 BGRA, double buffered. Audio is 48 kHz stereo S16 in a 1 MiB ring. The header is `HYPRWIN1` in [include/hw_abi.h](include/hw_abi.h).
- **Pads.** The latest pad is in that header. On AVF the guest also listens on vsock port 5601 and the app connects with `VirtualMachine.connectVsock`. A virtual HID pad is the version anti-cheat will accept. [guest/bootstrap/xinput.c](guest/bootstrap/xinput.c) is the fallback DLL, not the default install.
- **TPM.** A small TPM 2.0 engine (startup, random, capabilities, PCR read/extend) serves a mailbox in the window. The 32-byte seed is wrapped by an Android Keystore AES-GCM key (`TpmSeed`). The commands run in the app. Keystore holds the wrapping key. This is not a TPM inside TrustZone, and Windows' inbox `tpm.sys` still needs an ACPI CRB front end before BitLocker will see it.
- **Install.** [tools/unattend/autounattend.xml](tools/unattend/autounattend.xml) is an ARM64 answer file for the tiny11 image. First logon copies `hyperwin\` off the OEM USB to `C:\HyperWin` and registers the ICD. It does not replace anything in `System32`.

The install button in the app is the Windows setup flow. Give it an ISO with `--es iso /path/to/tiny11.iso`, or copy the image to the app's external `install.iso`. Prepare copies that ISO into private storage, creates a 40 GiB `system.img` whose length is a multiple of 4096, and packs `oem.img`: a FAT volume with `autounattend.xml` and the driver files. `GuestBoot.installImage` then does what SweetCider does for an iOS root: `setKernelPath(hyperwin.img)` and one `RWDisk` for the system disk, the driver disk, and the ISO, with gfxstream Vulkan turned on. First logon copies `hyperwin\` to `C:\HyperWin` and registers the ICD. It does not replace `System32` DLLs.

The boot path matches SweetCider. `VmSmokeTest` calls `setKernelPath` with a Linux ARM64 Image and `VirtualMachineCustomImageConfig.Disk.RWDisk` with a private raw disk whose length is a multiple of 4096. `GuestBoot` does those two calls for `hyperwin.img` and `system.img`. The Image is the probe: it finds the shared window from the device-tree `pmem-region`, the same kind of extra memory SweetCider maps above the guest. AVF enters that Image directly. It does not open a UEFI shell.

EDK2 (`edk2-aarch64-code.fd`, QEMU's AAVMF build) is only the firmware for the tiny11 ISO test. Windows Setup still needs a UEFI firmware, which AVF does not provide, so that boot stays a QEMU stand-in. The ISO, a raw disk, and the OEM answer file are USB storage, because Windows PE has no virtio disk driver. `virtio-gpu-pci` is on the bus so it matches the gfxstream device AVF adds.

```
make test          # host-side frame, audio, pad, TPM
make dll           # ARM64 bootstrap DLLs and the ICD stub
tools/qemu/run-probe.sh
HYPERWIN_ISO=/path/to/windows11-arm64.iso tools/qemu/run-iso.sh
```

x86_64 builds of the same DLLs can be made on `another machine`, which has `x86_64-w64-mingw32-gcc`. The guest image is ARM64, so the DLLs that tiny11 loads are the ARM64 ones.

The Android app is `dev.hyperwin.app`. It stages the answer file, builds the gfxstream `GpuConfig`, allocates the shared window, and wraps the TPM seed. It is not installed on a device from here.

## First start

Android only lets a shell switch on the two permissions that start virtual machines. The app asks for them once.
Install [Shizuku](https://shizuku.rikka.app/), start it from Wireless debugging, then tap Start Windows and Allow.
Without Shizuku, run this once from a computer:

```
adb shell pm grant dev.hyperwin.app android.permission.MANAGE_VIRTUAL_MACHINE
adb shell pm grant dev.hyperwin.app android.permission.USE_CUSTOM_VIRTUAL_MACHINE
```
