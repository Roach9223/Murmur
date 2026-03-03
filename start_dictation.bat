@echo off
title Whisper Dictation
cd /d "%~dp0"
call venv\Scripts\activate.bat
python app.py %*
pause
