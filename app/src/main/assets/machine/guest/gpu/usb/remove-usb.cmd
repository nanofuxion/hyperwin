@echo off
rem Takes the raw USB ring driver out again.
sc stop hwusbraw >nul 2>&1
sc delete hwusbraw >nul 2>&1
del %SystemRoot%\System32\drivers\hwusbraw.sys >nul 2>&1
schtasks /end /tn HyperWinUsbWatch >nul 2>&1
schtasks /delete /f /tn HyperWinUsbWatch >nul 2>&1
