@echo off
setlocal

REM ============================================================
REM build_v5_2_plus_repair.bat
REM
REM Complete TSM v5.2 compact escape + repair build.
REM ============================================================

set "SCRIPT_DIR=%~dp0"
set "SRC_DIR=%SCRIPT_DIR%..\src"
set "OBJ_DIR=%SCRIPT_DIR%..\obj"
set "BIN_DIR=%SCRIPT_DIR%..\bin"

if not exist "%OBJ_DIR%" mkdir "%OBJ_DIR%"
if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"

set CFLAGS=-std=c99 -O2 -Wall

echo Building TSM v5.2 compact escape + repair toolchain...

echo.
echo [1/9] Compiling tsm_v5.c
gcc %CFLAGS% -c "%SRC_DIR%\tsm_v5.c" -o "%OBJ_DIR%\tsm_v5.o"
if errorlevel 1 goto build_error

echo.
echo [2/9] Compiling wav_io_simple.c
gcc %CFLAGS% -c "%SRC_DIR%\wav_io_simple.c" -o "%OBJ_DIR%\wav_io_simple.o"
if errorlevel 1 goto build_error

echo.
echo [3/9] Compiling indexed_common.c
gcc %CFLAGS% -c "%SRC_DIR%\indexed_common.c" -o "%OBJ_DIR%\indexed_common.o"
if errorlevel 1 goto build_error

echo.
echo [4/9] Linking wav_tsm_advisor_v5_indexed.exe
gcc %CFLAGS% ^
    "%SRC_DIR%\wav_tsm_advisor_v5_indexed.c" ^
    "%OBJ_DIR%\tsm_v5.o" ^
    "%OBJ_DIR%\wav_io_simple.o" ^
    "%OBJ_DIR%\indexed_common.o" ^
    -o "%BIN_DIR%\wav_tsm_advisor_v5_indexed.exe"
if errorlevel 1 goto build_error

echo.
echo [5/9] Linking wav2tsm_indexed_v5.exe
gcc %CFLAGS% ^
    "%SRC_DIR%\wav2tsm_indexed_v5.c" ^
    "%OBJ_DIR%\tsm_v5.o" ^
    "%OBJ_DIR%\wav_io_simple.o" ^
    "%OBJ_DIR%\indexed_common.o" ^
    -o "%BIN_DIR%\wav2tsm_indexed_v5.exe"
if errorlevel 1 goto build_error

echo.
echo [6/9] Linking tsm2wav_v5.exe
gcc %CFLAGS% ^
    "%SRC_DIR%\tsm2wav_v5.c" ^
    "%OBJ_DIR%\tsm_v5.o" ^
    "%OBJ_DIR%\wav_io_simple.o" ^
    -o "%BIN_DIR%\tsm2wav_v5.exe"
if errorlevel 1 goto build_error

echo.
echo [7/9] Linking wav_gap_scan.exe
gcc %CFLAGS% ^
    "%SRC_DIR%\wav_gap_scan.c" ^
    "%OBJ_DIR%\tsm_v5.o" ^
    "%OBJ_DIR%\wav_io_simple.o" ^
    -o "%BIN_DIR%\wav_gap_scan.exe"
if errorlevel 1 goto build_error

echo.
echo [8/9] Linking tsm_v5_indexed_audit.exe
gcc %CFLAGS% ^
    "%SRC_DIR%\tsm_v5_indexed_audit.c" ^
    "%OBJ_DIR%\tsm_v5.o" ^
    -o "%BIN_DIR%\tsm_v5_indexed_audit.exe"
if errorlevel 1 goto build_error

echo.
echo [9/9] Linking wav_repair_kcs.exe
gcc %CFLAGS% ^
    "%SRC_DIR%\wav_repair_kcs.c" ^
    "%OBJ_DIR%\tsm_v5.o" ^
    "%OBJ_DIR%\wav_io_simple.o" ^
    "%OBJ_DIR%\indexed_common.o" ^
    -o "%BIN_DIR%\wav_repair_kcs.exe"
if errorlevel 1 goto build_error


echo.
echo [extra] Linking wav_repair_kcs_framed.exe
gcc %CFLAGS% ^
    "%SRC_DIR%\wav_repair_kcs_framed.c" ^
    "%OBJ_DIR%\tsm_v5.o" ^
    "%OBJ_DIR%\wav_io_simple.o" ^
    "%OBJ_DIR%\indexed_common.o" ^
    -o "%BIN_DIR%\wav_repair_kcs_framed.exe"
if errorlevel 1 goto build_error


echo.
echo [extra] Linking wav2bits_kcs.exe
gcc %CFLAGS% ^
    "%SRC_DIR%\wav2bits_kcs.c" ^
    "%OBJ_DIR%\tsm_v5.o" ^
    "%OBJ_DIR%\wav_io_simple.o" ^
    "%OBJ_DIR%\indexed_common.o" ^
    -o "%BIN_DIR%\wav2bits_kcs.exe"
if errorlevel 1 goto build_error


echo.
echo Build completed successfully.
echo.
pause
exit /b 0

:build_error
echo.
echo ERROR: build failed.
echo.
pause
exit /b 1
