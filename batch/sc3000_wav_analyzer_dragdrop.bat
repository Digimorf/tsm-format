@echo off
setlocal

REM ============================================================
REM sc3000_wav_analyzer_dragdrop.bat
REM
REM Drag-and-drop an SC-3000 cassette WAV file.
REM Creates:
REM   <name>_sc3000_analysis.txt
REM ============================================================

set "SCRIPT_DIR=%~dp0"
set "BIN_DIR=%SCRIPT_DIR%..\bin"

if "%~1"=="" (
    echo Drag and drop an SC-3000 WAV file onto this batch.
    pause
    exit /b
)

set "INPUT=%~1"
set "REPORT=%~dpn1_sc3000_analysis.txt"

"%BIN_DIR%\sc3000_wav_analyzer.exe" "%INPUT%" "%REPORT%" 4000 0.35 1 8.0 1.0 52 104 0.35

if errorlevel 1 (
    echo ERROR: SC-3000 WAV analysis failed.
    pause
    exit /b 1
)

echo.
echo Created:
echo %REPORT%
echo.
pause
