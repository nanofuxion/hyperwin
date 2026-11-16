# Later: skip OOBE and create a generic user

The boot target right now is the Windows setup screen (OOBE). Reaching that screen means the loader handed off to the kernel and the display path is alive.

After that, the app should offer a setup choice instead of making the user click through OOBE on the phone:

- Create a generic local account and sign in to the desktop.
- Skip OOBE. Do not ship a modified Windows ISO for this. Apply it to the user's installed image (unattend answer file on the OEM disk, or a first-boot command) the same way test signing is set on the installed BCD before boot.

Leave the choice off until a boot actually reaches OOBE.

# Later: install from the user's ISO without WinPE

Every AVF disk is virtio. Windows PE cannot read a driver disk until a virtio driver is already running, so Setup from the ISO is the wrong place to inject drivers. When install work starts, apply `install.wim` to `system.img` inside the app (wimlib and ntfs-3g), then inject the drivers, BCD, and unattend offline. Do not ship or rewrite the user's ISO.

# Later: user-mode cycle counter

With no PMU, user-mode reads of `PMCCNTR_EL0` (`ReadTimeStampCounter`, and x86 `RDTSC` under emulation) fault. The QEMU run with `pmu=off` and stock EDK2 is what shows how often that happens. Do not patch it until that run has a program counter and a fault address.
