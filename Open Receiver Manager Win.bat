@echo off
rem RADIO DAYS SDR - Windows 11: Receiver Manager only (the radio is NOT started). Double-click this file.
rem Opens the page in your browser every time. For normal listening you do not need this:
rem "Start RADIO DAYS Win.bat" already opens it.  If a Receiver Manager is already running,
rem no second one is started; the existing page is opened instead.
rem (Messages in this file are plain English on purpose: batch files and Japanese text do not mix reliably.)
setlocal EnableExtensions
title RADIO DAYS SDR - Receiver Manager
chcp 65001 >nul
set "PYTHONUTF8=1"
cd /d "%~dp0mac" || goto :nofolder

set "PY=.venv\Scripts\python.exe"
if exist "%PY%" goto :run
set "PY="
py -3 -c "import sys; sys.exit(0 if sys.version_info>=(3,9) else 1)" >nul 2>nul && set "PY=py -3"
if not defined PY python -c "import sys; sys.exit(0 if sys.version_info>=(3,9) else 1)" >nul 2>nul && set "PY=python"
if not defined PY goto :nopython

:run
%PY% receiver_manager.py --open %*
set "RC=%ERRORLEVEL%"
echo.
pause
exit /b %RC%

:nofolder
echo Cannot find the "mac" folder next to this file. Keep the whole RADIODAYS-SDR folder together.
goto :fail
:nopython
echo Python 3.9 or newer was not found. Install Python 3 from https://www.python.org/downloads/windows/
echo (tick "Add python.exe to PATH" in the installer), or run "Start RADIO DAYS Win.bat" once.
:fail
echo.
pause
exit /b 1
