# SPDX-License-Identifier: MIT
#
# AlicePET Jack In for Windows. Runs from the PET's USB drive with the
# PowerShell 5.1 that ships with Windows 10/11; nothing is installed and nothing
# is written to the PC (the session log is only saved if you click Save Log).
#
#  - Starts READ-ONLY: a scan (CPU, RAM, disk, startup items, Defender, top RAM users).
#  - Access: READ (look only), ASK (PET commands run as you after you OK each one),
#    FULL (PET commands run as you without asking). Typing into the PET's screen
#    works in ASK and FULL only; every mode can watch it.
#  - FOLDER: open Terminal, Jellyfin, Retro or LAN Tech on the PET. Its screen is
#    mirrored here while this window is up and not minimized.
#  - Run as admin...: relaunches this app through the Windows UAC prompt that you click.
#  - NET: the PET borrows this PC's internet (outbound connections only).
#  - NAVI: chat with Alice. Type, or hold MIC (or F9) to talk; her answers can be read aloud.
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

$AppVersion = '0.4'
$Here = Split-Path -Parent $PSCommandPath

# Draws each PET frame: run-length RGB565 (count, hi, lo) to ARGB pixels.
Add-Type -TypeDefinition @'
public static class JackInFrame {
    public static void Decode(byte[] rle, int[] dst, int offset, int count) {
        int p = 0, o = offset, end = offset + count;
        while (p + 2 < rle.Length && o < end) {
            int run = rle[p];
            int v = (rle[p + 1] << 8) | rle[p + 2];
            p += 3;
            int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            int argb = unchecked((int)0xFF000000) | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
            for (int i = 0; i < run && o < end; i++) dst[o++] = argb;
        }
    }
}
'@

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
    Busy       = $false   # a scan or a confirm box is up (they pump UI events)
    Confirm    = ($StartMode -ne 'Full')  # Ask mode: OK each PET command first
    Speak      = $true    # read Alice's answers aloud
    Linked     = $false
    Chips      = @()      # chips the PET can open: {id, name, number, code, desc, art}
    ChipId     = ''       # the chip the PC last asked the PET to open
    MirrorSent = $false   # last mirror on/off sent to the PET
}
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
$S.Admin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

# --- fonts (bundled OFL fonts; Consolas if they are missing) ---------------------

$Fonts = New-Object System.Drawing.Text.PrivateFontCollection
$PixName = 'Consolas'
$VtName = 'Consolas'
$PixFam = $null
$VtFam = $null
try {
    $pixFile = Join-Path $Here 'fonts\PressStart2P-Regular.ttf'
    $vtFile = Join-Path $Here 'fonts\VT323-Regular.ttf'
    if (Test-Path $pixFile) { $Fonts.AddFontFile($pixFile) }
    if (Test-Path $vtFile) { $Fonts.AddFontFile($vtFile) }
    foreach ($f in $Fonts.Families) {
        if ($f.Name -eq 'Press Start 2P') { $PixName = $f.Name; $PixFam = $f }
        if ($f.Name -eq 'VT323') { $VtName = $f.Name; $VtFam = $f }
    }
} catch { }

function Pix([float]$size) { if ($PixFam) { return New-Object System.Drawing.Font($PixFam, $size, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Point) }; return New-Object System.Drawing.Font($PixName, $size, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Point) }
function Vt([float]$size) { if ($VtFam) { return New-Object System.Drawing.Font($VtFam, $size, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Point) }; return New-Object System.Drawing.Font($VtName, $size, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Point) }
function RGB($r, $g, $b) { return [System.Drawing.Color]::FromArgb($r, $g, $b) }

# Palette from mockups/jackin-operator.html.
$cNavy = RGB 0x0a 0x1f 0x3c
$cFrame = RGB 0x0c 0x2a 0x6a
$cPanel = RGB 0x08 0x20 0x50
$cEdge = RGB 0x00 0x10 0x30
$cYellow = RGB 0xf8 0xe8 0x58
$cInk = RGB 0x40 0x20 0x00
$cSel = RGB 0xf8 0xf0 0x70
$cSelInk = RGB 0x20 0x30 0x60
$cIdle = RGB 0x58 0x78 0xa0
$cIdleInk = RGB 0xd0 0xe0 0xf0
$cNet = RGB 0x30 0xa0 0xe8
$cOut = RGB 0xe8 0x30 0x30
$cScreen = RGB 0x00 0x08 0x14
$cGreen = RGB 0x58 0xf8 0x98
$cPaper = RGB 0xf8 0xf8 0xf8
$cHint = RGB 0x78 0x98 0xc0
$cPink = RGB 0xf8 0xa8 0xe0
$cText = RGB 0xc8 0xe8 0xff

# --- UI --------------------------------------------------------------------------

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Jack In - Alice.exe'
$form.ClientSize = New-Object System.Drawing.Size(1100, 640)
$form.FormBorderStyle = 'FixedSingle'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.BackColor = $cNavy
$form.Font = Vt 16
$form.KeyPreview = $true

# A blue frame: dark outer edge, light inner bevel (the mockup's .frame).
function Paint-Frame($panel, $e) {
    $w = $panel.ClientSize.Width
    $h = $panel.ClientSize.Height
    $pen = New-Object System.Drawing.Pen -ArgumentList $cEdge, 3
    $e.Graphics.DrawRectangle($pen, 1, 1, $w - 3, $h - 3)
    $pen.Dispose()
    $bevel = New-Object System.Drawing.Pen -ArgumentList (RGB 0x58 0xc8 0xf8), 2
    $e.Graphics.DrawRectangle($bevel, 7, 7, $w - 15, $h - 15)
    $bevel.Dispose()
}

function New-Frame($x, $y, $w, $h, $back) {
    $p = New-Object System.Windows.Forms.Panel
    $p.SetBounds($x, $y, $w, $h)
    $p.BackColor = $back
    $p.Tag = 'frame'
    $p.Add_Paint({ param($ctl, $e) Paint-Frame $ctl $e })
    $form.Controls.Add($p)
    $p.SendToBack()
    return $p
}

function New-Label($text, $x, $y, $w, $h, $font, $fore, $back) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text
    $l.SetBounds($x, $y, $w, $h)
    $l.Font = $font
    $l.ForeColor = $fore
    $l.BackColor = $back
    $form.Controls.Add($l)
    return $l
}

function New-Button($text, $x, $y, $w, $h, $back, $fore, $font) {
    $btn = New-Object System.Windows.Forms.Button
    $btn.Text = $text
    $btn.SetBounds($x, $y, $w, $h)
    $btn.FlatStyle = 'Flat'
    $btn.FlatAppearance.BorderSize = 2
    $btn.FlatAppearance.BorderColor = $cEdge
    $btn.BackColor = $back
    $btn.ForeColor = $fore
    $btn.Font = $font
    $form.Controls.Add($btn)
    return $btn
}

# Top bar: title, status, access, NET, JACK OUT.
$topFrame = New-Frame 8 8 1084 58 $cFrame
$titleLabel = New-Label 'JACK IN!' 20 22 130 30 (Pix 11) $cYellow $cFrame
$statusLabel = New-Label '' 160 20 470 34 (Vt 22) $cGreen $cFrame
$ModeButtons = [ordered]@{}
$modeX = 640
foreach ($m in @('Read', 'Ask', 'Full')) {
    $b = New-Button $m $modeX 16 80 36 $cIdle $cIdleInk (Pix 8)
    $b.Tag = $m
    $b.Add_Click({ param($sender, $e) Set-Mode ([string]$sender.Tag) })
    $ModeButtons[$m] = $b
    $modeX += 86
}
$btnNet = New-Button 'NET' 904 16 84 36 $cNet (RGB 0xff 0xff 0xff) (Pix 8)
$btnOut = New-Button 'JACK OUT' 994 16 90 36 $cOut (RGB 0xff 0xff 0xff) (Pix 7)

# Left: FOLDER with the PC-capable chips, the chip's card, admin link.
$leftFrame = New-Frame 8 76 230 556 $cFrame
$folderHdr = New-Label 'FOLDER' 18 86 110 24 (Pix 7) $cInk $cYellow
$folderHdr.TextAlign = 'MiddleCenter'
$chipList = New-Object System.Windows.Forms.Panel
$chipList.SetBounds(16, 116, 214, 236)
$chipList.BackColor = $cPanel
$chipList.Add_Paint({ param($ctl, $e) Paint-Frame $ctl $e })
$form.Controls.Add($chipList)
$script:ChipRows = @()
$infoArt = New-Object System.Windows.Forms.PictureBox
$infoArt.SetBounds(18, 364, 96, 120)
$infoArt.BackColor = $cEdge
$infoArt.SizeMode = 'CenterImage'
$form.Controls.Add($infoArt)
$infoText = New-Label 'waiting for the PET...' 122 364 110 120 (Vt 19) $cText $cFrame
$adminLink = New-Object System.Windows.Forms.LinkLabel
$adminLink.Text = 'Run as admin...'
$adminLink.LinkColor = RGB 0xff 0xd5 0x4a
$adminLink.ActiveLinkColor = RGB 0xff 0xff 0xff
$adminLink.BackColor = $cFrame
$adminLink.Font = Vt 17
$adminLink.SetBounds(18, 530, 210, 22)
$form.Controls.Add($adminLink)
$footLabel = New-Label 'Voice, Remote and Camera stay in the PET.' 18 578 210 44 (Vt 15) $cHint $cFrame

# Centre: chip tab, view tag, ask banner, PET screen mirror, live log.
$centreFrame = New-Frame 246 76 580 556 $cFrame
$tabLabel = New-Label 'LAUNCHER.CHP' 256 86 260 26 (Pix 8) $cInk $cYellow
$tabLabel.TextAlign = 'MiddleCenter'
$viewLabel = New-Label '' 520 86 298 26 (Vt 20) $cText $cFrame
$viewLabel.TextAlign = 'MiddleRight'
$askBanner = New-Label '' 256 116 560 26 (Vt 18) (RGB 0x3a 0x1f 0x00) $cSel
$askBanner.TextAlign = 'MiddleLeft'
$askBanner.Visible = $false
$mirror = New-Object System.Windows.Forms.Panel
$mirror.SetBounds(256, 146, 560, 318)
$mirror.BackColor = $cScreen
$mirror.TabStop = $true
$form.Controls.Add($mirror)
$logBox = New-Object System.Windows.Forms.RichTextBox
$logBox.ReadOnly = $true
$logBox.BorderStyle = 'None'
$logBox.ScrollBars = 'Vertical'
$logBox.SetBounds(256, 472, 560, 150)
$logBox.BackColor = RGB 0x0b 0x18 0x30
$logBox.ForeColor = RGB 0xdf 0xe8 0xff
$logBox.Font = New-Object System.Drawing.Font('Consolas', 9)
$logBox.SelectionIndent = 8
$logBox.SelectionRightIndent = 8
$form.Controls.Add($logBox)

# Right: NAVI with Alice, chat, mic, voice toggle, save log.
$naviFrame = New-Frame 834 76 258 556 $cFrame
$naviHdr = New-Label 'NAVI' 844 86 90 24 (Pix 7) $cInk $cYellow
$naviHdr.TextAlign = 'MiddleCenter'
$aliceBox = New-Object System.Windows.Forms.PictureBox
$aliceBox.SetBounds(844, 116, 76, 76)
$aliceBox.BackColor = RGB 0x20 0x30 0x60
$aliceBox.SizeMode = 'Zoom'
$alicePng = Join-Path $Here 'alice.png'
if (Test-Path $alicePng) { $aliceBox.Image = [System.Drawing.Image]::FromFile($alicePng) }
$form.Controls.Add($aliceBox)
$aliceName = New-Label 'ALICE.EXE' 930 116 160 22 (Pix 9) $cPink $cFrame
$aliceStatus = New-Label '' 930 142 160 50 (Vt 20) $cGreen $cFrame
$chatLog = New-Object System.Windows.Forms.RichTextBox
$chatLog.ReadOnly = $true
$chatLog.BorderStyle = 'None'
$chatLog.ScrollBars = 'Vertical'
$chatLog.SetBounds(844, 204, 238, 300)
$chatLog.BackColor = $cPaper
$chatLog.ForeColor = RGB 0x10 0x20 0x40
$chatLog.Font = Vt 18
$chatLog.SelectionIndent = 6
$chatLog.SelectionRightIndent = 6
$form.Controls.Add($chatLog)
$chatBox = New-Object System.Windows.Forms.TextBox
$chatBox.SetBounds(844, 512, 238, 28)
$chatBox.Font = Vt 18
$form.Controls.Add($chatBox)
$btnMic = New-Button 'MIC' 844 548 124 30 (RGB 0xc0 0x50 0xf0) (RGB 0xff 0xff 0xff) (Pix 8)
$btnVoice = New-Button 'SPK' 972 548 40 30 (RGB 0x40 0x80 0xd0) (RGB 0xff 0xff 0xff) (Pix 6)
$btnSend = New-Button 'SEND' 1016 548 66 30 $cNet (RGB 0xff 0xff 0xff) (Pix 7)
$btnLog = New-Button 'SAVE LOG' 844 586 238 30 $cIdle $cIdleInk (Pix 8)
$tips = New-Object System.Windows.Forms.ToolTip
$tips.SetToolTip($btnVoice, 'Alice reads answers aloud (click to mute)')
$tips.SetToolTip($btnMic, 'Hold to talk (or hold F9)')
$form.AcceptButton = $btnSend

# --- helpers ---------------------------------------------------------------------

function Write-Rich($box, [string]$text, $color, [bool]$bold = $false, $back = $null, [float]$size = 10) {
    $box.SelectionStart = $box.TextLength
    $box.SelectionLength = 0
    $box.SelectionColor = $color
    $box.SelectionBackColor = $(if ($back) { $back } else { $box.BackColor })
    $style = $(if ($bold) { [System.Drawing.FontStyle]::Bold } else { [System.Drawing.FontStyle]::Regular })
    $box.SelectionFont = New-Object System.Drawing.Font('Consolas', $size, $style)
    $box.AppendText($text)
}

# Everything goes to the saved log; the window shows app and PET events as grey lines.
function Add-Log([string]$line) {
    $stamp = (Get-Date).ToString('HH:mm:ss')
    $S.Log.Add("$stamp  $line")
    Write-Rich $logBox "$line`n" (RGB 0x7f 0x93 0xb5) $false $null 8.5
    $logBox.ScrollToCaret()
}

function Add-Chat([string]$who, [string]$text) {
    $stamp = (Get-Date).ToString('HH:mm:ss')
    $S.Log.Add("$stamp  ${who}: $text")
    if ($who -eq 'Alice') {
        Write-Rich $chatLog 'ALICE  ' (RGB 0xc0 0x28 0x80) $true
        Write-Rich $chatLog "$text`n" (RGB 0x10 0x20 0x40)
    } else {
        Write-Rich $chatLog "$who  " (RGB 0x20 0x50 0xc0) $true
        Write-Rich $chatLog "$text`n" (RGB 0x10 0x20 0x40)
    }
    Write-Rich $chatLog "`n" (RGB 0x10 0x20 0x40) $false $null 4
    $chatLog.ScrollToCaret()
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
    $msg = [ordered]@{ t = 'state'; full = $S.Full; admin = $S.Admin; net = $S.Net; input = [bool]$S.Full }
    if ($denied) { $msg.denied = $denied }
    Send-Pet $msg
}

function Get-Mode {
    if (-not $S.Full) { return 'Read' }
    if ($S.Confirm) { return 'Ask' }
    return 'Full'
}

# Keys and typing go to the PET only in Ask or Full, and only while linked.
function Input-Allowed {
    return [bool]($S.Linked -and $S.Full)
}

function Update-Ui {
    $mode = Get-Mode
    if ($S.Linked) {
        $statusLabel.Text = "> OPERATOR: $env:USERNAME  -  LINK: $($env:COMPUTERNAME)" + $(if ($S.Net) { '  -  NET ON' } else { '' })
        $aliceStatus.Text = "* ONLINE`nHP 1000"
        $aliceStatus.ForeColor = $cGreen
    } else {
        $statusLabel.Text = '- looking for the PET...'
        $aliceStatus.Text = "- NO LINK`n"
        $aliceStatus.ForeColor = $cHint
    }
    foreach ($m in $ModeButtons.Keys) {
        $b = $ModeButtons[$m]
        if ($m -eq $mode) {
            $b.BackColor = $cYellow
            $b.ForeColor = $cInk
        } else {
            $b.BackColor = $cIdle
            $b.ForeColor = $cIdleInk
        }
    }
    $viewLabel.Text = $(if (Input-Allowed) { 'VIEW: FULL PET' } else { 'VIEW: READ ONLY' })
    $adminLink.Visible = -not $S.Admin
    $btnNet.Text = $(if ($S.Net) { 'STOP' } else { 'NET' })
    $btnNet.BackColor = $(if ($S.Net) { $cYellow } else { $cNet })
    $btnNet.ForeColor = $(if ($S.Net) { $cInk } else { RGB 0xff 0xff 0xff })
    $chatBox.Enabled = $S.Linked
    $btnSend.Enabled = $S.Linked
    $btnMic.Enabled = $S.Linked -or $Mic.On
    $tabLabel.Text = $(if ($S.ChipId) { (($S.Chips | Where-Object { $_.id -eq $S.ChipId } | Select-Object -First 1).name).ToUpper() + '.CHP' } else { 'LAUNCHER.CHP' })
    $mirror.Invalidate()
}

function Set-Mode([string]$mode) {
    $before = Get-Mode
    if ($mode -eq $before) { return }
    $S.Full = ($mode -ne 'Read')
    $S.Confirm = ($mode -ne 'Full')
    Hide-Ask
    Add-Log $(switch ($mode) {
            'Read' { 'you set access to Read: the PET can look but not run commands or take keys' }
            'Ask' { 'you set access to Ask: the PET can run commands as you after you OK each one, and take keys' }
            default { 'you set access to Full: the PET can run commands as you without asking, and take keys' }
        })
    if ($S.Linked) { Send-State }
    Update-Ui
}

function Hide-Ask {
    $askBanner.Visible = $false
}

function Show-Ask([string]$what) {
    $names = @{ full = 'Ask or Full access'; admin = 'admin (Run as admin... in the sidebar)'; net = 'NET' }
    if (-not $names.ContainsKey($what)) { return }
    $askBanner.Text = "  The PET asks for $($names[$what]). Turn it on to allow it, or ignore this."
    $askBanner.Visible = $true
    $askBanner.BringToFront()
    $form.Activate()
}

# --- chips and mirror --------------------------------------------------------------

# Scales a small chip image by a whole number so it stays sharp (nearest neighbour).
function Scale-Art($img, [int]$boxW, [int]$boxH) {
    if ($null -eq $img) { return $null }
    $f = [Math]::Max(1, [Math]::Min([Math]::Floor($boxW / $img.Width), [Math]::Floor($boxH / $img.Height)))
    $bmp = New-Object System.Drawing.Bitmap -ArgumentList ([int]($img.Width * $f)), ([int]($img.Height * $f))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $g.DrawImage($img, 0, 0, $bmp.Width, $bmp.Height)
    $g.Dispose()
    return $bmp
}

function Decode-Art([string]$b64) {
    if (-not $b64) { return $null }
    try {
        $ms = New-Object System.IO.MemoryStream -ArgumentList (,[Convert]::FromBase64String($b64))
        $img = [System.Drawing.Image]::FromStream($ms)
        $bmp = New-Object System.Drawing.Bitmap -ArgumentList $img
        $img.Dispose()
        $ms.Dispose()
        return $bmp
    } catch {
        return $null
    }
}

function Select-ChipClick {
    param($sender, $e)
    Select-Chip ([string]$sender.Tag)
}

function Build-Chips([object[]]$list) {
    foreach ($r in $script:ChipRows) { $r.Row.Dispose() }
    $script:ChipRows = @()
    $S.Chips = @()
    $y = 6
    foreach ($c in $list) {
        $id = [string](Get-Field $c 'id')
        if (-not $id) { continue }
        $chip = [ordered]@{
            id = $id; name = [string](Get-Field $c 'name'); number = [string](Get-Field $c 'number')
            code = [string](Get-Field $c 'code'); desc = [string](Get-Field $c 'desc'); art = [string](Get-Field $c 'art')
        }
        $S.Chips += [pscustomobject]$chip
        $row = New-Object System.Windows.Forms.Panel
        $row.SetBounds(6, $y, 202, 50)
        $row.Tag = $id
        $row.Cursor = [System.Windows.Forms.Cursors]::Hand
        $row.Add_Click({ param($sender, $e) Select-ChipClick $sender $e })
        $art = New-Object System.Windows.Forms.PictureBox
        $art.SetBounds(4, 8, 34, 34)
        $art.BackColor = $cEdge
        $art.SizeMode = 'Zoom'
        $art.Tag = $id
        $art.Add_Click({ param($sender, $e) Select-ChipClick $sender $e })
        $thumb = Decode-Art $chip.art
        if ($thumb) { $art.Image = $thumb }
        $name = New-Object System.Windows.Forms.Label
        $name.Text = $chip.name
        $name.SetBounds(46, 10, 124, 30)
        $name.Font = Vt 22
        $name.ForeColor = $cPaper
        $name.BackColor = $cPanel
        $name.Tag = $id
        $name.Add_Click({ param($sender, $e) Select-ChipClick $sender $e })
        $code = New-Object System.Windows.Forms.Label
        $code.Text = $chip.code
        $code.SetBounds(172, 13, 24, 24)
        $code.Font = Pix 8
        $code.ForeColor = RGB 0xc0 0x18 0x18
        $code.BackColor = $cPaper
        $code.TextAlign = 'MiddleCenter'
        $code.Tag = $id
        $code.Add_Click({ param($sender, $e) Select-ChipClick $sender $e })
        $row.Controls.Add($art)
        $row.Controls.Add($name)
        $row.Controls.Add($code)
        $chipList.Controls.Add($row)
        $script:ChipRows += @{ Id = $id; Row = $row; Name = $name; Art = $art; Chip = $chip }
        $y += 56
    }
    Update-Folder
}

function Update-Folder {
    foreach ($r in $script:ChipRows) {
        $on = ($r.Id -eq $S.ChipId)
        $r.Row.BackColor = $(if ($on) { $cSel } else { $cPanel })
        $r.Name.BackColor = $r.Row.BackColor
        $r.Name.ForeColor = $(if ($on) { $cSelInk } else { $cPaper })
    }
    $sel = $script:ChipRows | Where-Object { $_.Id -eq $S.ChipId } | Select-Object -First 1
    if ($sel) {
        $c = $sel.Chip
        $infoArt.Image = Scale-Art (Decode-Art $c.art) 90 114
        $infoText.Text = "$($c.name.ToUpper())`nCODE $($c.code)`n$($c.desc)"
    } else {
        $infoArt.Image = $null
        $infoText.Text = $(if ($S.Linked) { 'Pick a chip to open it on the PET.' } else { 'waiting for the PET...' })
    }
    Update-Ui
}

function Select-Chip([string]$id) {
    if (-not $S.Linked -or -not $id) { return }
    $S.ChipId = $id
    Send-Pet ([ordered]@{ t = 'open'; chip = $id })
    $name = ($S.Chips | Where-Object { $_.id -eq $id } | Select-Object -First 1).name
    Add-Log "you opened $name on the PET"
    Update-Folder
}

function Apply-Frame($msg) {
    $w = [int](Get-Field $msg 'w')
    $h = [int](Get-Field $msg 'h')
    $y = [int](Get-Field $msg 'y')
    $n = [int](Get-Field $msg 'n')
    $rle = [string](Get-Field $msg 'rle')
    if ($w -le 0 -or $h -le 0 -or -not $rle) { return }
    if ($script:FrameW -ne $w -or $script:FrameH -ne $h -or $null -eq $script:Pixels) {
        $script:FrameW = $w
        $script:FrameH = $h
        $script:Pixels = New-Object 'int[]' -ArgumentList ($w * $h)
    }
    $start = [Math]::Max(0, $y) * $w
    $count = [Math]::Min($n * $w, $script:Pixels.Length - $start)
    if ($count -le 0) { return }
    [JackInFrame]::Decode([Convert]::FromBase64String($rle), $script:Pixels, $start, $count)
    $bmp = New-Object System.Drawing.Bitmap -ArgumentList $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $data = $bmp.LockBits((New-Object System.Drawing.Rectangle -ArgumentList 0, 0, $w, $h), [System.Drawing.Imaging.ImageLockMode]::WriteOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    [Runtime.InteropServices.Marshal]::Copy($script:Pixels, 0, $data.Scan0, $script:Pixels.Length)
    $bmp.UnlockBits($data)
    if ($script:FrameBmp) { $script:FrameBmp.Dispose() }
    $script:FrameBmp = $bmp
    $mirror.Invalidate()
}

function Clear-Mirror {
    if ($script:FrameBmp) { $script:FrameBmp.Dispose() }
    $script:FrameBmp = $null
    $script:Pixels = $null
    $script:FrameW = 0
    $script:FrameH = 0
    $mirror.Invalidate()
}

# The PET only sends frames while this window is up and not minimized.
function Sync-Mirror {
    $want = [bool]($S.Linked -and $form.Visible -and $form.WindowState -ne 'Minimized')
    if ($want -ne $S.MirrorSent) {
        $S.MirrorSent = $want
        Send-Pet ([ordered]@{ t = 'mirror'; on = $want })
    }
}

$script:FrameBmp = $null
$script:Pixels = $null
$script:FrameW = 0
$script:FrameH = 0

$mirror.Add_Paint({
    param($sender, $e)
    $g = $e.Graphics
    $g.Clear($cScreen)
    $cw = $sender.ClientSize.Width
    $ch = $sender.ClientSize.Height
    if ($script:FrameBmp) {
        $sc = [Math]::Max(1, [Math]::Floor([Math]::Min($cw / $script:FrameW, $ch / $script:FrameH)))
        $dw = $script:FrameW * $sc
        $dh = $script:FrameH * $sc
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
        $g.DrawImage($script:FrameBmp, [int](($cw - $dw) / 2), [int](($ch - $dh) / 2), $dw, $dh)
    } else {
        $font = Vt 26
        $msg = $(if ($S.Linked) { 'waiting for the PET screen...' } else { 'NO SIGNAL' })
        $size = $g.MeasureString($msg, $font)
        $g.DrawString($msg, $font, (New-Object System.Drawing.SolidBrush $cGreen), [float](($cw - $size.Width) / 2), [float](($ch - $size.Height) / 2))
        $font.Dispose()
    }
    $pen = New-Object System.Drawing.Pen -ArgumentList $cEdge, 2
    $g.DrawRectangle($pen, 1, 1, $cw - 3, $ch - 3)
    $pen.Dispose()
})

function Map-Key($code) {
    switch ([string]$code) {
        'Left' { return 'Left' }
        'Right' { return 'Right' }
        'Up' { return 'Up' }
        'Down' { return 'Down' }
        'Return' { return 'Enter' }
        'Escape' { return 'Escape' }
        'Back' { return 'Backspace' }
        'Tab' { return 'Tab' }
        default { return '' }
    }
}

# Arrows and Tab reach the PET too, not the focus change.
$mirror.Add_Click({ $mirror.Focus() | Out-Null })
$mirror.Add_PreviewKeyDown({ param($ctl, $e) $e.IsInputKey = $true })
$mirror.Add_KeyDown({
    param($ctl, $e)
    if (-not (Input-Allowed)) { return }
    $name = Map-Key $e.KeyCode
    if ($name) {
        Send-Pet ([ordered]@{ t = 'key'; k = $name })
        $e.Handled = $true
    }
})
$mirror.Add_KeyPress({
    param($ctl, $e)
    if (-not (Input-Allowed) -or [char]::IsControl($e.KeyChar)) { return }
    Send-Pet ([ordered]@{ t = 'char'; c = [string]$e.KeyChar })
    $e.Handled = $true
})

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

# --- NET: Share PC Internet (the PC dials both ends; nothing listens here) --------

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
    $btnMic.Text = 'REC'
    $btnMic.BackColor = RGB 0xe5 0x38 0x4f
}

function Stop-Mic([bool]$send = $true) {
    if (-not $Mic.On) { return }
    $Mic.On = $false
    $btnMic.Text = 'MIC'
    $btnMic.BackColor = RGB 0xc0 0x50 0xf0
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
    $S.MirrorSent = $false
    if ($S.Linked) {
        $S.Linked = $false
        # No PET, no access: back to read-only, sharing off.
        $S.Full = $false
        $S.Chips = @()
        $S.ChipId = ''
        Stop-Mic $false
        Stop-Jobs
        Stop-Sharing
        Clear-Mirror
        Build-Chips @()
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
        'chips' { Build-Chips @(Get-Field $msg 'list') }
        'frame' { Apply-Frame $msg }
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
                admin = $S.Admin; full = $S.Full; input = [bool]$S.Full; app = $AppVersion })
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

$adminLink.Add_LinkClicked({
    Hide-Ask
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-STA', '-WindowStyle', 'Hidden', '-File', "`"$PSCommandPath`"",
        '-Pet', $Pet, '-Port', $Port, '-TunnelPort', $TunnelPort, '-Elevated')
    $argList += @('-StartMode', (Get-Mode))
    try {
        # Windows shows the UAC prompt; you click Yes or No.
        Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -Verb RunAs | Out-Null
    } catch {
        Add-Log 'admin: UAC prompt was cancelled'
        if ($S.Linked) { Send-State 'admin' }
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
        Add-Log "you stopped sharing this PC's internet"
    } else {
        $S.Net = $true
        Add-Log "you turned on NET (the PET's connections go out through this PC)"
    }
    if ($S.Linked) { Send-State }
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
$btnVoice.Add_Click({
    $S.Speak = -not $S.Speak
    if (-not $S.Speak -and $script:Voice) { $script:Voice.SpeakAsyncCancelAll() }
    $btnVoice.Text = $(if ($S.Speak) { 'SPK' } else { 'OFF' })
    $btnVoice.BackColor = $(if ($S.Speak) { RGB 0x40 0x80 0xd0 } else { $cIdle })
    $tips.SetToolTip($btnVoice, $(if ($S.Speak) { 'Alice reads answers aloud (click to mute)' } else { 'Alice is muted (click to hear her)' }))
})

$btnOut.Add_Click({ Add-Log 'you pressed JACK OUT'; Jack-Out 'PC' })
$form.Add_FormClosing({ if (-not $S.Closing) { Jack-Out 'window closed' } })

# --- run -------------------------------------------------------------------------

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 100
$script:tick = 0
$timer.Add_Tick({
    if ($S.Busy) { return }
    try {
        # Dial the PET about every 2 s until it answers.
        $script:tick++
        if ($S.Client -or $S.Connecting -or ($script:tick % 20) -eq 1) { Step-Link }
        Step-Jobs
        Step-Tunnels
        Sync-Mirror
        if ($Mic.On -and ((Get-Date) - $Mic.Started).TotalSeconds -ge 15) { Stop-Mic }
    } catch {
        Add-Log ('error: ' + $_.Exception.Message)
    }
})

# Controls.Add puts each new control at the back, so the frames drawn first
# would cover everything added after them. Send every frame behind once.
foreach ($c in @($form.Controls)) { if ($c.Tag -eq 'frame') { $c.SendToBack() } }
# The bundled pixel fonts are private (GDI+ only): draw text with GDI+ so
# labels and buttons actually use them instead of a fallback font.
function Use-GdiPlusText($parent) {
    foreach ($c in @($parent.Controls)) {
        if ($c.PSObject.Properties['UseCompatibleTextRendering']) { $c.UseCompatibleTextRendering = $true }
        Use-GdiPlusText $c
    }
}
Use-GdiPlusText $form

Build-Chips @()
Add-Log "Jack In $AppVersion$(if ($Elevated) { ' (admin)' }) - $($env:COMPUTERNAME) - read-only until you pick Ask or Full"
Update-Ui
$timer.Start()
[void]$form.ShowDialog()
$timer.Dispose()
if ($script:Voice) { $script:Voice.Dispose() }
Clear-Mirror
$Fonts.Dispose()
$form.Dispose()
