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
$want = @('Get-CmdAst', 'Get-CmdCalls', 'Get-CmdMethods', 'Get-CmdStaticCalls', 'Get-CmdTypes', 'Get-ForEachMembers', 'Test-ReadOnly',
    'Test-Destructive', 'New-ReadRunspace')
foreach ($f in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    if ($want -contains $f.Name) { . ([scriptblock]::Create($f.Extent.Text)) }
}
foreach ($a in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] }, $false)) {
    if ($a.Left.Extent.Text -in @('$ReadVerbs', '$ReadNames', '$ReadNative', '$ReadRefused', '$ReadMethods', '$ReadTypes', '$DangerMembers')) {
        . ([scriptblock]::Create($a.Extent.Text))
    }
}

$fail = 0
function Check([string]$what, [bool]$got, [bool]$want) {
    if ($got -ne $want) { Write-Host "FAIL $what -> $got (want $want)"; $script:fail++ }
}
$readOk = @('Get-Process | Sort-Object WS -Descending | Select-Object -First 5', 'whoami', 'ipconfig /all', 'systeminfo', 'tasklist /v',
    'Get-Service | Where-Object { $_.Status -eq "Running" } | Measure-Object', 'Test-NetConnection 8.8.8.8 -Port 443',
    'Get-ChildItem C:\Users | Format-Table Name', 'gps | sort cpu -desc | select -f 3', 'route print', 'sc.exe query wuauserv',
    'Get-CimInstance Win32_OperatingSystem | Select-Object Caption, LastBootUpTime', '(Get-Date).ToString()',
    '$p = Get-Process; $p | % { $_.Name }', 'Test-ComputerSecureChannel',
    # still fine after the 0.9.4 tightening
    'Get-ChildItem C:\Windows -Force | Select-Object -First 3', '$env:COMPUTERNAME', '[int](Get-Date).Year', '[math]::Round(3.14159, 2)',
    'Get-Process | % { $_.Name.ToUpper() }', 'gps | % -Process { $_.Id }', 'Get-Content C:\Windows\win.ini -TotalCount 5',
    'Get-CimInstance Win32_LogicalDisk | ForEach-Object { [pscustomobject]@{ Drive = $_.DeviceID; Free = [math]::Round($_.FreeSpace / 1GB, 1) } }',
    'powercfg /list', 'dsregcmd /status', 'gpresult /r', 'Get-Service | Sort-Object Status | Format-Table -AutoSize', 'ping.exe 10.55.0.1',
    'Get-ChildItem | Where-Object Length -gt 1MB | Measure-Object Length -Sum', 'Resolve-DnsName example.com', 'Get-Date | Out-String')
$readNo = @('Remove-Item C:\temp\x', 'Stop-Process -Name OneDrive', 'ipconfig /release', 'Set-Content x.txt hi', 'sc stop x', 'sc.exe stop wuauserv',
    'Get-Process > procs.txt', 'Get-Process | ForEach-Object { $_.Kill() }', '& $cmd', 'iex "whoami"', 'New-Item x -ItemType File',
    '[IO.File]::Delete("x")', 'Restart-Service spooler', 'Get-Process | Stop-Process', 'cmd /c dir', 'this is ( not valid',
    'gps notepad | % Kill', 'Get-Process notepad | ForEach-Object -MemberName Kill', '(Get-Item C:\x).Attributes = "Hidden"',
    'Test-ComputerSecureChannel -Repair', '$m = "Kill"; gps notepad | % $m', 'gps | % Name',
    # 0.9.4: ways past the old list (rev26 review)
    '${function:Get-X} = "Remove-Item C:\x -Recurse -Force"; Get-X', '${alias:Get-X} = "powershell"; Get-X', '${C:\temp\x.txt} = "hi"',
    '$function:prompt = "x"', 'function Get-X { whoami }; Get-X', 'gps notepad | % -MemberName:Kill', 'gci C:\data | % -MemberName:Delete',
    'gps notepad | % -Mem Kill', '[IO.StreamWriter]"C:\temp\x.txt"', '[IO.File]::Replace("a", "b", $null)', '[IO.File]::ReadAllText("C:\x")',
    '(Get-WmiObject Win32_Volume).Format()', '"a".Replace("a", "b") | Out-Null; (Get-Item C:\x).Replace("b", "c")', 'dsregcmd /leave',
    'powercfg /batteryreport /output C:\temp\b.html', 'powercfg /batteryreport', 'gpresult /h C:\temp\r.html /f', 'sort.exe /o C:\temp\x.txt C:\temp\y.txt',
    'Test-ComputerSecureChannel -Rep', 'Get-Clipboard', 'Get-WindowsUpdateLog', 'Get-WindowsUpdate -Install -AcceptAll', 'Get-Credential',
    'Get-Process | Out-File procs.txt', 'Split-WindowsImage -ImagePath a.wim -SplitImagePath b.swm -FileSize 100', '[scriptblock]::Create("whoami").Invoke()',
    '[xml]$x = Get-Content a.xml', '$env:PATH = "C:\evil"', '$ExecutionContext.InvokeCommand.InvokeScript("whoami")', 'Get-Item C:\x | % { $_.Delete() }',
    'New-Object -ComObject WScript.Shell', '. C:\temp\x.ps1', '& "Remove-Item" C:\x', 'Get-Process | Tee-Object procs.txt', 'winver', 'nbtstat -R',
    'Invoke-WebRequest http://example.com -OutFile x', 'Get-ChildItem -OutVariable x | Set-Content y', 'where.exe /r C:\ *.txt')
$destructive = @('Remove-Item C:\temp -Recurse', 'del x', 'rd /s /q x', 'Stop-Process -Id 4', 'taskkill /im x.exe /f', 'Uninstall-Package x',
    'msiexec /x {GUID}', 'winget uninstall foo', 'Set-MpPreference -DisableRealtimeMonitoring $true', 'Disable-NetAdapter Wi-Fi',
    'reg delete HKCU\Software\X /f', 'bcdedit /set safeboot minimal', 'shutdown /r /t 0', 'Restart-Computer', 'net user bob /add',
    'Set-ExecutionPolicy Unrestricted', 'takeown /f C:\x', 'icacls C:\x /grant Everyone:F', 'diskpart', 'format D:', 'Clear-RecycleBin -Force',
    'Get-Process x | Stop-Process', 'Get-ChildItem *.tmp | Remove-Item', '[IO.File]::Delete("x")', 'iex (irm http://x)', 'cmd /c del x', 'not ( valid',
    # 0.9.4
    'gci x | % Delete', 'gps x | % Kill', 'gps x | % -MemberName:Kill', '$m = "Kill"; gps x | % $m', 'clc file.txt', 'sp HKLM:\Software\X Name 1',
    'Start-Process cmd "/c del C:\x"', 'Start-Process powershell -Verb RunAs', 'saps C:\Windows\System32\cmd.exe', '[scriptblock]::Create("del x").Invoke()',
    '${function:Get-X} = "Remove-Item C:\x"; Get-X', '${C:\temp\x.txt} = ""', '[IO.File]::Replace("a", "b", $null)', '[IO.File]::WriteAllText("C:\x", "")',
    '[Diagnostics.Process]::Start("cmd")', 'Set-Alias x Remove-Item; x C:\y', '$ExecutionContext.InvokeCommand.InvokeScript("del x")', 'mshta http://x')
$notDestructive = @('Get-Process', 'ipconfig /all', 'winget list', 'Get-Service spooler', 'Start-Process notepad', 'New-Item C:\temp\x -ItemType Directory',
    'Test-Path C:\x', 'Copy-Item a b', 'net view', 'reg query HKLM\Software', 'cls',
    '"a b".Replace(" ", "_")', '[regex]::Replace("a", "a", "b")', '[string]::Join(",", @(1, 2))', 'gps | % Name', 'gps | % { $_.Name }', '$env:FOO = "bar"',
    'Start-Process "C:\Program Files\App\app.exe"', '[IO.Path]::Combine("a", "b")', 'Get-Content x | Set-Content y')
foreach ($c in $readOk) { Check "ReadOnly '$c'" (Test-ReadOnly $c) $true }
foreach ($c in $readNo) { Check "ReadOnly '$c'" (Test-ReadOnly $c) $false }
foreach ($c in $destructive) { Check "Destructive '$c'" (Test-Destructive $c) $true }
foreach ($c in $notDestructive) { Check "Destructive '$c'" (Test-Destructive $c) $false }
$n = $readOk.Count + $readNo.Count + $destructive.Count + $notDestructive.Count

# The READ runspace is the second lock: these would do no harm if they ran, but a
# locked session must refuse them (and still run the plain read-only ones).
function Invoke-InReadRunspace([string]$text) {
    $rs = New-ReadRunspace
    $ps = [PowerShell]::Create()
    try {
        $ps.Runspace = $rs
        [void]$ps.AddScript($text)
        try { $out = $ps.Invoke() } catch { return [pscustomobject]@{ Failed = $true; Why = $_.Exception.Message; Out = '' } }
        $why = (($ps.Streams.Error | ForEach-Object { $_.ToString() }) -join '; ')
        return [pscustomobject]@{ Failed = [bool]$ps.HadErrors; Why = $why; Out = (($out | Out-String).Trim()) }
    } finally {
        $ps.Dispose()
        $rs.Dispose()
    }
}
$lockedOut = @('[IO.File]::Exists("C:\Windows\win.ini")', '(Get-Item $env:TEMP).GetFiles().Count', 'New-Object -ComObject Scripting.FileSystemObject',
    'Add-Type -TypeDefinition "public class JackInPolicyProbe { }"', '[Diagnostics.Process]::GetCurrentProcess().Id',
    '[scriptblock]::Create("1 + 1").Invoke()')
$stillRuns = @('(Get-Date).Year', 'Get-Process -Id $PID | ForEach-Object { $_.ProcessName }', '"a,b".Split(",").Count', '[math]::Round(2.6)',
    'Get-ChildItem $env:WINDIR -ErrorAction Stop | Select-Object -First 1 | ForEach-Object { $_.Name }', '$ExecutionContext.SessionState.LanguageMode')
foreach ($c in $lockedOut) {
    $r = Invoke-InReadRunspace $c
    if (-not $r.Failed) { Write-Host "FAIL read runspace ran '$c' -> $($r.Out)"; $script:fail++ }
}
foreach ($c in $stillRuns) {
    $r = Invoke-InReadRunspace $c
    if ($r.Failed -or -not $r.Out) { Write-Host "FAIL read runspace refused '$c': $($r.Why)"; $script:fail++ }
}
$mode = (Invoke-InReadRunspace '$ExecutionContext.SessionState.LanguageMode').Out
if ($mode -ne 'ConstrainedLanguage') { Write-Host "FAIL read runspace language mode is '$mode'"; $script:fail++ }
$n += $lockedOut.Count + $stillRuns.Count + 1

if ($fail) { Write-Host "$fail of $n policy checks failed"; exit 1 }
Write-Host "policy ok: $n checks"
