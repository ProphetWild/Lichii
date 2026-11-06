@echo off
rem Launch Ocarina of Time on a build with the live debug server on 127.0.0.1:6545.
rem   run-oot.cmd [exe]          default: build\zen5\bin\lichii.exe
rem Environment set before calling (e.g. EDEN_HDR_OUTPUT=1) is passed through.
setlocal
set "ROOT=%~dp0.."
set "EXE=%~1"
if "%EXE%"=="" set "EXE=%ROOT%\build\zen5\bin\lichii.exe"
set "GAME=D:\Switch\The Legend of Zelda Ocarina of Time [0400486024dec000][v131072].nup"
if not defined EDEN_NX2_DEBUG_SERVER set "EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545"
if not defined EDEN_NX2_DEBUG_DIR set "EDEN_NX2_DEBUG_DIR=%ROOT%\build\debug"
if not exist "%EDEN_NX2_DEBUG_DIR%" mkdir "%EDEN_NX2_DEBUG_DIR%"
for %%I in ("%EXE%") do set "EXE_DIR=%%~dpI"
cd /d "%EXE_DIR%"
rem Fullscreen unless OOT_WINDOWED=1 (a game path without -f forces windowed).
set "FULLSCREEN=-f"
if "%OOT_WINDOWED%"=="1" set "FULLSCREEN="
start "" "%EXE%" %FULLSCREEN% -g "%GAME%"
endlocal
