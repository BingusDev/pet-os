# SPDX-License-Identifier: MIT
#
# AlicePET Jack In for Windows (first cut). Runs from the PET's USB drive with
# the PowerShell 5.1 that ships with Windows 10/11; nothing is installed and
# nothing is written to the PC (the session log is only saved if you click Log).
#
#  - Starts READ-ONLY: a scan (CPU, RAM, disk, startup items, Defender, top RAM users).
#  - Access toggle: Read (look only), Ask (PET commands run as you after you OK
#    each one), Full (PET commands run as you without asking).
#  - Run as admin...: relaunches this app through the Windows UAC prompt that you click.
#  - Share PC Internet: the PET borrows this PC's internet (outbound connections only).
#  - Chat with Alice: type, or hold the mic button to talk (the mic records only
#    while you hold it). The PET carries it to Alice and her answer shows here.
#  - Alice's answers can be read aloud; hold F9 (or the talk button) to talk.
#  - Every command is shown in the live log. JACK OUT ends everything at once.
# The app only dials out to the PET (10.55.0.1); it never listens. See PROTOCOL.md.

param(
    [string]$Pet = '10.55.0.1',
    [int]$Port = 7341,
    [int]$TunnelPort = 7342,
    [switch]$Elevated,
    [switch]$StartFull,
    [ValidateSet('', 'Read', 'Ask', 'Full')][string]$StartMode = ''
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$AppVersion = '0.2'
$Here = Split-Path -Parent $PSCommandPath

# --- session state -------------------------------------------------------------

$S = [ordered]@{
    Client     = $null    # TcpClient to the PET
    Stream     = $null
    Writer     = $null
    Buffer     = ''
    Connecting = $null    # Task from ConnectAsync
    Full       = [bool]$StartFull -or $StartMode -eq 'Ask' -or $StartMode -eq 'Full'
    Admin      = $false
    Net        = $false
    Jobs       = @()      # running PET commands: @{Id; Ps; Handle; Started; Cmd}
    Tunnels    = @()      # @{Id; Target; Pet; Phase; Tasks}
    Log        = New-Object System.Collections.Generic.List[string]
    Closing    = $false
    Busy       = $false    # a scan or a confirm box is up (they pump UI events)
    Confirm    = ($StartMode -ne 'Full')  # Ask mode: OK each PET command first
    Speak      = $true     # read Alice's answers aloud
    Linked     = $false
}
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
$S.Admin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

# --- UI --------------------------------------------------------------------------

function RGB($r, $g, $b) { return [System.Drawing.Color]::FromArgb($r, $g, $b) }

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Jack In · Alice'
$form.ClientSize = New-Object System.Drawing.Size(800, 445)
$form.FormBorderStyle = 'FixedSingle'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.BackColor = RGB 0xd7 0xde 0xeb
$form.Font = New-Object System.Drawing.Font('Segoe UI', 10)
$form.KeyPreview = $true

# Sidebar: portrait, link status, access mode, voice.
$left = New-Object System.Windows.Forms.Panel
$left.Dock = 'Left'
$left.Width = 190
$left.BackColor = RGB 0x0f 0x3b 0x7a
$form.Controls.Add($left)

$pic = New-Object System.Windows.Forms.PictureBox
$pic.SizeMode = 'Zoom'
$pic.SetBounds(20, 14, 150, 150)
$alicePng = Join-Path $Here 'alice.png'
if (Test-Path $alicePng) { $pic.Image = [System.Drawing.Image]::FromFile($alicePng) }
$left.Controls.Add($pic)

$linkLabel = New-Object System.Windows.Forms.Label
$linkLabel.ForeColor = RGB 0x7f 0xd3 0xff
$linkLabel.Font = New-Object System.Drawing.Font('Consolas', 9, [System.Drawing.FontStyle]::Bold)
$linkLabel.SetBounds(6, 172, 178, 20)
$linkLabel.TextAlign = 'MiddleCenter'
$left.Controls.Add($linkLabel)

$accessLabel = New-Object System.Windows.Forms.Label
$accessLabel.Text = 'ACCESS'
$accessLabel.ForeColor = RGB 0xbb 0xcc 0xdd
$accessLabel.Font = New-Object System.Drawing.Font('Segoe UI', 8)
$accessLabel.SetBounds(14, 202, 160, 16)
$left.Controls.Add($accessLabel)

# One toggle replaces the old Full Access button, mode badge and
# "ask before each command" box: Read = read-only, Ask = commands run after
# you say yes to each one, Full = commands run without asking.
$ModeButtons = [ordered]@{}
$modeX = 14
foreach ($m in @('Read', 'Ask', 'Full')) {
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $m
    $b.Tag = $m
    $b.SetBounds($modeX, 220, 54, 30)
    $b.FlatStyle = 'Flat'
    $b.FlatAppearance.BorderSize = 0
    $b.Font = New-Object System.Drawing.Font('Segoe UI', 9, [System.Drawing.FontStyle]::Bold)
    $left.Controls.Add($b)
    $ModeButtons[$m] = $b
    $modeX += 55
}

$modeNote = New-Object System.Windows.Forms.Label
$modeNote.ForeColor = RGB 0x9b 0xbb 0xcc
$modeNote.Font = New-Object System.Drawing.Font('Segoe UI', 8)
$modeNote.SetBounds(14, 254, 168, 34)
$left.Controls.Add($modeNote)

# Admin is a separate Windows UAC prompt, so it stays a small link, shown only
# while this copy isn't elevated.
$lnkAdmin = New-Object System.Windows.Forms.LinkLabel
$lnkAdmin.Text = 'Run as admin...'
$lnkAdmin.LinkColor = RGB 0xff 0xd5 0x4a
$lnkAdmin.ActiveLinkColor = RGB 0xff 0xff 0xff
$lnkAdmin.Font = New-Object System.Drawing.Font('Segoe UI', 8)
$lnkAdmin.SetBounds(14, 290, 160, 18)
$left.Controls.Add($lnkAdmin)

$netLabel = New-Object System.Windows.Forms.Label
$netLabel.ForeColor = RGB 0x7f 0xd3 0xff
$netLabel.Font = New-Object System.Drawing.Font('Segoe UI', 8)
$netLabel.SetBounds(14, 312, 168, 18)
$left.Controls.Add($netLabel)

$chkSpeak = New-Object System.Windows.Forms.CheckBox
$chkSpeak.Text = 'Read answers aloud'
$chkSpeak.Checked = $true
$chkSpeak.ForeColor = [System.Drawing.Color]::White
$chkSpeak.Font = New-Object System.Drawing.Font('Segoe UI', 9)
$chkSpeak.SetBounds(14, 412, 170, 22)
$left.Controls.Add($chkSpeak)

# Main panel: request banner, chat log, input row, action bar.
$ask = New-Object System.Windows.Forms.Label
$ask.SetBounds(200, 10, 590, 26)
$ask.BackColor = RGB 0xff 0xd5 0x4a
$ask.ForeColor = RGB 0x3a 0x1f 0x00
$ask.TextAlign = 'MiddleLeft'
$ask.Visible = $false
$form.Controls.Add($ask)

$LogTop = 10
$LogHeight = 330
$logBox = New-Object System.Windows.Forms.RichTextBox
$logBox.ReadOnly = $true
$logBox.BorderStyle = 'None'
$logBox.ScrollBars = 'Vertical'
$logBox.SetBounds(200, $LogTop, 590, $LogHeight)
$logBox.BackColor = RGB 0x0b 0x18 0x30
$logBox.ForeColor = RGB 0xdf 0xe8 0xff
$logBox.Font = New-Object System.Drawing.Font('Segoe UI', 10)
$logBox.SelectionIndent = 8
$logBox.SelectionRightIndent = 8
$form.Controls.Add($logBox)

$chatBox = New-Object System.Windows.Forms.TextBox
$chatBox.SetBounds(200, 352, 400, 30)
$chatBox.Font = New-Object System.Drawing.Font('Segoe UI', 11)
$form.Controls.Add($chatBox)

function New-Button($text, $x, $y, $w, $h, $color) {
    $btn = New-Object System.Windows.Forms.Button
    $btn.Text = $text
    $btn.SetBounds($x, $y, $w, $h)
    $btn.FlatStyle = 'Flat'
    $btn.FlatAppearance.BorderSize = 0
    $btn.BackColor = $color
    $btn.ForeColor = [System.Drawing.Color]::White
    $btn.Font = New-Object System.Drawing.Font('Segoe UI', 9, [System.Drawing.FontStyle]::Bold)
    $form.Controls.Add($btn)
    return $btn
}
$MicIdle = 'Hold to talk · F9'
$btnSend = New-Button 'Send' 606 351 70 30 (RGB 0x2d 0x6b 0xe0)
$btnMic = New-Button $MicIdle 680 351 110 30 (RGB 0xc0 0x4a 0xff)
$form.AcceptButton = $btnSend
$btnNet = New-Button 'Share PC Internet' 200 397 140 36 (RGB 0x2f 0x7f 0xd8)
$btnLog = New-Button 'Save Log' 346 397 90 36 (RGB 0x5d 0x6b 0x82)
$btnOut = New-Button 'JACK OUT' 680 397 110 36 (RGB 0xe2 0x3b 0x4e)

# --- helpers ---------------------------------------------------------------------

function Write-Rich([string]$text, $color, [bool]$bold = $false, $back = $null, [float]$size = 10) {
    $logBox.SelectionStart = $logBox.TextLength
    $logBox.SelectionLength = 0
    $logBox.SelectionColor = $color
    $logBox.SelectionBackColor = $(if ($back) { $back } else { $logBox.BackColor })
    $style = $(if ($bold) { [System.Drawing.FontStyle]::Bold } else { [System.Drawing.FontStyle]::Regular })
    $logBox.SelectionFont = New-Object System.Drawing.Font('Segoe UI', $size, $style)
    $logBox.AppendText($text)
}

# Everything goes to the saved log; the window shows chat as colored turns and
# app/PET events as small grey lines.
function Add-Log([string]$line) {
    $stamp = (Get-Date).ToString('HH:mm:ss')
    $S.Log.Add("$stamp  $line")
    Write-Rich "$line`n" (RGB 0x7f 0x93 0xb5) $false $null 8.5
    $logBox.ScrollToCaret()
}

function Add-Chat([string]$who, [string]$text) {
    $stamp = (Get-Date).ToString('HH:mm:ss')
    $S.Log.Add("$stamp  ${who}: $text")
    if ($who -eq 'Alice') {
        $card = RGB 0x13 0x28 0x4d
        Write-Rich ' Alice  ' (RGB 0xff 0x9b 0xe3) $true $card
        Write-Rich "$text `n" (RGB 0xdf 0xe8 0xff) $false $card
    } else {
        Write-Rich "$who  " (RGB 0x7f 0xd3 0xff) $true
        Write-Rich "$text`n" (RGB 0xdf 0xe8 0xff)
    }
    Write-Rich "`n" (RGB 0xdf 0xe8 0xff) $false $null 4
    $logBox.ScrollToCaret()
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

function Get-Mode {
    if (-not $S.Full) { return 'Read' }
    if ($S.Confirm) { return 'Ask' }
    return 'Full'
}

function Update-Ui {
    if ($S.Linked) {
        $linkLabel.Text = "● LINKED · $Pet"
    } else {
        $linkLabel.Text = '○ looking for the PET...'
    }
    $mode = Get-Mode
    foreach ($m in $ModeButtons.Keys) {
        $b = $ModeButtons[$m]
        if ($m -eq $mode) {
            $b.BackColor = $(switch ($m) { 'Read' { RGB 0xff 0xd5 0x4a } 'Ask' { RGB 0x7f 0xd3 0xff } default { RGB 0x3d 0xdc 0x84 } })
            $b.ForeColor = RGB 0x05 0x20 0x30
        } else {
            $b.BackColor = RGB 0x1b 0x4f 0x97
            $b.ForeColor = RGB 0xcc 0xee 0xff
        }
    }
    $who = $(if ($S.Admin) { 'as admin' } else { "as $env:USERNAME" })
    $modeNote.Text = $(switch ($mode) {
            'Read' { 'Read = Alice can look, not change' }
            'Ask' { "Ask = runs $who after you OK each one" }
            default { "Full = runs $who, no prompts" }
        })
    $lnkAdmin.Visible = -not $S.Admin
    $netLabel.Text = $(if ($S.Net) { 'sharing internet (' + @($S.Tunnels).Count + ' open)' } else { '' })
    $btnNet.Text = $(if ($S.Net) { 'Stop Sharing' } else { 'Share PC Internet' })
    $chatBox.Enabled = $S.Linked
    $btnSend.Enabled = $S.Linked
    $btnMic.Enabled = $S.Linked -or $Mic.On
}

function Set-Mode([string]$mode) {
    $before = Get-Mode
    if ($mode -eq $before) { return }
    $S.Full = ($mode -ne 'Read')
    $S.Confirm = ($mode -ne 'Full')
    Hide-Ask
    Add-Log $(switch ($mode) {
            'Read' { 'you set access to Read: the PET can look but not run commands' }
            'Ask' { 'you set access to Ask: the PET can run commands as you after you OK each one' }
            default { 'you set access to Full: the PET can run commands as you without asking' }
        })
    if (($before -eq 'Read') -ne ($mode -eq 'Read')) { Send-State }
    Update-Ui
}

function Hide-Ask {
    $ask.Visible = $false
    $logBox.SetBounds(200, $LogTop, 590, $LogHeight)
}

function Show-Ask([string]$what) {
    $names = @{ full = 'Ask or Full access'; admin = 'admin (Run as admin... in the sidebar)'; net = 'Share PC Internet' }
    if (-not $names.ContainsKey($what)) { return }
    $ask.Text = "  The PET asks for $($names[$what]). Turn it on to allow it, or ignore this."
    $ask.Visible = $true
    $ask.BringToFront()
    $logBox.SetBounds(200, $LogTop + 32, 590, $LogHeight - 32)
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
    Add-Log ('scan done (read-only): ' + ($summary -join ', ') + '. Look at the PET for what Alice thinks.')
    Add-Log 'read-only checks complete'
    $form.Cursor = [System.Windows.Forms.Cursors]::Default
    $S.Busy = $false
}

# --- commands from the PET -------------------------------------------------------

function Start-PetCommand($id, [string]$cmd) {
    if (-not $S.Full) {
        Add-Log "PET asked to run (refused, read-only): $cmd"
        Send-Pet ([ordered]@{ t = 'result'; id = $id; ok = $false; out = 'read-only: pick Ask or Full on the PC'; reason = 'read-only' })
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
    Add-Chat 'you' $text
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
    $btnMic.Text = $MicIdle
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
    Add-Chat 'you' ('(voice, {0:N0} s)' -f $seconds)
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

# A field of a PET message, or $null when the PET left it out. Under
# Set-StrictMode, reading a missing property throws, and a throw here used to
# be taken for a broken link.
function Get-Field($msg, [string]$name) {
    if ($null -eq $msg) { return $null }
    $prop = $msg.PSObject.Properties[$name]
    if ($null -eq $prop) { return $null }
    return $prop.Value
}

function On-Line([string]$line) {
    try { $msg = $line | ConvertFrom-Json } catch { return }
    $what = [string](Get-Field $msg 'what')
    switch ([string](Get-Field $msg 't')) {
        'ask' { Show-Ask $what }
        'drop' {
            if ($what -eq 'full' -and $S.Full) { $S.Full = $false; Add-Log 'PET set access back to Read'; Send-State }
            if ($what -eq 'net' -and $S.Net) { Stop-Sharing; Add-Log 'PET stopped sharing'; Send-State }
            Update-Ui
        }
        'run' { Start-PetCommand (Get-Field $msg 'id') ([string](Get-Field $msg 'cmd')) }
        'scan' { Invoke-Scan }
        'net_open' { Open-Tunnel (Get-Field $msg 'id') ([string](Get-Field $msg 'host')) ([int](Get-Field $msg 'port')) }
        'jackout' { Add-Log 'PET pressed JACK OUT'; Jack-Out 'PET' }
        'alice' {
            $heard = [string](Get-Field $msg 'heard')
            $text = [string](Get-Field $msg 'text')
            $cmd = [string](Get-Field $msg 'cmd')
            if ($heard) { Add-Chat 'you' $heard }
            Add-Chat 'Alice' $text
            if (Get-Field $msg 'ok') { Speak-Alice $text }
            if ($cmd) { Add-Log "Alice suggests: $cmd   (press Enter on the PET to run it; needs Ask or Full)" }
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
            if ($line) {
                # One message the app can't handle is logged, not treated as a lost link.
                try { On-Line $line } catch { Add-Log "skipped a PET message: $($_.Exception.Message)" }
            }
        }
    } catch {
        Drop-Link 'the link broke'
    }
}

# --- buttons -----------------------------------------------------------------------

foreach ($b in $ModeButtons.Values) { $b.Add_Click({ param($sender, $e) Set-Mode ([string]$sender.Tag) }) }

$lnkAdmin.Add_LinkClicked({
    Hide-Ask
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-STA', '-WindowStyle', 'Hidden', '-File', "`"$PSCommandPath`"",
        '-Pet', $Pet, '-Port', $Port, '-TunnelPort', $TunnelPort, '-Elevated')
    $argList += @('-StartMode', (Get-Mode))
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

Add-Log "Jack In $AppVersion$(if ($Elevated) { ' (admin)' }) · $env:COMPUTERNAME · read-only until you pick Ask or Full"
Update-Ui
$timer.Start()
[void]$form.ShowDialog()
$timer.Dispose()
if ($Voice) { $Voice.Dispose() }
$form.Dispose()
