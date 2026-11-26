# SPDX-License-Identifier: MIT
#
# Starts the real Jack In app against a scripted stand-in PET on 127.0.0.1 and
# checks that a Retro game plays INSIDE the app (WebView2), not in the browser:
# the PET sends `play` kind=game with a link to a stand-in web relay here, the
# page reports back from inside WebView2 (cross-origin isolated, like RomM's
# EmulatorJS needs), a jump to another site is blocked, Shift+Esc in the game
# ends it, and JACK OUT closes the app and its WebView2. Nothing leaves 127.0.0.1.
#   powershell -NoProfile -ExecutionPolicy Bypass -File TestGame.ps1 [-Shot C:\path\game.png]
param(
    [string]$App = (Join-Path (Split-Path -Parent $PSCommandPath) '..\windows\JackIn.ps1'),
    [int]$Port = 47351,
    [int]$WebPort = 47355,
    # A screenshot of the window while the game page shows (skipped if empty).
    [string]$Shot = ''
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$fail = 0
function Check([string]$what, [bool]$ok, [string]$detail = '') {
    if ($ok) { Write-Host "PASS $what" } else { Write-Host "FAIL $what $detail"; $script:fail++ }
}

$listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $Port)
$listener.Start()
$web = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $WebPort)
$web.Start()
$client = $null
$stream = $null
$writer = $null
$pending = ''
$got = New-Object System.Collections.Generic.List[object]
$hits = New-Object System.Collections.Generic.List[string]   # paths the game page asked for
$decoder = [System.Text.Encoding]::UTF8.GetDecoder()

# The stand-in game: RomM's player needs SharedArrayBuffer, so it checks that too.
$page = @'
<!doctype html><html><head><meta charset="utf-8"><title>Test game</title></head>
<body style="margin:0;background:#103;color:#fff;font:32px monospace;display:flex;align-items:center;justify-content:center;height:100vh">
<div>TEST GAME<br><small id="s">starting</small></div>
<script>
var wv = !!(window.chrome && window.chrome.webview);
fetch('/report?webview=' + wv + '&isolated=' + window.crossOriginIsolated + '&sab=' + (typeof SharedArrayBuffer)).then(function () {
  document.getElementById('s').textContent = 'webview=' + wv + ' isolated=' + window.crossOriginIsolated;
  // ?leave: try to go to another site (the app must block it), then say it is still here.
  if (location.search.indexOf('leave') >= 0) {
    location.href = 'http://127.0.0.2:9/';
    setTimeout(function () { fetch('/still-here'); }, 2000);
  }
  // ?esc: what Shift+Esc in the game does.
  if (location.search.indexOf('esc') >= 0) {
    window.addEventListener('pagehide', function () { navigator.sendBeacon('/bye'); });
    setTimeout(function () { window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', shiftKey: true })); }, 500);
  }
});
</script></body></html>
'@

function Serve-Web {
    while ($script:web.Pending()) {
        $c = $script:web.AcceptTcpClient()
        try {
            $c.ReceiveTimeout = 2000
            $ns = $c.GetStream()
            $buf = New-Object byte[] 8192
            $req = ''
            while ($req.IndexOf("`r`n`r`n") -lt 0) {
                $n = $ns.Read($buf, 0, $buf.Length)
                if ($n -le 0) { break }
                $req += [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
            }
            $path = ($req -split ' ')[1]
            $script:hits.Add([string]$path)
            $body = [byte[]]@()
            $status = '404 Not Found'
            $type = 'text/plain'
            if ($path -like '/rom/*') { $status = '200 OK'; $type = 'text/html; charset=utf-8'; $body = [System.Text.Encoding]::UTF8.GetBytes($script:page) }
            elseif ($path -like '/report*' -or $path -like '/still*' -or $path -like '/bye*') { $status = '204 No Content' }
            $head = "HTTP/1.1 $status`r`nContent-Type: $type`r`nContent-Length: $($body.Length)`r`n" +
                "Cross-Origin-Opener-Policy: same-origin`r`nCross-Origin-Embedder-Policy: require-corp`r`nCache-Control: no-store`r`nConnection: close`r`n`r`n"
            $hb = [System.Text.Encoding]::ASCII.GetBytes($head)
            $ns.Write($hb, 0, $hb.Length)
            if ($body.Length) { $ns.Write($body, 0, $body.Length) }
            $ns.Flush()
        } catch { } finally { $c.Close() }
    }
}

function Send-App($obj) {
    $script:writer.WriteLine(($obj | ConvertTo-Json -Compress -Depth 4))
}

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
        }
        while ($script:stream -and $script:stream.DataAvailable) {
            $count = $script:stream.Read($bytes, 0, $bytes.Length)
            if ($count -le 0) { break }
            $script:pending += New-Object System.String($chars, 0, $script:decoder.GetChars($bytes, 0, $count, $chars, 0))
        }
        while (($nl = $script:pending.IndexOf("`n")) -ge 0) {
            $line = $script:pending.Substring(0, $nl).Trim()
            $script:pending = $script:pending.Substring($nl + 1)
            if ($line) { try { $script:got.Add(($line | ConvertFrom-Json)) } catch { } }
        }
        Serve-Web
        if (& $done) { return $true }
        Start-Sleep -Milliseconds 50
    }
    return [bool](& $done)
}

function Find-Msg([string]$type) {
    foreach ($m in $script:got) { if ($m.t -eq $type) { return $m } }
    return $null
}
function Find-Hit([string]$prefix) {
    foreach ($h in $script:hits) { if ($h.StartsWith($prefix)) { return $h } }
    return $null
}
# WebView2's processes started for this app (msedgewebview2.exe under its powershell.exe).
function Get-WebProcs([int]$parent) {
    $all = @(Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe'" -ErrorAction SilentlyContinue)
    return @($all | Where-Object { $_.ParentProcessId -eq $parent })
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class TestGameWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
'@

$trustFile = Join-Path ([System.IO.Path]::GetTempPath()) "JackIn-trust-game-$PID.dat"
$proc = Start-Process powershell.exe -PassThru -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-STA', '-WindowStyle', 'Hidden',
    '-File', "`"$App`"", '-Pet', '127.0.0.1', '-Port', $Port, '-TunnelPort', ($Port + 1), '-TrustFile', "`"$trustFile`"")
try {
    Check 'the app dials the PET and says hello' (Wait-App 40 { $null -ne (Find-Msg 'hello') })
    Send-App ([ordered]@{ t = 'pet'; operator = 'Tester'; control = $true; terminal = $false })
    [void](Wait-App 3 { $false })

    $link = "http://127.0.0.1:$WebPort/rom/358/ejs"
    Send-App ([ordered]@{ t = 'play'; kind = 'game'; title = 'Union Arena (test)'; url = $link; direct = 'https://romm.example/rom/358/ejs' })
    Check 'the game page loads' (Wait-App 40 { $null -ne (Find-Hit '/report') }) "hits: $($hits -join ', ')"
    $r = Find-Hit '/report'
    Check 'it loads inside the app (WebView2), not in a browser' ([bool]($r -and $r.Contains('webview=true'))) "$r"
    Check 'the page is cross-origin isolated (SharedArrayBuffer for EmulatorJS)' ([bool]($r -and $r.Contains('isolated=true') -and $r.Contains('sab=function'))) "$r"
    $kids = @(Get-WebProcs $proc.Id)
    Check 'WebView2 runs under the Jack In app' ($kids.Count -gt 0) "msedgewebview2 children: $($kids.Count)"

    if ($Shot) {
        [void](Wait-App 2 { $false })
        try {
            [void][TestGameWin]::SetProcessDPIAware()
            $proc.Refresh()
            $h = $proc.MainWindowHandle
            [void][TestGameWin]::SetForegroundWindow($h)
            [void](Wait-App 1 { $false })
            $rc = New-Object TestGameWin+RECT
            [void][TestGameWin]::GetWindowRect($h, [ref]$rc)
            Add-Type -AssemblyName System.Drawing
            $bmp = New-Object System.Drawing.Bitmap ($rc.R - $rc.L), ($rc.B - $rc.T)
            # The window renders itself (PW_RENDERFULLCONTENT), so windows on top never get in the shot.
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            $dc = $g.GetHdc()
            $ok = [TestGameWin]::PrintWindow($h, $dc, 2)
            $g.ReleaseHdc($dc)
            $g.Dispose()
            if ($ok) { $bmp.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png) }
            $bmp.Dispose()
            if (-not $ok) { throw 'PrintWindow failed' }
            Write-Host "  screenshot: $Shot"
        } catch { Write-Host "  (no screenshot: $($_.Exception.Message))" }
    }

    # A second game replaces the first; this page tries to leave the relay's site.
    $hits.Clear()
    Send-App ([ordered]@{ t = 'play'; kind = 'game'; title = 'Union Arena (test)'; url = "http://127.0.0.1:$WebPort/rom/358/ejs?leave=1" })
    Check 'a second game replaces the first' (Wait-App 30 { $null -ne (Find-Hit '/report') })
    Check 'the game page can''t leave for another site' (Wait-App 10 { $null -ne (Find-Hit '/still-here') }) "hits: $($hits -join ', ')"

    # Shift+Esc in the game comes back: the page goes, the app stays up.
    $hits.Clear()
    Send-App ([ordered]@{ t = 'play'; kind = 'game'; title = 'Union Arena (test)'; url = "http://127.0.0.1:$WebPort/rom/358/ejs?esc=1" })
    [void](Wait-App 30 { $null -ne (Find-Hit '/report') })
    Check 'Shift+Esc in the game ends it' (Wait-App 15 { $null -ne (Find-Hit '/bye') }) "hits: $($hits -join ', ')"
    Check 'the app is still up after the game' (-not $proc.HasExited)
    Check 'no video progress is sent for a game' ($null -eq (Find-Msg 'progress'))
} finally {
    if (-not $proc.HasExited) {
        try {
            Send-App ([ordered]@{ t = 'jackout'; why = 'test' })
            [void](Wait-App 15 { $proc.HasExited })
        } catch { }
    }
    Check 'JACK OUT closes the app' $proc.HasExited
    [void](Wait-App 5 { $false })
    Check 'WebView2 is gone after JACK OUT' (@(Get-WebProcs $proc.Id).Count -eq 0)
    if (-not $proc.HasExited) { try { $proc.Kill() } catch { } }
    if ($client) { try { $client.Close() } catch { } }
    $listener.Stop()
    $web.Stop()
    if (Test-Path -LiteralPath $trustFile) { Remove-Item -LiteralPath $trustFile -Force }
}
if ($fail) { Write-Host "$fail game checks failed"; exit 1 }
Write-Host 'game ok'
