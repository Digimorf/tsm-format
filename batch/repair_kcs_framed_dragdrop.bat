@echo off
setlocal

REM ============================================================
REM repair_kcs_framed_dragdrop.bat
REM
REM Drag-and-drop a dirty KCS-like WAV file.
REM Creates:
REM   <name>_framed_kcs_phase_locked_repaired.wav
REM
REM This version forces every repaired bit to start with HIGH half-wave.
REM ============================================================

set "SCRIPT_DIR=%~dp0"
set "BIN_DIR=%SCRIPT_DIR%bin"

if "%~1"=="" (
    echo Drag and drop a WAV file onto this batch.
    pause
    exit /b
)

set "INPUT=%~1"
set "OUTPUT=%~dpn1_framed_kcs_phase_locked_repaired.wav"

"%BIN_DIR%\wav_repair_kcs_framed.exe" "%INPUT%" "%OUTPUT%" 4000 0.35 1 8.0 1.0 52 104 5000 0.35 1

if errorlevel 1 (
    echo ERROR: framed KCS phase-locked repair failed.
    pause
    exit /b 1
)

echo.
echo Created:
echo %OUTPUT%
echo.
echo Now convert the repaired WAV using the normal TSM v5.2 archival converter.
pause
