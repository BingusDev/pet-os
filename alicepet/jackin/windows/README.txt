AlicePET Jack In
================

Double-click JackIn.cmd. Nothing is installed. The only files it leaves are
small diagnostics in %TEMP% (JackIn-*.txt and JackIn-timing.log).

- It starts READ-ONLY and runs a quick scan (CPU, memory, disk, startup apps,
  Defender, biggest programs). The PET shows the results and Alice's advice.
- Full Access lets the PET run PowerShell commands as you. Every command shows
  in the log. Click it again to go back to read-only.
- Request Admin shows the normal Windows UAC prompt. Nothing gets admin unless
  you click Yes.
- Share PC Internet lets the PET use this PC's internet while it is on.
- Opening a chip (Jellyfin, Retro, LAN Tech, Terminal) from this window needs
  [K] pressed on the PET first. Terminal needs its own [K] every time you
  jack in, because it uses the PET's SSH keys. The window tells you when the
  PET is waiting.
- Chat with Alice: type in the box and press Enter, or hold "Hold to talk"
  (or hold F9) and speak (up to 15 s). Alice reads her answers aloud unless you
  untick that. The mic records only while you hold the button.
  The PET passes it to Alice and her answer shows in the window.
- JACK OUT (or closing the window, or unplugging the PET) ends everything.
