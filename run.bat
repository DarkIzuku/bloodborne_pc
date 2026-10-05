@echo off
rem Windows counterpart of run.sh (scripts/run_windows.py): builds the port through MSYS2 when
rem needed and starts the game. Usage: run.bat [--game-dir DIR] [bb-probe options...]
rem MSYS2 is expected in C:\msys64 (set BB_MSYS2 otherwise); see README "Windows".
setlocal
if not defined BB_MSYS2 set "BB_MSYS2=C:\msys64"
rem Packaged builds put an embeddable Python runtime under tools\python. Prefer it so a
rem prebuilt Bloodborne PC package does not require MSYS2 on the player's machine.
set "BB_PYTHON=%~dp0tools\python\python.exe"
if not exist "%BB_PYTHON%" set "BB_PYTHON=%BB_MSYS2%\clang64\bin\python.exe"
if not exist "%BB_PYTHON%" (
    if defined BB_LOG_FILE (
        >>"%BB_LOG_FILE%" echo Python runtime not found. Use the packaged build, or install MSYS2 and set BB_MSYS2.
    ) else (
        echo Python runtime not found. Use the packaged build, or install MSYS2 and set BB_MSYS2.
    )
    exit /b 1
)
if defined BB_LOG_FILE (
    "%BB_PYTHON%" "%~dp0scripts\run_windows.py" %* >>"%BB_LOG_FILE%" 2>&1
) else (
    "%BB_PYTHON%" "%~dp0scripts\run_windows.py" %*
)
exit /b %ERRORLEVEL%
