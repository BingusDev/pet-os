@echo off
rem AlicePET Jack In: runs the app from this drive. Nothing is installed.
rem The app window has no console. If PowerShell can't start it (policy, a
rem broken file, a crash at startup) a message box says why, the error goes to
rem %TEMP%\JackIn-error.txt, and this shows how to run it by hand.
set "JACKIN_PS1=%~dp0JackIn.ps1"
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -Command "$p = $env:JACKIN_PS1; try { & $p; if ($LASTEXITCODE) { exit $LASTEXITCODE } } catch { $m = 'Jack In could not start:' + [char]13 + [char]10 + [char]13 + [char]10 + $_ + [char]13 + [char]10 + [char]13 + [char]10 + 'Run it by hand in PowerShell:' + [char]13 + [char]10 + '  powershell -NoProfile -ExecutionPolicy Bypass -STA -File ' + [char]34 + $p + [char]34; try { [IO.File]::WriteAllText((Join-Path $env:TEMP 'JackIn-error.txt'), $m) } catch { }; Add-Type -AssemblyName System.Windows.Forms; [void][System.Windows.Forms.MessageBox]::Show($m, 'Jack In', 'OK', 'Error'); exit 1 }"
if errorlevel 1 (
    echo Jack In could not start PowerShell.
    echo Run it by hand:
    echo   powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%JACKIN_PS1%"
    pause
)
