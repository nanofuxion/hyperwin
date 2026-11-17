@echo off
rem Installs or updates the HyperWin GPU in Windows (gpu/README.md): the ring driver hwgpu.sys and
rem the Vulkan driver vulkan_hwgpu.dll. Run as administrator or SYSTEM, by firstboot.cmd on new
rem machines and by the app on older ones. Safe to run again.
set G=C:\HyperWin\gpu
sc stop hwgpu >nul 2>&1
copy /y %G%\hwgpu.sys %SystemRoot%\System32\drivers\hwgpu.sys
sc query hwgpu >nul 2>&1 || sc create hwgpu type= kernel start= auto binPath= System32\drivers\hwgpu.sys DisplayName= "HyperWin GPU ring"
sc config hwgpu start= auto
sc start hwgpu
rem Apps link to the Vulkan loader, which Windows does not ship. Keep one already installed.
if not exist %SystemRoot%\System32\vulkan-1.dll copy /y %G%\vulkan-1.dll %SystemRoot%\System32\vulkan-1.dll
rem The Vulkan loader finds drivers through this key: value name = the driver's manifest, data 0.
reg add HKLM\SOFTWARE\Khronos\Vulkan\Drivers /v %G%\vulkan_hwgpu.json /t REG_DWORD /d 0 /f
