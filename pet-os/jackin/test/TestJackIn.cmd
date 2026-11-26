@echo off
rem Try the Jack In app on this PC without a PET: starts a stand-in PET
rem (a console you type into) and the app pointed at it. Nothing is installed.
start "Stand-in PET" powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0FakePet.ps1" -SaveVoice
timeout /t 2 /nobreak >nul
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File "%~dp0..\windows\JackIn.ps1" -Pet 127.0.0.1
