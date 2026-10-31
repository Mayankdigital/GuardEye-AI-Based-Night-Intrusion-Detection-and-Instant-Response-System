@echo off
title Covered Face Detection Server
cd /d "%~dp0"

REM ====== settings ======
set API_KEY=CHANGE_ME_TO_A_LONG_RANDOM_KEY
set MODEL_PATH=best.pt
set IMG_SIZE=640
set CONF=0.5
REM Python environment is kept OUTSIDE OneDrive so OneDrive doesn't sync thousands of files
set VENV=%USERPROFILE%\mask_server_venv
REM ======================

if not exist "%MODEL_PATH%" (
    echo ERROR: %MODEL_PATH% not found in this folder.
    pause & exit /b 1
)

where python >nul 2>nul
if errorlevel 1 (
    echo ERROR: Python is not installed.
    echo Install Python 3.11 or 3.12 from https://www.python.org/downloads/
    echo and tick "Add python.exe to PATH" during install. Then run this file again.
    pause & exit /b 1
)

if not exist "%VENV%\installed.ok" (
    echo === First run: installing packages, this takes 5-15 minutes ===
    python -m venv "%VENV%"
    "%VENV%\Scripts\python" -m pip install --upgrade pip
    "%VENV%\Scripts\python" -m pip install ultralytics fastapi "uvicorn[standard]" python-multipart
    if errorlevel 1 (
        echo ERROR: package install failed. Check your internet and run again.
        pause & exit /b 1
    )
    echo ok> "%VENV%\installed.ok"
)

echo.
echo ================================================================
echo  Your laptop IP address (use the Wi-Fi one, e.g. 192.168.x.x):
ipconfig | findstr /i "IPv4"
echo.
echo  On the Raspberry Pi, in pi\config.env:
echo      SERVER_URL=ws://YOUR_IP:7860
echo      API_KEY=%API_KEY%
echo.
echo  Live viewer (phone or laptop on same Wi-Fi):
echo      http://YOUR_IP:7860/?key=%API_KEY%
echo  On this laptop:  http://localhost:7860/?key=%API_KEY%
echo.
echo  If Windows Firewall pops up: tick "Private networks" and click Allow.
echo  Press Ctrl+C to stop the server.
echo ================================================================
echo.

"%VENV%\Scripts\python" -m uvicorn app:app --host 0.0.0.0 --port 7860
pause
