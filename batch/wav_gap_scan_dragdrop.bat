@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "BIN_DIR=%SCRIPT_DIR%bin"

if "%~1"=="" (
    echo Drag and drop a WAV file onto this batch.
    pause
    exit /b
)

set "INPUT=%~1"
set "REPORT=%~dpn1_gap_scan.txt"

"%BIN_DIR%\wav_gap_scan.exe" "%INPUT%" 0.03 20 > "%REPORT%"

if errorlevel 1 (
    echo ERROR: gap scan failed.
    pause
    exit /b 1
)

echo Gap report created:
echo %REPORT%
pause
