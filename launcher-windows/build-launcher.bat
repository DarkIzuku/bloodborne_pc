@echo off
rem Double-click: build the launcher, optionally upgrade an install and make a zip.
set PS=powershell
where pwsh >nul 2>nul && set PS=pwsh
%PS% -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-launcher.ps1" %*
pause
