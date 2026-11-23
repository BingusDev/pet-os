JACK IN for macOS and Linux
===========================

Plug the PET's USB-C cable into this computer. The PET shows up as a USB
network adapter and a read-only drive named ALICEPET. Nothing is installed.
The client uses only what the system already has: bash, sed, awk, ps, df and
open (macOS) or xdg-open (Linux).

Start it
--------
macOS: open the ALICEPET drive and double-click JackIn.command. The first time,
       right-click it and choose Open. Terminal opens and connects. If macOS refuses to run it, open Terminal and type
       bash /Volumes/ALICEPET/JackIn.command (drag the file into the window
       to fill in the path), then press Return.

Linux: bash /media/$USER/ALICEPET/JackIn.sh
       (or wherever your desktop mounted the drive).

Keys (type the letter, then Return)
-----------------------------------
  c   say something to Alice (she answers in this window)
  s   scan again (read-only: CPU, memory, disk, login items, top processes)
  h   help
  q   jack out

The PET can also ask Alice to show things, play a movie in your default
player, or open a RomM game in your browser. Those open in the apps you
already have.

Read mode only
--------------
This client always stays in Read mode, like the Windows app does before you
click Full Access. It never sends keys to the PET, never runs the PET's
commands, and never shares this computer's Internet. Requests for those are
answered "not run" or declined on the PET.

What the Windows app does that this client doesn't (gaps)
---------------------------------------------------------
- Full Access (keyboard control of the PET) and Request Admin: Windows only.
- Running PowerShell commands for Alice: Windows only. Alice's command loop
  does not run here.
- Share PC Internet (the PET's network through this computer): not built.
- Microphone and voice to Alice: not built. Type with c instead.
- PET screen mirror: not built.
- Video: no pause, seek or volume from the PET's arrows; the file opens in
  your default player and the PET goes back to its list.
- RomM games: open in your browser straight from the PET's relay address.
  Windows runs a local secure-context proxy so some cores work; this client
  doesn't, so a core that needs HTTPS may not load.
- Scan: Defender status is Windows-only and shows as "?".

Troubleshooting
---------------
- "can't reach the PET": the cable is not in the PET's USB-C port that the
  setup uses, the PET is off, or the PET is in its own menu. Wait a few
  seconds and run again.
- "no 10.55.0.x address": check the network adapter exists (macOS: System
  Settings > Network; Linux: ip -4 addr). It can take a few seconds.
- The drive or launcher won't open: that is the file-permission rule of a
  FAT drive; use the bash command above.
