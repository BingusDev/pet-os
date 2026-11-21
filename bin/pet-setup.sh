#!/usr/bin/env bash
# AlicePET first-boot setup. Run ON the CardputerZero, once, after flashing:
#
#   scp bin/pet-setup.sh alicepet/dist/AlicePET_*.deb <user>@<pet>:~
#   ssh -t <user>@<pet> 'sudo bash ~/pet-setup.sh ~/AlicePET_*.deb'
#
# What it does (each step says what it changed; nothing secret is written):
#   1. installs the AlicePET .deb (if given) and the tools the chips use
#   2. lets the app user read keys and set the backlight (input, video groups)
#   3. installs the JACK IN sudo rule (only usb-gadget.sh up|down|status, as root)
#   4. builds the JACK IN drive image (/var/lib/alicepet/jackin.img)
#   5. starts /etc/alicepet.local.conf for your bridge URL and token (you type them)
#   6. with --usb-otg: adds dtoverlay=dwc2,dr_mode=peripheral so JACK IN can work (reboot after;
#      USB-A and Ethernet stop working, since they need host mode)
#   7. speaker at 16-bit 48 kHz (WirePlumber rule) and the fn volume keys (volkeys.service)
# Then run bin/device-day.sh for the checks.
#
# Options: --user <name> (default: the sudo user), --usb-otg, --no-apt
set -euo pipefail

DEB=""
APP_USER="${SUDO_USER:-}"
USB_OTG=0
APT=1
while [[ $# -gt 0 ]]; do
    case "$1" in
    --user) APP_USER="$2"; shift ;;
    --usb-otg) USB_OTG=1 ;;
    --no-apt) APT=0 ;;
    *.deb) DEB="$1" ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

[[ $(id -u) -eq 0 ]] || { echo "run with sudo" >&2; exit 1; }
# A pattern that matched nothing arrives here unexpanded: say so before changing anything.
[[ -z "$DEB" || -f "$DEB" ]] || { echo "no such .deb: $DEB (the package is named AlicePET_<version>_arm64.deb)" >&2; exit 1; }
[[ -n "$APP_USER" ]] && id "$APP_USER" >/dev/null 2>&1 || { echo "which user runs AlicePET? use --user <name>" >&2; exit 1; }

step() { printf '\n\033[1;36m== %s ==\033[0m\n' "$1"; }
done_() { printf '  \033[32mdone\033[0m %s\n' "$1"; }
note() { printf '  \033[33mnote\033[0m %s\n' "$1"; }

step "1. Packages"
if (( APT )); then
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        alsa-utils openssh-client mpv curl iw lldpd iputils-ping iproute2 dosfstools mtools dnsmasq-base \
        evtest v4l-utils >/dev/null
    done_ "chip tools installed"
    if [[ -n "$DEB" ]]; then
        DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "$(realpath "$DEB")" >/dev/null
        done_ "installed $(basename "$DEB")"
    else
        note "no .deb given; install it later with: sudo apt install ./AlicePET_<version>_arm64.deb"
    fi
    # dnsmasq-base only; make sure no system dnsmasq service grabs port 53 or every interface.
else
    note "skipped (--no-apt)"
fi

step "2. Groups for $APP_USER"
for g in input video audio; do
    if getent group "$g" >/dev/null; then
        usermod -aG "$g" "$APP_USER"
        done_ "$APP_USER is in $g"
    fi
done
note "group changes apply at the next login (or reboot)"

step "3. JACK IN sudo rule"
GADGET=/usr/libexec/alicepet/usb-gadget.sh
RULE=/etc/sudoers.d/alicepet-jackin
TMP_RULE="$(mktemp)"
printf '# AlicePET JACK IN: switch the USB-C port to device mode and back.\n%s ALL=(root) NOPASSWD: %s up, %s down, %s status\n' \
    "$APP_USER" "$GADGET" "$GADGET" "$GADGET" >"$TMP_RULE"
if visudo -cf "$TMP_RULE" >/dev/null; then
    install -m 0440 "$TMP_RULE" "$RULE"
    done_ "$RULE (only usb-gadget.sh up/down/status)"
else
    note "visudo rejected the rule; nothing installed"
fi
rm -f "$TMP_RULE"

step "4. JACK IN drive image"
if [[ -x "$GADGET" ]]; then
    "$GADGET" image && done_ "/var/lib/alicepet/jackin.img"
else
    note "$GADGET missing (install the .deb first, then run: sudo $GADGET image)"
fi

step "5. Your settings"
LOCAL=/etc/alicepet.local.conf
if [[ -f "$LOCAL" ]]; then
    note "$LOCAL already exists; left as is"
else
    url=""
    token=""
    if [[ -t 0 ]]; then
        read -r -p "  Alice bridge URL (e.g. http://mac-mini.tailnet.ts.net:8797, Enter to skip): " url
        if [[ -n "$url" ]]; then read -r -s -p "  Bridge token (hidden, Enter for none): " token; echo; fi
    fi
    umask 077
    {
        echo "# AlicePET settings for this PET only (read after /etc/alicepet.conf). Never commit this file."
        echo "[bridge]"
        echo "url=$url"
        echo "token=$token"
        echo ""
        echo "# [terminal]"
        echo "# host=name|user@address|group"
        echo "# [jellyfin]"
        echo "# url="
        echo "# api_key="
        echo "# [romm]"
        echo "# url="
        echo "# token="
    } >"$LOCAL"
    chown "root:$APP_USER" "$LOCAL"
    chmod 0640 "$LOCAL"
    done_ "$LOCAL (readable by $APP_USER only)"
fi

step "6. USB device mode (JACK IN)"
CONFIG=""
for c in /boot/firmware/config.txt /boot/config.txt; do [[ -f "$c" ]] && { CONFIG="$c"; break; }; done
if [[ -z "$CONFIG" ]]; then
    note "no config.txt found"
else
    # Only a dwc2 line that applies to every board ([all], or before any [section])
    # and forces device mode counts. On the CardputerZero the CM0's USB runs
    # through the board's USB-SW switch, so the controller never sees an ID pin
    # change: plain `dtoverlay=dwc2` and `dr_mode=otg` both stay in host mode and
    # JACK IN never attaches. The stock image's plain line under [all] is
    # therefore not enough, and `dr_mode=host` under [cm4]/[cm5] doesn't count.
    DWC2_LINES="$(awk '
        /^[[:space:]]*\[/ { sec = $0; gsub(/[[:space:]]/, "", sec); next }
        /^[[:space:]]*dtoverlay=dwc2([,[:space:]]|$)/ {
            line = $0; sub(/^[[:space:]]+/, "", line); sub(/[[:space:]]+$/, "", line)
            print ((sec == "" || sec == "[all]") ? "all" : sec) "\t" line
        }' "$CONFIG")"
    # The last line for every board is the one that takes effect.
    LAST_LINE="$(awk -F'\t' '$1 == "all" { print $2 }' <<<"$DWC2_LINES" | tail -n 1)"
    DEVICE_LINE=""
    HOST_LINE=""
    case "$LAST_LINE" in
    "") ;;
    *dr_mode=peripheral*) DEVICE_LINE="$LAST_LINE" ;;
    *) HOST_LINE="$LAST_LINE" ;;
    esac
    OTHER_LINES="$(awk -F'\t' '$1 != "all" && $1 != "" { print $2 " (under " $1 ")" }' <<<"$DWC2_LINES")"
    if [[ -n "$DEVICE_LINE" ]]; then
        done_ "device mode already set: $DEVICE_LINE"
    elif (( USB_OTG )); then
        [[ -f "$CONFIG.alicepet-bak" ]] || cp "$CONFIG" "$CONFIG.alicepet-bak"  # keep the first, untouched copy
        # [all] first, so the line applies whatever section the file ends in.
        printf '\n# AlicePET JACK IN: USB-C2 acts as a device (added by pet-setup.sh)\n[all]\ndtoverlay=dwc2,dr_mode=peripheral\n' >>"$CONFIG"
        done_ "added [all] dtoverlay=dwc2,dr_mode=peripheral to $CONFIG (backup: $CONFIG.alicepet-bak). Reboot to apply."
        [[ -z "$HOST_LINE" ]] || note "the new line follows '$HOST_LINE'; if JACK IN finds no USB device controller after the reboot, comment that older line out"
        note "USB-A, USB-C1 and Ethernet are host-only and stop working; restore the backup to get them back"
        note "to JACK IN: cable in USB-C2 (not C1 beside USB-A) and the USB-SW slider set to C2"
    else
        note "not changed. JACK IN needs dtoverlay=dwc2,dr_mode=peripheral under [all]; rerun with --usb-otg to add it (config.txt is backed up first)"
        [[ -z "$HOST_LINE" ]] || note "found for every board, not device-only: $HOST_LINE"
    fi
    if [[ -n "$OTHER_LINES" ]]; then
        while IFS= read -r l; do note "ignored, other boards only: $l"; done <<<"$OTHER_LINES"
    fi
fi

step "7. Speaker audio and volume keys"
# The ES8389 plays silence at PipeWire's default s32le; force S16LE/48 kHz on the speaker sink.
APP_HOME=$(getent passwd "$APP_USER" | cut -d: -f6)
APP_UID=$(id -u "$APP_USER")
WP_DIR="$APP_HOME/.config/wireplumber/wireplumber.conf.d"
install -d -o "$APP_USER" -g "$APP_USER" "$APP_HOME/.config" "$APP_HOME/.config/wireplumber" "$WP_DIR"
cat > "$WP_DIR/60-es8389-s16.conf" <<'WPEOF'
monitor.alsa.rules = [
  { matches = [ { node.name = "~alsa_output.platform-sound.*" } ]
    actions = { update-props = { audio.format = "S16LE", audio.rate = 48000, api.alsa.period-size = 1024, session.suspend-timeout-seconds = 0 } } }
]
WPEOF
chown "$APP_USER:$APP_USER" "$WP_DIR/60-es8389-s16.conf"
done_ "$WP_DIR/60-es8389-s16.conf (speaker at 16-bit 48 kHz)"
# fn+D / fn+S / fn+A -> PipeWire volume, and the state file AlicePET's volume bar reads.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$HERE/volkeys.py" ]]; then
    install -D -o "$APP_USER" -g "$APP_USER" -m 755 "$HERE/volkeys.py" "$APP_HOME/.local/bin/volkeys.py"
    install -d -o "$APP_USER" -g "$APP_USER" "$APP_HOME/.config/systemd" "$APP_HOME/.config/systemd/user"
    cat > "$APP_HOME/.config/systemd/user/volkeys.service" <<'SVCEOF'
[Unit]
Description=CardputerZero volume keys -> PipeWire
After=pipewire.service wireplumber.service
[Service]
ExecStart=%h/.local/bin/volkeys.py
Restart=always
[Install]
WantedBy=default.target
SVCEOF
    chown "$APP_USER:$APP_USER" "$APP_HOME/.config/systemd/user/volkeys.service"
    loginctl enable-linger "$APP_USER" >/dev/null 2>&1 || true
    runuser -u "$APP_USER" -- env XDG_RUNTIME_DIR="/run/user/$APP_UID" systemctl --user daemon-reload >/dev/null 2>&1 || true
    runuser -u "$APP_USER" -- env XDG_RUNTIME_DIR="/run/user/$APP_UID" systemctl --user enable --now volkeys.service >/dev/null 2>&1 \
        && done_ "volkeys.service (fn+D up, fn+S down, fn+A mute)" \
        || note "volkeys.service installed; it starts at the next login"
    runuser -u "$APP_USER" -- env XDG_RUNTIME_DIR="/run/user/$APP_UID" systemctl --user restart wireplumber pipewire pipewire-pulse >/dev/null 2>&1 || true
else
    note "volkeys.py not next to this script; copy bin/volkeys.py here and rerun for the volume keys"
fi

printf '\n\033[1mSetup done.\033[0m Next: sudo bash device-day.sh\n'
