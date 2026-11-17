# SPDX-License-Identifier: MIT
#
# AlicePET Jack In for Windows (first cut). Runs from the PET's USB drive with
# the PowerShell 5.1 that ships with Windows 10/11; nothing is installed and
# nothing is written to the PC (the session log is only saved if you click Log).
#
#  - Starts READ-ONLY: a scan (CPU, RAM, disk, startup items, Defender, top RAM users).
#  - Full Access: lets the PET send PowerShell commands, as you, until you turn it off.
#  - Request Admin: relaunches this app through the Windows UAC prompt that you click.
#  - Share PC Internet: the PET borrows this PC's internet (outbound connections only).
#  - Chat with Alice: type, or hold the mic button to talk (the mic records only
#    while you hold it). The PET carries it to Alice and her answer shows here.
#  - With Full Access, each PET command still asks you first (untick "Ask before
#    each command" to stop that). Alice's answers can be read aloud; hold F9 to talk.
#  - Every command is shown in the live log. JACK OUT ends everything at once.
# The app only dials out to the PET (10.55.0.1); it never listens. See PROTOCOL.md.

param(
    [string]$Pet = '10.55.0.1',
    [int]$Port = 7341,
    [int]$TunnelPort = 7342,
    [switch]$Elevated,
    [switch]$StartFull
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$AppVersion = '0.1'
$Here = Split-Path -Parent $PSCommandPath

# --- session state -------------------------------------------------------------

$S = [ordered]@{
    Client     = $null    # TcpClient to the PET
    Stream     = $null
    Writer     = $null
    Buffer     = ''
    Connecting = $null    # Task from ConnectAsync
    Full       = [bool]$StartFull
    Admin      = $false
    Net        = $false
    Jobs       = @()      # running PET commands: @{Id; Ps; Handle; Started; Cmd}
    Tunnels    = @()      # @{Id; Target; Pet; Phase; Tasks}
    Log        = New-Object System.Collections.Generic.List[string]
    Closing    = $false
    Busy       = $false    # a scan or a confirm box is up (they pump UI events)
    Confirm    = $true     # ask before each PET command, even with Full Access
    Speak      = $true     # read Alice's answers aloud
    Linked     = $false
}
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
$S.Admin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

# --- UI --------------------------------------------------------------------------

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Jack In · Alice'
$form.Size = New-Object System.Drawing.Size(820, 460)
$form.StartPosition = 'CenterScreen'
$form.BackColor = [System.Drawing.Color]::FromArgb(0xd7, 0xde, 0xeb)
$form.Font = New-Object System.Drawing.Font('Segoe UI', 10)
$form.TopMost = $true

$left = New-Object System.Windows.Forms.Panel
$left.Dock = 'Left'
$left.Width = 190
$left.BackColor = [System.Drawing.Color]::FromArgb(0x0f, 0x3b, 0x7a)
$form.Controls.Add($left)

$pic = New-Object System.Windows.Forms.PictureBox
$pic.SizeMode = 'Zoom'
$pic.SetBounds(15, 40, 160, 160)
$alicePng = Join-Path $Here 'alice.png'
if (Test-Path $alicePng) { $pic.Image = [System.Drawing.Image]::FromFile($alicePng) }
$left.Controls.Add($pic)

$linkLabel = New-Object System.Windows.Forms.Label
$linkLabel.ForeColor = [System.Drawing.Color]::FromArgb(0x7f, 0xd3, 0xff)
$linkLabel.Font = New-Object System.Drawing.Font('Consolas', 9, [System.Drawing.FontStyle]::Bold)
$linkLabel.SetBounds(10, 210, 170, 40)
$linkLabel.TextAlign = 'TopCenter'
$left.Controls.Add($linkLabel)

$modeLabel = New-Object System.Windows.Forms.Label
$modeLabel.SetBounds(10, 260, 170, 24)
$modeLabel.TextAlign = 'MiddleCenter'
$modeLabel.Font = New-Object System.Drawing.Font('Segoe UI', 9, [System.Drawing.FontStyle]::Bold)
$left.Controls.Add($modeLabel)

$extraLabel = New-Object System.Windows.Forms.Label
$extraLabel.SetBounds(10, 290, 170, 40)
$extraLabel.TextAlign = 'TopCenter'
$extraLabel.ForeColor = [System.Drawing.Color]::White
$extraLabel.Font = New-Object System.Drawing.Font('Segoe UI', 9)
$left.Controls.Add($extraLabel)

$alice = New-Object System.Windows.Forms.Label
$alice.SetBounds(205, 12, 590, 70)
$alice.BackColor = [System.Drawing.Color]::White
$alice.Padding = New-Object System.Windows.Forms.Padding(8)
$alice.Text = 'Alice: Looking for the PET...'
$form.Controls.Add($alice)

$ask = New-Object System.Windows.Forms.Label
$ask.SetBounds(205, 120, 590, 26)
$ask.BackColor = [System.Drawing.Color]::FromArgb(0xff, 0xd5, 0x4a)
$ask.ForeColor = [System.Drawing.Color]::FromArgb(0x3a, 0x1f, 0x00)
$ask.TextAlign = 'MiddleLeft'
$ask.Visible = $false
$form.Controls.Add($ask)

# Chat with Alice: type and press Enter, or hold the mic button and talk.
$chatBox = New-Object System.Windows.Forms.TextBox
$chatBox.SetBounds(205, 89, 400, 26)
$chatBox.Font = New-Object System.Drawing.Font('Segoe UI', 11)
$form.Controls.Add($chatBox)
$btnSend = New-Object System.Windows.Forms.Button
$btnSend.Text = 'Send'
$btnSend.SetBounds(610, 88, 80, 28)
$form.Controls.Add($btnSend)
$form.AcceptButton = $btnSend
$btnMic = New-Object System.Windows.Forms.Button
$btnMic.Text = 'Hold to talk'
$btnMic.SetBounds(695, 88, 100, 28)
$btnMic.FlatStyle = 'Flat'
$btnMic.BackColor = [System.Drawing.Color]::FromArgb(0xc0, 0x4a, 0xff)
$btnMic.ForeColor = [System.Drawing.Color]::White
$form.Controls.Add($btnMic)

function New-Check($text, $y, $checked) {
    $c = New-Object System.Windows.Forms.CheckBox
    $c.Text = $text
    $c.Checked = $checked
    $c.ForeColor = [System.Drawing.Color]::White
    $c.Font = New-Object System.Drawing.Font('Segoe UI', 9)
    $c.SetBounds(14, $y, 170, 22)
    $left.Controls.Add($c)
    return $c
}
$chkConfirm = New-Check 'Ask before each command' 334 $true
$chkSpeak = New-Check 'Alice reads answers aloud' 356 $true
$hint = New-Object System.Windows.Forms.Label
$hint.Text = 'Hold F9 to talk'
$hint.ForeColor = [System.Drawing.Color]::FromArgb(0x7f, 0xd3, 0xff)
$hint.Font = New-Object System.Drawing.Font('Segoe UI', 8)
$hint.SetBounds(14, 382, 170, 18)
$left.Controls.Add($hint)
$form.KeyPreview = $true

$logBox = New-Object System.Windows.Forms.TextBox
$logBox.Multiline = $true
$logBox.ReadOnly = $true
$logBox.ScrollBars = 'Vertical'
$logBox.SetBounds(205, 120, 590, 220)
$logBox.BackColor = [System.Drawing.Color]::FromArgb(0x0b, 0x15, 0x30)
$logBox.ForeColor = [System.Drawing.Color]::FromArgb(0x9f, 0xd8, 0xff)
$logBox.Font = New-Object System.Drawing.Font('Consolas', 9)
$form.Controls.Add($logBox)

function New-Button($text, $x, $w, $r, $g, $b) {
    $btn = New-Object System.Windows.Forms.Button
    $btn.Text = $text
    $btn.SetBounds($x, 360, $w, 40)
    $btn.FlatStyle = 'Flat'
    $btn.FlatAppearance.BorderSize = 0
    $btn.BackColor = [System.Drawing.Color]::FromArgb($r, $g, $b)
    $btn.ForeColor = [System.Drawing.Color]::White
    $btn.Font = New-Object System.Drawing.Font('Segoe UI', 9, [System.Drawing.FontStyle]::Bold)
    $form.Controls.Add($btn)
    return $btn
}
$btnFull = New-Button 'Full Access' 205 110 0x2f 0xc0 0x6a
$btnAdmin = New-Button 'Request Admin' 320 120 0xe8 0xa3 0x1a
$btnNet = New-Button 'Share PC Internet' 445 140 0x2f 0x7d 0xe0
$btnLog = New-Button 'Save Log' 590 85 0x5a 0x6b 0x8a
$btnOut = New-Button 'JACK OUT' 685 110 0xe5 0x38 0x4f

# --- helpers ---------------------------------------------------------------------

function Add-Log([string]$line) {
    $stamp = (Get-Date).ToString('HH:mm:ss')
    $S.Log.Add("$stamp  $line")
    $logBox.AppendText("$line`r`n")
}

function Send-Pet($obj) {
    if ($null -eq $S.Writer) { return }
    try {
        $S.Writer.WriteLine(($obj | ConvertTo-Json -Compress -Depth 4))
    } catch {
        Drop-Link 'the PET stopped answering'
    }
}

function Send-State([string]$denied = '') {
    $msg = [ordered]@{ t = 'state'; full = $S.Full; admin = $S.Admin; net = $S.Net }
    if ($denied) { $msg.denied = $denied }
    Send-Pet $msg
}

function Update-Ui {
    if ($S.Linked) {
        $linkLabel.Text = "ALICE.EXE`r`n● LINKED ($Pet)"
    } else {
        $linkLabel.Text = "ALICE.EXE`r`n○ looking for the PET..."
    }
    if ($S.Full) {
        $modeLabel.Text = 'FULL ACCESS'
        $modeLabel.BackColor = [System.Drawing.Color]::FromArgb(0x47, 0xf0, 0x8a)
        $btnFull.Text = 'Read-only'
    } else {
        $modeLabel.Text = 'READ-ONLY'
        $modeLabel.BackColor = [System.Drawing.Color]::FromArgb(0xff, 0xd5, 0x4a)
        $btnFull.Text = 'Full Access'
    }
    $extra = @()
    if ($S.Admin) { $extra += 'running as admin' }
    if ($S.Net) { $extra += ('sharing internet (' + @($S.Tunnels).Count + ' open)') }
    $extraLabel.Text = ($extra -join "`r`n")
    $btnAdmin.Enabled = -not $S.Admin
    $btnNet.Text = $(if ($S.Net) { 'Stop Sharing' } else { 'Share PC Internet' })
    $chatBox.Enabled = $S.Linked
    $btnSend.Enabled = $S.Linked
    $btnMic.Enabled = $S.Linked -or $Mic.On
}

function Hide-Ask {
    $ask.Visible = $false
    $logBox.SetBounds(205, 120, 590, 220)
}

function Show-Ask([string]$what) {
    $names = @{ full = 'Full Access'; admin = 'Request Admin'; net = 'Share PC Internet' }
    if (-not $names.ContainsKey($what)) { return }
    $ask.Text = "  The PET asks for $($names[$what]). Click that button below to allow it, or ignore this."
    $ask.Visible = $true
    $ask.BringToFront()
    $logBox.SetBounds(205, 150, 590, 190)
    $form.Activate()
}

# --- read-only scan --------------------------------------------------------------

function Invoke-Scan {
    $S.Busy = $true
    $form.Cursor = [System.Windows.Forms.Cursors]::WaitCursor
    $scan = [ordered]@{ t = 'scan' }
    $steps = @(
        @{ Text = 'Get-CimInstance Win32_Processor (load)'; Do = {
            $scan.cpu = [int](Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average } },
        @{ Text = 'Get-CimInstance Win32_OperatingSystem (memory)'; Do = {
            $os = Get-CimInstance Win32_OperatingSystem
            $scan.ram = [int](100 - 100 * $os.FreePhysicalMemory / $os.TotalVisibleMemorySize) } },
        @{ Text = 'Get-CimInstance Win32_LogicalDisk C:'; Do = {
            $d = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='C:'"
            $scan.disk = 'C:'
            $scan.disk_free = [int](100 * $d.FreeSpace / $d.Size) } },
        @{ Text = 'Get-CimInstance Win32_StartupCommand'; Do = {
            $scan.startup = @(Get-CimInstance Win32_StartupCommand).Count } },
        @{ Text = 'Get-MpComputerStatus'; Do = {
            $mp = Get-MpComputerStatus
            $scan.defender = $(if ($mp.RealTimeProtectionEnabled) { 'on' } else { 'off' }) } },
        @{ Text = 'Get-Process | sort WS -desc | select -f 5'; Do = {
            $scan.top = @(Get-Process | Group-Object ProcessName | ForEach-Object {
                    [ordered]@{ name = $_.Name; mb = [int](($_.Group | Measure-Object WorkingSet64 -Sum).Sum / 1MB) }
                } | Sort-Object { $_.mb } -Descending | Select-Object -First 5) } }
    )
    foreach ($step in $steps) {
        $ok = $true
        try { & $step.Do } catch { $ok = $false }
        Add-Log ("> " + $step.Text + $(if ($ok) { '' } else { '   (failed)' }))
        Send-Pet ([ordered]@{ t = 'log'; text = $step.Text; ok = $ok })
        [System.Windows.Forms.Application]::DoEvents()
    }
    if (-not $scan.Contains('defender')) { $scan.defender = '?' }
    Send-Pet $scan
    $summary = @()
    if ($scan.Contains('ram')) { $summary += "RAM $($scan.ram)%" }
    if ($scan.Contains('disk_free')) { $summary += "C: $($scan.disk_free)% free" }
    if ($scan.Contains('startup')) { $summary += "$($scan.startup) startup apps" }
    $summary += "Defender $($scan.defender)"
    $alice.Text = 'Alice: Scan done (read-only): ' + ($summary -join ', ') + '. Look at the PET for what I think.'
    Add-Log 'read-only checks complete'
    $form.Cursor = [System.Windows.Forms.Cursors]::Default
    $S.Busy = $false
}

# --- commands from the PET -------------------------------------------------------

function Start-PetCommand($id, [string]$cmd) {
    if (-not $S.Full) {
        Add-Log "PET asked to run (refused, read-only): $cmd"
        Send-Pet ([ordered]@{ t = 'result'; id = $id; ok = $false; out = 'read-only: turn on Full Access on the PC'; reason = 'read-only' })
        Show-Ask 'full'
        return
    }
    if ($S.Confirm) {
        $S.Busy = $true
        $answer = [System.Windows.Forms.MessageBox]::Show($form,
            "The PET wants to run this as $env:USERNAME$(if ($S.Admin) { ' (admin)' }):`r`n`r`n$cmd`r`n`r`nRun it?",
            'Jack In: run a command?', 'YesNo', 'Question', 'Button2')
        $S.Busy = $false
        if ($answer -ne 'Yes') {
            Add-Log "you declined: $cmd"
            Send-Pet ([ordered]@{ t = 'result'; id = $id; ok = $false; out = 'declined on the PC' })
            return
        }
    }
    Add-Log "PET > $cmd"
    $ps = [PowerShell]::Create()
    [void]$ps.AddScript($cmd).AddCommand('Out-String').AddParameter('Width', 120)
    $S.Jobs += @{ Id = $id; Ps = $ps; Handle = $ps.BeginInvoke(); Started = Get-Date; Cmd = $cmd }
}

function Step-Jobs {
    $left = @()
    foreach ($job in $S.Jobs) {
        $timedOut = ((Get-Date) - $job.Started).TotalSeconds -gt 120
        if (-not $job.Handle.IsCompleted -and -not $timedOut) { $left += $job; continue }
        $out = ''
        $ok = $true
        try {
            if ($timedOut) { $job.Ps.Stop(); throw 'stopped after 2 minutes' }
            $out = ($job.Ps.EndInvoke($job.Handle) | Out-String).Trim()
            if ($job.Ps.HadErrors) {
                $ok = $false
                $out = (($job.Ps.Streams.Error | ForEach-Object { $_.ToString() }) -join '; ') + ' ' + $out
            }
        } catch {
            $ok = $false
            $out = $_.Exception.Message
        }
        $job.Ps.Dispose()
        if ($out.Length -gt 4000) { $out = $out.Substring(0, 4000) + '...' }
        Add-Log ($(if ($ok) { '  ok' } else { '  failed' }) + $(if ($out) { ': ' + ($out -split "`n")[0] } else { '' }))
        Send-Pet ([ordered]@{ t = 'result'; id = $job.Id; ok = $ok; out = $out })
    }
    $S.Jobs = $left
}

function Stop-Jobs {
    foreach ($job in $S.Jobs) { try { $job.Ps.Stop(); $job.Ps.Dispose() } catch { } }
    $S.Jobs = @()
}

# --- Share PC Internet (the PC dials both ends; nothing listens here) -------------

function Open-Tunnel($id, [string]$targetHost, [int]$targetPort) {
    if (-not $S.Net) { Send-Pet ([ordered]@{ t = 'net_fail'; id = $id }); return }
    $target = New-Object System.Net.Sockets.TcpClient
    $back = New-Object System.Net.Sockets.TcpClient   # back to the PET's tunnel port
    $S.Tunnels += @{
        Id = $id; Target = $target; Pet = $back; Phase = 'connect'; Started = Get-Date
        Tasks = @($target.ConnectAsync($targetHost, $targetPort), $back.ConnectAsync($script:Pet, $TunnelPort))
    }
}

function Close-Tunnel($t) {
    try { $t.Target.Close() } catch { }
    try { $t.Pet.Close() } catch { }
}

function Step-Tunnels {
    $left = @()
    foreach ($t in $S.Tunnels) {
        if ($t.Phase -eq 'connect') {
            $failed = @($t.Tasks | Where-Object { $_.IsFaulted -or $_.IsCanceled }).Count -gt 0
            $slow = ((Get-Date) - $t.Started).TotalSeconds -gt 12
            if ($failed -or $slow) {
                Close-Tunnel $t
                Send-Pet ([ordered]@{ t = 'net_fail'; id = $t.Id })
                continue
            }
            if (@($t.Tasks | Where-Object { -not $_.IsCompleted }).Count -gt 0) { $left += $t; continue }
            $hello = [System.Text.Encoding]::ASCII.GetBytes("tunnel $($t.Id)`n")
            $ps = $t.Pet.GetStream()
            $ps.Write($hello, 0, $hello.Length)
            $ts = $t.Target.GetStream()
            $t.Tasks = @($ps.CopyToAsync($ts), $ts.CopyToAsync($ps))
            $t.Phase = 'open'
            $left += $t
        } elseif (@($t.Tasks | Where-Object { $_.IsCompleted }).Count -gt 0) {
            Close-Tunnel $t
        } else {
            $left += $t
        }
    }
    $S.Tunnels = $left
}

function Stop-Sharing {
    foreach ($t in $S.Tunnels) { Close-Tunnel $t }
    $S.Tunnels = @()
    $S.Net = $false
}

# Alice's answers read aloud with the Windows voice (System.Speech, built in).
$Voice = $null
function Speak-Alice([string]$text) {
    if (-not $S.Speak -or -not $text) { return }
    try {
        if ($null -eq $script:Voice) {
            Add-Type -AssemblyName System.Speech
            $script:Voice = New-Object System.Speech.Synthesis.SpeechSynthesizer
            $female = $script:Voice.GetInstalledVoices() | Where-Object { $_.VoiceInfo.Gender -eq 'Female' } | Select-Object -First 1
            if ($female) { $script:Voice.SelectVoice($female.VoiceInfo.Name) }
        }
        $script:Voice.SpeakAsyncCancelAll()
        [void]$script:Voice.SpeakAsync($text)
    } catch {
        Add-Log "voice: can't speak ($($_.Exception.Message))"
        $S.Speak = $false
    }
}

# --- chat with Alice ---------------------------------------------------------------

function Send-Chat {
    $text = $chatBox.Text.Trim()
    if (-not $text -or -not $S.Linked) { return }
    $chatBox.Text = ''
    Add-Log "you: $text"
    $alice.Text = 'Alice: ...'
    Send-Pet ([ordered]@{ t = 'chat'; text = $text })
}

# The mic records only while the button is held (max 15 s), through the
# Windows multimedia API; the clip goes to the PET and its temp file is deleted.
$Mic = @{ On = $false; Started = $null; Ready = $false }
function Initialize-Mic {
    if ($Mic.Ready) { return $true }
    try {
        Add-Type -Namespace JackIn -Name Mci -MemberDefinition @'
[DllImport("winmm.dll", CharSet = CharSet.Unicode)]
public static extern int mciSendString(string command, System.Text.StringBuilder ret, int retLen, System.IntPtr hwnd);
'@
        $Mic.Ready = $true
    } catch {
        Add-Log "mic: can't load the Windows audio API ($($_.Exception.Message))"
    }
    return $Mic.Ready
}
function Invoke-Mci([string]$command) {
    return [JackIn.Mci]::mciSendString($command, $null, 0, [IntPtr]::Zero)
}

function Start-Mic {
    if ($Mic.On -or -not $S.Linked -or -not (Initialize-Mic)) { return }
    [void](Invoke-Mci 'close jackmic')
    $rc = Invoke-Mci 'open new type waveaudio alias jackmic'
    if ($rc -eq 0) { $rc = Invoke-Mci 'set jackmic bitspersample 16 channels 1 samplespersec 16000 bytespersec 32000 alignment 2' }
    if ($rc -eq 0) { $rc = Invoke-Mci 'record jackmic' }
    if ($rc -ne 0) {
        [void](Invoke-Mci 'close jackmic')
        Add-Log "mic: no microphone to record from (error $rc)"
        return
    }
    $Mic.On = $true
    $Mic.Started = Get-Date
    $btnMic.Text = '● REC (let go)'
    $btnMic.BackColor = [System.Drawing.Color]::FromArgb(0xe5, 0x38, 0x4f)
}

function Stop-Mic([bool]$send = $true) {
    if (-not $Mic.On) { return }
    $Mic.On = $false
    $btnMic.Text = 'Hold to talk'
    $btnMic.BackColor = [System.Drawing.Color]::FromArgb(0xc0, 0x4a, 0xff)
    [void](Invoke-Mci 'stop jackmic')
    $seconds = ((Get-Date) - $Mic.Started).TotalSeconds
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('jackin-' + [guid]::NewGuid().ToString('N') + '.wav')
    $bytes = $null
    try {
        if ($send -and $seconds -ge 0.6 -and (Invoke-Mci ('save jackmic "' + $tmp + '"')) -eq 0) {
            $bytes = [IO.File]::ReadAllBytes($tmp)
        }
    } finally {
        [void](Invoke-Mci 'close jackmic')
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    }
    if (-not $send) { return }
    if ($null -eq $bytes) { Add-Log 'mic: too short, hold the button while you talk'; return }
    Add-Log ('you: (voice, {0:N0} s)' -f $seconds)
    $alice.Text = 'Alice: listening back...'
    Send-Pet ([ordered]@{ t = 'voice'; wav = [Convert]::ToBase64String($bytes) })
}

# --- link ------------------------------------------------------------------------

function Drop-Link([string]$why) {
    if ($S.Client) { try { $S.Client.Close() } catch { } }
    $S.Client = $null; $S.Stream = $null; $S.Writer = $null; $S.Buffer = ''
    if ($S.Linked) {
        $S.Linked = $false
        # No PET, no access: back to read-only, sharing off.
        $S.Full = $false
        Stop-Mic $false
        Stop-Jobs
        Stop-Sharing
        Add-Log "link lost: $why (back to read-only)"
        $alice.Text = 'Alice: Lost the PET. Plug it back in and I''ll reconnect read-only.'
    }
    Update-Ui
}

function Jack-Out([string]$why) {
    if ($S.Closing) { return }
    $S.Closing = $true
    Stop-Mic $false
    Send-Pet ([ordered]@{ t = 'bye'; why = $why })
    $S.Full = $false
    Stop-Jobs
    Stop-Sharing
    if ($S.Client) { try { $S.Client.Close() } catch { } }
    $timer.Stop()
    $form.Close()
}

function On-Line([string]$line) {
    try { $msg = $line | ConvertFrom-Json } catch { return }
    switch ($msg.t) {
        'ask' { Show-Ask $msg.what }
        'drop' {
            if ($msg.what -eq 'full' -and $S.Full) { $S.Full = $false; Add-Log 'PET turned Full Access off'; Send-State }
            if ($msg.what -eq 'net' -and $S.Net) { Stop-Sharing; Add-Log 'PET stopped sharing'; Send-State }
            Update-Ui
        }
        'run' { Start-PetCommand $msg.id ([string]$msg.cmd) }
        'scan' { Invoke-Scan }
        'net_open' { Open-Tunnel $msg.id ([string]$msg.host) ([int]$msg.port) }
        'jackout' { Add-Log 'PET pressed JACK OUT'; Jack-Out 'PET' }
        'alice' {
            if ($msg.heard) { Add-Log "you (heard): $($msg.heard)" }
            $alice.Text = 'Alice: ' + $msg.text
            Add-Log ('Alice: ' + $msg.text)
            if ($msg.ok) { Speak-Alice ([string]$msg.text) }
            if ($msg.cmd) { Add-Log "Alice suggests: $($msg.cmd)   (press Enter on the PET to run it; needs Full Access)" }
        }
    }
}

function Step-Link {
    if ($null -eq $S.Client) {
        if ($null -eq $S.Connecting) {
            $S.Client = New-Object System.Net.Sockets.TcpClient
            $S.Connecting = $S.Client.ConnectAsync($Pet, $Port)
            return
        }
    }
    if ($S.Connecting) {
        if (-not $S.Connecting.IsCompleted) { return }
        $failed = $S.Connecting.IsFaulted -or $S.Connecting.IsCanceled
        $S.Connecting = $null
        if ($failed) { try { $S.Client.Close() } catch { }; $S.Client = $null; return }
        $S.Client.NoDelay = $true
        $S.Stream = $S.Client.GetStream()
        $S.Writer = New-Object System.IO.StreamWriter($S.Stream, (New-Object System.Text.UTF8Encoding($false)))
        $S.Writer.AutoFlush = $true
        $S.Writer.NewLine = "`n"
        $S.Linked = $true
        $os = (Get-CimInstance Win32_OperatingSystem).Caption -replace '^Microsoft ', ''
        Send-Pet ([ordered]@{ t = 'hello'; host = $env:COMPUTERNAME; os = $os; user = $env:USERNAME
                admin = $S.Admin; full = $S.Full; app = $AppVersion })
        Add-Log "linked to the PET at $Pet ($(if ($S.Full) { 'full access' } else { 'read-only' }))"
        Update-Ui
        Invoke-Scan
        return
    }
    # Read whatever arrived.
    try {
        if (-not $S.Client.Connected -or ($S.Client.Client.Poll(0, 'SelectRead') -and $S.Client.Available -eq 0)) {
            Drop-Link 'unplugged or PET closed the link'
            return
        }
        $bytes = New-Object byte[] 8192
        while ($S.Stream -and $S.Stream.DataAvailable) {
            $n = $S.Stream.Read($bytes, 0, $bytes.Length)
            if ($n -le 0) { break }
            $S.Buffer += [System.Text.Encoding]::UTF8.GetString($bytes, 0, $n)
        }
        while ($S.Stream -and ($i = $S.Buffer.IndexOf("`n")) -ge 0) {
            $line = $S.Buffer.Substring(0, $i).Trim()
            $S.Buffer = $S.Buffer.Substring($i + 1)
            if ($line) { On-Line $line }
        }
    } catch {
        Drop-Link 'the link broke'
    }
}

# --- buttons -----------------------------------------------------------------------

$btnFull.Add_Click({
    $S.Full = -not $S.Full
    Hide-Ask
    Add-Log $(if ($S.Full) { 'you turned Full Access ON: the PET can run commands as you' } else { 'you turned Full Access off' })
    Send-State
    Update-Ui
})

$btnAdmin.Add_Click({
    Hide-Ask
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-STA', '-WindowStyle', 'Hidden', '-File', "`"$PSCommandPath`"",
        '-Pet', $Pet, '-Port', $Port, '-TunnelPort', $TunnelPort, '-Elevated')
    if ($S.Full) { $argList += '-StartFull' }
    try {
        # Windows shows the UAC prompt; you click Yes or No.
        Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -Verb RunAs | Out-Null
    } catch {
        Add-Log 'admin: UAC prompt was cancelled'
        Send-State 'admin'
        return
    }
    Add-Log 'admin: handing over to the elevated copy'
    # The elevated copy dials the PET itself; this one leaves quietly.
    $S.Closing = $true
    Stop-Jobs
    Stop-Sharing
    $timer.Stop()
    $form.Close()
})

$btnNet.Add_Click({
    Hide-Ask
    if ($S.Net) {
        Stop-Sharing
        Add-Log 'you stopped sharing this PC''s internet'
    } else {
        $S.Net = $true
        Add-Log 'you turned on Share PC Internet (the PET''s connections go out through this PC)'
    }
    Send-State
    Update-Ui
})

$btnLog.Add_Click({
    $dlg = New-Object System.Windows.Forms.SaveFileDialog
    $dlg.FileName = 'jackin-' + (Get-Date).ToString('yyyy-MM-dd-HHmm') + '.txt'
    $dlg.Filter = 'Text|*.txt'
    if ($dlg.ShowDialog($form) -eq 'OK') {
        [System.IO.File]::WriteAllLines($dlg.FileName, $S.Log)
        Add-Log "log saved to $($dlg.FileName)"
    }
})

$btnSend.Add_Click({ Send-Chat })
$btnMic.Add_MouseDown({ Start-Mic })
$btnMic.Add_MouseUp({ Stop-Mic })
# Hold F9 anywhere in the window to talk (key repeat is ignored while recording).
$form.Add_KeyDown({ param($sender, $e) if ($e.KeyCode -eq 'F9') { $e.Handled = $true; if (-not $Mic.On) { Start-Mic } } })
$form.Add_KeyUp({ param($sender, $e) if ($e.KeyCode -eq 'F9') { $e.Handled = $true; Stop-Mic } })
$chkConfirm.Add_CheckedChanged({
    $S.Confirm = $chkConfirm.Checked
    Add-Log $(if ($S.Confirm) { 'you will be asked before each PET command' } else { 'PET commands run without asking while Full Access is on' })
})
$chkSpeak.Add_CheckedChanged({
    $S.Speak = $chkSpeak.Checked
    if (-not $S.Speak -and $Voice) { $Voice.SpeakAsyncCancelAll() }
})

$btnOut.Add_Click({ Add-Log 'you pressed JACK OUT'; Jack-Out 'PC' })
$form.Add_FormClosing({ if (-not $S.Closing) { Jack-Out 'window closed' } })

# --- run -------------------------------------------------------------------------

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 100
$tick = 0
$timer.Add_Tick({
    if ($S.Busy) { return }
    try {
        # Dial the PET about every 2 s until it answers.
        $script:tick++
        if ($S.Client -or $S.Connecting -or ($script:tick % 20) -eq 1) { Step-Link }
        Step-Jobs
        Step-Tunnels
        if ($Mic.On -and ((Get-Date) - $Mic.Started).TotalSeconds -ge 15) { Stop-Mic }
        if ($S.Net) { Update-Ui }
    } catch {
        Add-Log ('error: ' + $_.Exception.Message)
    }
})

Add-Log "Jack In $AppVersion$(if ($Elevated) { ' (admin)' }) · $env:COMPUTERNAME · read-only until you click Full Access"
Update-Ui
$timer.Start()
[void]$form.ShowDialog()
$timer.Dispose()
if ($Voice) { $Voice.Dispose() }
$form.Dispose()
