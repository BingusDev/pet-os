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
#   6. with --usb-otg: adds dtoverlay=dwc2,dr_mode=otg so JACK IN can work (reboot after)
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
    # and leaves the port able to act as a device counts. Stock images carry
    # `dtoverlay=dwc2,dr_mode=host` under [cm4]/[cm5], which is neither.
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
    *dr_mode=host*) HOST_LINE="$LAST_LINE" ;;
    *) DEVICE_LINE="$LAST_LINE" ;;
    esac
    OTHER_LINES="$(awk -F'\t' '$1 != "all" && $1 != "" { print $2 " (under " $1 ")" }' <<<"$DWC2_LINES")"
    if [[ -n "$DEVICE_LINE" ]]; then
        done_ "device mode already set: $DEVICE_LINE"
    elif (( USB_OTG )); then
        [[ -f "$CONFIG.alicepet-bak" ]] || cp "$CONFIG" "$CONFIG.alicepet-bak"  # keep the first, untouched copy
        # [all] first, so the line applies whatever section the file ends in.
        printf '\n# AlicePET JACK IN: USB-C can act as a device (added by pet-setup.sh)\n[all]\ndtoverlay=dwc2,dr_mode=otg\n' >>"$CONFIG"
        done_ "added [all] dtoverlay=dwc2,dr_mode=otg to $CONFIG (backup: $CONFIG.alicepet-bak). Reboot to apply."
        [[ -z "$HOST_LINE" ]] || note "the new line follows '$HOST_LINE'; if JACK IN finds no USB device controller after the reboot, comment that older line out"
        note "if USB-A or Ethernet stop working after the reboot, restore the backup"
    else
        note "not changed. JACK IN needs dtoverlay=dwc2,dr_mode=otg under [all]; rerun with --usb-otg to add it (config.txt is backed up first)"
        [[ -z "$HOST_LINE" ]] || note "found for every board, host mode only: $HOST_LINE"
    fi
    if [[ -n "$OTHER_LINES" ]]; then
        while IFS= read -r l; do note "ignored, other boards only: $l"; done <<<"$OTHER_LINES"
    fi
fi

printf '\n\033[1mSetup done.\033[0m Next: sudo bash device-day.sh\n'
