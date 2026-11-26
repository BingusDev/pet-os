PET-os Jack In
================

Double-click JackIn.cmd. Nothing is installed. The only files it leaves are
small diagnostics in %TEMP% (JackIn-*.txt and JackIn-timing.log) and, once
you play a Retro game, its web cache (%TEMP%\JackIn-WebView2).

- It starts READ-ONLY and runs a quick scan (CPU, memory, disk, startup apps,
  Defender, biggest programs). The PET shows the results and Alice's advice.
- Full Access lets the PET run PowerShell commands as you. Every command shows
  in the log. Click it again to go back to read-only.
- Request Admin shows the normal Windows UAC prompt. Nothing gets admin unless
  you click Yes.
- Share PC Internet lets the PET use this PC's internet while it is on.
- Chips (Jellyfin, Retro, LAN Tech, Terminal) and chat with Alice work from
  this window as soon as it is linked. [K] on the PET takes that away for the
  session. If the PET is set to ask first (ask_on_pet=on in its settings), the
  window tells you when it is waiting for [K].
- [T] twice on the PET while linked trusts this PC (for your Windows account
  only): its clock may then set the PET's. [T] twice again forgets it.
- When you jack out, the PET saves a short text report of the visit (the scan,
  what Alice said, every command that ran).
- Chat with Alice: type in the box and press Enter, or hold "Hold to talk"
  (or hold F9) and speak (up to 15 s). Alice reads her answers aloud unless you
  untick that. The mic records only while you hold the button.
  The PET passes it to Alice and her answer shows in the window.
- Jellyfin videos and Retro games you pick play inside this window. Retro
  uses RomM's web player in WebView2 (Microsoft Edge's engine, already on
  Windows 10/11; the webview2 folder has its files and license). Shift+Esc or
  STOP comes back. Without WebView2 the game opens in your browser instead.
- JACK OUT (or closing the window, or unplugging the PET) ends everything.
