@echo off
rem Puts a graphics layer beside a program, so it draws through the HyperWin GPU (Vulkan):
rem   dxvk   Direct3D 8, 9, 10 and 11 (DXVK)
rem   sarek  the same with DXVK-Sarek, for drivers DXVK refuses (it runs on Mali without Vortek)
rem   vkd3d  Direct3D 12 (vkd3d-proton). Known exception to "no DLLs beside a program", for Direct3D 12 only:
rem          the files stay in this folder and the program gets a small <program>.exe.manifest that points at them
rem          (Windows' own d3d12.dll is not touched; PATH and .local do not work, see wddm/README.md). A program with its
rem          own manifest gets copies of the DLLs beside it instead.
rem   zink   OpenGL (Mesa Zink)
rem   hwd3d  Direct3D 11 drawn by DXVK on the phone itself (HwD3D, d3d/README.md), full screen
rem   off    takes ours away again, so the program uses Windows' own
rem   use.cmd <layer> <program.exe or its folder> ...
rem Explorer has these under Send To. Layers add up: dxvk then vkd3d gives both. Windows' own files
rem are never touched. The DLLs are ARM64X: ARM64 and x64 programs both work. A 32-bit program given by
rem its file name gets the 32-bit Zink (OpenGL); the Direct3D layers are not built for 32-bit yet.
setlocal EnableExtensions
set "LAYERS=%~dp0"
set "MODE=%~1"
if "%~2"=="" (
  echo Usage: %~nx0 dxvk^|sarek^|vkd3d^|zink^|hwd3d^|off program.exe ...
  exit /b 1
)
shift
:each
if "%~1"=="" exit /b 0
set "PROG="
if exist "%~1\*" (set "DEST=%~f1") else set "DEST=%~dp1." & set "PROG=%~f1"
for %%d in ("%DEST%") do set "DEST=%%~fd"
set "ZK=zink"
if not exist "%~1\*" if exist "%~1" for /f %%m in ('powershell -NoProfile -Command "$f=[IO.File]::OpenRead('%~f1');$b=New-Object byte[] 4096;[void]$f.Read($b,0,4096);$f.Close();[BitConverter]::ToUInt16($b,[BitConverter]::ToInt32($b,60)+4)"') do if "%%m"=="332" set "ZK=zink-x86"
if "%ZK%"=="zink-x86" if not "%MODE%"=="zink" if not "%MODE%"=="off" (
  echo %~1 is a 32-bit program: only zink and off are built for it.
  shift
  goto each
)
call :%MODE% "%DEST%" 2>nul || (
  echo Unknown layer "%MODE%". Use dxvk, sarek, vkd3d, zink, hwd3d or off.
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
rem Through the manifest when the program has none of its own: nothing is copied into its folder.
if defined PROG powershell -NoProfile -ExecutionPolicy Bypass -File "%LAYERS%hasmanifest.ps1" "%PROG%" && (
  (echo ^<?xml version="1.0" encoding="UTF-8" standalone="yes"?^>
   echo ^<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0"^>
   echo   ^<file name="d3d12.dll" loadFrom="%LAYERS%vkd3d\d3d12.dll"/^>
   echo   ^<file name="d3d12core.dll" loadFrom="%LAYERS%vkd3d\d3d12core.dll"/^>
   echo   ^<file name="dxgi.dll" loadFrom="%LAYERS%dxvk\dxgi.dll"/^>
   echo ^</assembly^>) > "%PROG%.manifest"
  copy /b "%PROG%+,," "%PROG%" >nul 2>&1
  echo vkd3d-proton: Direct3D 12 for %PROG% through %PROG%.manifest, files stay in %LAYERS%vkd3d
  exit /b 0
)
for %%f in (d3d12 d3d12core) do copy /y "%LAYERS%vkd3d\%%f.dll" "%~1\" >nul
copy /y "%LAYERS%dxvk\dxgi.dll" "%~1\" >nul
echo vkd3d-proton: Direct3D 12 in %~1 (copies: the program has its own manifest or is a folder)
exit /b 0

:zink
for %%f in (opengl32 libgallium_wgl) do copy /y "%LAYERS%%ZK%\%%f.dll" "%~1\" >nul
echo Zink: OpenGL in %~1
exit /b 0

:hwd3d
for %%f in (d3d11 dxgi) do copy /y "%LAYERS%hwd3d\%%f.dll" "%~1\" >nul
echo HwD3D: Direct3D 11 on the phone in %~1
exit /b 0

:off
rem Our manifest for a program goes too.
if defined PROG if exist "%PROG%.manifest" findstr /c:"%LAYERS%vkd3d" "%PROG%.manifest" >nul 2>&1 && (
  del "%PROG%.manifest"
  copy /b "%PROG%+,," "%PROG%" >nul 2>&1
)
rem Only our own copies go. A program's own file with one of these names stays.
for %%l in (dxvk sarek vkd3d zink zink-x86 hwd3d) do for %%f in ("%LAYERS%%%l\*.dll") do (
  if exist "%~1\%%~nxf" fc /b "%%~f" "%~1\%%~nxf" >nul && del "%~1\%%~nxf"
)
echo Windows' own graphics in %~1
exit /b 0
