# SPDX-License-Identifier: MIT
#
# PET-os Jack In for Windows. Runs from the PET's USB drive with the
# PowerShell 5.1 that ships with Windows 10/11; nothing is installed. The only
# files it writes are small diagnostics in %TEMP% (JackIn-ready.txt,
# JackIn-launch.txt, JackIn-error.txt, JackIn-timing.log, capped at 256 KB) and the
# WebView2 cache for Retro games (%TEMP%\JackIn-WebView2); the session log is only
# saved if you click Save Log. webview2\ holds Microsoft's WebView2 SDK (its LICENSE.txt).
#
#  - Starts READ-ONLY: a scan (CPU, RAM, disk, startup items, Defender, top RAM users).
#  - Access: READ (look only), ASK (PET commands run as you after you OK each one),
#    FULL (PET commands run as you without asking). Typing into the PET's screen
#    works in ASK and FULL only; every mode can watch it.
#  - FOLDER: HOME (your Navi, chat and PC status) or one chip at a time: Terminal,
#    Jellyfin, Retro or LAN Tech. The PET decides whether this PC may open chips:
#    [K] on the PET allows it, and Terminal needs its own [K] every session (this
#    window says so when it is waiting). The PET closes the open chip (and its player or
#    game) before it opens the next. The chip's screen is mirrored here while it
#    is open and this window is up and not minimized. < HOME or Esc comes back.
#    (Esc goes to the chip while you type into its screen; use Shift+Esc there.)
#  - Run as admin...: relaunches this app through the Windows UAC prompt that you click.
#  - NET: the PET borrows this PC's internet (outbound connections only).
#  - Chat with your Navi (the PET's [navi] name and picture; Alice and alice.png until it says):
#    type (Enter sends), or hold MIC (or F9) to talk; answers can be read aloud.
#    The Navi can run PowerShell here to answer you: each command it sends goes through the access
#    mode (READ: read-only commands only, run in a locked-down session; ASK: Yes/No for each; FULL: Yes/No only for destructive
#    ones) and its output goes back to it automatically; after 25 commands (8 in Read/Ask)
#    for one message you are asked whether it keeps going.
#  - Jellyfin video and Retro games picked on the PET's chip play HERE: video in a
#    NOW PLAYING player in the chip area (Windows Media Player; Esc or STOP
#    comes back to the chip and tells Jellyfin where you stopped), games in the same
#    area (RomM's web player in a WebView2, on http://127.0.0.1 so it counts as secure;
#    Shift+Esc or STOP comes back; in your browser if WebView2 is missing). Both come
#    through the PET's relay on 10.55.0.1, so no server passwords reach this PC. (Only if the PET
#    is set to [jellyfin] pc_direct=on does it also offer a direct Jellyfin link with a key for that play.)
#  - Every command is shown in the live log. JACK OUT ends everything at once.
# The app only dials out to the PET (10.55.0.1); it never listens. See PROTOCOL.md.

param(
    [string]$Pet = '10.55.0.1',
    [int]$Port = 7341,
    [int]$TunnelPort = 7342,
    [switch]$Elevated,
    [switch]$StartFull,
    [ValidateSet('', 'Read', 'Ask', 'Full')][string]$StartMode = '',
    # Where the PET's trust token is kept (tests point this somewhere else).
    [string]$TrustFile = ''
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

$AppVersion = '0.9.7'
$Here = Split-Path -Parent $PSCommandPath

# --- trusted PC ----------------------------------------------------------------------
# [T] twice on the PET trusts this PC: the PET makes a token, sends it here, and
# this app shows it back in every hello. It is kept for this Windows user only
# (DPAPI), so another account on this PC is not trusted along with it. The PET
# keeps the list and can forget this PC at any time; the token then stops working.
$TrustPath = $(if ($TrustFile) { $TrustFile } else { Join-Path $env:APPDATA 'PET-os\jackin-trust.dat' })
# The salt keeps its old name: tokens saved before the rename still open.
$TrustSalt = [System.Text.Encoding]::ASCII.GetBytes('AlicePET Jack In trust')
# Before rev31 the app was called AlicePET: carry this PC's trust over once.
$OldTrustPath = Join-Path $env:APPDATA 'AlicePET\jackin-trust.dat'
if (-not $TrustFile -and -not (Test-Path -LiteralPath $TrustPath) -and (Test-Path -LiteralPath $OldTrustPath)) {
    try {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $TrustPath) | Out-Null
        Copy-Item -LiteralPath $OldTrustPath -Destination $TrustPath
    } catch { }
}
function Get-TrustToken {
    try {
        if (-not (Test-Path -LiteralPath $TrustPath)) { return '' }
        Add-Type -AssemblyName System.Security
        $plain = [System.Security.Cryptography.ProtectedData]::Unprotect([System.IO.File]::ReadAllBytes($TrustPath), $TrustSalt, 'CurrentUser')
        $token = [System.Text.Encoding]::ASCII.GetString($plain)
        if ($token -match '^[0-9a-f]{64}$') { return $token }
    } catch { }
    return ''
}
function Set-TrustToken([string]$token) {
    try {
        if ($token -notmatch '^[0-9a-f]{64}$') {
            if (Test-Path -LiteralPath $TrustPath) { Remove-Item -LiteralPath $TrustPath -Force }
            return $true
        }
        Add-Type -AssemblyName System.Security
        $dir = Split-Path -Parent $TrustPath
        if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        $sealed = [System.Security.Cryptography.ProtectedData]::Protect([System.Text.Encoding]::ASCII.GetBytes($token), $TrustSalt, 'CurrentUser')
        [System.IO.File]::WriteAllBytes($TrustPath, $sealed)
        return $true
    } catch { return $false }
}

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
    Decoder    = $null    # UTF-8 decoder kept across reads, so a character split between reads survives
    Connecting = $null    # Task from ConnectAsync
    Full       = [bool]$StartFull -or $StartMode -eq 'Ask' -or $StartMode -eq 'Full'
    Admin      = $false
    Net        = $false
    Jobs       = @()      # running PET commands: @{Id; Ps; Handle; Output; Started; Cmd}
    Stopping   = @()      # pipelines being stopped without blocking the UI: @{Ps; Ar}
    Tunnels    = @()      # @{Id; Target; Pet; Phase; Tasks}
    Log        = New-Object System.Collections.Generic.List[string]
    Closing    = $false
    Busy       = $false   # a scan or a confirm box is up (they pump UI events)
    Confirm    = ($StartMode -ne 'Full')  # Ask mode: OK each PET command first
    Speak      = $false   # read the Navi's answers aloud (muted until the speaker button)
    Navi       = 'Alice'  # the Navi's name; the PET sends its [navi] name (and picture) after hello
    Linked     = $false
    Chips      = @()      # chips the PET can open: {id, name, number, code, desc, art}
    ChipId     = ''       # the chip the PC last asked the PET to open
    MirrorSent = $false   # last mirror on/off sent to the PET
    View       = 'home'   # centre shows 'home' or 'chip'
    Operator   = 'Holden' # who runs the PET (the PET's [jackin] operator=), never the Windows user
    ShowLog    = $false   # HOME shows the live log instead of the chat
    Agent      = $false   # Alice's command loop is running for the last chat message
    AskStale   = $false   # the PET took access back (or jacked out) while a Yes/No box was open
    WantChip   = ''       # the chip waiting for [K] on the PET
    PetControl = $false   # the PET lets this PC open and drive chips ([K] there)
    PetTerminal = $false  # ... and open Terminal
    PetTrusted = $false   # the PET knows this PC's trust token ([T] twice there)
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
    Game     = $false  # a Retro game (RomM's web player) in the WebView2 below, not a video
    Web      = $null   # that WebView2 control (kept between games, closed after a minute unused)
    WebReady = $false  # ... and its Edge engine has started
    WebIdle  = [datetime]::MinValue
    GameUrl  = ''      # the game's link, for the browser if the WebView2 can't start
    Origin   = ''      # the only site the game's WebView2 may navigate to
    WebError = ''      # set by WebView2 events, handled by the UI timer
}
$script:WebMissing = ''   # why WebView2 can't be used here (then games go to the browser)
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
$tips.SetToolTip($btnVoice, 'Alice is muted (click to hear answers)')
$tips.SetToolTip($btnMic, 'Hold to talk (or hold F9)')
$tips.SetToolTip($btnHome, 'Back to HOME (Esc; Shift+Esc while typing into the PET screen)')
$tips.SetToolTip($chatBox, 'Send mail to Alice (Enter)')
[JackInFrame]::Cue($chatBox.Handle, 'Send mail...')

$HomeCtl = @($hintLabel, $portrait, $aliceName, $aliceStatus, $chatLog, $statusHdr, $logBox, $btnLog, $btnShowLog, $btnVoice) + @($Stats.Values | ForEach-Object { $_.Box })
$ChipCtl = @($btnHome, $viewKey, $viewMode, $viewPet, $mirror, $miniFace, $aliceWho, $aliceLine)

# Input in the mirror without access: say so instead of ignoring it.
function Show-ReadHint {
    $why = $(if (-not $S.Linked) { 'not linked to the PET yet' } elseif (-not $S.PetControl) { 'press [K] on the PET to let this PC control chips' } else { 'READ mode: pick ASK or FULL to control chips' })
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
    $S.Log.Add("$stamp  $(if ($who -eq 'navi') { $S.Navi } else { $who }): $text")
    if ($who -eq 'navi') {
        Write-Rich $chatLog "$($S.Navi.ToUpper()): " (RGB 0xc0 0x28 0x80) $true $null 11
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

# Timing diagnostics go to a file, not the UI: %TEMP%\JackIn-timing.log (new in rev26).
$script:TimingLog = Join-Path $env:TEMP 'JackIn-timing.log'
# It only ever grows while the app runs: start over once it passes 256 KB.
try { if ((Test-Path -LiteralPath $script:TimingLog) -and (Get-Item -LiteralPath $script:TimingLog).Length -gt 262144) { Remove-Item -LiteralPath $script:TimingLog -Force } } catch { }
$script:TimingNext = [DateTime]::MinValue
$script:InputSentAt = $null

function Write-Timing([string]$text) {
    try { Add-Content -LiteralPath $script:TimingLog -Value ("{0} {1}" -f (Get-Date -Format 'HH:mm:ss.fff'), $text) } catch { }
}

function Send-Pet($obj) {
    if ($null -eq $S.Writer) { return }
    if ($obj.t -in @('key', 'char', 'pointer') -and $null -eq $script:InputSentAt) { $script:InputSentAt = [Diagnostics.Stopwatch]::StartNew() }
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
    return [bool]($S.Linked -and $S.Full -and $S.PetControl)
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
    foreach ($b in @($btnBack10, $btnFwd10)) { $b.Visible = $playing -and -not $Play.Game }
    $btnStop.Visible = $playing
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
    # Read stops Alice's command loop (the PET stops it when it sees mode read),
    # and anything a command is still doing.
    if ($mode -eq 'Read') { $S.Agent = $false; Stop-Jobs 'stopped: access went back to Read' }
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

# The PET's [navi] name and picture, sent once after hello. An older PET sends no
# 'navi' line, and a PET with no [navi] picture sends no art: then alice.png stays.
function Set-Navi([string]$name, [string]$art) {
    $name = ($name -replace '[^\x20-\x7e]', '').Trim()
    if ($name.Length -gt 12) { $name = $name.Substring(0, 12).Trim() }
    if ($name) { $S.Navi = $name }
    $img = Decode-Art $art
    if ($img) {
        if ($script:AliceImg) { $script:AliceImg.Dispose() }
        $script:AliceImg = $img
        foreach ($p in @($portrait, $miniFace)) { if ($p.Image) { $p.Image.Dispose() } }
        $portrait.Image = Crop-Cover $script:AliceImg 230 342 0.5 0.2
        $miniFace.Image = Crop-Cover $script:AliceImg 32 32 0.5 0.12
    }
    $up = $S.Navi.ToUpper()
    $form.Text = "Jack In - $($S.Navi).exe"
    $aliceName.Font = Pix $(if ($up.Length -gt 11) { 7 } else { 8 })
    $aliceName.Text = "$up.EXE"
    # The chip view's speaker label grows with the name; the answer line takes the rest.
    $aliceWho.Text = "${up}:"
    $right = $aliceLine.Right
    $aliceWho.Width = [Math]::Max([JackInUi]::S(80), [System.Windows.Forms.TextRenderer]::MeasureText($aliceWho.Text, $aliceWho.Font).Width + [JackInUi]::S(10))
    $aliceLine.Left = $aliceWho.Right
    $aliceLine.Width = $right - $aliceLine.Left
    $tips.SetToolTip($chatBox, "Send mail to $($S.Navi) (Enter)")
    $tips.SetToolTip($btnVoice, $(if ($S.Speak) { "$($S.Navi) reads answers aloud (click to mute)" } else { "$($S.Navi) is muted (click to hear answers)" }))
    Add-Log "Navi: $($S.Navi)$(if ($img) { ' (picture from the PET)' })"
    Update-Folder
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
            $chip = [ordered]@{ id = 'home'; name = 'HOME'; number = ''; code = 'H'; desc = "$($S.Navi), chat and PC status"; art = '' }
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
        $infoText.Text = $(if ($S.Linked) { "HOME`nCODE H`n$($S.Navi) and`nPC status" } else { 'waiting for the PET...' })
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
    $script:FramePending = $true
}

# Copies the decoded pixels to the mirror once per frame, after all of its bands.
function Flush-Frame {
    if (-not $script:FramePending -or $null -eq $script:Pixels) { return }
    $script:FramePending = $false
    $w = $script:FrameW
    $h = $script:FrameH
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
    if ($script:InputSentAt) {
        $ms = $script:InputSentAt.Elapsed.TotalMilliseconds
        $script:InputSentAt = $null
        if ($ms -ge 50 -or [DateTime]::Now -ge $script:TimingNext) {
            $script:TimingNext = [DateTime]::Now.AddMilliseconds(500)
            Write-Timing ("key-to-frame {0:N1} ms" -f $ms)
        }
    }
}

function Clear-Mirror {
    if ($script:FrameBmp) { $script:FrameBmp.Dispose() }
    $script:FrameBmp = $null
    $script:Pixels = $null
    $script:FrameW = 0
    $script:FrameH = 0
    $script:FramePending = $false
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
# Read by Flush-Frame on the first link pass, before any frame has set it: under
# Set-StrictMode an unset variable throws, which was taken for a broken link (rev26).
$script:FramePending = $false

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
    try {
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
    Add-Log ('scan done (read-only): ' + ($summary -join ', ') + '. Look at the PET for what ' + $S.Navi + ' thinks.')
    Add-Log 'read-only checks complete'
    } finally {
        $form.Cursor = [System.Windows.Forms.Cursors]::Default
        $S.Busy = $false
    }
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
        $rest = @($c.CommandElements | Select-Object -Skip 1)
        $argv = @($rest | ForEach-Object { $_.Extent.Text.Trim([char]39, [char]34).ToLower() })
        # Parameter names without the dash, and with any ":value" cut off (-MemberName:Kill -> membername).
        $params = @($rest | Where-Object { $_ -is [System.Management.Automation.Language.CommandParameterAst] } |
            ForEach-Object { $_.ParameterName.ToLower() })
        $raw = $(if ($name) { $name.ToLower() } else { '' })
        $calls += [pscustomobject]@{ Name = ($raw -replace '\.exe$', ''); Raw = $raw; Args = $argv; Params = $params; Elems = $rest }
    }
    return $calls
}

function Get-CmdMethods($ast) {
    return @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.InvokeMemberExpressionAst] }, $true) |
        ForEach-Object { $_.Member.Extent.Text.Trim([char]39, [char]34).ToLower() })
}

# "[type]::Method(...)" calls as "type::method", lower case, without "system.".
function Get-CmdStaticCalls($ast) {
    return @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.InvokeMemberExpressionAst] -and $n.Static }, $true) |
        ForEach-Object {
            $type = $(if ($_.Expression -is [System.Management.Automation.Language.TypeExpressionAst]) { $_.Expression.TypeName.FullName.ToLower() -replace '^system\.', '' } else { '?' })
            $type + '::' + $_.Member.Extent.Text.Trim([char]39, [char]34).ToLower()
        })
}

# Every type a command names ([IO.File], [int]$x, [xml]...), lower case, without "system." and "[]".
function Get-CmdTypes($ast) {
    return @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.TypeExpressionAst] -or
                $n -is [System.Management.Automation.Language.TypeConstraintAst] }, $true) |
        ForEach-Object { (($_.TypeName.FullName.ToLower() -replace '^system\.', '') -replace '(\[\])+$', '') })
}

# What a ForEach-Object call would run by name instead of by script block:
# "% Kill", "% -MemberName Kill", "% -MemberName:Kill". Script blocks are judged
# by what is inside them; anything else here is a member (or can't be told).
function Get-ForEachMembers($call) {
    $members = @()
    $skipBlock = $false
    foreach ($e in $call.Elems) {
        if ($e -is [System.Management.Automation.Language.ScriptBlockExpressionAst]) { continue }
        if ($e -is [System.Management.Automation.Language.CommandParameterAst]) {
            $p = $e.ParameterName.ToLower()
            # -Begin / -Process / -End take script blocks; nothing else is expected here.
            if ('begin'.StartsWith($p) -or 'process'.StartsWith($p) -or 'end'.StartsWith($p)) {
                if ($e.Argument -and $e.Argument -isnot [System.Management.Automation.Language.ScriptBlockExpressionAst]) { $members += '?' }
                continue
            }
            if ($e.Argument) { $members += $e.Argument.Extent.Text.Trim([char]39, [char]34).ToLower() } else { $members += '-' + $p }
            continue
        }
        $members += $e.Extent.Text.Trim([char]39, [char]34).ToLower()
    }
    return $members
}

# READ mode runs only these: look-only cmdlets, the pipeline helpers, and native
# tools that only report (with their changing switches refused). The command is
# parsed and every part of it must be on a list; then it runs in a constrained
# runspace (New-ReadRunspace) as a second lock.
$ReadVerbs = '^(get-|test-|(measure|select|sort|where|group|compare|foreach)-object$|measure-command$|select-(string|xml)$|convertto-(json|csv|html|xml)$|convertfrom-(json|csv|stringdata|string)$|resolve-(path|dnsname)$|(join|split)-path$|join-string$|format-(table|list|wide|custom|hex)$|out-(string|null|host)$|write-(output|host)$)'
$ReadNames = @('%', '?', 'foreach', 'where', 'sort', 'select', 'measure', 'group', 'ft', 'fl', 'fw', 'gps', 'ps', 'gci', 'ls', 'dir', 'gc', 'cat',
    'type', 'gsv', 'gcim', 'gwmi', 'gp', 'gi', 'gl', 'pwd', 'echo', 'write', 'gm', 'sls', 'gcm', 'gdr', 'gal', 'gv', 'gmo')
# Programs (not PowerShell commands). A name ending in .exe must be one of these.
$ReadNative = @('whoami', 'ipconfig', 'systeminfo', 'tasklist', 'hostname', 'ping', 'tracert', 'pathping', 'nslookup', 'netstat', 'getmac',
    'driverquery', 'gpresult', 'quser', 'qwinsta', 'arp', 'nbtstat', 'route', 'w32tm', 'powercfg', 'manage-bde', 'dsregcmd')
# Get-/Test- commands that change or reveal more than "look": refused by name.
$ReadRefused = @('get-credential', 'get-clipboard', 'get-windowsupdatelog', 'get-wuinstall', 'get-windowsupdate', 'get-wulist')
$ReadMethods = @('tostring', 'trim', 'trimend', 'trimstart', 'split', 'tolower', 'toupper', 'substring', 'contains', 'startswith',
    'endswith', 'indexof', 'padleft', 'padright', 'gettype', 'equals', 'join', 'round', 'floor', 'ceiling', 'count')
# Types a READ command may name. Anything else ([IO.File], [IO.StreamWriter]'path', [scriptblock], [wmi]...) is refused.
$ReadTypes = @('string', 'char', 'int', 'int16', 'int32', 'int64', 'long', 'uint16', 'uint32', 'uint64', 'double', 'single', 'float', 'decimal',
    'bool', 'boolean', 'byte', 'datetime', 'timespan', 'version', 'guid', 'array', 'object', 'hashtable', 'pscustomobject', 'ordered', 'void')
# ([math] is not on the list: the constrained READ session refuses [math]::Round(...) anyway. Use [int](...) or '{0:N1}' -f ...)
function Test-ReadOnly([string]$cmd) {
    $ast = Get-CmdAst $cmd
    if ($null -eq $ast) { return $false }
    if (@($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FileRedirectionAst] }, $true)).Count -gt 0) { return $false }
    # New commands under harmless names (function Get-X { ... }), classes, attributes, "using".
    if (@($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -or
                    $n -is [System.Management.Automation.Language.TypeDefinitionAst] -or
                    $n -is [System.Management.Automation.Language.AttributeAst] -or
                    $n -is [System.Management.Automation.Language.UsingStatementAst] }, $true)).Count -gt 0) { return $false }
    foreach ($m in (Get-CmdMethods $ast)) { if ($ReadMethods -notcontains $m) { return $false } }
    foreach ($t in (Get-CmdTypes $ast)) { if ($ReadTypes -notcontains $t) { return $false } }
    # ${function:Get-X} = '...', ${alias:x}, ${C:\file} = '...' are writes dressed as variables:
    # only $env:NAME may name a drive, and only to read it.
    foreach ($v in @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.VariableExpressionAst] }, $true))) {
        if ($v.VariablePath.IsDriveQualified -and $v.VariablePath.DriveName -ne 'env') { return $false }
    }
    foreach ($asg in @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] }, $true))) {
        # Setting a property changes things too: (Get-Item x).Attributes = 'Hidden'.
        if ($asg.Left -isnot [System.Management.Automation.Language.VariableExpressionAst]) { return $false }
        if ($asg.Left.VariablePath.IsDriveQualified) { return $false }
    }
    foreach ($u in @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.UnaryExpressionAst] }, $true))) {
        if ($u.Child -is [System.Management.Automation.Language.VariableExpressionAst] -and $u.Child.VariablePath.IsDriveQualified) { return $false }
    }
    foreach ($c in (Get-CmdCalls $ast)) {
        $n = $c.Name
        $a = ' ' + ($c.Args -join ' ') + ' '
        if (-not $n) { return $false }   # & $something: can't tell what it runs
        # ForEach-Object's member form runs a method: gps notepad | % Kill, % -MemberName:Kill
        if (@('foreach-object', '%', 'foreach') -contains $n) {
            foreach ($x in (Get-ForEachMembers $c)) { if ($ReadMethods -notcontains $x) { return $false } }
        }
        if ($ReadRefused -contains $n) { return $false }
        # Parameters that write a file, install, repair or ask for a password, in full or shortened
        # (-Repair, -Rep, -OutFile, -LogPath, -Install, -AcceptAll, -Credential...).
        foreach ($p in $c.Params) { if ($p -match '^(rep|inst|downl|accepta|autor|outf|outp|logp|cred)') { return $false } }
        if ($n -eq 'ipconfig' -and $a -match ' /(release|renew|flushdns|registerdns|setclassid)') { return $false }
        if ($n -eq 'route' -and $a -notmatch '^\s*print\s') { return $false }
        if ($n -eq 'arp' -and $a -match ' -(d|s) ') { return $false }
        if ($n -eq 'nbtstat' -and $a -match ' -rr? ') { return $false }
        if ($n -eq 'w32tm' -and $a -match ' /(resync|config|register|unregister)') { return $false }
        if ($n -eq 'powercfg' -and ($a -notmatch ' /(l|list|q|query|a|availablesleepstates|getactivescheme|requests)( |$)' -or $a -match ' /output')) { return $false }
        if ($n -eq 'manage-bde' -and $a -notmatch ' -status ') { return $false }
        if ($n -eq 'dsregcmd' -and $a -notmatch '^\s*/status\s*$') { return $false }
        if ($n -eq 'gpresult' -and $a -match ' /(h|x|f)( |$)') { return $false }
        if ($c.Raw -eq 'sc') { return $false }  # sc is Set-Content in PowerShell
        if ($c.Raw -eq 'sc.exe') { if ($a -match '^\s*(query|queryex|qc|qdescription)\s') { continue }; return $false }
        if ($ReadNative -contains $n) { continue }
        # sort.exe /o file, where.exe ...: an .exe that isn't a known reporting tool.
        if ($c.Raw -like '*.exe') { return $false }
        if ($n -match $ReadVerbs -or $ReadNames -contains $n) { continue }
        return $false
    }
    return $true
}

# READ commands run in their own runspace in ConstrainedLanguage mode: even if a
# command slipped past Test-ReadOnly, it can't call .NET methods on files,
# processes or the registry, create COM objects or add types there.
function New-ReadRunspace {
    $iss = [System.Management.Automation.Runspaces.InitialSessionState]::CreateDefault()
    $iss.LanguageMode = [System.Management.Automation.PSLanguageMode]::ConstrainedLanguage
    $rs = [System.Management.Automation.Runspaces.RunspaceFactory]::CreateRunspace($iss)
    $rs.Open()
    return $rs
}

# FULL mode still asks Yes/No for these: anything that deletes, kills, uninstalls,
# changes accounts, security, registry, boot or services, or that we can't read.
$DangerMembers = '^(delete|kill|remove|format|uninstall|terminate|clear|stop|shutdown|reboot|setacl|setaccesscontrol|invoke|create$|createsubkey|start$|setvalue|moveto|encrypt|decrypt|change$)'
function Test-Destructive([string]$cmd) {
    $ast = Get-CmdAst $cmd
    if ($null -eq $ast) { return $true }
    foreach ($m in (Get-CmdMethods $ast)) { if ($m -match $DangerMembers) { return $true } }
    # [IO.File]::Replace / ::Move / ::WriteAllText, [scriptblock]::Create, [Diagnostics.Process]::Start ...
    foreach ($sc in (Get-CmdStaticCalls $ast)) {
        if ($sc -match '^(string|regex|text\.regularexpressions\.regex|math|convert|datetime|timespan|array|char|guid|int|int32|int64|double|decimal|version|io\.path)::') { continue }
        if ($sc -match '::(replace|move|copy|write|append|create|start|delete|kill|set|encrypt|decrypt|open|load|exit)') { return $true }
    }
    # ${function:x} = '...' / ${alias:x} / ${C:\file} = '...': code or a file written through a variable.
    foreach ($asg in @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] }, $true))) {
        if ($asg.Left -is [System.Management.Automation.Language.VariableExpressionAst] -and $asg.Left.VariablePath.IsDriveQualified -and
            $asg.Left.VariablePath.DriveName -ne 'env') { return $true }
    }
    foreach ($c in (Get-CmdCalls $ast)) {
        $n = $c.Name
        $a = ' ' + ($c.Args -join ' ') + ' '
        if (-not $n) { return $true }
        if ($n -eq 'clear-host' -or $n -eq 'cls') { continue }
        # gci x | % Delete, gps x | % -MemberName:Kill, % $name
        if (@('foreach-object', '%', 'foreach') -contains $n) {
            foreach ($x in (Get-ForEachMembers $c)) { if ($x -match $DangerMembers -or $x -match '^[$?(-]') { return $true } }
        }
        if ($n -match '^(remove|uninstall|disable|clear|reset|stop|unregister|dismount|suspend|revoke|block|restart|initialize|repair|format-volume|set-mppreference|add-mppreference|set-executionpolicy|set-acl|set-service|set-localuser|new-localuser|add-localgroupmember|set-itemproperty|new-itemproperty|rename-computer|add-computer|set-netfirewall|set-bitlocker|enable-bitlocker|invoke-expression|invoke-command|invoke-wmimethod|invoke-cimmethod|set-date|set-tpm|set-processmitigation|set-alias|new-alias)') { return $true }
        if (@('rm', 'del', 'erase', 'rd', 'rmdir', 'ri', 'rp', 'rv', 'kill', 'spps', 'spsv', 'iex', 'icm', 'sc', 'format', 'diskpart', 'taskkill', 'shutdown',
                'bcdedit', 'takeown', 'icacls', 'cacls', 'cipher', 'vssadmin', 'wbadmin', 'fsutil', 'mountvol', 'bootrec', 'reagentc', 'cmd', 'powershell',
                'pwsh', 'wmic', 'logoff', 'bcdboot', 'diskmgmt', 'sdelete', 'clear-recyclebin', 'net1', 'clc', 'cli', 'clp', 'clv', 'sp', 'sal', 'nal',
                'wscript', 'cscript', 'mshta', 'rundll32', 'regsvr32') -contains $n) { return $true }
        # Start-Process is fine for an app, not as a way round this list (Start-Process cmd '/c del ...') or to elevate.
        if (@('start-process', 'saps', 'start') -contains $n -and
            ($a -match '[ \\/](cmd|powershell|pwsh|wscript|cscript|mshta|rundll32|regsvr32|reg|sc|net|net1|msiexec|schtasks|wmic|diskpart|format|bcdedit|takeown|icacls|shutdown|taskkill|certutil|bitsadmin)(\.exe)? ' -or
             @($c.Params | Where-Object { $_ -match '^v' }).Count -gt 0)) { return $true }
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
    $who = $(if ($agent) { $S.Navi } else { 'The PET' })
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
        $S.AskStale = $false
        $S.Busy = $true
        $runsAs = "$env:USERNAME$(if ($S.Admin) { ' (admin)' })"
        $warn = $(if ($danger) { "This looks DESTRUCTIVE (it can delete, stop, uninstall or change settings).`r`n`r`n" } else { '' })
        try {
            $answer = [System.Windows.Forms.MessageBox]::Show($form,
                "$warn$who wants to run this (runs as Windows user $runsAs):`r`n`r`n$cmd`r`n`r`nRun it?",
                'Jack In: run a command?', 'YesNo', $(if ($danger) { 'Warning' } else { 'Question' }), 'Button2')
        } finally {
            $S.Busy = $false
        }
        if ($answer -ne 'Yes') {
            Add-Log "you declined: $cmd"
            if ($agent) { Add-ChatResult $false 'you said No' }
            Send-Pet ([ordered]@{ t = 'result'; id = $id; ok = $false; out = 'declined on the PC'; reason = 'declined' })
            return
        }
        # The box can sit there for minutes. A Yes only counts if nothing changed
        # behind it: still linked, same access mode, and the PET didn't take access
        # back or jack out meanwhile (those messages are read while the box is up).
        if ($S.AskStale -or -not $S.Linked -or (Get-Mode) -ne $mode) {
            Add-Log "not run (the link or the access mode changed while the question was open): $cmd"
            if ($agent) { Add-ChatResult $false 'not run: access changed while you were deciding' }
            Send-Pet ([ordered]@{ t = 'result'; id = $id; ok = $false; out = 'not run: access changed on the PC'; reason = 'declined' })
            return
        }
    }
    Add-Log "$(if ($agent) { $S.Navi } else { 'PET' }) > $cmd   ($mode)"
    $ps = [PowerShell]::Create()
    $rs = $null
    if ($mode -eq 'Read') {
        # Second lock behind Test-ReadOnly: a session that can't call .NET on files or processes.
        try {
            $rs = New-ReadRunspace
            $ps.Runspace = $rs
        } catch {
            try { $ps.Dispose() } catch { }
            Add-Log "couldn't open the read-only session: $($_.Exception.Message)"
            Send-Pet ([ordered]@{ t = 'result'; id = $id; ok = $false; out = 'refused: the read-only session could not start'; reason = 'read-only' })
            return
        }
    }
    [void]$ps.AddScript($cmd).AddCommand('Out-String').AddParameter('Width', 120)
    # Output goes into our own collection, so what a command printed is still readable if it times out.
    $output = New-Object System.Management.Automation.PSDataCollection[psobject]
    $feed = New-Object System.Management.Automation.PSDataCollection[psobject]
    $feed.Complete()
    $handle = $ps.BeginInvoke($feed, $output)
    $S.Jobs += @{ Id = $id; Ps = $ps; Rs = $rs; Handle = $handle; Output = $output; Started = Get-Date; Cmd = $cmd; Agent = $agent }
}

# Text a job has printed so far.
function Get-JobText($job) {
    return ((@($job.Output) | ForEach-Object { [string]$_ }) -join '').Trim()
}

# Stop a pipeline without blocking the UI thread. Reap-Stopping disposes it once the stop has finished.
function Stop-PsAsync($ps, $rs = $null) {
    try {
        $ar = $ps.BeginStop($null, $null)
        $S.Stopping += ,@{ Ps = $ps; Rs = $rs; Ar = $ar }
    } catch {
        try { $ps.Dispose() } catch { }
        if ($rs) { try { $rs.Dispose() } catch { } }
    }
}

function Reap-Stopping {
    $left = @()
    foreach ($st in @($S.Stopping)) {
        if ($st.Ar.IsCompleted) {
            try { $st.Ps.EndStop($st.Ar) } catch { }
            try { $st.Ps.Dispose() } catch { }
            if ($st.Rs) { try { $st.Rs.Dispose() } catch { } }
        } else {
            $left += $st
        }
    }
    $S.Stopping = $left
}

function Step-Jobs {
    Reap-Stopping
    $finished = @()   # Ids of the jobs handled in this pass
    foreach ($job in @($S.Jobs)) {
        # Stop-Jobs (from a dropped link inside this loop) may have taken this job already.
        $stillOpen = @($S.Jobs | Where-Object { [object]::ReferenceEquals($_, $job) }).Count -gt 0
        if (-not $stillOpen) { continue }
        # A job that finished on the same tick as the timeout is a result, not a timeout.
        $done = $job.Handle.IsCompleted
        $timedOut = -not $done -and ((Get-Date) - $job.Started).TotalSeconds -gt 120
        if (-not $done -and -not $timedOut) { continue }
        $finished += $job.Id
        $out = ''
        $ok = $true
        $stopped = $false
        try {
            if ($timedOut) {
                # Keep what it printed before the stop; the PET still gets it.
                $stopped = $true
                $ok = $false
                $out = Get-JobText $job
                Stop-PsAsync $job.Ps $job.Rs
                $out = (($out + "`r`n" + 'stopped after 2 minutes').Trim())
            } else {
                $job.Ps.EndInvoke($job.Handle) | Out-Null
                $out = Get-JobText $job
                if ($job.Ps.HadErrors) {
                    $ok = $false
                    $out = (($job.Ps.Streams.Error | ForEach-Object { $_.ToString() }) -join '; ') + ' ' + $out
                }
            }
        } catch {
            $ok = $false
            $out = $_.Exception.Message
        }
        if (-not $stopped) {
            $job.Ps.Dispose()
            if ($job.Rs) { try { $job.Rs.Dispose() } catch { } }
        }
        if ($out.Length -gt 4000) { $out = $out.Substring(0, 4000) + '...' }
        Add-Log ($(if ($ok) { '  ok' } else { '  failed' }) + $(if ($out) { ': ' + ($out -split "`n")[0] } else { '' }))
        if ($job.Agent) { Add-ChatResult $ok $out }
        Send-Pet ([ordered]@{ t = 'result'; id = $job.Id; ok = $ok; out = $out })
    }
    # Re-read $S.Jobs: Send-Pet can drop the link, and Stop-Jobs then empties the list.
    # Writing back a list built before the loop would bring those disposed jobs back.
    $S.Jobs = @($S.Jobs | Where-Object { $finished -notcontains $_.Id })
}

# Stops every running command. With $why (access went back to Read) the PET is
# told what happened to each; without it the link is going away anyway.
function Stop-Jobs([string]$why = '') {
    $jobs = @($S.Jobs)
    $S.Jobs = @()
    foreach ($job in $jobs) {
        Stop-PsAsync $job.Ps $job.Rs
        if ($why) {
            Add-Log "  stopped: $($job.Cmd)"
            if ($job.Agent) { Add-ChatResult $false $why }
            Send-Pet ([ordered]@{ t = 'result'; id = $job.Id; ok = $false; out = $why; reason = 'declined' })
        }
    }
}

# Before the app exits: give the stops up to 2 s to finish, so a program a
# command started (ping -t, robocopy...) is not left running after JACK OUT.
function Wait-Stopping {
    $until = (Get-Date).AddSeconds(2)
    while ($S.Stopping.Count -and (Get-Date) -lt $until) {
        Reap-Stopping
        if ($S.Stopping.Count) { Start-Sleep -Milliseconds 50 }
    }
}

# --- NET: Share PC Internet (the PC dials both ends; nothing listens here) --------

# Addresses that are this PC itself: loopback, "any", and every address of its own adapters.
function Test-OwnAddress($ip) {
    if ($null -eq $ip) { return $true }
    if ($ip.IsIPv4MappedToIPv6) { $ip = $ip.MapToIPv4() }
    if ([System.Net.IPAddress]::IsLoopback($ip) -or $ip.Equals([System.Net.IPAddress]::Any) -or $ip.Equals([System.Net.IPAddress]::IPv6Any)) { return $true }
    try {
        foreach ($nic in [System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces()) {
            foreach ($ua in $nic.GetIPProperties().UnicastAddresses) {
                if ($ua.Address.Equals($ip)) { return $true }
            }
        }
    } catch { }
    return $false
}

function Start-TunnelConnect($t, $ip) {
    $t.Target = New-Object System.Net.Sockets.TcpClient -ArgumentList $ip.AddressFamily
    $t.Pet = New-Object System.Net.Sockets.TcpClient   # back to the PET's tunnel port
    $t.Phase = 'connect'
    $t.Tasks = @($t.Target.ConnectAsync($ip, $t.Port), $t.Pet.ConnectAsync($script:Pet, $TunnelPort))
}

function Open-Tunnel($id, [string]$targetHost, [int]$targetPort) {
    if (-not $S.Net) { Send-Pet ([ordered]@{ t = 'net_fail'; id = $id }); return }
    # Sharing the internet never opens this PC's own local services to the PET.
    # The name is resolved here first and the tunnel dials that address, so a
    # name that points back at this PC (localhost, its own name, a DNS record for
    # 127.0.0.1 or its LAN address) is refused like the address itself.
    $tunnel = @{ Id = $id; Host = $targetHost; Port = $targetPort; Target = $null; Pet = $null; Phase = 'resolve'; Started = Get-Date; Tasks = @() }
    $ip = $null
    if ([System.Net.IPAddress]::TryParse($targetHost.Trim('[', ']'), [ref]$ip)) {
        if (Test-OwnAddress $ip) {
            Add-Log "PET asked to reach $targetHost on this PC; refused"
            Send-Pet ([ordered]@{ t = 'net_fail'; id = $id }); return
        }
        Start-TunnelConnect $tunnel $ip
    } elseif ($targetHost -eq 'localhost' -or $targetHost -like '*.localhost') {
        Add-Log "PET asked to reach $targetHost on this PC; refused"
        Send-Pet ([ordered]@{ t = 'net_fail'; id = $id }); return
    } else {
        $tunnel.Tasks = @([System.Net.Dns]::GetHostAddressesAsync($targetHost))
    }
    $S.Tunnels += $tunnel
}

function Close-Tunnel($t) {
    if ($t.Target) { try { $t.Target.Close() } catch { } }
    if ($t.Pet) { try { $t.Pet.Close() } catch { } }
}

function Step-Tunnels {
    $left = @()
    foreach ($t in $S.Tunnels) {
        if ($t.Phase -eq 'resolve') {
            $lookup = $t.Tasks[0]
            if ($lookup.IsFaulted -or $lookup.IsCanceled -or ((Get-Date) - $t.Started).TotalSeconds -gt 12) {
                Send-Pet ([ordered]@{ t = 'net_fail'; id = $t.Id })
                continue
            }
            if (-not $lookup.IsCompleted) { $left += $t; continue }
            $found = @($lookup.Result)
            $own = @($found | Where-Object { Test-OwnAddress $_ }).Count -gt 0
            if ($found.Count -eq 0 -or $own) {
                if ($own) { Add-Log "PET asked to reach $($t.Host), which is this PC; refused" }
                Send-Pet ([ordered]@{ t = 'net_fail'; id = $t.Id })
                continue
            }
            $pick = @($found | Where-Object { $_.AddressFamily -eq 'InterNetwork' }) + @($found) | Select-Object -First 1
            try { Start-TunnelConnect $t $pick } catch { Close-Tunnel $t; Send-Pet ([ordered]@{ t = 'net_fail'; id = $t.Id }); continue }
            $left += $t
            continue
        }
        if ($t.Phase -eq 'connect') {
            $failed = @($t.Tasks | Where-Object { $_.IsFaulted -or $_.IsCanceled }).Count -gt 0
            $slow = ((Get-Date) - $t.Started).TotalSeconds -gt 12
            if ($failed -or $slow) {
                Close-Tunnel $t
                Send-Pet ([ordered]@{ t = 'net_fail'; id = $t.Id })
                continue
            }
            if (@($t.Tasks | Where-Object { -not $_.IsCompleted }).Count -gt 0) { $left += $t; continue }
            try {
                $hello = [System.Text.Encoding]::ASCII.GetBytes("tunnel $($t.Id)`n")
                $ps = $t.Pet.GetStream()
                $ps.Write($hello, 0, $hello.Length)
                $ts = $t.Target.GetStream()
                $t.Tasks = @($ps.CopyToAsync($ts), $ts.CopyToAsync($ps))
            } catch {
                # The PET side died before the hello went out: this tunnel is over, the rest go on.
                Close-Tunnel $t
                Send-Pet ([ordered]@{ t = 'net_fail'; id = $t.Id })
                continue
            }
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
    foreach ($t in $S.Tunnels) {
        # A tunnel still connecting never reached the PET, which is waiting on it: say the host could not be reached.
        if ($t.Phase -ne 'open') { Send-Pet ([ordered]@{ t = 'net_fail'; id = $t.Id }) }
        Close-Tunnel $t
    }
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

# Retro games play here too: RomM's web player in a WebView2 (the Edge engine
# Windows 10/11 already have) in the NOW PLAYING area. The SDK DLLs come with
# this app (webview2\); without them or the WebView2 runtime the game opens in
# the default browser as before. Shift+Esc in the game, or STOP, comes back.
$GameKeys = @'
(function () {
  if (window.__petJackIn) return;
  window.__petJackIn = 1;
  window.addEventListener('keydown', function (e) {
    if (e.key === 'Escape' && e.shiftKey) {
      e.preventDefault(); e.stopPropagation();
      try { window.chrome.webview.postMessage('stop'); } catch (x) { }
    }
  }, true);
})();
'@

# A new WebView2 control in the play panel, or $null (and why in $script:WebMissing).
function New-GameView {
    if ($script:WebMissing) { return $null }
    try {
        if (-not ('Microsoft.Web.WebView2.WinForms.WebView2' -as [type])) {
            $dir = Join-Path $Here 'webview2'
            Add-Type -Path (Join-Path $dir 'Microsoft.Web.WebView2.Core.dll')
            Add-Type -Path (Join-Path $dir 'Microsoft.Web.WebView2.WinForms.dll')
            # The loader DLL for this PowerShell's own architecture (x86 PowerShell on x64 says x86).
            $arch = switch ([string]$env:PROCESSOR_ARCHITECTURE) { 'ARM64' { 'arm64' } 'x86' { 'x86' } default { 'x64' } }
            [Microsoft.Web.WebView2.Core.CoreWebView2Environment]::SetLoaderDllFolderPath((Join-Path $dir $arch))
        }
        # Throws when the WebView2 runtime isn't installed.
        $ver = [Microsoft.Web.WebView2.Core.CoreWebView2Environment]::GetAvailableBrowserVersionString()
        if (-not $ver) { throw 'the WebView2 runtime is not installed' }
    } catch {
        $script:WebMissing = $_.Exception.Message
        Add-Log "can't play games in this window ($($script:WebMissing)); they open in your browser"
        return $null
    }
    $w = New-Object Microsoft.Web.WebView2.WinForms.WebView2
    $props = New-Object Microsoft.Web.WebView2.WinForms.CoreWebView2CreationProperties
    # Its cache and cookies (the relay's login, which dies with the link) stay in %TEMP%.
    $props.UserDataFolder = Join-Path ([IO.Path]::GetTempPath()) 'JackIn-WebView2'
    $w.CreationProperties = $props
    $w.DefaultBackgroundColor = [System.Drawing.Color]::Black
    [JackInUi]::B($w, 0, 28, 826, 438)
    $w.add_CoreWebView2InitializationCompleted({
        param($ctl, $e)
        try {
            if (-not $e.IsSuccess) { $Play.WebError = [string]$e.InitializationException.Message; return }
            $Play.WebReady = $true
            $Play.Started = $true
            $c = $ctl.CoreWebView2
            $c.Settings.AreDevToolsEnabled = $false
            $c.Settings.AreDefaultContextMenusEnabled = $false
            $c.Settings.IsStatusBarEnabled = $false
            $c.Settings.IsZoomControlEnabled = $false
            [void]$c.AddScriptToExecuteOnDocumentCreatedAsync($GameKeys)
            # Only RomM through the PET's relay: no pop-ups, no other sites.
            $c.add_NewWindowRequested({ param($x, $a) $a.Handled = $true })
            $c.add_ProcessFailed({ param($x, $a) $Play.WebError = 'the game page crashed (' + [string]$a.ProcessFailedKind + ')' })
        } catch { $Play.WebError = $_.Exception.Message }
    })
    $w.add_NavigationStarting({
        param($ctl, $e)
        $u = [string]$e.Uri
        if ($u -eq 'about:blank' -or $u -eq $Play.Origin -or $u.StartsWith($Play.Origin + '/')) { return }
        $e.Cancel = $true
        Add-Log "the game page tried to leave RomM ($u); blocked"
    })
    $w.add_NavigationCompleted({
        param($ctl, $e)
        try {
            # Also here: the first page can load before the script above is registered.
            [void]$ctl.CoreWebView2.ExecuteScriptAsync($GameKeys)
            if ($Play.On -and $Play.Game) { $ctl.Focus() | Out-Null }
        } catch { }
    })
    $w.add_WebMessageReceived({
        param($ctl, $e)
        try { if ($e.TryGetWebMessageAsString() -eq 'stop') { $EscFilter.Pressed = $true } } catch { }
    })
    # Like WMP, it needs the visible panel to start up in.
    $playPanel.Visible = $true
    $playPanel.BringToFront()
    $playPanel.Controls.Add($w)
    return $w
}

# Plays the game at `link` in this window; $false sends it to the browser instead.
function Start-Game([string]$title, [string]$link) {
    if ($Play.On) { Stop-Play }
    # One WebView2 for the session's games: a new one right after the last one
    # closed can find its Edge profile still in use and fail.
    $w = $Play.Web
    if ($null -eq $w) { $w = New-GameView }
    if ($null -eq $w) { return $false }
    $Play.On = $true
    $Play.Game = $true
    $Play.Web = $w
    $Play.Item = ''
    $Play.Title = $title
    $Play.GameUrl = $link
    $Play.Origin = ([Uri]$link).GetLeftPart([UriPartial]::Authority)
    $Play.WebError = ''
    $Play.Started = $Play.WebReady
    $Play.Opened = Get-Date
    $nowLabel.Text = '  PLAYING: ' + $title + '   (Shift+Esc or STOP comes back)'
    try {
        $w.Source = [Uri]$link
    } catch {
        $Play.WebError = $_.Exception.Message
    }
    Add-Log "playing $title here (RomM's web player via the PET)"
    if ($S.View -ne 'chip') { Set-View 'chip' } else { Update-Ui }
    $w.Focus() | Out-Null
    return $true
}

# Lets the game's WebView2 go (its Edge processes end with it).
function Close-GameView {
    if (-not $Play.Web) { return }
    $w = $Play.Web
    $Play.Web = $null
    $Play.WebReady = $false
    $Play.WebError = ''
    try { $playPanel.Controls.Remove($w); $w.Dispose() } catch { }
}

# Called by the UI timer while a game is up: a WebView2 that fails sends the game to the browser.
function Step-Game {
    $why = $Play.WebError
    if (-not $why -and -not $Play.Started -and ((Get-Date) - $Play.Opened).TotalSeconds -gt 30) { $why = "WebView2 didn't start" }
    if (-not $why) { return }
    $link = $Play.GameUrl
    $title = $Play.Title
    Stop-Play 'stopped' $false
    Close-GameView
    if (-not $Play.Started) { $script:WebMissing = $why }
    Add-Log "couldn't play $title in this window ($why); opening it in your browser"
    try { Start-Process $link | Out-Null } catch { Add-Log "browser: $($_.Exception.Message)" }
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
        if (Start-Game $title $open) { return }
        Start-Process $open | Out-Null
        Add-Log "opened $title in your browser (RomM web player via the PET, $open)"
        $direct = [string](Get-Field $msg 'direct')
        if ($direct) { Add-Log "if it doesn't load, RomM itself: $direct" }
        return
    }
    $item = [string](Get-Field $msg 'item')
    if ($Play.Game) { Stop-Play }
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
        # Nothing reports back from there. 'handoff' sends the PET back to its list
        # without telling Jellyfin the play stopped ('stopped' ended the transcode
        # the player had just started reading).
        Send-Progress 'handoff'
        $Play.On = $false
        return
    }
    # Through the PET's relay, unless the PET is set to [jellyfin] pc_direct=on and
    # this PC can reach Jellyfin itself (then the PET hands over a key made for
    # this one play and deletes it on stop).
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
    # Off first: Send-Progress can re-enter (a dropped link runs Stop-Play again).
    $Play.On = $false
    $wasGame = $Play.Game
    $Play.Game = $false
    if ($tell -and -not $wasGame) { Send-Progress $state $err }
    $EscFilter.Armed = $false
    $EscFilter.Pressed = $false
    try { $Play.Ax.Player.controls.stop(); $Play.Ax.Player.URL = '' } catch { }
    # Let WMP go (tens of MB); the next video makes a fresh one in the visible panel.
    if ($Play.Ax) {
        $ax = $Play.Ax
        $Play.Ax = $null
        try { $playPanel.Controls.Remove($ax); $ax.Dispose() } catch { }
    }
    # The game's page goes (and its sound); the WebView2 waits a minute for another game.
    if ($wasGame -and $Play.Web) {
        $Play.WebIdle = Get-Date
        if ($Play.WebReady) { try { $Play.Web.CoreWebView2.Navigate('about:blank') } catch { Close-GameView } }
        else { Close-GameView }
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
    if ($Play.On -and $Play.Game) { Step-Game; return }
    if (-not $Play.On -or -not $Play.Ax) { return }
    $p = $Play.Ax.Player
    try { $ps = [int]$p.playState } catch { Stop-Play 'error' $true 'the player stopped answering'; return }
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
    $S.Client = $null; $S.Stream = $null; $S.Writer = $null; $S.Buffer = ''; $S.Decoder = $null
    $S.Agent = $false
    $S.MirrorSent = $false
    if ($S.Linked) {
        $S.Linked = $false
        # No PET, no access: back to read-only, sharing off.
        $S.Full = $false
        $S.Chips = @()
        $S.ChipId = ''
        $S.WantChip = ''
        $S.PetControl = $false
        $S.PetTerminal = $false
        $S.PetTrusted = $false
        Hide-Ask
        Stop-Play 'stopped' $false
        Close-GameView
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
    Close-GameView
    if ($S.Agent) { Send-Pet ([ordered]@{ t = 'abort' }) }
    Send-Pet ([ordered]@{ t = 'bye'; why = $why })
    $S.Full = $false
    Stop-Jobs
    Stop-Sharing
    $Loop.Stop()
    if ($S.Client) { try { $S.Client.Close() } catch { } }
    $timer.Stop()
    Wait-Stopping
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
            if ($what -eq 'full' -and $S.Full) {
                $S.Full = $false; $S.Agent = $false
                Add-Log 'PET set access back to Read'
                Send-State
                Stop-Jobs 'stopped: the PET set access back to Read'
            }
            if ($what -eq 'net' -and $S.Net) { Stop-Sharing; Add-Log 'PET stopped sharing'; Send-State }
            Update-Ui
        }
        'run' { Start-PetCommand (Get-Field $msg 'id') ([string](Get-Field $msg 'cmd')) ([bool](Get-Field $msg 'agent')) }
        'pet' {
            $op = [string](Get-Field $msg 'operator')
            if ($op) { $S.Operator = $op }
            # What the PET lets this PC do with it ([K] there); an older PET sends neither and allows both.
            $ctl = Get-Field $msg 'control'
            $term = Get-Field $msg 'terminal'
            $S.PetControl = ($null -eq $ctl) -or [bool]$ctl
            $S.PetTerminal = ($null -eq $term) -or [bool]$term
            $S.PetTrusted = [bool](Get-Field $msg 'trusted')
            if (-not $S.PetControl -and $S.View -eq 'chip') {
                Add-Log 'the PET took chip control back'
                Stop-Play
                $S.ChipId = ''
                Restart-Mirror
                Set-View 'home'
                Update-Folder
            }
            # [K] was pressed for the chip this PC was waiting on: the PET opens it itself.
            if ($S.WantChip -and $S.PetControl -and ($S.WantChip -ne 'terminal' -or $S.PetTerminal)) {
                $S.ChipId = $S.WantChip
                $S.WantChip = ''
                Hide-Ask
                Add-Log "the PET allowed it: $(Chip-Name $S.ChipId) is open"
                Set-View 'chip'
                Update-Folder
                $mirror.Focus() | Out-Null
            }
            Update-Ui
        }
        'trust' {
            # [T] twice on the PET: keep its token for the next link, or forget it.
            $token = [string](Get-Field $msg 'token')
            if (Set-TrustToken $token) {
                Add-Log $(if ($token -match '^[0-9a-f]{64}$') { 'the PET trusts this PC now: next time chips and ' + $S.Navi + ' work without [K]' } else { 'the PET no longer trusts this PC' })
            } else {
                Add-Log 'the PET trusted this PC, but the token could not be saved here'
            }
        }
        'denied' {
            # The PET (not this PC) decides who opens chips. Say what to press there.
            $chipAsked = [string](Get-Field $msg 'chip')
            $S.WantChip = $chipAsked
            $S.ChipId = ''
            Restart-Mirror
            Set-View 'home'
            Update-Folder
            $askBanner.Text = $(if ($what -eq 'terminal') { '  TERMINAL uses the PET''s SSH keys: press [K] on the PET to allow it for this session.' }
                else { '  Press [K] on the PET to let this PC open and control chips.' })
            $askBanner.Visible = $true
            $askBanner.BringToFront()
            Add-Log "the PET is waiting for [K] before it opens $(Chip-Name $chipAsked)"
        }
        'agent_more' {
            # The Navi hit its step cap (25 in Full, 8 otherwise): the operator decides.
            $steps = [string](Get-Field $msg 'steps')
            $next = [string](Get-Field $msg 'next')
            $S.AskStale = $false
            $S.Busy = $true
            try {
                $answer = [System.Windows.Forms.MessageBox]::Show($form,
                    "$($S.Navi) has run $steps commands for this message.`r`n`r`nNext it wants:`r`n$next`r`n`r`nKeep going?",
                    'Jack In: keep going?', 'YesNo', 'Question', 'Button1')
            } finally {
                $S.Busy = $false
            }
            $yes = ($answer -eq 'Yes') -and $S.Linked -and -not $S.AskStale
            Add-Log $(if ($yes) { "you let $($S.Navi) keep going after $steps commands" } else { "you stopped $($S.Navi) after $steps commands" })
            if (-not $yes) { $S.Agent = $false; Add-ChatResult $false "stopped after $steps commands" }
            Send-Pet ([ordered]@{ t = 'agent_more'; yes = $yes })
        }
        'agent_stop' {
            $S.Agent = $false
            Add-Log "$($S.Navi)'s command loop stopped ($([string](Get-Field $msg 'why')))"
        }
        'scan' { Invoke-Scan }
        'net_open' { Open-Tunnel (Get-Field $msg 'id') ([string](Get-Field $msg 'host')) ([int](Get-Field $msg 'port')) }
        'jackout' { Add-Log 'PET pressed JACK OUT'; Jack-Out 'PET' }
        'navi' { Set-Navi ([string](Get-Field $msg 'name')) ([string](Get-Field $msg 'art')) }
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
            if ($text -or -not $cmd) { Add-Chat 'navi' $text }
            if ((Get-Field $msg 'ok') -and $text) { Speak-Alice $text }
            # Her command (if any) arrives next as a 'run' with agent=true; the mode decides.
            $S.Agent = [bool]$cmd
            if ($cmd) { Add-Log "$($S.Navi) step $([string](Get-Field $msg 'step')): $cmd" }
        }
    }
}

# While a Yes/No box is open (or the scan runs) the app can't act on PET
# messages, but it has to keep reading them: an unread socket fills up and the
# PET would end up dropping the link. Mirror frames are applied right away;
# everything else waits, in order, for Step-Link. If the PET takes access back
# or jacks out meanwhile, the open question no longer counts (AskStale).
function Read-LinkBusy {
    if (-not $S.Linked -or $null -eq $S.Stream) { return }
    try {
        if (-not $S.Client.Connected -or ($S.Client.Client.Poll(0, 'SelectRead') -and $S.Client.Available -eq 0)) { $S.AskStale = $true; return }
        if (-not $S.Stream.DataAvailable) { return }
        $bytes = New-Object byte[] 65536
        $chars = New-Object char[] 65536
        if (-not $S.Decoder) { $S.Decoder = [System.Text.Encoding]::UTF8.GetDecoder() }
        $text = New-Object System.Text.StringBuilder
        while ($S.Stream.DataAvailable) {
            $got = $S.Stream.Read($bytes, 0, $bytes.Length)
            if ($got -le 0) { break }
            $count = $S.Decoder.GetChars($bytes, 0, $got, $chars, 0)
            [void]$text.Append($chars, 0, $count)
        }
        $all = $S.Buffer + $text.ToString()
        $keep = New-Object System.Text.StringBuilder
        $at = 0
        while (($nl = $all.IndexOf("`n", $at)) -ge 0) {
            $line = $all.Substring($at, $nl - $at)
            $at = $nl + 1
            if ($line.StartsWith('{"t":"frame"')) {
                try { Apply-Frame ($line | ConvertFrom-Json) } catch { }
                continue
            }
            if ($line -match '^\s*\{"t":"(jackout|drop|bye)"') { $S.AskStale = $true }
            [void]$keep.Append($line).Append("`n")
        }
        $S.Buffer = $keep.ToString() + $all.Substring($at)
        Flush-Frame
    } catch {
        $S.AskStale = $true
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
        # `time`: this PC's clock (the PET has none of its own; it only takes it from a
        # PC it trusts, or when its own is certainly wrong). `trust`: the token from [T].
        Send-Pet ([ordered]@{ t = 'hello'; host = $env:COMPUTERNAME; os = $os; user = $env:USERNAME
                admin = $S.Admin; full = $S.Full; input = [bool]$S.Full; mode = (Get-Mode).ToLower(); app = $AppVersion
                time = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds(); trust = (Get-TrustToken) })
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
        $chars = New-Object char[] 8192
        if (-not $S.Decoder) { $S.Decoder = [System.Text.Encoding]::UTF8.GetDecoder() }
        while ($S.Stream -and $S.Stream.DataAvailable) {
            $n = $S.Stream.Read($bytes, 0, $bytes.Length)
            if ($n -le 0) { break }
            $count = $S.Decoder.GetChars($bytes, 0, $n, $chars, 0)
            $S.Buffer += New-Object System.String($chars, 0, $count)
        }
        while ($S.Stream -and ($i = $S.Buffer.IndexOf("`n")) -ge 0) {
            $line = $S.Buffer.Substring(0, $i).Trim()
            $S.Buffer = $S.Buffer.Substring($i + 1)
            if ($line) {
                # One message the app can't handle is logged, not treated as a lost link.
                try { On-Line $line } catch { Add-Log "skipped a PET message: $($_.Exception.Message)" }
            }
        }
        Flush-Frame
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
    if ($TrustFile) { $argList += @('-TrustFile', "`"$TrustFile`"") }
    try {
        # Windows shows the UAC prompt; you click Yes or No.
        Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -Verb RunAs | Out-Null
    } catch {
        Add-Log 'admin: UAC prompt was cancelled'
        if ($S.Linked) { Send-State 'admin' }
        return
    }
    Add-Log 'admin: handing over to the elevated copy'
    # The elevated copy dials the PET itself; this one leaves quietly, and tells the PET so.
    $S.Closing = $true
    if ($S.Agent) { Send-Pet ([ordered]@{ t = 'abort' }) }
    Send-Pet ([ordered]@{ t = 'bye'; why = 'admin handover' })
    Stop-Jobs
    Stop-Sharing
    $timer.Stop()
    Wait-Stopping
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
    $tips.SetToolTip($btnVoice, $(if ($S.Speak) { "$($S.Navi) reads answers aloud (click to mute)" } else { "$($S.Navi) is muted (click to hear answers)" }))
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
    if ($S.Busy) { Read-LinkBusy; return }
    try {
        $nowMs = $Clock.ElapsedMilliseconds
        # Dial the PET about every 2 s until it answers.
        if ($S.Client -or $S.Connecting -or $nowMs -ge $script:NextDialMs) { $script:NextDialMs = $nowMs + 2000; Step-Link }
        if ($S.Jobs.Count -or $S.Stopping.Count) { Step-Jobs }
        if ($S.Tunnels.Count) { Step-Tunnels }
        if ($EscFilter.Pressed) { $EscFilter.Pressed = $false; Stop-Play }
        if ($Play.On) { Step-Play }
        elseif ($Play.Web -and ($Play.WebError -or ((Get-Date) - $Play.WebIdle).TotalSeconds -gt 60)) { Close-GameView }
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
