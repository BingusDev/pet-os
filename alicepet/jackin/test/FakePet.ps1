# SPDX-License-Identifier: MIT
#
# A stand-in PET for trying the Jack In app on any Windows PC, no PET needed.
# It listens on 127.0.0.1 only (ports 7341 and 7342, like the PET), prints what
# the app sends, answers chat and voice with canned "Alice" lines, and lets you
# type the things the PET would do. Start both with TestJackIn.cmd.
#
# Type `help` in this window for the commands.

param(
    [int]$Port = 7341,
    [int]$TunnelPort = 7342,
    [switch]$SaveVoice   # keep each voice clip as voice-N.wav in the current folder
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$control = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $Port)
$tunnels = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $TunnelPort)
$control.Start()
$tunnels.Start()

$client = $null
$stream = $null
$writer = $null
$buffer = ''
$typed = ''
$nextId = 1
$voiceCount = 0
$suggestion = 'Stop-Process -Name notepad'
$pendingWeb = @{}   # tunnel id -> host

function Say([string]$text, [string]$color = 'Gray') {
    if ($script:typed) { Write-Host '' }
    Write-Host $text -ForegroundColor $color
    if ($script:typed) { Write-Host -NoNewline ('pet> ' + $script:typed) }
}

function Send($obj) {
    if ($null -eq $script:writer) { Say '  (no app connected)' 'DarkYellow'; return }
    $line = $obj | ConvertTo-Json -Compress -Depth 4
    $script:writer.WriteLine($line)
    Say ("PET -> " + $line) 'Cyan'
}

function Show-Help {
    Say @'
Commands (what the PET's keys do):
  full | admin | net   ask the PC for Full Access / admin / Share PC Internet ([F] [A] [N])
  drop full | drop net take it away again
  fix                  send the canned fix ([Enter] on Alice's suggestion)
  run <command>        send any PowerShell command (only runs with Full Access)
  scan                 ask for a new scan ([R])
  busy                 send Alice's short "still answering" reply (the app must stay linked)
  web                  fetch http://example.com through Share PC Internet (tests tunnels)
  jackout              JACK OUT ([X]); the app should close
  quit                 stop this stand-in
'@ 'White'
}

function On-Command([string]$line) {
    $parts = $line.Trim() -split '\s+', 2
    $word = $parts[0].ToLower()
    $rest = $(if ($parts.Count -gt 1) { $parts[1] } else { '' })
    switch ($word) {
        '' { }
        'help' { Show-Help }
        'full' { Send ([ordered]@{ t = 'ask'; what = 'full' }) }
        'admin' { Send ([ordered]@{ t = 'ask'; what = 'admin' }) }
        'net' { Send ([ordered]@{ t = 'ask'; what = 'net' }) }
        'drop' { Send ([ordered]@{ t = 'drop'; what = $rest }) }
        'fix' { Send ([ordered]@{ t = 'run'; id = $script:nextId++; cmd = $script:suggestion }) }
        'run' { Send ([ordered]@{ t = 'run'; id = $script:nextId++; cmd = $rest }) }
        'scan' { Send ([ordered]@{ t = 'scan' }) }
        'busy' { Send ([ordered]@{ t = 'alice'; ok = $false; text = 'Still answering the last one...' }) }
        'web' {
            $id = $script:nextId++
            $script:pendingWeb[$id] = 'example.com'
            Send ([ordered]@{ t = 'net_open'; id = $id; host = 'example.com'; port = 80 })
        }
        'jackout' { Send ([ordered]@{ t = 'jackout'; why = 'JACK OUT on the PET' }) }
        'quit' { $script:running = $false }
        default { Say "  unknown: $word (try help)" 'DarkYellow' }
    }
}

function Reply-Alice([string]$text, [string]$heard = '', [string]$cmd = '') {
    Send ([ordered]@{ t = 'alice'; ok = $true; text = $text; heard = $heard; cmd = $cmd })
}

function On-Line([string]$line) {
    Say ("PC  <- " + $(if ($line.Length -gt 300) { $line.Substring(0, 300) + '...' } else { $line })) 'Green'
    try { $msg = $line | ConvertFrom-Json } catch { Say '  (not JSON!)' 'Red'; return }
    switch ($msg.t) {
        'hello' { Say "  linked: $($msg.host) · $($msg.os) · $($msg.user) · admin=$($msg.admin) full=$($msg.full)" 'White' }
        'scan' {
            Say "  scan: CPU $($msg.cpu)% · RAM $($msg.ram)% · $($msg.disk) $($msg.disk_free)% free · startup $($msg.startup) · Defender $($msg.defender)" 'White'
            Say "  (the real PET would now ask Alice; type 'fix' to send a canned suggestion: $script:suggestion)" 'DarkGray'
        }
        'chat' {
            $text = [string]$msg.text
            if ($text -match 'slow|ram|memory') {
                Reply-Alice "Stand-in Alice: you asked '$text'. Memory looks tight; closing Notepad would be my test fix." '' $script:suggestion
            } else {
                Reply-Alice "Stand-in Alice: got it, you said '$text'. The real PET sends this to me over the bridge."
            }
        }
        'voice' {
            $bytes = [Convert]::FromBase64String([string]$msg.wav)
            $ok = $bytes.Length -gt 44 -and [Text.Encoding]::ASCII.GetString($bytes, 0, 4) -eq 'RIFF'
            $rate = $(if ($ok) { [BitConverter]::ToInt32($bytes, 24) } else { 0 })
            $channels = $(if ($ok) { [BitConverter]::ToInt16($bytes, 22) } else { 0 })
            $bits = $(if ($ok) { [BitConverter]::ToInt16($bytes, 34) } else { 0 })
            $secs = $(if ($rate -gt 0) { [math]::Round(($bytes.Length - 44) / ($rate * $channels * $bits / 8), 1) } else { 0 })
            Say "  voice: $($bytes.Length) bytes, WAV=$ok, $rate Hz, $channels ch, $bits bit, about $secs s" 'White'
            if ($SaveVoice) {
                $script:voiceCount++
                $path = Join-Path (Get-Location) "voice-$script:voiceCount.wav"
                [IO.File]::WriteAllBytes($path, $bytes)
                Say "  saved $path (play it to check the mic)" 'White'
            }
            $want = ($rate -eq 16000 -and $channels -eq 1 -and $bits -eq 16)
            Reply-Alice ("Stand-in Alice: I got $secs s of audio" + $(if ($want) { ', in the right format.' } else { ", but the PET wants 16000 Hz mono 16-bit." })) '(the stand-in cannot transcribe)'
        }
        'net_fail' { Say "  tunnel $($msg.id): the PC could not connect (is Share PC Internet on?)" 'Yellow'; $script:pendingWeb.Remove([int]$msg.id) }
        'bye' { Say "  the app said bye ($($msg.why))" 'White' }
    }
}

function Step-Tunnels {
    while ($tunnels.Pending()) {
        $t = $tunnels.AcceptTcpClient()
        $t.ReceiveTimeout = 8000
        $s = $t.GetStream()
        $hello = ''
        while (-not $hello.EndsWith("`n")) {
            $b = $s.ReadByte()
            if ($b -lt 0) { break }
            $hello += [char]$b
        }
        Say "PC  <- (tunnel port) $($hello.Trim())" 'Green'
        $id = [int](($hello.Trim() -split ' ')[1])
        $hostName = $script:pendingWeb[$id]
        $script:pendingWeb.Remove($id)
        $req = [Text.Encoding]::ASCII.GetBytes("GET / HTTP/1.1`r`nHost: $hostName`r`nConnection: close`r`n`r`n")
        $s.Write($req, 0, $req.Length)
        $reader = New-Object System.IO.StreamReader($s)
        try {
            $status = $reader.ReadLine()
            $body = $reader.ReadToEnd()
            $title = $(if ($body -match '<title>(.*?)</title>') { $Matches[1] } else { '?' })
            Say "  tunnel $id works: $status · page title '$title' · $($body.Length) bytes" 'White'
        } catch {
            Say "  tunnel ${id}: no answer through the PC ($($_.Exception.Message))" 'Yellow'
        }
        $t.Close()
    }
}

Say "Stand-in PET listening on 127.0.0.1:$Port (and $TunnelPort for tunnels). Start the app with -Pet 127.0.0.1." 'White'
Show-Help
Write-Host -NoNewline 'pet> '
$running = $true
while ($running) {
    if ($control.Pending()) {
        if ($client) { $client.Close(); Say '  (newer connection replaces the old one, like the PET)' 'DarkGray' }
        $client = $control.AcceptTcpClient()
        $stream = $client.GetStream()
        $writer = New-Object System.IO.StreamWriter($stream, (New-Object System.Text.UTF8Encoding($false)))
        $writer.AutoFlush = $true
        $writer.NewLine = "`n"
        $buffer = ''
        Say 'PC connected' 'White'
    }
    if ($client) {
        try {
            $gone = $client.Client.Poll(0, 'SelectRead') -and $client.Available -eq 0
            $bytes = New-Object byte[] 65536
            while ($stream.DataAvailable) {
                $n = $stream.Read($bytes, 0, $bytes.Length)
                if ($n -le 0) { break }
                $buffer += [Text.Encoding]::UTF8.GetString($bytes, 0, $n)
            }
            while (($i = $buffer.IndexOf("`n")) -ge 0) {
                $line = $buffer.Substring(0, $i).Trim()
                $buffer = $buffer.Substring($i + 1)
                if ($line) { On-Line $line }
            }
            if ($gone) { throw 'closed' }
        } catch {
            Say 'PC disconnected' 'Yellow'
            $client.Close(); $client = $null; $writer = $null; $stream = $null
        }
    }
    Step-Tunnels
    while ([Console]::KeyAvailable) {
        $key = [Console]::ReadKey($true)
        if ($key.Key -eq 'Enter') {
            Write-Host ''
            $line = $typed
            $typed = ''
            On-Command $line
            Write-Host -NoNewline 'pet> '
        } elseif ($key.Key -eq 'Backspace') {
            if ($typed.Length -gt 0) { $typed = $typed.Substring(0, $typed.Length - 1); Write-Host -NoNewline "`b `b" }
        } elseif ($key.KeyChar -ge ' ') {
            $typed += $key.KeyChar
            Write-Host -NoNewline $key.KeyChar
        }
    }
    Start-Sleep -Milliseconds 50
}
if ($client) { $client.Close() }
$control.Stop()
$tunnels.Stop()
