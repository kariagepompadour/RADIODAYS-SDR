@echo off
rem RADIO DAYS SDR - Windows 11 launcher: double-click this file.
rem Starts the radio and opens the Receiver Manager (http://127.0.0.1:8780/) in your browser every time.
rem Closing the browser does not stop the radio; close this window or press Ctrl+C to stop it.
rem (Messages in this file are plain English on purpose: batch files and Japanese text do not mix reliably.)
setlocal EnableExtensions
title RADIO DAYS SDR
chcp 65001 >nul
set "PYTHONUTF8=1"
cd /d "%~dp0mac" || goto :nofolder

set "VPY=.venv\Scripts\python.exe"
if exist "%VPY%" goto :havevenv

echo First-time setup (a few minutes, needs the internet)...
call :findpython
if not defined PY goto :nopython
%PY% -m venv .venv
if errorlevel 1 goto :venvfail
"%VPY%" -m pip install -q -r requirements.txt
if errorlevel 1 goto :pipfail

:havevenv
if exist "vendor\kiwiclient\kiwi\__init__.py" goto :havekiwi
echo Fetching the KiwiSDR client library (pinned version, SHA-256 checked)...
"%VPY%" tools\fetch_kiwiclient.py
if errorlevel 1 goto :kiwifail

:havekiwi
echo Starting RADIO DAYS SDR. Stop with Ctrl+C or by closing this window.
"%VPY%" radiodays_sdr.py --open-manager %*
set "RC=%ERRORLEVEL%"
echo.
echo Stopped (code %RC%).
pause
exit /b %RC%

:findpython
set "PY="
py -3 -c "import sys; sys.exit(0 if sys.version_info>=(3,9) else 1)" >nul 2>nul && (set "PY=py -3" & exit /b 0)
python -c "import sys; sys.exit(0 if sys.version_info>=(3,9) else 1)" >nul 2>nul && (set "PY=python" & exit /b 0)
exit /b 1

:nofolder
echo Cannot find the "mac" folder next to this file. Keep the whole RADIODAYS-SDR folder together.
goto :fail
:nopython
echo Python 3.9 or newer was not found. Install Python 3 from https://www.python.org/downloads/windows/
echo (tick "Add python.exe to PATH" in the installer), then double-click this file again.
goto :fail
:venvfail
echo Could not create the Python virtual environment (mac\.venv).
goto :fail
:pipfail
echo Installing the Python packages failed (check the internet connection). Removing the incomplete mac\.venv.
rmdir /s /q .venv
goto :fail
:kiwifail
echo Could not fetch the KiwiSDR client library (check the internet connection).
goto :fail
:fail
echo.
pause
exit /b 1
