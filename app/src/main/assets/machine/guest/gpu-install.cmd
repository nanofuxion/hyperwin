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
rem The same for 32-bit programs, which look in SysWOW64.
if not exist %SystemRoot%\SysWOW64\vulkan-1.dll echo.> %G%\vulkan-1-x86.ours
if exist %G%\vulkan-1-x86.ours copy /y %G%\vulkan-1_x86.dll %SystemRoot%\SysWOW64\vulkan-1.dll
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
rem The render-only GPU adapter (wddm/README.md). Windows runs it from the driver store, so what is
rem installed is remembered by the package's hash rather than by a copy in System32.
rem The firmware describes the GPU as ACPI\HWGP0001, with an interrupt line that Windows needs for the
rem screen. Older firmware had none, and the adapter was a root device; that one goes once the ACPI one exists.
set "GPUID=Root\HwGpuAdapter"
powershell -NoProfile -Command "if (Get-PnpDevice | ? HardwareID -contains 'ACPI\HWGP0001') { exit 0 } else { exit 1 }" && set "GPUID=ACPI\HWGP0001"
if "%GPUID%"=="ACPI\HWGP0001" powershell -NoProfile -Command "if (Get-PnpDevice | ? HardwareID -contains 'Root\HwGpuAdapter') { exit 0 } else { exit 1 }" && (%G%\hwdev.exe remove Root\HwGpuAdapter & del %G%\hwgpuw\installed.sha 2>nul)
set "NEWH="
for /f "skip=1 tokens=*" %%h in ('certutil -hashfile %G%\hwgpuw\hwgpuw.inf SHA256 2^>nul') do if not defined NEWH set "NEWH=%%h"
set "OLDH="
if exist %G%\hwgpuw\installed.sha set /p OLDH=<%G%\hwgpuw\installed.sha
if defined NEWH if not "%NEWH%"=="%OLDH%" powershell -NoProfile -ExecutionPolicy Bypass -File %G%\install-driver.ps1 hwgpuw %GPUID% && (echo %NEWH%> %G%\hwgpuw\installed.sha)
rem The Vulkan loader finds drivers through this key: value name = the driver's manifest, data 0.
rem Vulkan finds the driver through the adapter's VulkanDriverName. The global key is only for a
rem machine that has no adapter (wddm/README.md).
set HWAD=0
powershell -NoProfile -Command "if (Get-PnpDevice | ? HardwareID -contains '%GPUID%') { exit 0 } else { exit 1 }" && set HWAD=1
rem With the adapter, Vulkan is found through its own VulkanDriverName: the global key goes. Without it the key is added.
if "%HWAD%"=="1" (reg delete HKLM\SOFTWARE\Khronos\Vulkan\Drivers /v %G%\vulkan_hwgpu.json /f >nul 2>&1) else (reg add HKLM\SOFTWARE\Khronos\Vulkan\Drivers /v %G%\vulkan_hwgpu.json /t REG_DWORD /d 0 /f)
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
