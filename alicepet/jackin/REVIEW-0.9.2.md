# Jack In 0.9.2 / PET rev15: performance and safety review

Measured 2026-11-22 on the gaming PC (Windows 11, Windows PowerShell 5.1) and the
CardputerZero PET (2 to 4 cores, 352 MB RAM).

## Measured

PC: `JackIn.ps1` started in the logged-in session (scheduled task /IT), sampled
for 30 s with `Get-Process` (TotalProcessorTime delta, WorkingSet64 every second).
"Idle" means not linked (`-Pet 127.0.0.9`). "Playing" means a scripted stand-in
PET on 127.0.0.1 sends one `play` for a local 90 s 720p H.264 clip.

| PC (one-core %, working set) | 0.9.1          | 0.9.2          |
|------------------------------|----------------|----------------|
| idle, not linked             | 0.7-2.4 %, 190-198 MB | 1.6 %, 145 MB |
| playing a video              | see commit     | see commit     |

The PC side was never expensive: under 3 % of one core idle. The noise between
runs (0.7 to 2.4 %) is larger than the change.

PET: `alicepet` used **~95 % of one core** whenever the screen was on or dimmed,
including the Jack-blanked state (linked, panel off). It dropped to ~0 % only
when napping. Cause: the fbdev/DRM drivers give LVGL a tick but no delay
callback, so `lv_delay_ms()` in the main loop is LVGL's busy-wait loop. That
is the largest cost in the whole system (battery, heat, and it starves the relay
and apt on a 2-core profile).

## Fixed (no behaviour change)

PET (rev15)
- `lv_delay_set_cb()` with a real sleep on the device: the main loop sleeps
  between frames instead of spinning.

PC (0.9.2)
- STOP button: "■ STOP" (Esc in the tooltip), 110 px, so it fits at any scaling.
- UI timer: 4 Hz instead of 10 Hz while nothing is linked or playing; job,
  tunnel and player steps only run when there is something to step; dialing
  and stats are time-based instead of tick-count based.
- PC STATUS refresh skipped while the window is minimized or hidden;
  Defender's CIM query every ~5 min instead of every 30 s.
- Mirror: one bitmap per frame size, refilled in place, instead of a new
  32-bit bitmap per frame (no GC churn at 12 fps). The mirror was already off
  while minimized or while a video plays.
- Windows Media Player control is disposed when a video stops (tens of MB);
  the next video creates a fresh one in the visible panel (0.9.1 rule).

## Security

Good
- PET control port, media relay and web relay bind to `jack_listen`
  (10.55.0.1, the br-jack /30), not 0.0.0.0. The internet-sharing proxy binds
  127.0.0.1 on the PET.
- Relay URLs carry 128-bit random tokens; upstream credentials (Jellyfin and
  RomM headers) stay on the PET and are added by the relay. The PC never sees
  them. (0.9.3 added a direct Jellyfin URL carrying a per-play API key, which is
  an administrator key; since 0.9.4 that is off unless `[jellyfin] pc_direct=on`.) The relay strips Authorization/Cookie from the PC side and never passes
  its own gate cookie upstream.
- The PC's loopback forwarder (RomM secure context) listens on 127.0.0.1 only.
- READ mode is an allowlist parsed with the PowerShell AST; FULL mode still
  asks before anything destructive.

Fixed in 0.9.2
- READ-mode bypasses: `gps x | % Kill` (ForEach-Object member form),
  `% $name`, property assignment `(Get-Item x).Attributes = ...`, and
  `Test-ComputerSecureChannel -Repair` were classed read-only. Now refused;
  covered in `test/TestPolicy.ps1` (75 checks pass).
- Loopback forwarder: the PET could point it at any host; now only at the PET
  itself. It is also closed when the link drops, not only on Jack Out.
- Internet sharing: the PET could open tunnels to the PC's own loopback
  services (`localhost`, 127.x, ::1). Now refused.

Open (not changed)
- Relay file tokens are reusable until the relay stops (needed for range
  requests and seeking), not one-time. Up to 65 are kept.
- The PET relays start one thread per connection with no cap; anything on
  br-jack (only the PC) could exhaust threads. A cap of ~32 would be cheap.
- READ mode can still read any file the Windows user can (`gc`, `gci`) and
  send it to the PET. That is the meaning of "read", but it includes browser
  profiles and SSH keys. A path denylist (AppData\*\User Data, .ssh, *.kdbx)
  would be a reasonable next step.
- Token comparisons are not constant-time (128-bit tokens on a /30 link;
  negligible).

## Architecture

PowerShell + WinForms + inline C# works and is cheap at runtime; the costs
are startup (Add-Type compiles the C# on every launch) and a 150-200 MB
working set from the PowerShell host. The real problems are maintainability
(2000 lines, StrictMode traps, case-insensitive variables) and that the
command policy relies on parsing PowerShell, which is hard to make airtight.

- Small compiled C# exe (WinForms, same protocol): ~30-50 MB, instant start,
  real types and tests, same UI. It needs a signed binary or SmartScreen
  will warn, and it adds a build step. Recommended if Jack In keeps growing.
- Browser UI served by the PET: no install on the PC. But it cannot run
  commands, play video in-process, or open loopback forwards without a native
  helper, so it does not replace the PC side, it adds a second one.
- Keep PowerShell, but move the helpers (frame decode, forwarder, policy)
  into one precompiled DLL shipped on the drive: smaller step, keeps
  "double-click JackIn.cmd".

Recommendation: keep 0.9.x as is, and plan a compiled C# app (option 1) for
1.0, keeping the line protocol unchanged so the PET does not change.
