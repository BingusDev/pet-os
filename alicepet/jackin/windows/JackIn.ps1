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
#  - FOLDER: HOME (Alice, chat and PC status) or one chip at a time: Terminal,
#    Jellyfin, Retro or LAN Tech. The PET closes the open chip (and its player or
#    game) before it opens the next. The chip's screen is mirrored here while it
#    is open and this window is up and not minimized. < HOME or Esc comes back.
#    (Esc goes to the chip while you type into its screen; use Shift+Esc there.)
#  - Run as admin...: relaunches this app through the Windows UAC prompt that you click.
#  - NET: the PET borrows this PC's internet (outbound connections only).
#  - Chat with Alice: type (Enter sends), or hold MIC (or F9) to talk; her answers can be read aloud.
#    Alice can run PowerShell here to answer you: each command she sends goes through the access
#    mode (READ: read-only commands only; ASK: Yes/No for each; FULL: Yes/No only for destructive
#    ones) and its output goes back to her automatically; after 25 commands (8 in Read/Ask)
#    for one message you are asked whether she keeps going.
#  - Jellyfin video and Retro games picked on the PET's chip play HERE: video in a
#    NOW PLAYING player in the chip area (Windows Media Player; Esc or STOP
#    comes back to the chip and tells Jellyfin where you stopped), games in your
#    browser (RomM's web player, on http://127.0.0.1 so the browser treats it as secure). Both come
#    through the PET's relay on 10.55.0.1, so no server passwords reach this PC.
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

# A failure at startup must never just vanish (JackIn.cmd starts this hidden):
# show what broke, how to run it by hand, and keep it in %TEMP%\JackIn-error.txt.
function Show-StartupError($err) {
    $text = "Jack In could not start:`r`n`r`n$err"
    try { $text += "`r`n`r`n" + $err.ScriptStackTrace } catch { }
    $hint = "To see more, run this in a PowerShell window:`r`n  powershell -NoProfile -ExecutionPolicy Bypass -STA -File `"$PSCommandPath`""
    try { [System.IO.File]::WriteAllText((Join-Path ([IO.Path]::GetTempPath()) 'JackIn-error.txt'), "$text`r`n`r`n$hint") } catch { }
    try {
        Add-Type -AssemblyName System.Windows.Forms
        [void][System.Windows.Forms.MessageBox]::Show("$text`r`n`r`n$hint`r`n`r`n(Saved to %TEMP%\JackIn-error.txt)", 'Jack In', 'OK', 'Error')
    } catch { }
}
trap { Show-StartupError $_; exit 1 }

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

# Layout is written for 96 DPI; [JackInUi]::B scales every position and size by K
# (the screen's DPI / 96, cut down if the window would not fit), and fonts are
# made in pixels times K, so 100/125/150% all look the same, only bigger.
Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition @'
public static class JackInUi {
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    static extern bool SetProcessDPIAware();
    public static double K = 1.0;
    public static void Aware() { try { SetProcessDPIAware(); } catch { } }
    public static int S(double v) { return (int)System.Math.Round(v * K); }
    public static void B(System.Windows.Forms.Control c, double x, double y, double w, double h) {
        c.SetBounds(S(x), S(y), S(w), S(h));
    }
}
'@
[JackInUi]::Aware()
[System.Windows.Forms.Application]::EnableVisualStyles()
$dpiG = [System.Drawing.Graphics]::FromHwnd([IntPtr]::Zero)
$Dpi = [double]$dpiG.DpiX
$dpiG.Dispose()
$work = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$K = [Math]::Min($Dpi / 96.0, [Math]::Min(($work.Height - 40) / 640.0, ($work.Width - 16) / 1100.0))
if ($env:JACKIN_SCALE) { $K = [double]$env:JACKIN_SCALE }
$K = [Math]::Max(0.7, $K)
[JackInUi]::K = $K

$AppVersion = '0.9.3'
$Here = Split-Path -Parent $PSCommandPath

# Draws each PET frame: run-length RGB565 (count, hi, lo) to ARGB pixels.
Add-Type -TypeDefinition @'
public static class JackInFrame {
    [System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    static extern System.IntPtr SendMessage(System.IntPtr hwnd, int msg, System.IntPtr wp, string lp);
    // Grey placeholder text in an empty TextBox (EM_SETCUEBANNER).
    public static void Cue(System.IntPtr hwnd, string text) { SendMessage(hwnd, 0x1501, System.IntPtr.Zero, text); }
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

# Windows Media Player as a WinForms control (no interop DLLs needed).
Add-Type -ReferencedAssemblies System.Windows.Forms -TypeDefinition @'
public class JackInWmp : System.Windows.Forms.AxHost {
    public JackInWmp() : base("6bf52a52-394a-11d3-b153-00c04f79faa6") { }
    public object Player { get { return this.GetOcx(); } }
}
'@

# Esc while a video plays: the player control eats keys before the form sees
# them, so a message filter catches Esc first (the UI timer acts on Pressed).
Add-Type -ReferencedAssemblies System.Windows.Forms -TypeDefinition @'
public class JackInEsc : System.Windows.Forms.IMessageFilter {
    public bool Armed;
    public bool Pressed;
    public bool PreFilterMessage(ref System.Windows.Forms.Message m) {
        if (Armed && m.Msg == 0x100 && m.WParam.ToInt64() == 0x1B) { Pressed = true; return true; }
        return false;
    }
}
'@

# RomM's web player in the browser: a page from http://10.55.0.1 is not a
# "secure context", so browsers give it no SharedArrayBuffer and RomM refuses
# the threaded cores (DOS, PSP, 3DS) with "HTTPS required". http://127.0.0.1
# counts as secure, so the app forwards a loopback port to the PET's web relay
# and opens the game there. Plain bytes both ways; RomM's own COOP/COEP headers
# come through the relay and make the page cross-origin isolated.
Add-Type -TypeDefinition @'
using System.Net;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
public class JackInLoopback {
    TcpListener listener;
    volatile string host = "";
    volatile int port;
    public int Port { get { return listener == null ? 0 : ((IPEndPoint)listener.LocalEndpoint).Port; } }
    // Listens on 127.0.0.1 (the preferred port, else any free one); returns the port or 0.
    public int Start(int preferred, string targetHost, int targetPort) {
        host = targetHost;
        port = targetPort;
        if (listener != null) return Port;
        foreach (int p in new int[] { preferred, 0 }) {
            try {
                TcpListener l = new TcpListener(IPAddress.Loopback, p);
                l.Start();
                listener = l;
                break;
            } catch { }
        }
        if (listener == null) return 0;
        Thread t = new Thread(Accept);
        t.IsBackground = true;
        t.Start(listener);
        return Port;
    }
    void Accept(object o) {
        TcpListener l = (TcpListener)o;
        while (true) {
            TcpClient c;
            try { c = l.AcceptTcpClient(); } catch { return; }
            Thread t = new Thread(Pipe);
            t.IsBackground = true;
            t.Start(c);
        }
    }
    void Pipe(object o) {
        TcpClient c = (TcpClient)o;
        TcpClient up = new TcpClient();
        try {
            up.Connect(host, port);
            c.NoDelay = true;
            up.NoDelay = true;
            NetworkStream a = c.GetStream();
            NetworkStream b = up.GetStream();
            Task.WaitAny(a.CopyToAsync(b), b.CopyToAsync(a));
        } catch { } finally {
            try { c.Close(); } catch { }
            try { up.Close(); } catch { }
        }
    }
    public void Stop() {
        if (listener == null) return;
        try { listener.Stop(); } catch { }
        listener = null;
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
    Speak      = $false   # read Alice's answers aloud (muted until the speaker button)
    Linked     = $false
    Chips      = @()      # chips the PET can open: {id, name, number, code, desc, art}
    ChipId     = ''       # the chip the PC last asked the PET to open
    MirrorSent = $false   # last mirror on/off sent to the PET
    View       = 'home'   # centre shows 'home' or 'chip'
    Operator   = 'Holden' # who runs the PET (the PET's [jackin] operator=), never the Windows user
    ShowLog    = $false   # HOME shows the live log instead of the chat
    Agent      = $false   # Alice's command loop is running for the last chat message
}
# Video from the PET (Jellyfin chip) playing in this window.
$Play = [ordered]@{
    On       = $false
    Item     = ''
    Title    = ''
    Base     = 0.0     # show time of the stream's 0 (a transcode started mid-show)
    StartAt  = 0.0     # where to seek once it opens (a file stream)
    Seekable = $true
    Started  = $false
    Opened   = [datetime]::MinValue
    Reported = [datetime]::MinValue
    Ax       = $null
}
$EscFilter = New-Object JackInEsc
[System.Windows.Forms.Application]::AddMessageFilter($EscFilter)
$Loop = New-Object JackInLoopback
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

# Sizes are points at 96 DPI; made in pixels so they scale with the layout (K), not twice.
function Px([float]$size) { return [float]($size * 96.0 / 72.0 * $K) }
function F([string]$name, [float]$size, $style = [System.Drawing.FontStyle]::Regular) { return New-Object System.Drawing.Font($name, (Px $size), $style, [System.Drawing.GraphicsUnit]::Pixel) }
function Pix([float]$size) { if ($PixFam) { return New-Object System.Drawing.Font($PixFam, (Px $size), [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel) }; return F $PixName $size }
function Vt([float]$size) { if ($VtFam) { return New-Object System.Drawing.Font($VtFam, (Px $size), [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel) }; return F $VtName $size }
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
$form.AutoScaleMode = 'None'  # scaled by hand (K), see JackInUi
$form.ClientSize = New-Object System.Drawing.Size([JackInUi]::S(1100), [JackInUi]::S(640))
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
    $pen = New-Object System.Drawing.Pen -ArgumentList $cEdge, ([float](3 * $K))
    $e.Graphics.DrawRectangle($pen, 1, 1, $w - 3, $h - 3)
    $pen.Dispose()
    $in = [JackInUi]::S(7)
    $bevel = New-Object System.Drawing.Pen -ArgumentList (RGB 0x58 0xc8 0xf8), ([float](2 * $K))
    $e.Graphics.DrawRectangle($bevel, $in, $in, $w - 2 * $in - 1, $h - 2 * $in - 1)
    $bevel.Dispose()
}

function New-Frame($x, $y, $w, $h, $back) {
    $p = New-Object System.Windows.Forms.Panel
    [JackInUi]::B($p, $x, $y, $w, $h)
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
    [JackInUi]::B($l, $x, $y, $w, $h)
    $l.Font = $font
    $l.ForeColor = $fore
    $l.BackColor = $back
    $form.Controls.Add($l)
    return $l
}

function New-Button($text, $x, $y, $w, $h, $back, $fore, $font) {
    $btn = New-Object System.Windows.Forms.Button
    $btn.Text = $text
    [JackInUi]::B($btn, $x, $y, $w, $h)
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
# Everything in the bar sits on its centre line (y 37); the status text shrinks to fit.
$titleLabel = New-Label 'JACK IN!' 20 21 132 32 (Pix 11) $cYellow $cFrame
$titleLabel.TextAlign = 'MiddleLeft'
$statusLabel = New-Label '' 158 19 432 36 (Vt 22) $cGreen $cFrame
$statusLabel.TextAlign = 'MiddleLeft'
$statusLabel.AutoEllipsis = $true
$ModeButtons = [ordered]@{}
$modeX = 600
foreach ($m in @('Read', 'Ask', 'Full')) {
    $b = New-Button $m.ToUpper() $modeX 19 80 36 $cIdle $cIdleInk (Pix 8)
    $b.Tag = $m
    $b.Add_Click({ param($sender, $e) Set-Mode ([string]$sender.Tag) })
    $ModeButtons[$m] = $b
    $modeX += 86
}
$btnNet = New-Button 'NET' 858 19 84 36 $cNet (RGB 0xff 0xff 0xff) (Pix 8)
$btnOut = New-Button 'JACK OUT' 948 19 132 36 $cOut (RGB 0xff 0xff 0xff) (Pix 8)
foreach ($b in @($btnNet, $btnOut) + @($ModeButtons.Values)) { $b.TextAlign = 'MiddleCenter'; $b.Padding = New-Object System.Windows.Forms.Padding(0) }

# Left: FOLDER with HOME and the PC-capable chips, the chip's card, admin link.
$leftFrame = New-Frame 8 76 230 556 $cFrame
$folderHdr = New-Label 'FOLDER' 18 86 110 24 (Pix 7) $cInk $cYellow
$folderHdr.TextAlign = 'MiddleCenter'
$chipList = New-Object System.Windows.Forms.Panel
[JackInUi]::B($chipList, 16, 116, 214, 268)
$chipList.BackColor = $cPanel
$chipList.Add_Paint({ param($ctl, $e) Paint-Frame $ctl $e })
$form.Controls.Add($chipList)
$script:ChipRows = @()
$infoArt = New-Object System.Windows.Forms.PictureBox
[JackInUi]::B($infoArt, 18, 394, 96, 120)
$infoArt.BackColor = $cEdge
$infoArt.SizeMode = 'CenterImage'
$form.Controls.Add($infoArt)
$infoText = New-Label 'waiting for the PET...' 122 394 110 126 (Vt 19) $cText $cFrame
$adminLink = New-Object System.Windows.Forms.LinkLabel
$adminLink.Text = 'Run as admin...'
$adminLink.LinkColor = RGB 0xff 0xd5 0x4a
$adminLink.ActiveLinkColor = RGB 0xff 0xff 0xff
$adminLink.BackColor = $cFrame
$adminLink.Font = Vt 17
[JackInUi]::B($adminLink, 18, 534, 210, 22)
$form.Controls.Add($adminLink)
$footLabel = New-Label 'Voice, Remote and Camera stay in the PET.' 18 578 210 44 (Vt 15) $cHint $cFrame

# Alice's art (alice.png on the drive): cropped to fill a box like CSS object-fit: cover.
$AliceImg = $null
$alicePng = Join-Path $Here 'alice.png'
if (Test-Path $alicePng) { $AliceImg = [System.Drawing.Image]::FromFile($alicePng) }
function Crop-Cover($img, [int]$w, [int]$h, [float]$fx, [float]$fy) {
    if ($null -eq $img) { return $null }
    $w = [JackInUi]::S($w)
    $h = [JackInUi]::S($h)
    $scale = [Math]::Max($w / $img.Width, $h / $img.Height)
    $sw = $w / $scale
    $sh = $h / $scale
    $sx = ($img.Width - $sw) * $fx
    $sy = ($img.Height - $sh) * $fy
    $bmp = New-Object System.Drawing.Bitmap -ArgumentList $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $g.DrawImage($img, (New-Object System.Drawing.RectangleF -ArgumentList 0, 0, $w, $h), (New-Object System.Drawing.RectangleF -ArgumentList $sx, $sy, $sw, $sh), [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose()
    return $bmp
}
function New-Picture($x, $y, $w, $h, $img) {
    $p = New-Object System.Windows.Forms.PictureBox
    [JackInUi]::B($p, $x, $y, $w, $h)
    $p.BackColor = RGB 0x20 0x30 0x60
    $p.SizeMode = 'Normal'
    $p.Image = $img
    $form.Controls.Add($p)
    return $p
}

# Centre: one area with two states (mockups/jackin-operator-home.html and -chip.html).
#  HOME: Alice big, the chat box, PC STATUS from the read-only scan, the live log.
#  CHIP: the PET screen mirror across the whole centre and a one-line chat bar.
$centreFrame = New-Frame 246 76 846 556 $cFrame
$tabLabel = New-Label 'HOME' 256 86 90 24 (Pix 8) $cInk $cYellow
$tabLabel.TextAlign = 'MiddleCenter'
$askBanner = New-Label '' 256 116 826 26 (Vt 18) (RGB 0x3a 0x1f 0x00) $cSel
$askBanner.TextAlign = 'MiddleLeft'
$askBanner.Visible = $false

# HOME state
$hintLabel = New-Label 'pick a chip, or talk to me' 640 84 442 26 (Vt 20) $cHint $cFrame
$hintLabel.TextAlign = 'MiddleRight'
$portrait = New-Picture 256 116 230 342 (Crop-Cover $AliceImg 230 342 0.5 0.2)
$aliceName = New-Label 'ALICE.EXE' 256 458 124 30 (Pix 8) $cPink $cEdge
$aliceName.TextAlign = 'MiddleRight'
$aliceStatus = New-Label '' 380 458 106 30 (F 'Segoe UI' 10 ([System.Drawing.FontStyle]::Bold)) $cGreen $cEdge
$aliceStatus.TextAlign = 'MiddleLeft'
$chatLog = New-Object System.Windows.Forms.RichTextBox
$chatLog.ReadOnly = $true
$chatLog.BorderStyle = 'None'
$chatLog.ScrollBars = 'Vertical'
[JackInUi]::B($chatLog, 496, 116, 586, 334)
$chatLog.BackColor = $cPaper
$chatLog.ForeColor = RGB 0x10 0x20 0x40
$chatLog.Font = Vt 18
$chatLog.SelectionIndent = 8
$chatLog.SelectionRightIndent = 8
$form.Controls.Add($chatLog)
$statusHdr = New-Label 'PC STATUS' 256 500 140 26 (Pix 8) $cInk $cYellow
$statusHdr.TextAlign = 'MiddleCenter'
$cPlate = RGB 0x04 0x12 0x2c
$Stats = [ordered]@{}
$statX = 256
foreach ($statName in @('CPU', 'RAM', 'C:', 'DEFENDER')) {
    $box = New-Object System.Windows.Forms.Panel
    [JackInUi]::B($box, $statX, 532, 200, 84)
    $box.BackColor = $cPlate
    $box.Add_Paint({
        param($ctl, $e)
        $pen = New-Object System.Drawing.Pen -ArgumentList (RGB 0x58 0xc8 0xf8), ([float](2 * $K))
        $e.Graphics.DrawRectangle($pen, 1, 1, $ctl.Width - 3, $ctl.Height - 3)
        $pen.Dispose()
    })
    $key = New-Object System.Windows.Forms.Label
    $key.Text = $statName
    # Tall enough for the pixel font's ascent, clear of the 2 px border.
    [JackInUi]::B($key, 12, 6, 180, 26)
    $key.Font = Pix 8
    $key.TextAlign = 'MiddleLeft'
    $key.ForeColor = RGB 0xc8 0xe0 0xff
    $key.BackColor = $cPlate
    $val = New-Object System.Windows.Forms.Label
    $val.Text = '--'
    [JackInUi]::B($val, 10, 32, 184, 46)
    $val.Font = Vt 34
    $val.ForeColor = $cIdleInk
    $val.BackColor = $cPlate
    $val.TextAlign = 'MiddleLeft'
    $val.AutoEllipsis = $true
    $box.Controls.Add($key)
    $box.Controls.Add($val)
    $form.Controls.Add($box)
    $Stats[$statName] = @{ Box = $box; Value = $val }
    $statX += 209
}
$logBox = New-Object System.Windows.Forms.RichTextBox
$logBox.ReadOnly = $true
$logBox.BorderStyle = 'None'
$logBox.ScrollBars = 'Vertical'
[JackInUi]::B($logBox, 496, 116, 586, 334)
$logBox.BackColor = RGB 0x0b 0x18 0x30
$logBox.ForeColor = RGB 0xdf 0xe8 0xff
$logBox.Font = F 'Consolas' 9
$logBox.Visible = $false
$logBox.SelectionIndent = 8
$logBox.SelectionRightIndent = 8
$form.Controls.Add($logBox)
$btnShowLog = New-Button 'LOG' 992 500 90 26 $cIdle $cIdleInk (Pix 7)
$btnLog = New-Button 'SAVE LOG' 860 500 126 26 $cIdle $cIdleInk (Pix 7)

# CHIP state
$btnHome = New-Button '< HOME' 444 84 96 28 (RGB 0x58 0x78 0xa0) (RGB 0xff 0xff 0xff) (Pix 7)
$viewKey = New-Label 'VIEW:' 900 86 70 24 (Pix 7) $cText $cFrame
$viewKey.TextAlign = 'MiddleRight'
$viewMode = New-Label 'FULL' 972 86 54 24 (Pix 7) $cInk $cYellow
$viewMode.TextAlign = 'MiddleCenter'
$viewPet = New-Label 'PET' 1028 86 54 24 (Pix 7) $cText $cFrame
$viewPet.TextAlign = 'MiddleCenter'
$readHint = New-Label '' 548 86 348 24 (Vt 16) $cInk $cYellow
$readHint.TextAlign = 'MiddleCenter'
$readHint.AutoEllipsis = $true
$readHint.Visible = $false
$script:ReadHintUntil = [datetime]::MinValue
$mirror = New-Object System.Windows.Forms.Panel
[JackInUi]::B($mirror, 256, 116, 826, 466)
$mirror.BackColor = $cScreen
$mirror.TabStop = $true
$form.Controls.Add($mirror)
# NOW PLAYING: the PET's video, in the chip area instead of the mirror.
$playPanel = New-Object System.Windows.Forms.Panel
[JackInUi]::B($playPanel, 256, 116, 826, 466)
$playPanel.BackColor = [System.Drawing.Color]::Black
$playPanel.Visible = $false
$form.Controls.Add($playPanel)
$nowLabel = New-Label 'NOW PLAYING' 0 0 716 28 (Vt 20) $cText $cEdge
$nowLabel.TextAlign = 'MiddleLeft'
$nowLabel.AutoEllipsis = $true
$playPanel.Controls.Add($nowLabel)
# The big one: always in the player's own bar, right of the title.
$btnStopVideo = New-Button ([string][char]0x25A0 + ' STOP') 716 0 110 28 (RGB 0xd0 0x40 0x40) (RGB 0xff 0xff 0xff) (Pix 7)
$form.Controls.Remove($btnStopVideo)
$playPanel.Controls.Add($btnStopVideo)
$btnBack10 = New-Button '-10s' 560 84 70 28 $cIdle (RGB 0xff 0xff 0xff) (Pix 7)
$btnFwd10 = New-Button '+10s' 634 84 70 28 $cIdle (RGB 0xff 0xff 0xff) (Pix 7)
$btnStop = New-Button 'STOP' 708 84 90 28 (RGB 0xd0 0x40 0x40) (RGB 0xff 0xff 0xff) (Pix 7)
foreach ($b in @($btnBack10, $btnFwd10, $btnStop)) { $b.Visible = $false }
$miniFace = New-Picture 256 590 32 32 (Crop-Cover $AliceImg 32 32 0.5 0.12)
$aliceWho = New-Label 'ALICE:' 292 590 80 32 (Vt 20) (RGB 0xc0 0x28 0x80) $cPaper
$aliceWho.TextAlign = 'MiddleLeft'
$aliceWho.Padding = New-Object System.Windows.Forms.Padding(6, 0, 0, 0)
$aliceLine = New-Label '' 372 590 358 32 (Vt 20) (RGB 0x10 0x20 0x40) $cPaper
$aliceLine.TextAlign = 'MiddleLeft'
$aliceLine.AutoEllipsis = $true

# Shared by both states (moved by Set-View): Send mail, MIC, SPK.
$chatBox = New-Object System.Windows.Forms.TextBox
$chatBox.Font = Vt 20
$chatBox.BorderStyle = 'FixedSingle'
$form.Controls.Add($chatBox)
$btnMic = New-Button 'MIC' 0 0 68 30 (RGB 0xc0 0x50 0xf0) (RGB 0xff 0xff 0xff) (Pix 8)
$btnVoice = New-Button '' 0 0 68 30 $cIdle (RGB 0xff 0xff 0xff) (Pix 8)
$btnVoice.Add_Paint({
    param($ctl, $e)
    $g = $e.Graphics
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $h = [float]$ctl.ClientSize.Height
    $u = $h / 10.0
    $cx = [float]($ctl.ClientSize.Width / 2.0 - 2.5 * $u)
    $cy = $h / 2.0
    $pts = [System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF -ArgumentList $cx, ($cy - 1.4 * $u)),
        (New-Object System.Drawing.PointF -ArgumentList ($cx + 1.6 * $u), ($cy - 1.4 * $u)),
        (New-Object System.Drawing.PointF -ArgumentList ($cx + 3.6 * $u), ($cy - 3.4 * $u)),
        (New-Object System.Drawing.PointF -ArgumentList ($cx + 3.6 * $u), ($cy + 3.4 * $u)),
        (New-Object System.Drawing.PointF -ArgumentList ($cx + 1.6 * $u), ($cy + 1.4 * $u)),
        (New-Object System.Drawing.PointF -ArgumentList $cx, ($cy + 1.4 * $u)))
    $g.FillPolygon([System.Drawing.Brushes]::White, $pts)
    $pen = New-Object System.Drawing.Pen -ArgumentList ([System.Drawing.Color]::White), ([float]([Math]::Max(1.5, 0.7 * $u)))
    if ($S.Speak) {
        $g.DrawArc($pen, [float]($cx + 2.4 * $u), [float]($cy - 2.2 * $u), [float](3.0 * $u), [float](4.4 * $u), [float]-50, [float]100)
        $g.DrawArc($pen, [float]($cx + 2.6 * $u), [float]($cy - 3.6 * $u), [float](4.6 * $u), [float](7.2 * $u), [float]-50, [float]100)
    } else {
        $g.DrawLine($pen, [float]($cx + 5.0 * $u), [float]($cy - 1.8 * $u), [float]($cx + 8.2 * $u), [float]($cy + 1.8 * $u))
        $g.DrawLine($pen, [float]($cx + 5.0 * $u), [float]($cy + 1.8 * $u), [float]($cx + 8.2 * $u), [float]($cy - 1.8 * $u))
    }
    $pen.Dispose()
})
$tips = New-Object System.Windows.Forms.ToolTip
$tips.SetToolTip($btnStopVideo, 'Stop the video (Esc)')
$tips.SetToolTip($btnVoice, 'Alice is muted (click to hear her)')
$tips.SetToolTip($btnMic, 'Hold to talk (or hold F9)')
$tips.SetToolTip($btnHome, 'Back to HOME (Esc; Shift+Esc while typing into the PET screen)')
$tips.SetToolTip($chatBox, 'Send mail to Alice (Enter)')
[JackInFrame]::Cue($chatBox.Handle, 'Send mail...')

$HomeCtl = @($hintLabel, $portrait, $aliceName, $aliceStatus, $chatLog, $statusHdr, $logBox, $btnLog, $btnShowLog, $btnVoice) + @($Stats.Values | ForEach-Object { $_.Box })
$ChipCtl = @($btnHome, $viewKey, $viewMode, $viewPet, $mirror, $miniFace, $aliceWho, $aliceLine)

# Input in the mirror without access: say so instead of ignoring it.
function Show-ReadHint {
    $why = $(if (-not $S.Linked) { 'not linked to the PET yet' } else { 'READ mode: pick ASK or FULL to control chips' })
    $readHint.Text = $why
    $tips.SetToolTip($readHint, $why)
    $readHint.Visible = ($S.View -eq 'chip') -and -not $Play.On
    $readHint.BringToFront()
    $script:ReadHintUntil = (Get-Date).AddSeconds(4)
    if ($script:ReadHintLogged -ne $why) { Add-Log "ignored input on the PET screen: $why"; $script:ReadHintLogged = $why }
}
$script:ReadHintLogged = ''

# Shows HOME or the open chip in the centre.
function Set-View([string]$view) {
    $S.View = $view
    $atHome = ($view -eq 'home')
    $form.SuspendLayout()
    foreach ($c in $HomeCtl) { $c.Visible = $atHome }
    foreach ($c in $ChipCtl) { $c.Visible = -not $atHome }
    # HOME shows the chat or, after LOG, the live log in the same place.
    $chatLog.Visible = $atHome -and -not $S.ShowLog
    $logBox.Visible = $atHome -and $S.ShowLog
    $btnLog.Visible = $atHome -and $S.ShowLog
    $btnShowLog.Text = $(if ($S.ShowLog) { 'CHAT' } else { 'LOG' })
    if ($atHome) {
        [JackInUi]::B($tabLabel, 256, 86, 90, 24)
        [JackInUi]::B($chatBox, 496, 458, 440, 30)
        [JackInUi]::B($btnMic, 942, 458, 68, 30)
        [JackInUi]::B($btnVoice, 1014, 458, 68, 30)
    } else {
        [JackInUi]::B($tabLabel, 256, 86, 180, 24)
        [JackInUi]::B($chatBox, 736, 590, 270, 32)
        [JackInUi]::B($btnMic, 1012, 590, 70, 32)
    }
    $form.ResumeLayout()
    Update-Ui
}

# --- helpers ---------------------------------------------------------------------

function Write-Rich($box, [string]$text, $color, [bool]$bold = $false, $back = $null, [float]$size = 10) {
    $box.SelectionStart = $box.TextLength
    $box.SelectionLength = 0
    $box.SelectionColor = $color
    $box.SelectionBackColor = $(if ($back) { $back } else { $box.BackColor })
    $style = $(if ($bold) { [System.Drawing.FontStyle]::Bold } else { [System.Drawing.FontStyle]::Regular })
    $box.SelectionFont = F 'Consolas' $size $style
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
        Write-Rich $chatLog 'ALICE: ' (RGB 0xc0 0x28 0x80) $true $null 11
        Write-Rich $chatLog "$text`n" (RGB 0x10 0x20 0x40) $false $null 11
        $aliceLine.Text = ($text -replace '\s+', ' ')
        $tips.SetToolTip($aliceLine, $text)
    } else {
        # The operator is whoever runs the PET (Holden), never the Windows account.
        $name = $(if ($who -eq 'you') { $S.Operator } else { $who })
        Write-Rich $chatLog "${name}: " (RGB 0x20 0x50 0xc0) $true $null 11
        Write-Rich $chatLog "$text`n" (RGB 0x10 0x20 0x40) $false $null 11
    }
    Write-Rich $chatLog "`n" (RGB 0x10 0x20 0x40) $false $null 9
    $chatLog.ScrollToCaret()
}

# A command Alice ran (or asked to run) and a short look at what came back.
function Add-ChatCmd([string]$cmd, [string]$note = '') {
    $S.Log.Add((Get-Date).ToString('HH:mm:ss') + "  RUN: $cmd")
    Write-Rich $chatLog "  > $cmd`n" (RGB 0x40 0x50 0x70) $true (RGB 0xe4 0xea 0xf2) 9.5
    if ($note) { Write-Rich $chatLog "    $note`n" (RGB 0x80 0x60 0x10) $false $null 9.5 }
    $chatLog.ScrollToCaret()
}
function Add-ChatResult([bool]$ok, [string]$out) {
    $lines = @(($out -split "`r?`n") | Where-Object { $_.Trim() } | Select-Object -First 3)
    $short = (($lines | ForEach-Object { $_.TrimEnd() }) -join "`n    ")
    if ($short.Length -gt 300) { $short = $short.Substring(0, 300) + '...' }
    if (-not $short) { $short = '(no output)' }
    Write-Rich $chatLog ("    " + $(if ($ok) { 'ok: ' } else { 'failed: ' })) $(if ($ok) { RGB 0x10 0x80 0x40 } else { RGB 0xc0 0x20 0x20 }) $true $null 9.5
    Write-Rich $chatLog "$short`n" (RGB 0x30 0x40 0x60) $false $null 9.5
    Write-Rich $chatLog "`n" (RGB 0x10 0x20 0x40) $false $null 6
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
    $msg = [ordered]@{ t = 'state'; full = $S.Full; admin = $S.Admin; net = $S.Net; input = [bool]$S.Full; mode = (Get-Mode).ToLower() }
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
        Fit-Label $statusLabel ("OPERATOR: $($S.Operator)  |  LINK: $($env:COMPUTERNAME)" + $(if ($S.Net) { '  |  NET ON' } else { '' })) 22 14
        $aliceStatus.Text = [string][char]0x25CF + ' ONLINE'
        $aliceStatus.ForeColor = $cGreen
    } else {
        Fit-Label $statusLabel 'looking for the PET...' 22 14
        $aliceStatus.Text = [string][char]0x25CB + ' NO LINK'
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
    $viewMode.Text = $(if (Input-Allowed) { 'FULL' } else { 'READ' })
    $viewMode.BackColor = $(if (Input-Allowed) { $cYellow } else { $cIdle })
    $viewMode.ForeColor = $(if (Input-Allowed) { $cInk } else { $cIdleInk })
    $tips.SetToolTip($viewMode, $(if (Input-Allowed) { 'Click the PET screen, then type: keys go to the PET' } else { 'Read: you can watch the PET screen. Pick Ask or Full to type into it' }))
    $adminLink.Visible = -not $S.Admin
    $btnNet.Text = $(if ($S.Net) { 'STOP' } else { 'NET' })
    $btnNet.BackColor = $(if ($S.Net) { $cYellow } else { $cNet })
    $btnNet.ForeColor = $(if ($S.Net) { $cInk } else { RGB 0xff 0xff 0xff })
    $chatBox.Enabled = $S.Linked
    $btnMic.Enabled = $S.Linked -or $Mic.On
    $tabLabel.Text = $(if ($S.View -eq 'chip' -and $S.ChipId) { (Chip-Name $S.ChipId).ToUpper() + '.CHP' } else { 'HOME' })
    $playing = [bool]($Play.On -and $S.View -eq 'chip')
    $playPanel.Visible = $playing
    $mirror.Visible = ($S.View -eq 'chip') -and -not $playing
    foreach ($b in @($btnBack10, $btnFwd10, $btnStop)) { $b.Visible = $playing }
    if ($playing) { $playPanel.BringToFront(); foreach ($b in @($btnBack10, $btnFwd10, $btnStop)) { $b.BringToFront() } }
    if ($playing -or $S.View -ne 'chip' -or (Input-Allowed)) { $readHint.Visible = $false }
    $EscFilter.Armed = [bool]$Play.On
    $mirror.Invalidate()
}

# Shows `text` in the label at the biggest VT323 size (max..min) that fits on one
# line; past the minimum it ends in "..." and the tooltip has all of it.
function Fit-Label($label, [string]$text, [float]$max, [float]$min) {
    if ($label.Text -eq $text -and $label.Tag -eq 'fit') { return }
    $label.Tag = 'fit'
    $label.Text = $text
    $tips.SetToolTip($label, $text)
    $g = $label.CreateGraphics()
    try {
        $room = $label.ClientSize.Width - [JackInUi]::S(4)
        for ($sz = $max; $sz -ge $min; $sz -= 1) {
            $f = Vt $sz
            if ($sz -le $min -or $g.MeasureString($text, $f).Width -le $room) { $label.Font = $f; break }
            $f.Dispose()
        }
    } finally { $g.Dispose() }
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
    # Read stops Alice's command loop (the PET stops it when it sees mode read).
    if ($mode -eq 'Read') { $S.Agent = $false }
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
    $f = [Math]::Max(1, [Math]::Min([Math]::Floor([JackInUi]::S($boxW) / $img.Width), [Math]::Floor([JackInUi]::S($boxH) / $img.Height)))
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

function Chip-Name([string]$id) {
    $c = $S.Chips | Where-Object { $_.id -eq $id } | Select-Object -First 1
    if ($c) { return [string]$c.name }
    return $id
}

# HOME is always the first row (Alice's face, code H); the PET's chips follow.
function Build-Chips([object[]]$list) {
    foreach ($r in $script:ChipRows) { $r.Row.Dispose() }
    $script:ChipRows = @()
    $S.Chips = @()
    $y = 6
    $rows = @([pscustomobject]@{ id = 'home'; name = 'HOME'; code = 'H'; home = $true })
    foreach ($c in $list) {
        $id = [string](Get-Field $c 'id')
        if (-not $id -or $id -eq 'home') { continue }
        $rows += [pscustomobject]@{ id = $id; name = [string](Get-Field $c 'name'); code = [string](Get-Field $c 'code'); home = $false; src = $c }
    }
    foreach ($r in $rows) {
        $id = $r.id
        if ($r.home) {
            $chip = [ordered]@{ id = 'home'; name = 'HOME'; number = ''; code = 'H'; desc = 'Alice, chat and PC status'; art = '' }
        } else {
            $c = $r.src
            $chip = [ordered]@{
                id = $id; name = [string](Get-Field $c 'name'); number = [string](Get-Field $c 'number')
                code = [string](Get-Field $c 'code'); desc = [string](Get-Field $c 'desc'); art = [string](Get-Field $c 'art')
            }
            $S.Chips += [pscustomobject]$chip
        }
        $row = New-Object System.Windows.Forms.Panel
        [JackInUi]::B($row, 6, $y, 202, 46)
        $row.Tag = $id
        $row.Cursor = [System.Windows.Forms.Cursors]::Hand
        $row.Add_Click({ param($sender, $e) Select-ChipClick $sender $e })
        $art = New-Object System.Windows.Forms.PictureBox
        [JackInUi]::B($art, 4, 6, 34, 34)
        $art.BackColor = $cEdge
        $art.SizeMode = 'Zoom'
        $art.Tag = $id
        $art.Add_Click({ param($sender, $e) Select-ChipClick $sender $e })
        if ($r.home) {
            $art.SizeMode = 'Normal'
            $art.Image = Crop-Cover $AliceImg 34 34 0.5 0.12
        } else {
            $thumb = Decode-Art $chip.art
            if ($thumb) { $art.Image = $thumb }
        }
        $name = New-Object System.Windows.Forms.Label
        $name.Text = $chip.name
        [JackInUi]::B($name, 46, 8, 124, 30)
        $name.Font = Vt 22
        $name.ForeColor = $cPaper
        $name.BackColor = $cPanel
        $name.Tag = $id
        $name.Add_Click({ param($sender, $e) Select-ChipClick $sender $e })
        $code = New-Object System.Windows.Forms.Label
        $code.Text = $chip.code
        [JackInUi]::B($code, 172, 11, 24, 24)
        $code.Font = Pix 8
        $code.ForeColor = $(if ($r.home) { RGB 0x20 0x50 0xc0 } else { RGB 0xc0 0x18 0x18 })
        $code.BackColor = $cPaper
        $code.TextAlign = 'MiddleCenter'
        $code.Tag = $id
        $code.Add_Click({ param($sender, $e) Select-ChipClick $sender $e })
        $row.Controls.Add($art)
        $row.Controls.Add($name)
        $row.Controls.Add($code)
        $chipList.Controls.Add($row)
        $script:ChipRows += @{ Id = $id; Row = $row; Name = $name; Art = $art; Chip = $chip }
        $y += 50
    }
    Use-GdiPlusText $chipList
    Update-Folder
}

function Update-Folder {
    $cur = $(if ($S.View -eq 'chip' -and $S.ChipId) { $S.ChipId } else { 'home' })
    foreach ($r in $script:ChipRows) {
        $on = ($r.Id -eq $cur)
        $r.Row.BackColor = $(if ($on) { $cSel } else { $cPanel })
        $r.Name.BackColor = $r.Row.BackColor
        $r.Name.ForeColor = $(if ($on) { $cSelInk } else { $cPaper })
    }
    $sel = $script:ChipRows | Where-Object { $_.Id -eq $cur -and $_.Id -ne 'home' } | Select-Object -First 1
    if ($sel) {
        $c = $sel.Chip
        $infoArt.Image = Scale-Art (Decode-Art $c.art) 90 114
        $infoText.Text = "$($c.name.ToUpper())`nCODE $($c.code)`n$($c.desc)"
    } else {
        $infoArt.Image = Crop-Cover $AliceImg 90 114 0.5 0.15
        $infoText.Text = $(if ($S.Linked) { "HOME`nCODE H`nAlice and`nPC status" } else { 'waiting for the PET...' })
    }
    Update-Ui
}

# One chip at a time. The PET leaves the open chip (stopping its player or game)
# before it enters the next one, or goes back to its Jack In screen for HOME.
function Select-Chip([string]$id) {
    if (-not $id) { return }
    if ($id -eq 'home') { Go-Home; return }
    if (-not $S.Linked) { return }
    if ($id -ne $S.ChipId -or $S.View -ne 'chip') {
        if ($id -ne $S.ChipId) { Stop-Play }
        $before = $S.ChipId
        Restart-Mirror
        $S.ChipId = $id
        Send-Pet ([ordered]@{ t = 'open'; chip = $id })
        Add-Log $(if ($before) { "you switched the PET from $(Chip-Name $before) to $(Chip-Name $id)" } else { "you opened $(Chip-Name $id) on the PET" })
    }
    Set-View 'chip'
    Update-Folder
    $mirror.Focus() | Out-Null
}

function Go-Home {
    Stop-Play
    if ($S.ChipId -and $S.Linked) {
        Send-Pet ([ordered]@{ t = 'home' })
        Add-Log "you closed $(Chip-Name $S.ChipId); the PET is back on Jack In"
    }
    $S.ChipId = ''
    Restart-Mirror
    Set-View 'home'
    Update-Folder
    $chatBox.Focus() | Out-Null
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
    # One bitmap per frame size, refilled in place (no new 2 MB bitmap per frame).
    $bmp = $script:FrameBmp
    if ($null -eq $bmp -or $bmp.Width -ne $w -or $bmp.Height -ne $h) {
        if ($bmp) { $bmp.Dispose() }
        $bmp = New-Object System.Drawing.Bitmap -ArgumentList $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $script:FrameBmp = $bmp
    }
    $data = $bmp.LockBits((New-Object System.Drawing.Rectangle -ArgumentList 0, 0, $w, $h), [System.Drawing.Imaging.ImageLockMode]::WriteOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    [Runtime.InteropServices.Marshal]::Copy($script:Pixels, 0, $data.Scan0, $script:Pixels.Length)
    $bmp.UnlockBits($data)
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
    $want = [bool]($S.Linked -and $S.View -eq 'chip' -and -not $Play.On -and $form.Visible -and $form.WindowState -ne 'Minimized')
    if ($want -ne $S.MirrorSent) {
        $S.MirrorSent = $want
        Send-Pet ([ordered]@{ t = 'mirror'; on = $want })
    }
}

# A new chip: drop the old picture and turn the mirror off, so the PET starts the
# next chip with a full frame (it only sends changed rows otherwise).
function Restart-Mirror {
    if ($S.MirrorSent) { Send-Pet ([ordered]@{ t = 'mirror'; on = $false }) }
    $S.MirrorSent = $false
    Clear-Mirror
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

# A mouse point in the mirror panel -> PET pixels, through the same integer
# scale and letterbox as the paint handler; $null outside the picture.
function Map-MirrorPoint([int]$px, [int]$py) {
    if (-not $script:FrameBmp -or $script:FrameW -le 0 -or $script:FrameH -le 0) { return $null }
    $cw = $mirror.ClientSize.Width
    $ch = $mirror.ClientSize.Height
    $sc = [Math]::Max(1, [Math]::Floor([Math]::Min($cw / $script:FrameW, $ch / $script:FrameH)))
    $ox = [int](($cw - $script:FrameW * $sc) / 2)
    $oy = [int](($ch - $script:FrameH * $sc) / 2)
    $fx = [int][Math]::Floor(($px - $ox) / $sc)
    $fy = [int][Math]::Floor(($py - $oy) / $sc)
    if ($fx -lt 0 -or $fy -lt 0 -or $fx -ge $script:FrameW -or $fy -ge $script:FrameH) { return $null }
    return @($fx, $fy)
}

# Mouse in the mirror: left button = PET pointer, wheel = Up/Down (one per notch),
# right button = Back (Escape). Same access rule as keys (Ask or Full only).
$script:MouseDown = $false
$mirror.Add_MouseDown({
    param($ctl, $e)
    $mirror.Focus() | Out-Null
    if (-not (Input-Allowed)) { Show-ReadHint; return }
    if ($e.Button -eq [System.Windows.Forms.MouseButtons]::Left) {
        $pt = Map-MirrorPoint $e.X $e.Y
        if ($null -eq $pt) { return }
        $script:MouseDown = $true
        Send-Pet ([ordered]@{ t = 'pointer'; x = $pt[0]; y = $pt[1]; b = 'down' })
    } elseif ($e.Button -eq [System.Windows.Forms.MouseButtons]::Right) {
        Send-Pet ([ordered]@{ t = 'key'; k = 'Escape' })
    }
})
$mirror.Add_MouseUp({
    param($ctl, $e)
    if ($e.Button -ne [System.Windows.Forms.MouseButtons]::Left -or -not $script:MouseDown) { return }
    $script:MouseDown = $false
    if (-not (Input-Allowed)) { return }
    $pt = Map-MirrorPoint $e.X $e.Y
    if ($null -eq $pt) {
        # Released outside the picture: still release (x/y -1), so the PET never
        # keeps a stuck press, but nothing is activated.
        Send-Pet ([ordered]@{ t = 'pointer'; x = -1; y = -1; b = 'up' })
        return
    }
    Send-Pet ([ordered]@{ t = 'pointer'; x = $pt[0]; y = $pt[1]; b = 'up' })
})
$script:WheelAcc = 0
$mirror.Add_MouseWheel({
    param($ctl, $e)
    if (-not (Input-Allowed)) { Show-ReadHint; return }
    $script:WheelAcc += $e.Delta
    while ([Math]::Abs($script:WheelAcc) -ge 120) {
        if ($script:WheelAcc -gt 0) {
            Send-Pet ([ordered]@{ t = 'key'; k = 'Up' })
            $script:WheelAcc -= 120
        } else {
            Send-Pet ([ordered]@{ t = 'key'; k = 'Down' })
            $script:WheelAcc += 120
        }
    }
})

# Arrows and Tab reach the PET too, not the focus change.
$mirror.Add_Click({ $mirror.Focus() | Out-Null })
$mirror.Add_PreviewKeyDown({ param($ctl, $e) $e.IsInputKey = $true })
$mirror.Add_KeyDown({
    param($ctl, $e)
    if (-not (Input-Allowed)) {
        # Esc still goes HOME (the form's handler); anything else gets the hint.
        if ($e.KeyCode -ne 'Escape' -and $e.KeyCode -ne 'F9' -and (Map-Key $e.KeyCode)) { Show-ReadHint }
        return
    }
    $name = Map-Key $e.KeyCode
    if ($name) {
        Send-Pet ([ordered]@{ t = 'key'; k = $name })
        $e.Handled = $true
    }
})
$mirror.Add_KeyPress({
    param($ctl, $e)
    if ([char]::IsControl($e.KeyChar)) { return }
    if (-not (Input-Allowed)) { Show-ReadHint; return }
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
    Update-Stats $scan
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

# PC STATUS boxes on HOME: green is fine, yellow worth a look, red needs a fix.
function Set-Stat([string]$key, [string]$text, [int]$level) {
    $v = $Stats[$key].Value
    $v.Text = $text
    $v.ForeColor = $(switch ($level) { 0 { $cGreen } 1 { $cYellow } 2 { RGB 0xf8 0x58 0x58 } default { $cIdleInk } })
}

function Update-Stats($scan) {
    if ($null -eq $scan) {
        foreach ($statName in @($Stats.Keys)) { Set-Stat $statName '--' -1 }
        return
    }
    if ($scan.Contains('cpu')) { Set-Stat 'CPU' "$($scan.cpu)%" $(if ($scan.cpu -ge 90) { 2 } elseif ($scan.cpu -ge 60) { 1 } else { 0 }) }
    if ($scan.Contains('ram')) { Set-Stat 'RAM' "$($scan.ram)%" $(if ($scan.ram -ge 90) { 2 } elseif ($scan.ram -ge 60) { 1 } else { 0 }) }
    if ($scan.Contains('disk_free')) { Set-Stat 'C:' "$($scan.disk_free)% free" $(if ($scan.disk_free -lt 10) { 2 } elseif ($scan.disk_free -lt 20) { 1 } else { 0 }) }
    $d = [string]$scan.defender
    Set-Stat 'DEFENDER' $d.ToUpper() $(if ($d -eq 'on') { 0 } elseif ($d -eq 'off') { 2 } else { 1 })
}

# --- commands from the PET -------------------------------------------------------

# Every command the PET sends (Alice's, or one picked on the PET) is parsed here
# and judged by what it would call, never just by how it starts.
function Get-CmdAst([string]$cmd) {
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseInput($cmd, [ref]$tokens, [ref]$errors)
    if ($errors -and @($errors).Count -gt 0) { return $null }
    return $ast
}

function Get-CmdCalls($ast) {
    $calls = @()
    foreach ($c in @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.CommandAst] }, $true))) {
        $name = $c.GetCommandName()
        $argv = @($c.CommandElements | Select-Object -Skip 1 | ForEach-Object { $_.Extent.Text.Trim([char]39, [char]34).ToLower() })
        $raw = $(if ($name) { $name.ToLower() } else { '' })
        $calls += [pscustomobject]@{ Name = ($raw -replace '\.exe$', ''); Raw = $raw; Args = $argv }
    }
    return $calls
}

function Get-CmdMethods($ast) {
    return @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.InvokeMemberExpressionAst] }, $true) |
        ForEach-Object { $_.Member.Extent.Text.Trim([char]39, [char]34).ToLower() })
}

# READ mode runs only these: look-only cmdlet verbs, the pipeline helpers, and
# native tools that only report (with their changing switches refused).
$ReadVerbs = '^((get|test|measure|select|sort|where|group|compare|convertto|convertfrom|resolve|join|split)-|format-(table|list|wide|custom)$)'
$ReadNames = @('foreach-object', 'where-object', 'write-output', 'out-string', 'out-null', '%', '?', 'foreach', 'where', 'sort', 'select', 'measure',
    'group', 'ft', 'fl', 'fw', 'gps', 'ps', 'gci', 'ls', 'dir', 'gc', 'cat', 'type', 'gsv', 'gcim', 'gwmi', 'gp', 'gi', 'gl', 'pwd', 'echo', 'write', 'gm',
    'whoami', 'ipconfig', 'systeminfo', 'tasklist', 'hostname', 'ping', 'tracert', 'pathping', 'nslookup', 'netstat', 'getmac', 'driverquery',
    'gpresult', 'quser', 'qwinsta', 'ver', 'arp', 'nbtstat', 'route', 'w32tm', 'powercfg', 'manage-bde', 'dsregcmd', 'winver', 'chcp')
$ReadMethods = @('tostring', 'trim', 'trimend', 'trimstart', 'split', 'tolower', 'toupper', 'substring', 'replace', 'contains', 'startswith',
    'endswith', 'indexof', 'padleft', 'padright', 'gettype', 'equals', 'tostring', 'join', 'format', 'round', 'floor', 'ceiling', 'count')
function Test-ReadOnly([string]$cmd) {
    $ast = Get-CmdAst $cmd
    if ($null -eq $ast) { return $false }
    if (@($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FileRedirectionAst] }, $true)).Count -gt 0) { return $false }
    foreach ($m in (Get-CmdMethods $ast)) { if ($ReadMethods -notcontains $m) { return $false } }
    # Setting a property changes things too: (Get-Item x).Attributes = 'Hidden'.
    if (@($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and
                    $n.Left -isnot [System.Management.Automation.Language.VariableExpressionAst] }, $true)).Count -gt 0) { return $false }
    foreach ($c in (Get-CmdCalls $ast)) {
        $n = $c.Name
        $a = ' ' + ($c.Args -join ' ') + ' '
        if (-not $n) { return $false }   # & $something: can't tell what it runs
        # ForEach-Object's member form runs a method: gps notepad | % Kill
        if (@('foreach-object', '%', 'foreach') -contains $n) {
            foreach ($x in $c.Args) { if ($x -notmatch '^[-{]' -and $ReadMethods -notcontains $x) { return $false } }
        }
        if ($a -match ' -repair') { return $false }   # Test-ComputerSecureChannel -Repair
        if ($n -like 'get-credential') { return $false }
        if ($n -eq 'ipconfig' -and $a -match ' /(release|renew|flushdns|registerdns|setclassid)') { return $false }
        if ($n -eq 'route' -and $a -notmatch '^\s*print\s') { return $false }
        if ($n -eq 'arp' -and $a -match ' -(d|s) ') { return $false }
        if ($n -eq 'w32tm' -and $a -match ' /(resync|config|register|unregister)') { return $false }
        if ($n -eq 'powercfg' -and $a -notmatch ' /(l|list|q|query|a|availablesleepstates|batteryreport|getactivescheme|requests)( |$)') { return $false }
        if ($n -eq 'manage-bde' -and $a -notmatch ' -status ') { return $false }
        if ($c.Raw -eq 'sc') { return $false }  # sc is Set-Content in PowerShell
        if ($c.Raw -eq 'sc.exe') { if ($a -match '^\s*(query|queryex|qc|qdescription)\s') { continue }; return $false }
        if ($n -match $ReadVerbs -or $ReadNames -contains $n) { continue }
        return $false
    }
    return $true
}

# FULL mode still asks Yes/No for these: anything that deletes, kills, uninstalls,
# changes accounts, security, registry, boot or services, or that we can't read.
function Test-Destructive([string]$cmd) {
    $ast = Get-CmdAst $cmd
    if ($null -eq $ast) { return $true }
    foreach ($m in (Get-CmdMethods $ast)) { if ($m -match '^(delete|kill|remove|format|uninstall|terminate|clear|stop|shutdown|reboot|setacl)') { return $true } }
    foreach ($c in (Get-CmdCalls $ast)) {
        $n = $c.Name
        $a = ' ' + ($c.Args -join ' ') + ' '
        if (-not $n) { return $true }
        if ($n -eq 'clear-host' -or $n -eq 'cls') { continue }
        if ($n -match '^(remove|uninstall|disable|clear|reset|stop|unregister|dismount|suspend|revoke|block|restart|initialize|repair|format-volume|set-mppreference|add-mppreference|set-executionpolicy|set-acl|set-service|set-localuser|new-localuser|add-localgroupmember|set-itemproperty|new-itemproperty|rename-computer|add-computer|set-netfirewall|set-bitlocker|enable-bitlocker|invoke-expression|invoke-command|invoke-wmimethod|invoke-cimmethod|set-date|set-tpm|set-processmitigation)') { return $true }
        if (@('rm', 'del', 'erase', 'rd', 'rmdir', 'ri', 'rp', 'rv', 'kill', 'spps', 'spsv', 'iex', 'icm', 'sc', 'format', 'diskpart', 'taskkill', 'shutdown',
                'bcdedit', 'takeown', 'icacls', 'cacls', 'cipher', 'vssadmin', 'wbadmin', 'fsutil', 'mountvol', 'bootrec', 'reagentc', 'cmd', 'powershell',
                'pwsh', 'wmic', 'logoff', 'bcdboot', 'diskmgmt', 'sdelete', 'clear-recyclebin', 'net1') -contains $n) { return $true }
        if ($n -eq 'manage-bde' -and $a -notmatch ' -status ') { return $true }
        if ($n -eq 'reg' -and $a -match ' (delete|add|import|restore|load|unload|copy) ') { return $true }
        if ($n -eq 'net' -and $a -match ' (user|localgroup|group|stop|share|accounts|use) ') { return $true }
        if ($n -eq 'netsh' -and $a -match ' (set|delete|add|reset|del) ') { return $true }
        if ($c.Raw -eq 'sc.exe' -and $a -match ' (delete|stop|config|create|failure|sdset|pause) ') { return $true }
        if ($n -eq 'msiexec' -and $a -match ' /(x|uninstall)') { return $true }
        if (@('winget', 'choco', 'scoop') -contains $n -and $a -match ' (uninstall|remove|rm) ') { return $true }
        if ($n -eq 'schtasks' -and $a -match ' /(delete|change|create)') { return $true }
        if ($n -eq 'wevtutil' -and $a -match ' (cl|clear-log|um|sl) ') { return $true }
        if ($n -eq 'robocopy' -and $a -match ' /(mir|purge|move|mov)') { return $true }
        if ($n -eq 'dism' -and $a -match ' /(remove|disable|cleanup-image|set)') { return $true }
        if ($n -eq 'ipconfig' -and $a -match ' /release') { return $true }
    }
    return $false
}

function Start-PetCommand($id, [string]$cmd, [bool]$agent = $false) {
    $mode = Get-Mode
    $who = $(if ($agent) { 'Alice' } else { 'The PET' })
    if ($mode -eq 'Read' -and -not (Test-ReadOnly $cmd)) {
        Add-Log "$who asked to run (refused, not read-only in Read mode): $cmd"
        if ($agent) { Add-ChatCmd $cmd 'not run: Read mode only runs read-only commands (pick Ask or Full to allow it)' }
        Send-Pet ([ordered]@{ t = 'result'; id = $id; ok = $false; out = 'refused: not a read-only command and the PC is in Read mode'; reason = 'read-only' })
        if (-not $agent) { Show-Ask 'full' }
        return
    }
    $danger = ($mode -ne 'Read') -and (Test-Destructive $cmd)
    if ($agent) { Add-ChatCmd $cmd $(if ($danger) { 'destructive: waiting for your Yes/No' } elseif ($mode -eq 'Ask') { 'waiting for your Yes/No' } else { '' }) }
    if ($mode -eq 'Ask' -or $danger) {
        $S.Busy = $true
        $runsAs = "$env:USERNAME$(if ($S.Admin) { ' (admin)' })"
        $warn = $(if ($danger) { "This looks DESTRUCTIVE (it can delete, stop, uninstall or change settings).`r`n`r`n" } else { '' })
        $answer = [System.Windows.Forms.MessageBox]::Show($form,
            "$warn$who wants to run this (runs as Windows user $runsAs):`r`n`r`n$cmd`r`n`r`nRun it?",
            'Jack In: run a command?', 'YesNo', $(if ($danger) { 'Warning' } else { 'Question' }), 'Button2')
        $S.Busy = $false
        if ($answer -ne 'Yes') {
            Add-Log "you declined: $cmd"
            if ($agent) { Add-ChatResult $false 'you said No' }
            Send-Pet ([ordered]@{ t = 'result'; id = $id; ok = $false; out = 'declined on the PC'; reason = 'declined' })
            return
        }
    }
    Add-Log "$(if ($agent) { 'Alice' } else { 'PET' }) > $cmd   ($mode)"
    $ps = [PowerShell]::Create()
    [void]$ps.AddScript($cmd).AddCommand('Out-String').AddParameter('Width', 120)
    $S.Jobs += @{ Id = $id; Ps = $ps; Handle = $ps.BeginInvoke(); Started = Get-Date; Cmd = $cmd; Agent = $agent }
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
        if ($job.Agent) { Add-ChatResult $ok $out }
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
    # Sharing the internet never opens this PC's own local services to the PET.
    $ip = $null
    if ($targetHost -eq 'localhost' -or $targetHost -like '*.localhost' -or
        ([System.Net.IPAddress]::TryParse($targetHost.Trim('[', ']'), [ref]$ip) -and ([System.Net.IPAddress]::IsLoopback($ip) -or $ip.ToString() -eq '0.0.0.0' -or $ip.ToString() -eq '::'))) {
        Add-Log "PET asked to reach $targetHost on this PC; refused"
        Send-Pet ([ordered]@{ t = 'net_fail'; id = $id }); return
    }
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

# --- playing the PET's video and games here ------------------------------------------

# The player control, made the first time a video comes (null if WMP is missing).
function Get-Player {
    if ($Play.Ax) { return $Play.Ax.Player }
    try {
        $ax = New-Object JackInWmp
        ([System.ComponentModel.ISupportInitialize]$ax).BeginInit()
        [JackInUi]::B($ax, 0, 28, 826, 438)
        # WMP only makes its video window if it starts up in a visible panel;
        # made inside the hidden one it plays sound over a black area.
        $playPanel.Visible = $true
        $playPanel.BringToFront()
        $playPanel.Controls.Add($ax)
        ([System.ComponentModel.ISupportInitialize]$ax).EndInit()
        $Play.Ax = $ax
        $p = $ax.Player
        $p.uiMode = 'full'
        $p.stretchToFit = $true
        $p.enableContextMenu = $false
        $p.settings.autoStart = $true
        return $p
    } catch {
        Add-Log "no Windows Media Player here ($($_.Exception.Message)); using the default player"
        $Play.Ax = $null
        return $null
    }
}

# Show time now (the stream's position plus where it started), in seconds.
function Get-PlayPos {
    $pos = 0.0
    try { if ($Play.Ax) { $pos = [double]$Play.Ax.Player.controls.currentPosition } } catch { }
    return [Math]::Round($Play.Base + $pos, 1)
}

function Send-Progress([string]$state, [string]$err = '') {
    $paused = $false
    try { if ($Play.Ax) { $paused = ([int]$Play.Ax.Player.playState -eq 2) } } catch { }
    $msg = [ordered]@{ t = 'progress'; item = $Play.Item; pos = (Get-PlayPos); paused = $paused; state = $state }
    if ($err) { $msg.error = $err }
    Send-Pet $msg
    $Play.Reported = Get-Date
}

# Can this PC fetch the first byte of the stream itself within 2 s?
function Test-DirectUrl([string]$link) {
    $resp = $null
    try {
        $req = [System.Net.HttpWebRequest]::Create($link)
        $req.Method = 'GET'
        $req.AddRange(0, 0)
        $req.Timeout = 2000
        $req.ReadWriteTimeout = 2000
        $req.AllowAutoRedirect = $false
        $resp = $req.GetResponse()
        $code = [int]$resp.StatusCode
        return ($code -eq 200 -or $code -eq 206)
    } catch {
        return $false
    } finally {
        if ($resp) { $resp.Close() }
    }
}

function Start-Play($msg) {
    $kind = [string](Get-Field $msg 'kind')
    $title = [string](Get-Field $msg 'title')
    $url = [string](Get-Field $msg 'url')
    if (-not $url) { return }
    if ($kind -eq 'game') {
        # RomM's web player in the default browser, logged in through the PET's
        # relay, opened on 127.0.0.1 so the browser treats it as secure (see JackInLoopback).
        $open = $url
        try {
            $u = [Uri]$url
            $petHosts = @($Pet)
            try { $petHosts += $S.Client.Client.RemoteEndPoint.Address.ToString() } catch { }
            if ($petHosts -notcontains $u.Host) {
                Add-Log "game link isn't on the PET ($($u.Host)); not opening it"
                return
            }
            if (-not $u.IsLoopback) {
                $lp = $Loop.Start(7345, $u.Host, $u.Port)
                if ($lp -gt 0) { $open = 'http://127.0.0.1:' + $lp + $u.PathAndQuery }
                else { Add-Log "couldn't open a local port for the game; trying the PET's address (some cores need HTTPS there)" }
            }
        } catch { Add-Log "game link: $($_.Exception.Message)" }
        Start-Process $open | Out-Null
        Add-Log "opened $title in your browser (RomM web player via the PET, $open)"
        $direct = [string](Get-Field $msg 'direct')
        if ($direct) { Add-Log "if it doesn't load, RomM itself: $direct" }
        return
    }
    $item = [string](Get-Field $msg 'item')
    $same = $Play.On -and $Play.Item -eq $item
    $Play.Item = $item
    $Play.Title = $title
    $Play.Base = [double](Get-Field $msg 'base')
    $Play.StartAt = [double](Get-Field $msg 'start')
    $seek = Get-Field $msg 'seekable'
    $Play.Seekable = ($null -eq $seek) -or [bool]$seek
    $Play.Started = $false
    $Play.Opened = Get-Date
    $Play.Reported = Get-Date
    $p = Get-Player
    if ($null -eq $p) {
        # No player control: hand the stream to whatever plays video here.
        Start-Process $url | Out-Null
        Add-Log "playing $title in your default player"
        $Play.On = $true
        Send-Progress 'stopped'  # nothing reports back from there; the PET goes back to its list
        $Play.On = $false
        return
    }
    # Straight from Jellyfin when this PC can reach it (the PET hands a key made
    # for this one play and deletes it on stop); otherwise through the PET's relay.
    $via = 'the PET'
    $directUrl = [string](Get-Field $msg 'direct_url')
    if ($directUrl -and (Test-DirectUrl $directUrl)) { $url = $directUrl; $via = 'Jellyfin directly' }
    $Play.On = $true
    $EscFilter.Armed = $true
    $nowLabel.Text = '  NOW PLAYING: ' + $title + $(if ($Play.Seekable) { '' } else { '   (converted on the server: use -10s/+10s to jump)' })
    $p.URL = $url
    $p.controls.play()
    if (-not $same) { Add-Log "playing $title here (from the PET's Jellyfin, via $via)" }
    if ($S.View -ne 'chip') { Set-View 'chip' } else { Update-Ui }
}

# Ends the video; tells the PET where it stopped unless the PET asked for the stop.
function Stop-Play([string]$state = 'stopped', [bool]$tell = $true, [string]$err = '') {
    if (-not $Play.On) { return }
    if ($tell) { Send-Progress $state $err }
    $Play.On = $false
    $EscFilter.Armed = $false
    $EscFilter.Pressed = $false
    try { $Play.Ax.Player.controls.stop(); $Play.Ax.Player.URL = '' } catch { }
    # Let WMP go (tens of MB); the next video makes a fresh one in the visible panel.
    if ($Play.Ax) {
        $ax = $Play.Ax
        $Play.Ax = $null
        try { $playPanel.Controls.Remove($ax); $ax.Dispose() } catch { }
    }
    Add-Log $(switch ($state) { 'ended' { "$($Play.Title) finished" } 'error' { "couldn't play $($Play.Title): $err" } default { "stopped $($Play.Title)" } })
    Update-Ui
    $mirror.Focus() | Out-Null
}

function Seek-Play([double]$by) {
    if (-not $Play.On) { return }
    if ($Play.Seekable) {
        try {
            $c = $Play.Ax.Player.controls
            $c.currentPosition = [Math]::Max(0, [double]$c.currentPosition + $by)
        } catch { }
        return
    }
    # A transcode can't jump: the PET restarts it at the new time.
    Send-Pet ([ordered]@{ t = 'progress'; item = $Play.Item; pos = [Math]::Max(0, (Get-PlayPos) + $by); paused = $false; state = 'seek' })
}

# Commands from the PET's keys while its video plays here.
function On-PlayerCommand($msg) {
    if (-not $Play.On -or -not $Play.Ax) { return }
    $p = $Play.Ax.Player
    $by = [double](Get-Field $msg 'by')
    switch ([string](Get-Field $msg 'cmd')) {
        'seek' { Seek-Play $by }
        'volume' { $p.settings.volume = [int][Math]::Max(0, [Math]::Min(100, $p.settings.volume + $by)) }
        'pause' { if ([int]$p.playState -eq 3) { $p.controls.pause() } else { $p.controls.play() } }
    }
}

# Called by the UI timer: seek once open, notice the end or an error, report every 10 s.
function Step-Play {
    if (-not $Play.On -or -not $Play.Ax) { return }
    $p = $Play.Ax.Player
    $ps = [int]$p.playState
    if ($ps -eq 3 -and -not $Play.Started) {
        $Play.Started = $true
        if ($Play.StartAt -gt 1) { $p.controls.currentPosition = $Play.StartAt }
    }
    if ($ps -eq 8) { Stop-Play 'ended'; return }  # MediaEnded
    if ($ps -eq 1 -and $Play.Started) { Stop-Play 'stopped'; return }  # the player's own stop button
    if (-not $Play.Started) {
        $err = ''
        try { if ($p.error.errorCount -gt 0) { $err = [string]$p.error.item(0).errorDescription } } catch { }
        if ($err -or ($ps -eq 10 -and ((Get-Date) - $Play.Opened).TotalSeconds -gt 5) -or ((Get-Date) - $Play.Opened).TotalSeconds -gt 60) {
            Stop-Play 'error' $true $(if ($err) { $err } else { 'the stream never started' })
            return
        }
    }
    if ($Play.Started -and ((Get-Date) - $Play.Reported).TotalSeconds -ge 10) { Send-Progress 'playing' }
}

$btnStop.Add_Click({ Stop-Play })
$btnStopVideo.Add_Click({ Stop-Play })
$btnBack10.Add_Click({ Seek-Play -10 })
$btnFwd10.Add_Click({ Seek-Play 10 })

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
        Stop-Play 'stopped' $false
        Stop-Mic $false
        Stop-Jobs
        Stop-Sharing
        $Loop.Stop()
        Clear-Mirror
        Build-Chips @()
        Set-View 'home'
        Add-Log "link lost: $why (back to read-only)"
    }
    Update-Ui
}

function Jack-Out([string]$why) {
    if ($S.Closing) { return }
    $S.Closing = $true
    Stop-Mic $false
    Stop-Play
    if ($S.Agent) { Send-Pet ([ordered]@{ t = 'abort' }) }
    Send-Pet ([ordered]@{ t = 'bye'; why = $why })
    $S.Full = $false
    Stop-Jobs
    Stop-Sharing
    $Loop.Stop()
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
        'run' { Start-PetCommand (Get-Field $msg 'id') ([string](Get-Field $msg 'cmd')) ([bool](Get-Field $msg 'agent')) }
        'pet' {
            $op = [string](Get-Field $msg 'operator')
            if ($op) { $S.Operator = $op; Update-Ui }
        }
        'agent_more' {
            # Alice hit her step cap (25 in Full, 8 otherwise): Holden decides.
            $steps = [string](Get-Field $msg 'steps')
            $next = [string](Get-Field $msg 'next')
            $S.Busy = $true
            $answer = [System.Windows.Forms.MessageBox]::Show($form,
                "Alice has run $steps commands for this message.`r`n`r`nNext she wants:`r`n$next`r`n`r`nKeep going?",
                'Jack In: keep going?', 'YesNo', 'Question', 'Button1')
            $S.Busy = $false
            $yes = ($answer -eq 'Yes')
            Add-Log $(if ($yes) { "you let Alice keep going after $steps commands" } else { "you stopped Alice after $steps commands" })
            if (-not $yes) { $S.Agent = $false; Add-ChatResult $false "stopped after $steps commands" }
            Send-Pet ([ordered]@{ t = 'agent_more'; yes = $yes })
        }
        'agent_stop' {
            $S.Agent = $false
            Add-Log "Alice's command loop stopped ($([string](Get-Field $msg 'why')))"
        }
        'scan' { Invoke-Scan }
        'net_open' { Open-Tunnel (Get-Field $msg 'id') ([string](Get-Field $msg 'host')) ([int](Get-Field $msg 'port')) }
        'jackout' { Add-Log 'PET pressed JACK OUT'; Jack-Out 'PET' }
        'chips' { Build-Chips @(Get-Field $msg 'list') }
        'frame' { Apply-Frame $msg }
        'play' { Start-Play $msg }
        'stop_play' { if ([string](Get-Field $msg 'item') -eq $Play.Item) { Stop-Play 'stopped' $false } }
        'player' { On-PlayerCommand $msg }
        'alice' {
            $heard = [string](Get-Field $msg 'heard')
            $text = [string](Get-Field $msg 'text')
            $cmd = [string](Get-Field $msg 'cmd')
            if ($heard) { Add-Chat 'you' $heard }
            if ($text -or -not $cmd) { Add-Chat 'Alice' $text }
            if ((Get-Field $msg 'ok') -and $text) { Speak-Alice $text }
            # Her command (if any) arrives next as a 'run' with agent=true; the mode decides.
            $S.Agent = [bool]$cmd
            if ($cmd) { Add-Log "Alice step $([string](Get-Field $msg 'step')): $cmd" }
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
                admin = $S.Admin; full = $S.Full; input = [bool]$S.Full; mode = (Get-Mode).ToLower(); app = $AppVersion })
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

$chatBox.Add_KeyDown({
    param($ctl, $e)
    if ($e.KeyCode -eq 'Return') { $e.Handled = $true; $e.SuppressKeyPress = $true; Send-Chat }
})
$btnHome.Add_Click({ Go-Home })
$btnShowLog.Add_Click({
    $S.ShowLog = -not $S.ShowLog
    Set-View $S.View
    if ($S.ShowLog) { $logBox.ScrollToCaret() } else { $chatLog.ScrollToCaret() }
})
$btnMic.Add_MouseDown({ Start-Mic })
$btnMic.Add_MouseUp({ Stop-Mic })
# Hold F9 anywhere in the window to talk (key repeat is ignored while recording).
# Esc goes HOME from a chip. While you type into the PET screen (it has the focus
# in Ask or Full), plain Esc goes to the chip instead and Shift+Esc goes HOME.
$form.Add_KeyDown({
    param($ctl, $e)
    if ($e.KeyCode -eq 'F9') { $e.Handled = $true; if (-not $Mic.On) { Start-Mic }; return }
    if ($e.KeyCode -eq 'Escape' -and $Play.On) {
        $e.Handled = $true
        $e.SuppressKeyPress = $true
        Stop-Play
        return
    }
    if ($e.KeyCode -eq 'Escape' -and $S.View -eq 'chip') {
        if ($mirror.Focused -and (Input-Allowed) -and -not $e.Shift) { return }
        $e.Handled = $true
        $e.SuppressKeyPress = $true
        Go-Home
    }
})
$form.Add_KeyUp({ param($sender, $e) if ($e.KeyCode -eq 'F9') { $e.Handled = $true; Stop-Mic } })
$btnVoice.Add_Click({
    $S.Speak = -not $S.Speak
    if (-not $S.Speak -and $script:Voice) { $script:Voice.SpeakAsyncCancelAll() }
    $btnVoice.Invalidate()
    $btnVoice.BackColor = $(if ($S.Speak) { RGB 0x40 0x80 0xd0 } else { $cIdle })
    $tips.SetToolTip($btnVoice, $(if ($S.Speak) { 'Alice reads answers aloud (click to mute)' } else { 'Alice is muted (click to hear her)' }))
})

$btnOut.Add_Click({ Add-Log 'you pressed JACK OUT'; Jack-Out 'PC' })
# JackIn.cmd waits for this file: it means the window really came up.
$form.Add_Shown({ try { Set-Content -LiteralPath (Join-Path $env:TEMP 'JackIn-ready.txt') -Value $PID -ErrorAction Stop } catch { } })
$form.Add_FormClosing({ if (-not $S.Closing) { Jack-Out 'window closed' } })

# --- run -------------------------------------------------------------------------

# Live PC STATUS: CPU/RAM/disk every 3 s, Defender every 30 s, without CIM
# (fast enough to run on the UI timer). Any piece that fails is just skipped.
$script:CpuCounter = $null
try {
    $script:CpuCounter = New-Object System.Diagnostics.PerformanceCounter('Processor', '% Processor Time', '_Total')
    [void]$script:CpuCounter.NextValue()
} catch { $script:CpuCounter = $null }
try { Add-Type -AssemblyName Microsoft.VisualBasic } catch { }
$script:LiveTicks = 0
function Refresh-LiveStats {
    $live = [ordered]@{}
    try { if ($script:CpuCounter) { $live.cpu = [int][Math]::Round($script:CpuCounter.NextValue()) } } catch { }
    try {
        $info = New-Object Microsoft.VisualBasic.Devices.ComputerInfo
        $live.ram = [int][Math]::Round(100 - 100.0 * $info.AvailablePhysicalMemory / $info.TotalPhysicalMemory)
    } catch { }
    try {
        $drive = New-Object System.IO.DriveInfo('C')
        $live.disk_free = [int](100.0 * $drive.AvailableFreeSpace / $drive.TotalSize)
    } catch { }
    $script:LiveTicks++
    # Defender's state (a CIM query, the slow part) only every ~5 minutes.
    if (($script:LiveTicks % 100) -eq 1) {
        try { $live.defender = $(if ((Get-MpComputerStatus -ErrorAction Stop).RealTimeProtectionEnabled) { 'on' } else { 'off' }) } catch { }
    }
    if (-not $live.Contains('defender')) {
        $cur = [string]$Stats['DEFENDER'].Value.Text
        if ($cur -and $cur -ne '--') { $live.defender = $cur.ToLower() } else { $live.defender = '?' }
    }
    Update-Stats $live
}

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 100
$Clock = [System.Diagnostics.Stopwatch]::StartNew()
$script:NextDialMs = 0
$script:NextStatsMs = 3000
$timer.Add_Tick({
    if ($S.Busy) { return }
    try {
        $nowMs = $Clock.ElapsedMilliseconds
        # Dial the PET about every 2 s until it answers.
        if ($S.Client -or $S.Connecting -or $nowMs -ge $script:NextDialMs) { $script:NextDialMs = $nowMs + 2000; Step-Link }
        if ($S.Jobs.Count) { Step-Jobs }
        if ($S.Tunnels.Count) { Step-Tunnels }
        if ($EscFilter.Pressed) { $EscFilter.Pressed = $false; Stop-Play }
        if ($Play.On) { Step-Play }
        if ($readHint.Visible -and (Get-Date) -gt $script:ReadHintUntil) { $readHint.Visible = $false }
        Sync-Mirror
        # PC STATUS every 3 s, only while someone can see it.
        if ($nowMs -ge $script:NextStatsMs) {
            $script:NextStatsMs = $nowMs + 3000
            if ($form.Visible -and $form.WindowState -ne 'Minimized') { Refresh-LiveStats }
        }
        if ($Mic.On -and ((Get-Date) - $Mic.Started).TotalSeconds -ge 15) { Stop-Mic }
        # Nothing going on (no PET yet): tick 4x a second instead of 10x.
        $idle = -not ($S.Linked -or $Play.On -or $Mic.On -or $S.Jobs.Count -or $S.Tunnels.Count)
        $timer.Interval = $(if ($idle) { 250 } else { 100 })
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
Set-View 'home'
Add-Log "Jack In $AppVersion$(if ($Elevated) { ' (admin)' }) - $($env:COMPUTERNAME) - read-only until you pick Ask or Full"
Update-Ui
$timer.Start()
[void]$form.ShowDialog()
$timer.Dispose()
if ($script:Voice) { $script:Voice.Dispose() }
Clear-Mirror
if ($AliceImg) { $AliceImg.Dispose() }
$Fonts.Dispose()
$form.Dispose()
