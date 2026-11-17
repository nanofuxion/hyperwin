@echo off
rem Puts a graphics layer beside a program, so it draws through the HyperWin GPU (Vulkan):
rem   dxvk   Direct3D 8, 9, 10 and 11 (DXVK)
rem   sarek  the same with DXVK-Sarek, for drivers DXVK refuses (it runs on Mali without Vortek)
rem   vkd3d  Direct3D 12 (vkd3d-proton, with DXVK's dxgi.dll)
rem   zink   OpenGL (Mesa Zink)
rem   off    takes ours away again, so the program uses Windows' own
rem   use.cmd <layer> <program.exe or its folder> ...
rem Explorer has these under Send To. Layers add up: dxvk then vkd3d gives both. Windows' own files
rem are never touched. The DLLs are ARM64X: ARM64 and x64 programs both work, 32-bit ones do not.
setlocal EnableExtensions
set "LAYERS=%~dp0"
set "MODE=%~1"
if "%~2"=="" (
  echo Usage: %~nx0 dxvk^|sarek^|vkd3d^|zink^|off program.exe ...
  exit /b 1
)
shift
:each
if "%~1"=="" exit /b 0
if exist "%~1\*" (set "DEST=%~f1") else set "DEST=%~dp1."
for %%d in ("%DEST%") do set "DEST=%%~fd"
call :%MODE% "%DEST%" 2>nul || (
  echo Unknown layer "%MODE%". Use dxvk, sarek, vkd3d, zink or off.
  exit /b 1
)
shift
goto each

:dxvk
for %%f in (d3d8 d3d9 d3d10core d3d11 dxgi) do copy /y "%LAYERS%dxvk\%%f.dll" "%~1\" >nul
echo DXVK: Direct3D 8 to 11 in %~1
exit /b 0

:sarek
for %%f in ("%LAYERS%sarek\*.dll") do copy /y "%%~f" "%~1\" >nul
echo DXVK-Sarek: Direct3D 8 to 11 in %~1
exit /b 0

:vkd3d
for %%f in (d3d12 d3d12core) do copy /y "%LAYERS%vkd3d\%%f.dll" "%~1\" >nul
copy /y "%LAYERS%dxvk\dxgi.dll" "%~1\" >nul
echo vkd3d-proton: Direct3D 12 in %~1
exit /b 0

:zink
for %%f in (opengl32 libgallium_wgl) do copy /y "%LAYERS%zink\%%f.dll" "%~1\" >nul
echo Zink: OpenGL in %~1
exit /b 0

:off
rem Only our own copies go. A program's own file with one of these names stays.
for %%l in (dxvk sarek vkd3d zink) do for %%f in ("%LAYERS%%%l\*.dll") do (
  if exist "%~1\%%~nxf" fc /b "%%~f" "%~1\%%~nxf" >nul && del "%~1\%%~nxf"
)
echo Windows' own graphics in %~1
exit /b 0
