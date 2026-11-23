# SPDX-License-Identifier: MIT
#
# Starts the real Jack In app against a scripted stand-in PET on 127.0.0.1 and
# checks the link by itself (no typing): the first link stays up, READ mode runs
# a read-only command and refuses the ways round it, and JACK OUT closes the app.
# The Jack In window shows for about half a minute and then closes.
#   powershell -NoProfile -ExecutionPolicy Bypass -File TestLink.ps1
# Every command sent here is harmless even if a check fails and it runs.
param(
    [string]$App = (Join-Path (Split-Path -Parent $PSCommandPath) '..\windows\JackIn.ps1'),
    [int]$Port = 47341
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$fail = 0
function Check([string]$what, [bool]$ok, [string]$detail = '') {
    if ($ok) { Write-Host "PASS $what" } else { Write-Host "FAIL $what $detail"; $script:fail++ }
}

$listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $Port)
$listener.Start()
$client = $null
$stream = $null
$writer = $null
$pending = ''
$links = 0
$got = New-Object System.Collections.Generic.List[object]
$decoder = [System.Text.Encoding]::UTF8.GetDecoder()

function Send-App($obj) {
    $script:writer.WriteLine(($obj | ConvertTo-Json -Compress -Depth 4))
}

# Reads what the app sent until $done says so (or the time is up). A second
# connection means the app dropped the link and dialed again.
function Wait-App([int]$seconds, [scriptblock]$done) {
    $until = (Get-Date).AddSeconds($seconds)
    $bytes = New-Object byte[] 65536
    $chars = New-Object char[] 65536
    while ((Get-Date) -lt $until) {
        if ($script:listener.Pending()) {
            if ($script:client) { try { $script:client.Close() } catch { } }
            $script:client = $script:listener.AcceptTcpClient()
            $script:stream = $script:client.GetStream()
            $script:writer = New-Object System.IO.StreamWriter($script:stream, (New-Object System.Text.UTF8Encoding($false)))
            $script:writer.AutoFlush = $true
            $script:writer.NewLine = "`n"
            $script:pending = ''
            $script:links++
        }
        while ($script:stream -and $script:stream.DataAvailable) {
            $count = $script:stream.Read($bytes, 0, $bytes.Length)
            if ($count -le 0) { break }
            $script:pending += New-Object System.String($chars, 0, $script:decoder.GetChars($bytes, 0, $count, $chars, 0))
        }
        while (($nl = $script:pending.IndexOf("`n")) -ge 0) {
            $line = $script:pending.Substring(0, $nl).Trim()
            $script:pending = $script:pending.Substring($nl + 1)
            if ($line) { try { $script:got.Add(($line | ConvertFrom-Json)) } catch { Write-Host "  (not JSON: $line)" } }
        }
        if (& $done) { return $true }
        Start-Sleep -Milliseconds 50
    }
    return [bool](& $done)
}

function Find-Msg([string]$type, $id = $null) {
    foreach ($m in $script:got) {
        if ($m.t -ne $type) { continue }
        if ($null -ne $id -and $m.id -ne $id) { continue }
        return $m
    }
    return $null
}

$proc = Start-Process powershell.exe -PassThru -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-STA', '-WindowStyle', 'Hidden',
    '-File', "`"$App`"", '-Pet', '127.0.0.1', '-Port', $Port, '-TunnelPort', ($Port + 1))
try {
    Check 'the app dials the PET and says hello' (Wait-App 40 { $null -ne (Find-Msg 'hello') })
    $hello = Find-Msg 'hello'
    if ($hello) {
        Write-Host "  app $($hello.app), mode $($hello.mode), admin=$($hello.admin)"
        Check 'it starts in READ mode' ($hello.mode -eq 'read' -and -not $hello.full -and -not $hello.input)
    }
    Check 'the scan arrives' (Wait-App 60 { $null -ne (Find-Msg 'scan') })

    # What the PET sends next: who runs it, no chip control yet, and one mirror frame band.
    Send-App ([ordered]@{ t = 'pet'; operator = 'Tester'; control = $false; terminal = $false })
    Send-App ([ordered]@{ t = 'frame'; screen = 'test'; w = 4; h = 2; y = 0; n = 2; rle = [Convert]::ToBase64String([byte[]](8, 0, 31)) })
    [void](Wait-App 6 { $false })
    Check 'the first link stays up (rev26 dropped it and dialed again)' ($links -eq 1 -and $null -eq (Find-Msg 'bye')) "links=$links"

    Send-App ([ordered]@{ t = 'run'; id = 1; cmd = '(Get-Date).Year' })
    [void](Wait-App 30 { $null -ne (Find-Msg 'result' 1) })
    $r = Find-Msg 'result' 1
    Check 'READ runs a read-only command' ($r -and $r.ok -and ([string]$r.out).Trim() -eq [string](Get-Date).Year) $(if ($r) { [string]$r.out } else { 'no result' })

    $n = 2
    foreach ($cmd in @('${function:Get-JackInProbe} = "whoami"; Get-JackInProbe', '$PID | % -MemberName:GetHashCode', '[IO.Path]::GetTempPath()',
            '[IO.StreamReader]"C:\Windows\win.ini"', 'function Get-JackInProbe { hostname }; Get-JackInProbe', 'powercfg /batteryreport /output NUL')) {
        Send-App ([ordered]@{ t = 'run'; id = $n; cmd = $cmd })
        [void](Wait-App 15 { $null -ne (Find-Msg 'result' $n) })
        $r = Find-Msg 'result' $n
        Check "READ refuses: $cmd" ($r -and -not $r.ok -and $r.PSObject.Properties['reason'] -and $r.reason -eq 'read-only') $(if ($r) { [string]$r.out } else { 'no result' })
        $n++
    }

    # NET is off, and localhost is this PC: the app must say no either way.
    Send-App ([ordered]@{ t = 'net_open'; id = 77; host = 'localhost'; port = 80 })
    Check 'a tunnel to this PC is refused' (Wait-App 10 { $null -ne (Find-Msg 'net_fail' 77) })
    Check 'still one link at the end' ($links -eq 1) "links=$links"

    Send-App ([ordered]@{ t = 'jackout'; why = 'test' })
    [void](Wait-App 10 { $proc.HasExited })
    Check 'JACK OUT closes the app' $proc.HasExited
} finally {
    if (-not $proc.HasExited) { try { $proc.Kill() } catch { } }
    if ($client) { try { $client.Close() } catch { } }
    $listener.Stop()
}
if ($fail) { Write-Host "$fail link checks failed"; exit 1 }
Write-Host 'link ok'
