@echo off
setlocal EnableExtensions
REM Robust wrapper for MAME castool. Edit SYSTEM as needed.
set "TOOL_DIR=%~dp0"
set "SYSTEM=fmsx"
if "%~1"=="" ( echo Drag and drop a tape/container file onto this batch. & pause & exit /b 1 )
set "INPUT=%*"
if "%INPUT:~0,1%"=="^"" set "INPUT=%INPUT:~1%"
if "%INPUT:~-1%"=="^"" set "INPUT=%INPUT:~0,-1%"
for %%F in ("%INPUT%") do (
    set "OUTPUT=%%~dpnF.wav"
    set "TEMP_INPUT=%TEMP%\tsm_castool_input%%~xF"
)
echo.
echo MAME castool conversion
echo ----------------------
echo System : %SYSTEM%
echo Input  : %INPUT%
echo Temp   : %TEMP_INPUT%
echo Output : %OUTPUT%
echo.
copy /Y "%INPUT%" "%TEMP_INPUT%" >nul || ( echo ERROR: cannot create temporary input file. & pause & exit /b 1 )
"%TOOL_DIR%castool.exe" convert "%SYSTEM%" "%TEMP_INPUT%" "%OUTPUT%"
set "RESULT=%ERRORLEVEL%"
del "%TEMP_INPUT%" >nul 2>nul
if not "%RESULT%"=="0" ( echo. & echo ERROR: castool conversion failed. & pause & exit /b 1 )
echo Conversion completed: %OUTPUT%
pause
