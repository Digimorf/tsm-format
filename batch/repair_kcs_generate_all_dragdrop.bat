@echo off
setlocal

REM ============================================================
REM repair_kcs_generate_all_dragdrop.bat
REM
REM Drag-and-drop a dirty WAV file.
REM Creates repaired WAV, TSM, and verification WAV for:
REM     kcs_strict
REM     kcs_adaptive
REM     kcs_extended
REM ============================================================

set "SCRIPT_DIR=%~dp0"
set "BIN_DIR=%SCRIPT_DIR%bin"

if "%~1"=="" (
    echo.
    echo Drag and drop a WAV file onto this batch.
    echo.
    pause
    exit /b
)

set "INPUT=%~1"
set "BASE=%~dpn1"

set "TIME_UNIT_NS=4000"
set "EDGE_THRESHOLD=0.35"
set "MIN_DELTA_TICKS=1"
set "GAIN=8.0"
set "CLIP_LEVEL=1.0"
set "RAW_ESCAPE_THRESHOLD=5000"
set "ZERO_THRESHOLD=0.03"
set "MIN_ZERO_MS=20"

for %%P in (kcs_strict kcs_adaptive kcs_extended) do (
    echo.
    echo ============================================
    echo Repair profile: %%P
    echo ============================================

    set "REPAIRED_WAV=%BASE%_repair_%%P.wav"
    set "OUTPUT_TSM=%BASE%_repair_%%P.tsm"
    set "VERIFY_WAV=%BASE%_repair_%%P_rendered.wav"

    "%BIN_DIR%\wav_repair_kcs.exe" "%INPUT%" "%BASE%_repair_%%P.wav" %%P %TIME_UNIT_NS% %EDGE_THRESHOLD% %MIN_DELTA_TICKS% %GAIN% %CLIP_LEVEL% %RAW_ESCAPE_THRESHOLD% 22000
    if errorlevel 1 pause & exit /b 1

    "%BIN_DIR%\wav2tsm_indexed_v5.exe" "%BASE%_repair_%%P.wav" "%BASE%_repair_%%P.tsm" %TIME_UNIT_NS% 16 %EDGE_THRESHOLD% %MIN_DELTA_TICKS% %GAIN% %CLIP_LEVEL% %ZERO_THRESHOLD% %MIN_ZERO_MS%
    if errorlevel 1 pause & exit /b 1

    "%BIN_DIR%\tsm2wav_v5.exe" "%BASE%_repair_%%P.tsm" "%BASE%_repair_%%P_rendered.wav" 48000 22000
    if errorlevel 1 pause & exit /b 1
)

echo.
echo Repair outputs created next to:
echo %INPUT%
echo.
pause
