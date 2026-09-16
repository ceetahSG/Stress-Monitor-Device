@echo off
cd /d "%~dp0"
echo Installing/checking Python dependencies...
python -m pip install -r requirements.txt
echo.
echo Starting Stress Monitor...
python app.py
pause
