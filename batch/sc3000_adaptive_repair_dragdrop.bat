@echo off
setlocal

REM ============================================================
REM sc3000_adaptive_repair_dragdrop.bat
REM
REM Drag-and-drop an SC-3000 WAV file.
REM Creates:
REM   <name>_sc3000_adaptive_repaired.wav
REM   <name>_sc3000_adaptive_repair_report.txt
REM ============================================================

set "SCRIPT_DIR=%~dp0"
set "BIN_DIR=%SCRIPT_DIR%bin"

if "%~1"=="" (
    echo Drag and drop an SC-3000 WAV file onto this batch.
    pause
    exit /b
)

set "INPUT=%~1"
set "OUTPUT=%~dpn1_sc3000_adaptive_repaired.wav"
set "REPORT=%~dpn1_sc3000_adaptive_repair_report.txt"

"%BIN_DIR%\sc3000_wav_adaptive_repair.exe" "%INPUT%" "%OUTPUT%" "%REPORT%" 4000 0.35 8.0 52

if errorlevel 1 (
    echo ERROR: SC-3000 adaptive repair failed.
    pause
    exit /b 1
)

echo.
echo Created:
echo %OUTPUT%
echo %REPORT%
echo.
pause
