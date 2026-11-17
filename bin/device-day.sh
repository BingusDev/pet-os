#!/usr/bin/env bash
# AlicePET device-day checks (BUILD-PLAN Phase 0). Run ON the CardputerZero:
#
#   scp bin/device-day.sh <user>@<pet>:~ && ssh -t <user>@<pet> 'sudo bash ~/device-day.sh'
#
# Writes a Markdown report (default ~/device-day-<date>.md, or $REPORT) with a
# PASS / FAIL / CHECK line and the raw evidence for every item, ready to paste
# into NOTES.md. Interactive steps (press keys, listen, point a remote) ask
# first; `--auto` skips them. Nothing is installed and every setting it changes
# (backlight, CPU cores, CPU clock, Wi-Fi power save) is put back at the end.
#
# Tools it uses (and the app needs), if the image lacks them:
#   sudo apt install alsa-utils openssh-client mpv curl v4l-utils iw evtest lldpd retroarch
set -uo pipefail

AUTO=0
[[ "${1:-}" == "--auto" ]] && AUTO=1
HOME_DIR="${SUDO_USER:+$(getent passwd "$SUDO_USER" | cut -d: -f6)}"
HOME_DIR="${HOME_DIR:-$HOME}"
REPORT="${REPORT:-$HOME_DIR/device-day-$(date +%Y-%m-%d).md}"
IS_ROOT=0
[[ $(id -u) -eq 0 ]] && IS_ROOT=1
TMP="$(mktemp -d)"
declare -a RESTORE=()
SUMMARY=""
ps_state=""
BL=""
WLAN=""

cleanup() {
    local i
    for (( i=${#RESTORE[@]}-1; i>=0; i-- )); do eval "${RESTORE[$i]}" 2>/dev/null || true; done
    rm -rf "$TMP"
}
trap cleanup EXIT

# --- report helpers ----------------------------------------------------------

out() { printf '%s\n' "$*" >> "$REPORT"; }
section() { printf '\n\033[1;36m== %s ==\033[0m\n' "$1"; out ""; out "## $1"; }
result() {  # result PASS|FAIL|CHECK "item" "detail"
    local mark="$1" item="$2" detail="${3:-}"
    local color=32; [[ "$mark" == FAIL ]] && color=31; [[ "$mark" == CHECK ]] && color=33
    printf '  \033[%sm%-5s\033[0m %s %s\n' "$color" "$mark" "$item" "${detail:+($detail)}"
    out "- **$mark** $item${detail:+: $detail}"
    SUMMARY+="| $item | $mark | ${detail//|/\\|} |"$'\n'
}
evidence() {  # evidence "title" command...
    local title="$1"; shift
    local text
    text="$("$@" 2>&1 | head -n 60)"
    out ""; out "<details><summary>$title</summary>"; out ""; out '```'; out "$text"; out '```'; out "</details>"
}
have() { command -v "$1" >/dev/null 2>&1; }
ask() {  # ask "question" -> 0 yes, 1 no, 2 skipped
    (( AUTO )) && return 2
    local reply
    read -r -p "  $1 [y/n/s=skip] " reply </dev/tty || return 2
    case "$reply" in y|Y) return 0 ;; s|S|"") return 2 ;; *) return 1 ;; esac
}
judge() {  # judge "item" "question" "detail" -> records PASS / FAIL / CHECK from the answer
    ask "$2"
    case $? in 0) result PASS "$1" "$3" ;; 1) result FAIL "$1" "$3" ;; *) result CHECK "$1" "$3 (not confirmed)" ;; esac
}
readf() { cat "$1" 2>/dev/null | head -n 1; }
writef() {  # writef file value: write as root, remembering the old value to restore
    local old; old="$(readf "$1")"
    [[ -n "$old" ]] && RESTORE+=("echo '$old' > '$1'")
    echo "$2" > "$1" 2>/dev/null
}

: > "$REPORT"
out "# AlicePET device day: $(hostname) $(date '+%Y-%m-%d %H:%M')"
out ""
out "Run as $(id -un)$( (( IS_ROOT )) && echo ' (root)')$( (( AUTO )) && echo ', --auto (interactive checks skipped)')."
(( IS_ROOT )) || echo "Not root: backlight, CPU and power checks will only read. Re-run with sudo for the full set."

# --- 1. system ---------------------------------------------------------------
section "System"
result CHECK "OS" "$(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}") · kernel $(uname -r) · $(uname -m)"
result CHECK "CPU / RAM" "$(nproc) cores · $(awk '/MemTotal/ {printf "%d MB", $2/1024}' /proc/meminfo)"
evidence "cpuinfo / model" sh -c 'grep -m1 -i model /proc/cpuinfo; cat /proc/device-tree/model 2>/dev/null; echo'
APPLAUNCH_USER="$(ps -eo user,comm | awk 'tolower($2) ~ /applaunch/ {print $1; exit}')"
if [[ -n "$APPLAUNCH_USER" ]]; then
    groups_of="$(id -nG "$APPLAUNCH_USER" 2>/dev/null)"
    result CHECK "APPLaunch runs as" "$APPLAUNCH_USER (groups: $groups_of)"
    [[ " $groups_of " == *" input "* || "$APPLAUNCH_USER" == root ]] && result PASS "App user can read /dev/input" \
        || result FAIL "App user can read /dev/input" "add $APPLAUNCH_USER to group input"
else
    result CHECK "APPLaunch runs as" "APPLaunch not running; check which user starts apps"
fi

# --- 2. display --------------------------------------------------------------
section "Display"
if [[ -e /dev/fb0 ]]; then
    result PASS "Framebuffer /dev/fb0" "$(readf /sys/class/graphics/fb0/name) · $(readf /sys/class/graphics/fb0/virtual_size) · $(readf /sys/class/graphics/fb0/bits_per_pixel) bpp"
    bpp="$(readf /sys/class/graphics/fb0/bits_per_pixel)"
    [[ "$bpp" == 32 ]] || result CHECK "Panel colour depth" "$bpp bpp; LVGL is set to 32 (lv_conf_cm0.h LV_COLOR_DEPTH may need $bpp)"
else
    result FAIL "Framebuffer /dev/fb0" "missing; check APP_FRAMEBUFFER_DEVICE / APP_USE_DRM"
fi
evidence "DRM connectors (mpv --vo=drm picks the first card)" sh -c 'for c in /sys/class/drm/card*-*; do [ -e "$c/status" ] && echo "$(basename "$c"): $(cat "$c/status")"; done; ls -l /dev/dri 2>/dev/null'

# --- 3. backlight ------------------------------------------------------------
section "Backlight"
BL="$(ls -d /sys/class/backlight/* 2>/dev/null | head -n 1)"
if [[ -n "$BL" ]]; then
    max="$(readf "$BL/max_brightness")"; cur="$(readf "$BL/brightness")"
    result PASS "Backlight node" "$BL · brightness $cur of $max ([power] backlight=auto finds it)"
    if (( IS_ROOT )) && [[ -n "$max" ]]; then
        dim=$(( max * 15 / 100 )); (( dim < 1 )) && dim=1
        writef "$BL/brightness" "$dim"
        echo "  Screen dimmed to 15% ($dim/$max) for 4 seconds..."
        sleep 4
        echo "$cur" > "$BL/brightness"
        judge "Dim to 15% is readable" "Was the screen still readable at 15%?" "dim_percent=15 → $dim/$max"
    fi
else
    result FAIL "Backlight node" "nothing in /sys/class/backlight; dim and nap can't switch it"
fi

# --- 4. keyboard -------------------------------------------------------------
section "Keyboard"
evidence "Input devices" grep -E '^(N:|H:)' /proc/bus/input/devices
KBD=""
for ev in /dev/input/event*; do
    name="$(readf "/sys/class/input/$(basename "$ev")/device/name")"
    if grep -A5 "Name=\"$name\"" /proc/bus/input/devices 2>/dev/null | grep -q 'EV=1[0-9a-f]*3'; then KBD="$ev"; break; fi
done
[[ -z "$KBD" ]] && KBD="$(ls /dev/input/event* 2>/dev/null | head -n 1)"
if [[ -n "$KBD" ]]; then
    result CHECK "Keyboard device" "$KBD ($(readf "/sys/class/input/$(basename "$KBD")/device/name"))"
    if have evtest && ! (( AUTO )); then
        echo "  For 20 s press: ESC, ENTER, BACKSPACE, TAB, SPACE, the arrows (Fn+;,./?), Fn+A, Ctrl+C, Shift+1."
        timeout 20 evtest "$KBD" > "$TMP/evtest.txt" 2>&1 </dev/null
        keys="$(grep -o 'code [0-9]* (KEY_[A-Z0-9_]*), value 1' "$TMP/evtest.txt" | sed 's/.*(\(KEY_[A-Z0-9_]*\)).*/\1/' | awk '!seen[$0]++' | tr '\n' ' ')"
        result CHECK "Key codes seen" "${keys:-none} (compare with src/platform/linux_input.cpp)"
        evidence "evtest output" cat "$TMP/evtest.txt"
    else
        result CHECK "Key codes" "evtest not installed or --auto (apt install evtest)"
    fi
else
    result FAIL "Keyboard device" "no /dev/input/event*"
fi

# --- 5. audio ----------------------------------------------------------------
section "Audio"
evidence "Playback devices (aplay -l)" aplay -l
evidence "Capture devices (arecord -l)" arecord -l
if have arecord && have aplay && ! (( AUTO )); then
    echo "  Recording 3 s from the mic: say something..."
    arecord -q -D default -f S16_LE -r 16000 -c 1 -d 3 "$TMP/mic.wav" 2>"$TMP/arecord.err"
    if [[ -s "$TMP/mic.wav" ]]; then
        echo "  Playing it back..."
        aplay -q "$TMP/mic.wav" 2>/dev/null
        judge "Mic and speaker" "Did you hear your recording?" "arecord -D default 16 kHz mono → aplay"
    else
        result FAIL "Mic" "arecord failed: $(head -c 200 "$TMP/arecord.err")"
    fi
else
    result CHECK "Mic and speaker" "skipped (needs alsa-utils and an interactive run)"
fi

# --- 6. IR -------------------------------------------------------------------
section "IR"
if ls /dev/lirc* >/dev/null 2>&1; then
    if have ir-ctl; then
        for dev in /dev/lirc*; do evidence "ir-ctl -f -d $dev" ir-ctl -f -d "$dev"; done
        RX="$(for d in /dev/lirc*; do ir-ctl -f -d "$d" 2>/dev/null | grep -qi 'receive' && echo "$d"; done | head -n 1)"
        TX="$(for d in /dev/lirc*; do ir-ctl -f -d "$d" 2>/dev/null | grep -qi 'send' && echo "$d"; done | head -n 1)"
        result CHECK "IR devices" "send=${TX:-none} recv=${RX:-none} → [remote] send_device / recv_device"
        if [[ -n "$RX" ]] && ! (( AUTO )); then
            echo "  Point any TV remote at the PET and press a button within 15 s..."
            timeout 15 ir-ctl -d "$RX" --receive --one-shot > "$TMP/ir.txt" 2>&1
            edges="$(grep -o '[+-][0-9]*' "$TMP/ir.txt" | wc -l)"
            (( edges > 10 )) && result PASS "IR receive" "$edges edges captured" || result FAIL "IR receive" "nothing captured"
            if (( edges > 10 )) && [[ -n "$TX" ]]; then
                grep -o '[+-][0-9]*' "$TMP/ir.txt" | sed 's/^+/pulse /; s/^-/space /' > "$TMP/ir.send"
                ir-ctl -d "$TX" --send="$TMP/ir.send" 2>"$TMP/irsend.err" \
                    && judge "IR send (replay)" "Aim the PET's top edge at that device. Did it react?" "replayed the captured code" \
                    || result FAIL "IR send" "$(head -c 200 "$TMP/irsend.err")"
            fi
        fi
    else
        result FAIL "IR tools" "ir-ctl missing (apt install v4l-utils)"
    fi
else
    result FAIL "IR devices" "no /dev/lirc*; IR overlay not enabled?"
fi

# --- 7. network --------------------------------------------------------------
section "Network"
evidence "Interfaces (ip -br addr)" ip -br addr
WLAN="$(for n in /sys/class/net/*; do [[ -d "$n/wireless" ]] && basename "$n"; done | head -n 1)"
if [[ -n "$WLAN" ]]; then
    ps_state="$(iw dev "$WLAN" get power_save 2>/dev/null | awk '{print $NF}')"
    result PASS "Wi-Fi" "$WLAN · power save ${ps_state:-unknown}"
    if (( IS_ROOT )) && have iw; then
        iw dev "$WLAN" set power_save on 2>/dev/null && iw dev "$WLAN" set power_save "${ps_state:-off}" 2>/dev/null \
            && result PASS "Wi-Fi power save switch" "iw set power_save works (power profiles use it)" \
            || result FAIL "Wi-Fi power save switch" "iw can't change it on this driver"
    fi
else
    result FAIL "Wi-Fi" "no wireless interface"
fi
ETH="$(for n in /sys/class/net/*; do [[ -e "$n/device" && ! -d "$n/wireless" ]] && basename "$n"; done | head -n 1)"
[[ -n "$ETH" ]] && result CHECK "Ethernet" "$ETH · carrier $(readf "/sys/class/net/$ETH/carrier") · speed $(readf "/sys/class/net/$ETH/speed") Mb/s" \
    || result CHECK "Ethernet" "no wired interface (USB hub off?)"
have tailscale && result CHECK "Tailscale" "$(tailscale status --self 2>/dev/null | head -n 1)" || result FAIL "Tailscale" "not installed"
have lldpctl && result CHECK "LLDP (LAN TECH switch port)" "lldpd installed" || result CHECK "LLDP (LAN TECH switch port)" "apt install lldpd for switch-port names"

# --- 8. USB ------------------------------------------------------------------
section "USB"
evidence "lsusb -t" lsusb -t
if ls /sys/class/udc/* >/dev/null 2>&1; then
    result PASS "USB device mode controller" "$(ls /sys/class/udc | tr '\n' ' ') (JACK IN gadget can bind here)"
else
    result CHECK "USB device mode controller" "no /sys/class/udc; dwc2 overlay in dr_mode=peripheral/otg needed for JACK IN"
fi
evidence "Hub power control candidates (gpio / usb power)" sh -c 'ls /sys/bus/usb/devices/*/power/control 2>/dev/null | head; cat /sys/kernel/debug/gpio 2>/dev/null | grep -i -E "usb|hub|eth|lan" | head'

# --- 9. power ----------------------------------------------------------------
section "Power"
BAT="$(for p in /sys/class/power_supply/*; do [[ "$(readf "$p/type")" == Battery ]] && echo "$p"; done | head -n 1)"
sample_ma() {  # average current over $1 seconds (mA, absolute)
    local n=0 sum=0 v
    for (( i=0; i<$1; i++ )); do
        v="$(readf "$BAT/current_now")"; [[ -n "$v" ]] && { sum=$(( sum + ${v#-} )); n=$(( n + 1 )); }
        sleep 1
    done
    (( n > 0 )) && echo $(( sum / n / 1000 )) || echo ""
}
if [[ -n "$BAT" ]]; then
    result PASS "Fuel gauge" "$BAT · $(readf "$BAT/capacity")% · $(readf "$BAT/status") · $(( $(v="$(readf "$BAT/voltage_now")"; echo "${v:-0}") / 1000 )) mV"
    if [[ -n "$(readf "$BAT/current_now")" ]]; then
        [[ "$(readf "$BAT/status")" == Charging ]] && echo "  Unplug USB power for accurate numbers."
        base="$(sample_ma 8)"; base="${base:-0}"; result CHECK "Draw: idle, screen on" "${base} mA"
        if (( IS_ROOT )) && [[ -n "$BL" ]]; then
            writef "$BL/brightness" "$(( $(readf "$BL/max_brightness") * 15 / 100 ))"
            d="$(sample_ma 8)"; d="${d:-0}"; result CHECK "Draw: backlight 15%" "${d} mA ($(( base - d )) mA saved)"
            writef "$BL/brightness" 0
            o="$(sample_ma 8)"; o="${o:-0}"; result CHECK "Draw: backlight off" "${o} mA ($(( base - o )) mA saved)"
            eval "${RESTORE[-1]}"; eval "${RESTORE[-2]}"
        fi
        if (( IS_ROOT )) && [[ -e /sys/devices/system/cpu/cpu1/online ]]; then
            for c in 1 2 3; do [[ -e /sys/devices/system/cpu/cpu$c/online ]] && writef /sys/devices/system/cpu/cpu$c/online 0; done
            pol=/sys/devices/system/cpu/cpufreq/policy0
            [[ -e $pol/scaling_max_freq ]] && writef $pol/scaling_max_freq "$(readf $pol/cpuinfo_min_freq)"
            [[ -n "$WLAN" ]] && have iw && iw dev "$WLAN" set power_save on 2>/dev/null
            n="$(sample_ma 8)"; n="${n:-0}"; result CHECK "Draw: nap profile (1 core, min clock, Wi-Fi power save, screen on)" "${n} mA ($(( base - n )) mA saved)"
            [[ -n "$WLAN" ]] && have iw && iw dev "$WLAN" set power_save "${ps_state:-off}" 2>/dev/null
        fi
    else
        result CHECK "Draw" "fuel gauge has no current_now"
    fi
else
    result FAIL "Fuel gauge" "no battery in /sys/class/power_supply (BQ27220 driver?)"
fi
evidence "CPU frequency" sh -c 'p=/sys/devices/system/cpu/cpufreq/policy0; for f in scaling_governor cpuinfo_min_freq cpuinfo_max_freq scaling_available_frequencies; do echo "$f: $(cat $p/$f 2>/dev/null)"; done'
evidence "rfkill" sh -c 'for r in /sys/class/rfkill/rfkill*; do echo "$(basename $r) $(cat $r/type) soft=$(cat $r/soft)"; done'
evidence "LEDs" ls /sys/class/leds
(( IS_ROOT )) && result PASS "Power profile writes" "root can write cpu online / cpufreq / rfkill: profiles=on will work when the app runs as root" \
    || result CHECK "Power profile writes" "needs root or udev rules for the app's user"

# --- 10. AlicePET dependencies ----------------------------------------------
section "AlicePET dependencies"
for tool in arecord aplay ssh curl mpv ir-ctl retroarch iw evtest; do
    have "$tool" && result PASS "$tool" "$(command -v "$tool")" || result FAIL "$tool" "not installed"
done
cores=0
for f in /usr/lib/libretro/*_libretro.so /usr/lib/*-linux-gnu/libretro/*_libretro.so; do [[ -e "$f" ]] && cores=$(( cores + 1 )); done
result CHECK "libretro cores" "$cores found; set [retro] core_<platform>= to their paths"
evidence "libretro cores" sh -c 'ls -1 /usr/lib/libretro /usr/lib/*-linux-gnu/libretro 2>/dev/null'
if have mpv && [[ -n "$(ls /sys/class/drm/card*-*/status 2>/dev/null)" ]]; then
    result CHECK "mpv on the panel" "try: mpv --vo=drm --drm-device=/dev/dri/cardN <video>, and --hwdec=v4l2m2m-copy"
fi

# --- summary -----------------------------------------------------------------
out ""
out "## Summary"
out ""
out "| Item | Result | Detail |"
out "| --- | --- | --- |"
printf '%s' "$SUMMARY" >> "$REPORT"
[[ -n "${SUDO_USER:-}" ]] && chown "$SUDO_USER" "$REPORT" 2>/dev/null
printf '\n\033[1mReport: %s\033[0m (all settings were put back)\n' "$REPORT"
