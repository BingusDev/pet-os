@echo off
rem AlicePET Jack In: runs the app from this drive. Nothing is installed.
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File "%~dp0JackIn.ps1"
