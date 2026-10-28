@echo off
rem Starts LittleBigPlanet 3 (native). build.bat only compiles; this runs it.
rem Logs go to analysis\play_log.txt (errors, crashes) and analysis\play_stdout.txt.

rem --- options -------------------------------------------------------------
rem Skip the startup logos and live-action intro: 1 = skip, 0 = play them.
set SKIP_INTRO=1
rem ---------------------------------------------------------------------------

cd /d "%~dp0"
if not exist build\lbp3.exe (
    echo build\lbp3.exe not found - run build.bat first.
    pause
    exit /b 1
)
set EXTRA=
if "%SKIP_INTRO%"=="1" set EXTRA=--skip-intro
echo Starting LittleBigPlanet 3... close the game window to quit.
if not exist analysis mkdir analysis
rem Keep the logs of the last three sessions (play_log.1.txt is the previous one).
if exist analysis\play_log.2.txt move /y analysis\play_log.2.txt analysis\play_log.3.txt >nul
if exist analysis\play_log.1.txt move /y analysis\play_log.1.txt analysis\play_log.2.txt >nul
if exist analysis\play_log.txt move /y analysis\play_log.txt analysis\play_log.1.txt >nul
build\lbp3.exe --run %EXTRA% > analysis\play_stdout.txt 2> analysis\play_log.txt
if errorlevel 1 (
    echo.
    echo The game stopped with an error. Details: analysis\play_log.txt
    pause
)
