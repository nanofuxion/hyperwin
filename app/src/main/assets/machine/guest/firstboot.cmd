@echo off
rem HyperWin guest setup, copied to C:\HyperWin by machine prep.
rem "firstboot.cmd specialize" runs during Windows setup; with no argument it runs at first logon.

rem Windows setup demotes hwvsinput to demand start (StartOverride). That is fine: it loads on demand.
if "%1"=="specialize" exit /b 0

rem A boot cut short must not divert into the recovery environment, which this VM cannot run.
bcdedit /set {current} recoveryenabled no
bcdedit /set {current} bootstatuspolicy ignoreallfailures

rem A VM has nothing to save by sleeping, and a blanked display looks like a dead desktop.
powercfg /hibernate off
powercfg /change monitor-timeout-ac 0
powercfg /change monitor-timeout-dc 0
powercfg /change standby-timeout-ac 0
powercfg /change standby-timeout-dc 0
powercfg /change hibernate-timeout-ac 0
powercfg /change hibernate-timeout-dc 0

rem Trust the HyperWin package certificate so its driver packages install without a prompt.
certutil -addstore -f Root C:\HyperWin\hyperwin-ca.cer
certutil -addstore -f TrustedPublisher C:\HyperWin\hyperwin-pkg.cer

rem Guest agent: network (Wintun over vsock to the app), commands and file copies.
sc create HyperWinAgent binPath= "C:\HyperWin\hwagent.exe" start= auto DisplayName= "HyperWin guest agent"
sc failure HyperWinAgent reset= 60 actions= restart/2000/restart/2000/restart/2000
sc start HyperWinAgent

rem Sound: the Scream virtual sound card sends PCM to the app at 10.0.2.2:4010.
reg add "HKLM\SYSTEM\CurrentControlSet\Services\Scream\Options" /v UnicastIPv4 /t REG_SZ /d 10.0.2.2 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Services\Scream\Options" /v UnicastPort /t REG_DWORD /d 4010 /f
C:\HyperWin\audio\devcon.exe install C:\HyperWin\audio\Scream.inf *Scream

rem GPU: Vulkan over the shared window, rendered by the phone (gpu/README.md).
call C:\HyperWin\gpu-install.cmd
