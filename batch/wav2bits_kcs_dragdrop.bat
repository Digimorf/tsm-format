@echo off
setlocal

REM ============================================================
REM wav2bits_kcs_dragdrop.bat
REM
REM Drag-and-drop a KCS-like WAV file.
REM It creates a text file:
REM
REM   0 = one 1200 Hz full wave
REM   1 = one 2400 Hz full wave
REM     = approximately 833.33 us silence
REM ============================================================

set "SCRIPT_DIR=%~dp0"
set "BIN_DIR=%SCRIPT_DIR%bin"

if "%~1"=="" (
    echo Drag and drop a WAV file onto this batch.
    pause
    exit /b
)

set "INPUT=%~1"
set "OUTPUT=%~dpn1_kcs_bits.txt"

"%BIN_DIR%\wav2bits_kcs.exe" "%INPUT%" "%OUTPUT%" 4000 0.35 1 8.0 1.0 52 104 208 0.35

if errorlevel 1 (
    echo ERROR: wav2bits_kcs failed.
    pause
    exit /b 1
)

echo.
echo Created:
echo %OUTPUT%
echo.
pause
