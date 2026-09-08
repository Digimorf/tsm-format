@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "BIN_DIR=%SCRIPT_DIR%..\bin"

if "%~1"=="" (
    echo Drag and drop a TSM file onto this batch.
    pause
    exit /b
)

set "INPUT=%~1"
set "REPORT=%~dpn1_audit.txt"

"%BIN_DIR%\tsm_v5_indexed_audit.exe" "%INPUT%" > "%REPORT%"

if errorlevel 1 (
    echo ERROR: audit failed.
    pause
    exit /b 1
)

echo Audit report created:
echo %REPORT%
pause
