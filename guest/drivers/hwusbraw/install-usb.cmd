@echo off
rem USB passthrough in Windows (usb/README.md): usbip-win2 (a virtual USB host controller) plus a watcher that
rem attaches whatever the app exports from the phone, as it is plugged in. Nothing here touches the GPU or display
rem drivers. Run as administrator or SYSTEM. Safe to run again. Undo with remove-usb.cmd.
rem The older ring driver is only for the ring mode and tests; machines made by this app get it from drivers.json.
set G=C:\HyperWin\gpu\usb
if not exist "C:\Program Files\USBip\usbip.exe" if exist %G%\usbip-setup.exe %G%\usbip-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
copy /y %G%\usbwatch.ps1 C:\HyperWin\usbwatch.ps1 >nul
schtasks /create /f /tn HyperWinUsbWatch /sc onstart /ru SYSTEM /rl highest /tr "powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\HyperWin\usbwatch.ps1" >nul
schtasks /end /tn HyperWinUsbWatch >nul 2>&1
schtasks /run /tn HyperWinUsbWatch >nul
