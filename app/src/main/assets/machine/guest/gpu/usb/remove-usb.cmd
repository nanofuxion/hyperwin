@echo off
rem Takes the raw USB ring driver out again.
sc stop hwusbraw >nul 2>&1
sc delete hwusbraw >nul 2>&1
del %SystemRoot%\System32\drivers\hwusbraw.sys >nul 2>&1
