@echo off
rem Installs the raw USB ring driver for USB tests (usb/README.md). Off by default: nothing installs this on its own,
rem and it is a driver apart from the GPU and display drivers. Run as administrator. Undo with remove-usb.cmd.
set G=C:\HyperWin\gpu\usb
sc stop hwusbraw >nul 2>&1
copy /y %G%\hwusbraw.sys %SystemRoot%\System32\drivers\hwusbraw.sys
sc query hwusbraw >nul 2>&1 || sc create hwusbraw type= kernel start= demand binPath= System32\drivers\hwusbraw.sys DisplayName= "HyperWin USB raw ring"
sc start hwusbraw
