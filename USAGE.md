# How to use HyperWin

HyperWin runs Windows 11 for ARM on your Android phone, in a virtual machine.
This guide takes you from a fresh install to the Windows desktop.

## What you need

- A phone with Android 16 or newer that supports the Android Virtualization Framework (AVF).
  HyperWin has been tested on a Pixel 6 Pro.
- About 30 GB of free storage, or more for a bigger Windows disk.
  The disk is at least 24 GB and 64 GB by default. It only takes space as Windows fills it.
- A Windows 11 ARM64 ISO file, and a Windows license of your own. See below.
- The [Shizuku](https://shizuku.rikka.app/) app, used once to switch on two permissions.
  A computer with `adb` works instead.

## 1. Get a Windows 11 ARM64 ISO

HyperWin does not include Windows. You download the ISO yourself and use it under
Microsoft's license terms.

Microsoft offers the ARM64 ISO on its download page:
<https://www.microsoft.com/software-download/windows11arm64>.
Pick your language and download it to the phone, or copy it over from a computer.

The file must be the **ARM64** version. The x64 ISO will not work.

## 2. Install HyperWin

Download `hyperwin-<version>.apk` from the
[Releases page](https://github.com/nanofuxion/hyperwin/releases) and open it on the phone.
Allow installs from your browser or file manager if Android asks.

## 3. Turn on the virtual machine permissions with Shizuku

Android only lets the system shell grant the two permissions that start virtual machines.
Shizuku lends HyperWin that shell for a moment. You only do this once. The permissions
stay after a restart, so you can uninstall Shizuku afterwards if you like.

1. Turn on Developer options. Open **Settings → About phone** and tap **Build number** seven times.
2. Connect the phone to Wi-Fi. Wireless debugging needs it.
3. Install [Shizuku](https://shizuku.rikka.app/download/) and open it.
4. In Shizuku, tap **Pairing** under "Start via Wireless debugging" and follow its steps.
   It sends you to **Developer options → Wireless debugging → Pair device with pairing code**.
   Type the code into the Shizuku notification.
5. Back in Shizuku, tap **Start**. It should say Shizuku is running.
6. Open HyperWin. When it asks for access, tap **Allow**, then allow HyperWin in Shizuku's prompt.

HyperWin's own prompt tells you which step you are on. It offers **Get Shizuku** when Shizuku is
missing, **Open Shizuku** when it is not running, and **Allow** when it is ready.

### Without Shizuku

Connect the phone to a computer with USB debugging on, and run once:

```
adb shell pm grant dev.hyperwin.app android.permission.MANAGE_VIRTUAL_MACHINE
adb shell pm grant dev.hyperwin.app android.permission.USE_CUSTOM_VIRTUAL_MACHINE
```

## 4. Set up Windows

The first time HyperWin opens, it walks you through setup:

1. **Choose your Windows ISO.** Tap **Choose ISO file** and pick the ISO from step 1.
   HyperWin only reads it. Leave **Windows edition number** at 1 unless you know you need another.
2. **Give Windows some room.** Pick processor cores, memory and disk size.
   Cores and memory can be changed later. The disk size cannot.
3. **Create your Windows account.** Enter a user name. The password is optional.
4. **Setting up Windows.** HyperWin builds the Windows disk. This takes about fifteen minutes.
   Keep HyperWin open. Windows then starts by itself and finishes its own setup over a few restarts.

When it is done you land on the Windows desktop, signed in.

## 5. Use Windows

A see-through toolbar sits at the top of the screen:

| Button | What it does |
| --- | --- |
| **Keyboard** | Opens the phone's keyboard, or the touch keyboard inside Windows. |
| **Mouse** | Chooses how your fingers act: touch screen, trackpad, or game mouse. |
| **Controls** | On-screen game controls. Pick a profile, edit one, or import a Winlator `.icp` file. |
| **Settings** | Goes back to HyperWin's settings. Windows keeps running. |

Game controllers paired with the phone show up in Windows as Xbox controllers.
Windows keeps running while HyperWin is in the background. Open HyperWin and tap
**Return to Windows** to come back.

To turn Windows off, shut it down from the Windows Start menu, or tap **Shut down** in HyperWin.

## Settings

Settings are saved for each machine. Some take effect the next time Windows starts.

- **Display.** Screen size and refresh limit.
- **Graphics.** Add a GPU driver or DXVK file, and manage the shader cache.
- **Touch and mouse.** Trackpad mode, game mouse, mouse speed in games.
- **Controllers.** Rumble, and whether player 1 is an Xbox or a DualShock 4 controller.
  DualShock 4 adds gyro aiming: tilt the phone to aim.
- **Network.** Use the phone's own Wi-Fi address, so other devices can reach Windows. Handy for LAN games.
- **Files.** Show phone storage, SD cards and USB drives as drives in Windows.
- **Performance.** Processor cores and memory.
- **When HyperWin opens.** Open straight into Windows, or into settings.
- **Advanced.** Logs, force stop, and reinstall Windows.

## If something goes wrong

- **Black screen at start.** HyperWin notices and switches to its simpler display on its own.
- **Windows does not start.** Open **Advanced → Logs** and include them when you report the problem.
- **Windows is stuck.** **Advanced → Force stop** turns the virtual machine off at once.
  Unsaved work in Windows is lost.
- **Start over.** With Windows off, **Advanced → Reinstall Windows** builds a fresh Windows
  from an ISO. The old Windows, with its files and programs, is erased once the new one is ready.

Report problems on the [Issues page](https://github.com/nanofuxion/hyperwin/issues).
