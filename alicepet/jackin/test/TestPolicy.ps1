# Parses JackIn.ps1 and checks its command policy (Test-ReadOnly / Test-Destructive)
# without opening any window. Works in Windows PowerShell 5.1 and pwsh 7.
#   powershell -NoProfile -File TestPolicy.ps1 [path\to\JackIn.ps1]
param([string]$App = (Join-Path (Split-Path -Parent $PSCommandPath) '..\windows\JackIn.ps1'))
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path $App), [ref]$tokens, [ref]$errors)
if ($errors -and @($errors).Count -gt 0) {
    $errors | ForEach-Object { Write-Host "PARSE ERROR line $($_.Extent.StartLineNumber): $($_.Message)" }
    exit 1
}
Write-Host "parse ok: $App"

# Load only the policy functions and the lists they use.
$want = @('Get-CmdAst', 'Get-CmdCalls', 'Get-CmdMethods', 'Test-ReadOnly', 'Test-Destructive')
foreach ($f in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    if ($want -contains $f.Name) { . ([scriptblock]::Create($f.Extent.Text)) }
}
foreach ($a in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] }, $false)) {
    if ($a.Left.Extent.Text -in @('$ReadVerbs', '$ReadNames', '$ReadMethods')) { . ([scriptblock]::Create($a.Extent.Text)) }
}

$fail = 0
function Check([string]$what, [bool]$got, [bool]$want) {
    if ($got -ne $want) { Write-Host "FAIL $what -> $got (want $want)"; $script:fail++ }
}
$readOk = @('Get-Process | Sort-Object WS -Descending | Select-Object -First 5', 'whoami', 'ipconfig /all', 'systeminfo', 'tasklist /v',
    'Get-Service | Where-Object { $_.Status -eq "Running" } | Measure-Object', 'Test-NetConnection 8.8.8.8 -Port 443',
    'Get-ChildItem C:\Users | Format-Table Name', 'gps | sort cpu -desc | select -f 3', 'route print', 'sc.exe query wuauserv',
    'Get-CimInstance Win32_OperatingSystem | Select-Object Caption, LastBootUpTime', '(Get-Date).ToString()',
    '$p = Get-Process; $p | % { $_.Name }', 'Test-ComputerSecureChannel')
$readNo = @('Remove-Item C:\temp\x', 'Stop-Process -Name OneDrive', 'ipconfig /release', 'Set-Content x.txt hi', 'sc stop x', 'sc.exe stop wuauserv',
    'Get-Process > procs.txt', 'Get-Process | ForEach-Object { $_.Kill() }', '& $cmd', 'iex "whoami"', 'New-Item x -ItemType File',
    '[IO.File]::Delete("x")', 'Restart-Service spooler', 'Get-Process | Stop-Process', 'cmd /c dir', 'this is ( not valid',
    'gps notepad | % Kill', 'Get-Process notepad | ForEach-Object -MemberName Kill', '(Get-Item C:\x).Attributes = "Hidden"',
    'Test-ComputerSecureChannel -Repair', '$m = "Kill"; gps notepad | % $m', 'gps | % Name')
$destructive = @('Remove-Item C:\temp -Recurse', 'del x', 'rd /s /q x', 'Stop-Process -Id 4', 'taskkill /im x.exe /f', 'Uninstall-Package x',
    'msiexec /x {GUID}', 'winget uninstall foo', 'Set-MpPreference -DisableRealtimeMonitoring $true', 'Disable-NetAdapter Wi-Fi',
    'reg delete HKCU\Software\X /f', 'bcdedit /set safeboot minimal', 'shutdown /r /t 0', 'Restart-Computer', 'net user bob /add',
    'Set-ExecutionPolicy Unrestricted', 'takeown /f C:\x', 'icacls C:\x /grant Everyone:F', 'diskpart', 'format D:', 'Clear-RecycleBin -Force',
    'Get-Process x | Stop-Process', 'Get-ChildItem *.tmp | Remove-Item', '[IO.File]::Delete("x")', 'iex (irm http://x)', 'cmd /c del x', 'not ( valid')
$notDestructive = @('Get-Process', 'ipconfig /all', 'winget list', 'Get-Service spooler', 'Start-Process notepad', 'New-Item C:\temp\x -ItemType Directory',
    'Test-Path C:\x', 'Copy-Item a b', 'net view', 'reg query HKLM\Software', 'cls')
foreach ($c in $readOk) { Check "ReadOnly '$c'" (Test-ReadOnly $c) $true }
foreach ($c in $readNo) { Check "ReadOnly '$c'" (Test-ReadOnly $c) $false }
foreach ($c in $destructive) { Check "Destructive '$c'" (Test-Destructive $c) $true }
foreach ($c in $notDestructive) { Check "Destructive '$c'" (Test-Destructive $c) $false }
$n = $readOk.Count + $readNo.Count + $destructive.Count + $notDestructive.Count
if ($fail) { Write-Host "$fail of $n policy checks failed"; exit 1 }
Write-Host "policy ok: $n checks"
