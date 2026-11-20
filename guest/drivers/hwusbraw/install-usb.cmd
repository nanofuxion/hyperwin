@echo off
rem Installs the raw USB ring driver for USB tests (usb/README.md). Machines made by this app get it at prep (guest/drivers/drivers.json), starting
rem with the system. This script is for older machines. It is a driver apart from the GPU and display drivers, and
rem USB passthrough itself stays off until switched on in the app. Run as administrator. Undo with remove-usb.cmd.
set G=C:\HyperWin\gpu\usb
sc stop hwusbraw >nul 2>&1
copy /y %G%\hwusbraw.sys %SystemRoot%\System32\drivers\hwusbraw.sys
sc query hwusbraw >nul 2>&1 || sc create hwusbraw type= kernel start= auto binPath= System32\drivers\hwusbraw.sys DisplayName= "HyperWin USB raw ring"
sc config hwusbraw start= auto >nul
sc start hwusbraw
