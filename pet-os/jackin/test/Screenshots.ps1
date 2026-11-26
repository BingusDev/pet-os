# SPDX-License-Identifier: MIT
#
# Takes the README screenshots of the Windows Jack In app, with no PET and none of
# this PC's details: a stand-in PET on 127.0.0.1 sends the example Navi (Norm, from
# pet-os/assets/examples/norm.png), operator [USER] and a canned chat, and the app
# runs as if on a PC called LAPTOP-FRONT (user front-desk). It saves:
#   jackin-pc-home.png   HOME: the Navi, the chat and PC STATUS
#   jackin-pc-chip.png   the PC driving the PET's LAN TECH chip (shot-lan-tech.png)
# The PC STATUS numbers are this PC's real CPU/RAM/disk/startup/Defender.
#   powershell -NoProfile -ExecutionPolicy Bypass -File Screenshots.ps1 [-Out <folder>]
param(
    [string]$App = (Join-Path (Split-Path -Parent $PSCommandPath) '..\windows\JackIn.ps1'),
    [string]$Out = (Join-Path (Split-Path -Parent $PSCommandPath) 'shots'),
    [int]$Port = 47351
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ShotWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(int f, int x, int y, int d, IntPtr e);
}
'@
[void][ShotWin]::SetProcessDPIAware()

$here = Split-Path -Parent $PSCommandPath
$root = Join-Path $here '..\..'
New-Item -ItemType Directory -Force -Path $Out | Out-Null

function B64([string]$path) { return [Convert]::ToBase64String([System.IO.File]::ReadAllBytes((Resolve-Path $path))) }

# A PNG as one full 'frame' line: run-length RGB565, as the PET mirrors its screen.
function Frame-Line([string]$path) {
    $bmp = New-Object System.Drawing.Bitmap -ArgumentList (Resolve-Path $path).Path
    $w = $bmp.Width
    $h = $bmp.Height
    $out = New-Object System.Collections.Generic.List[byte]
    $px = New-Object 'int[]' ($w * $h)
    $data = $bmp.LockBits((New-Object System.Drawing.Rectangle 0, 0, $w, $h), 'ReadOnly', ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb))
    [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $px, 0, $px.Length)
    $bmp.UnlockBits($data)
    $bmp.Dispose()
    $i = 0
    while ($i -lt $px.Length) {
        $c = $px[$i]
        $run = 1
        while ($i + $run -lt $px.Length -and $run -lt 255 -and $px[$i + $run] -eq $c) { $run++ }
        $r = ($c -shr 16) -band 0xff
        $g = ($c -shr 8) -band 0xff
        $b = $c -band 0xff
        $v = (($r -shr 3) -shl 11) -bor (($g -shr 2) -shl 5) -bor ($b -shr 3)
        $out.Add([byte]$run)
        $out.Add([byte]($v -shr 8))
        $out.Add([byte]($v -band 0xff))
        $i += $run
    }
    return [ordered]@{ t = 'frame'; screen = 'lan-tech'; w = $w; h = $h; y = 0; n = $h; rle = [Convert]::ToBase64String($out.ToArray()) }
}

$chipArt = Join-Path $root 'assets\images\chips'
$chips = @(
    [ordered]@{ id = 'jellyfin'; name = 'JELLYFIN'; number = '002'; code = 'J'; desc = 'Your media library'; art = (B64 (Join-Path $chipArt 'jellyfin-small.png')) },
    [ordered]@{ id = 'lan-tech'; name = 'LAN TECH'; number = '004'; code = 'L'; desc = 'Check the LAN'; art = (B64 (Join-Path $chipArt 'lan-tech-small.png')) },
    [ordered]@{ id = 'retro'; name = 'RETRO'; number = '006'; code = 'R'; desc = 'Retro games (play on this PC)'; art = (B64 (Join-Path $chipArt 'retro-small.png')) },
    [ordered]@{ id = 'terminal'; name = 'TERMINAL'; number = '007'; code = 'T'; desc = 'Shell to any host'; art = (B64 (Join-Path $chipArt 'terminal-small.png')) }
)
$frame = Frame-Line (Join-Path $here 'shot-lan-tech.png')

$listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $Port)
$listener.Start()
$script:client = $null
$script:stream = $null
$script:writer = $null
$script:pending = ''
$script:got = New-Object System.Collections.Generic.List[object]
$decoder = [System.Text.Encoding]::UTF8.GetDecoder()

function Send-App($obj) { $script:writer.WriteLine(($obj | ConvertTo-Json -Compress -Depth 5)) }

function Wait-App([int]$seconds, [scriptblock]$done) {
    $until = (Get-Date).AddSeconds($seconds)
    $bytes = New-Object byte[] 65536
    $chars = New-Object char[] 65536
    while ((Get-Date) -lt $until) {
        if (-not $script:client -and $listener.Pending()) {
            $script:client = $listener.AcceptTcpClient()
            $script:stream = $script:client.GetStream()
            $script:writer = New-Object System.IO.StreamWriter($script:stream, (New-Object System.Text.UTF8Encoding($false)))
            $script:writer.AutoFlush = $true
            $script:writer.NewLine = "`n"
        }
        while ($script:stream -and $script:stream.DataAvailable) {
            $count = $script:stream.Read($bytes, 0, $bytes.Length)
            if ($count -le 0) { break }
            $script:pending += New-Object System.String($chars, 0, $decoder.GetChars($bytes, 0, $count, $chars, 0))
        }
        while (($nl = $script:pending.IndexOf("`n")) -ge 0) {
            $line = $script:pending.Substring(0, $nl).Trim()
            $script:pending = $script:pending.Substring($nl + 1)
            if ($line) { try { $script:got.Add(($line | ConvertFrom-Json)) } catch { } }
        }
        if (& $done) { return $true }
        Start-Sleep -Milliseconds 50
    }
    return [bool](& $done)
}
function Find-Msg([string]$type) { foreach ($m in $script:got) { if ($m.t -eq $type) { return $m } }; return $null }

function Save-Window([IntPtr]$hwnd, [string]$name) {
    [void][ShotWin]::SetForegroundWindow($hwnd)
    Start-Sleep -Milliseconds 600
    $r = New-Object ShotWin+RECT
    [void][ShotWin]::GetWindowRect($hwnd, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap -ArgumentList ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $g.Dispose()
    $path = Join-Path $Out $name
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "saved $path"
}

function Click-Text([IntPtr]$hwnd, [string]$text) {
    $win = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
    $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, $text)
    $el = $win.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
    if (-not $el) { throw "no '$text' in the window" }
    $b = $el.Current.BoundingRectangle
    [void][ShotWin]::SetCursorPos([int]($b.X + $b.Width / 2), [int]($b.Y + $b.Height / 2))
    Start-Sleep -Milliseconds 100
    [ShotWin]::mouse_event(2, 0, 0, 0, [IntPtr]::Zero)
    [ShotWin]::mouse_event(4, 0, 0, 0, [IntPtr]::Zero)
}

# The app reads the PC's name and user from the environment: show made-up ones.
$trustFile = Join-Path ([System.IO.Path]::GetTempPath()) "JackIn-trust-shots-$PID.dat"
$oldName = $env:COMPUTERNAME
$oldUser = $env:USERNAME
$env:COMPUTERNAME = 'LAPTOP-FRONT'
$env:USERNAME = 'front-desk'
try {
    $proc = Start-Process powershell.exe -PassThru -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-STA', '-WindowStyle', 'Hidden',
        '-File', "`"$App`"", '-Pet', '127.0.0.1', '-Port', $Port, '-TunnelPort', ($Port + 1), '-TrustFile', "`"$trustFile`"")
} finally {
    $env:COMPUTERNAME = $oldName
    $env:USERNAME = $oldUser
}
try {
    if (-not (Wait-App 40 { $null -ne (Find-Msg 'hello') })) { throw 'the app never said hello' }
    Send-App ([ordered]@{ t = 'pet'; operator = '[USER]'; control = $true; terminal = $true; trusted = $true })
    Send-App ([ordered]@{ t = 'navi'; name = 'Norm'; art = (B64 (Join-Path $root 'assets\examples\norm.png')) })
    Send-App ([ordered]@{ t = 'chips'; list = $chips })
    if (-not (Wait-App 60 { $null -ne (Find-Msg 'scan') })) { throw 'the scan never arrived' }
    Send-App ([ordered]@{ t = 'alice'; ok = $true; heard = 'why is this laptop so slow?'
            text = 'Chrome is using 3.1 GB and Defender has been off since Aug 12. Want me to turn Defender back on first? Click Full Access and I will.'
            cmd = ''; step = 0 })
    [void](Wait-App 3 { $false })
    $proc.Refresh()
    $hwnd = $proc.MainWindowHandle
    Save-Window $hwnd 'jackin-pc-home.png'

    Click-Text $hwnd 'LAN TECH'
    if (-not (Wait-App 10 { $null -ne (Find-Msg 'open') })) { throw 'clicking LAN TECH sent no open' }
    for ($k = 0; $k -lt 3; $k++) { Send-App $frame; [void](Wait-App 1 { $false }) }
    Save-Window $hwnd 'jackin-pc-chip.png'

    Send-App ([ordered]@{ t = 'jackout'; why = 'screenshots done' })
    [void](Wait-App 10 { $proc.HasExited })
} finally {
    if (-not $proc.HasExited) { try { $proc.Kill() } catch { } }
    if ($script:client) { try { $script:client.Close() } catch { } }
    $listener.Stop()
    if (Test-Path -LiteralPath $trustFile) { Remove-Item -LiteralPath $trustFile -Force }
}
