@echo off
rem PET-os Jack In: runs the app from this drive. Nothing is installed.
rem Starts the app normally and waits for its window. If it never comes up
rem (blocked by policy, crashed at startup), it asks Windows for admin (UAC)
rem and tries once more; if that fails too, the reasons are shown here and
rem kept in %TEMP%\JackIn-launch.txt. The launcher is the PowerShell text
rem after the marker at the end of this file (cmdlets only, so it also works
rem where PowerShell is locked to Constrained Language).
setlocal
set "JACKIN_PS1=%~dp0JackIn.ps1"
echo Starting PET-os Jack In... (this window closes when the app is up)
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$t = Get-Content -Raw -LiteralPath '%~f0'; Invoke-Expression ($t.Substring($t.LastIndexOf('#JACKIN-LAUNCHER#') + 17))"
if errorlevel 1 (
    echo.
    type "%TEMP%\JackIn-launch.txt"
    echo.
    pause
)
exit /b
#JACKIN-LAUNCHER#
$app = $env:JACKIN_PS1
$ready = Join-Path $env:TEMP 'JackIn-ready.txt'
$errFile = Join-Path $env:TEMP 'JackIn-error.txt'
$log = Join-Path $env:TEMP 'JackIn-launch.txt'
$lines = @("Jack In launcher, $(Get-Date)", "app: $app", "user: $env:USERDOMAIN\$env:USERNAME")

# Starts the app and waits for its window (it writes $ready when shown).
# Returns '' when it is up, else why not.
function Start-App([bool]$elevated) {
    Remove-Item -LiteralPath $ready, $errFile -Force -ErrorAction SilentlyContinue
    $q = [string][char]34
    $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-STA', '-WindowStyle', 'Hidden', '-File', ($q + $app + $q))
    if ($elevated) { $argv += '-Elevated' }
    try {
        if ($elevated) { $p = Start-Process -FilePath 'powershell.exe' -ArgumentList $argv -Verb RunAs -PassThru -ErrorAction Stop }
        else { $p = Start-Process -FilePath 'powershell.exe' -ArgumentList $argv -PassThru -ErrorAction Stop }
    } catch {
        return "could not start PowerShell: $($_.Exception.Message)"
    }
    $halfSeconds = $(if ($elevated) { 120 } else { 40 })
    for ($i = 0; $i -lt $halfSeconds; $i++) {
        if (Test-Path -LiteralPath $ready) { return '' }
        $gone = $false
        try { $gone = $p.HasExited } catch { }
        if ($gone) {
            Start-Sleep -Milliseconds 300
            if (Test-Path -LiteralPath $ready) { return '' }
            $code = '?'
            try { $code = $p.ExitCode } catch { }
            return "PowerShell exited (code $code) before the Jack In window opened"
        }
        Start-Sleep -Milliseconds 500
    }
    try { Stop-Process -Id $p.Id -Force -ErrorAction Stop } catch { }
    return "no Jack In window after $($halfSeconds / 2) s"
}

# What usually blocks a script on a removable drive for a normal user.
function Add-Diagnosis {
    $script:lines += "PowerShell language mode (non-admin): $($ExecutionContext.SessionState.LanguageMode)"
    $script:lines += 'execution policy:'
    $script:lines += (Get-ExecutionPolicy -List | Out-String).Trim()
    $drive = Split-Path -Qualifier $app -ErrorAction SilentlyContinue
    try {
        $type = (Get-CimInstance Win32_LogicalDisk -ErrorAction Stop | Where-Object { $_.DeviceID -eq $drive }).DriveType
        $script:lines += "drive $drive type $type (2 = removable)"
    } catch { }
    $checks = [ordered]@{
        'AppLocker script rules' = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\SrpV2\Script'
        'Software Restriction Policies' = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Safer\CodeIdentifiers'
        'Removable disks policy' = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\RemovableStorageDevices\{53f5630d-b6bf-11d0-94f2-00a0c91efb8b}'
        'Removable storage (all)' = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\RemovableStorageDevices'
    }
    foreach ($name in $checks.Keys) {
        $key = $checks[$name]
        if (Test-Path -LiteralPath $key) {
            $vals = (Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue | Out-String).Trim()
            $script:lines += "${name}: present $vals"
        } else {
            $script:lines += "${name}: none"
        }
    }
    if ($ExecutionContext.SessionState.LanguageMode -ne 'FullLanguage') {
        $script:lines += 'LIKELY CAUSE: PowerShell runs in Constrained Language for this user (AppLocker/WDAC), so the app cannot load Windows Forms. Admins are usually exempt.'
    }
    if (Test-Path -LiteralPath $errFile) {
        $script:lines += 'app error:'
        $script:lines += (Get-Content -Raw -LiteralPath $errFile)
    }
}

$why = Start-App $false
if (-not $why) { exit 0 }
$lines += "normal start failed: $why"
Add-Diagnosis
$lines += 'asking Windows for admin (UAC) and trying again...'
Set-Content -LiteralPath $log -Value $lines -ErrorAction SilentlyContinue
Write-Host 'Jack In did not start as you; asking Windows for admin (click Yes)...'
$why2 = Start-App $true
if (-not $why2) {
    $lines += 'admin start worked'
    Set-Content -LiteralPath $log -Value $lines -ErrorAction SilentlyContinue
    exit 0
}
$lines += "admin start failed too: $why2"
if (Test-Path -LiteralPath $errFile) { $lines += 'app error (admin):'; $lines += (Get-Content -Raw -LiteralPath $errFile) }
$lines += 'Run it by hand in PowerShell to see more:'
$lines += "  powershell -NoProfile -ExecutionPolicy Bypass -STA -File $([char]34)$app$([char]34)"
Set-Content -LiteralPath $log -Value $lines -ErrorAction SilentlyContinue
exit 1
