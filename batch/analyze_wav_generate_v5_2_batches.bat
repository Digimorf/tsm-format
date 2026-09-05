@echo off
setlocal

REM Drag-and-drop a WAV file.
REM The advisor generates conversion/render batches next to the WAV.

set "SCRIPT_DIR=%~dp0"
set "BIN_DIR=%SCRIPT_DIR%bin"

if "%~1"=="" (
    echo Drag and drop a WAV file onto this batch.
    pause
    exit /b
)

set "INPUT=%~1"
set "REPORT=%~dpn1_v5_2_advisor.txt"

"%BIN_DIR%\wav_tsm_advisor_v5_indexed.exe" "%INPUT%" 4000 0.35 1 8.0 1.0 0.03 20 > "%REPORT%"

if errorlevel 1 (
    echo ERROR: advisor failed.
    pause
    exit /b 1
)

echo Advisor report created:
echo %REPORT%
pause
