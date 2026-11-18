@echo off
rem Installs or updates the HyperWin GPU in Windows (gpu/README.md): the ring driver hwgpu.sys and
rem the Vulkan driver vulkan_hwgpu.dll; also the input driver hwvsinput.sys, the phone storage
rem service hwfiles.exe, the sound driver hwaudio.sys and the touch driver hwhid.sys. Run as administrator or SYSTEM, by firstboot.cmd on new machines and by
rem the app on older ones. Safe to run again.
set G=C:\HyperWin\gpu
rem The storage service keeps the ring driver open, so it stops first.
sc stop HyperWinFiles >nul 2>&1 && ping -n 3 127.0.0.1 >nul
sc stop hwgpu >nul 2>&1
copy /y %G%\hwgpu.sys %SystemRoot%\System32\drivers\hwgpu.sys
sc query hwgpu >nul 2>&1 || sc create hwgpu type= kernel start= auto binPath= System32\drivers\hwgpu.sys DisplayName= "HyperWin GPU ring"
sc config hwgpu start= auto
sc start hwgpu
rem Apps link to the Vulkan loader, which Windows does not ship. Keep one something else installed,
rem and update ours (the marker says System32's copy came from here).
if not exist %SystemRoot%\System32\vulkan-1.dll echo.> %G%\vulkan-1.ours
if exist %G%\vulkan-1.ours copy /y %G%\vulkan-1.dll %SystemRoot%\System32\vulkan-1.dll
rem The keyboard, mouse and gamepad driver: Windows keeps a loaded driver's file open, so the old
rem one is moved aside, and the new one loads at the next start.
set D=%SystemRoot%\System32\drivers
fc /b %G%\hwvsinput.sys %D%\hwvsinput.sys >nul 2>&1 || (
  del %D%\hwvsinput.old >nul 2>&1
  move /y %D%\hwvsinput.sys %D%\hwvsinput.old >nul
  copy /y %G%\hwvsinput.sys %D%\hwvsinput.sys
)
rem Phone storage as drives: WinFsp, then the HyperWinFiles service on it (guest/files/hwfiles.c).
rem It runs from its own folder, so the app can send a new one while it runs.
reg query HKLM\SOFTWARE\WOW6432Node\WinFsp /v InstallDir >nul 2>&1 || msiexec /i %G%\winfsp.msi /qn /norestart
if not exist C:\HyperWin\files mkdir C:\HyperWin\files
copy /y %G%\hwfiles.exe C:\HyperWin\files\hwfiles.exe
sc query HyperWinFiles >nul 2>&1 || sc create HyperWinFiles type= own start= auto binPath= C:\HyperWin\files\hwfiles.exe DisplayName= "HyperWin phone storage"
sc failure HyperWinFiles reset= 60 actions= restart/2000 >nul
sc start HyperWinFiles
rem Speakers and microphone over the shared window (audio/README.md), and the touch screen and
rem touchpad (mouse/README.md), each installed again when its driver changes. The script makes and
rem signs the package's catalog; for sound it also takes out the older Scream card.
fc /b %G%\hwaudio\hwaudio.sys %SystemRoot%\System32\drivers\hwaudio.sys >nul 2>&1 || powershell -NoProfile -ExecutionPolicy Bypass -File %G%\install-driver.ps1 hwaudio Root\HwAudio
fc /b %G%\hwhid\hwhid.sys %SystemRoot%\System32\drivers\hwhid.sys >nul 2>&1 || powershell -NoProfile -ExecutionPolicy Bypass -File %G%\install-driver.ps1 hwhid Root\HwHid
rem The Vulkan loader finds drivers through this key: value name = the driver's manifest, data 0.
reg add HKLM\SOFTWARE\Khronos\Vulkan\Drivers /v %G%\vulkan_hwgpu.json /t REG_DWORD /d 0 /f
rem Send To entries that put DXVK, vkd3d-proton or Zink beside a program (gpu\dx\use.cmd).
for /d %%u in (C:\Users\*) do if exist "%%u\AppData\Roaming\Microsoft\Windows\SendTo" call :sendto "%%u\AppData\Roaming\Microsoft\Windows\SendTo"
call :sendto "C:\Users\Default\AppData\Roaming\Microsoft\Windows\SendTo"
exit /b 0

:sendto
if not exist "%~1" mkdir "%~1"
(echo @call %G%\dx\use.cmd dxvk %%*^& timeout /t 3 ^>nul)> "%~1\GPU - DXVK, Direct3D 8 to 11.cmd"
(echo @call %G%\dx\use.cmd sarek %%*^& timeout /t 3 ^>nul)> "%~1\GPU - DXVK-Sarek, Direct3D 8 to 11.cmd"
(echo @call %G%\dx\use.cmd vkd3d %%*^& timeout /t 3 ^>nul)> "%~1\GPU - vkd3d-proton, Direct3D 12.cmd"
(echo @call %G%\dx\use.cmd zink %%*^& timeout /t 3 ^>nul)> "%~1\GPU - Zink, OpenGL.cmd"
(echo @call %G%\dx\use.cmd hwd3d %%*^& timeout /t 3 ^>nul)> "%~1\GPU - HwD3D, Direct3D 11 on the phone.cmd"
(echo @call %G%\dx\use.cmd off %%*^& timeout /t 3 ^>nul)> "%~1\GPU - Windows default.cmd"
exit /b 0
