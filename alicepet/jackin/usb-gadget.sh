#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# JACK IN: turn the PET's USB-C port into a composite USB device for a PC:
#   - a read-only drive "ALICEPET" holding the Jack In app (jackin.img)
#   - a USB network link: RNDIS for Windows (config 1, with Microsoft OS
#     descriptors so Windows binds its built-in driver), ECM for macOS/Linux
#     (config 2). Both sit on bridge br-jack at 10.55.0.1/30; dnsmasq hands the
#     PC 10.55.0.2 with no gateway and no DNS, so the PC never routes through
#     the PET. The PC app dials 10.55.0.1:7341.
#
#   usb-gadget.sh up      build the gadget and bind it (makes the image if missing
#                         or older than the app folder)
#   usb-gadget.sh down    unbind and remove everything this script created
#   usb-gadget.sh status
#   usb-gadget.sh image   (re)build the drive image from the app folder
#
# Needs root (configfs, ip, dnsmasq). The app runs it through sudo -n; see
# alicepet-jackin.sudoers. The port must be able to act as a device: on the
# CardputerZero that is `dtoverlay=dwc2,dr_mode=peripheral` in config.txt (otg
# stays in host mode there), the cable in USB-C2, and the USB-SW slider on C2.
# While the gadget is up, the USB-A ports and Ethernet behind the hub are off.
set -euo pipefail

G=/sys/kernel/config/usb_gadget/alicepet
APP_DIR=${ALICEPET_JACKIN_APP:-/usr/share/alicepet/jackin/windows}
IMAGE=${ALICEPET_JACKIN_IMAGE:-/var/lib/alicepet/jackin.img}
IMAGE_MB=16
BRIDGE=br-jack
PET_IP=10.55.0.1
PC_IP=10.55.0.2
PIDFILE=/run/alicepet-jackin-dnsmasq.pid
LOCKFILE=/run/alicepet-jackin.lock

say() { echo "usb-gadget: $*" >&2; }
die() { say "$*"; exit 1; }

need_root() { [ "$(id -u)" -eq 0 ] || die "needs root"; }

make_image() {
    command -v mkfs.vfat >/dev/null || die "mkfs.vfat missing (apt install dosfstools)"
    command -v mcopy >/dev/null || die "mcopy missing (apt install mtools)"
    [ -d "$APP_DIR" ] || die "no app folder at $APP_DIR"
    mkdir -p "$(dirname "$IMAGE")"
    local tmp="$IMAGE.new"
    rm -f "$tmp"
    truncate -s "${IMAGE_MB}M" "$tmp"
    mkfs.vfat -n ALICEPET "$tmp" >/dev/null
    # Copy the app folder's contents to the root of the drive.
    MTOOLS_SKIP_CHECK=1 mcopy -s -i "$tmp" "$APP_DIR"/* ::/
    mv "$tmp" "$IMAGE"
    say "image $IMAGE from $APP_DIR"
}

# One up/down/image at a time: the app starts `up` and `down` from separate
# threads, and two of them in the same configfs tree leave a half-built gadget.
LOCKED=0
lock() {
    [ "$LOCKED" -eq 0 ] || return 0
    exec 9>"$LOCKFILE"
    flock -w 20 9 || die "another usb-gadget.sh is still running"
    LOCKED=1
}

# A gadget is only "up" when it is bound to the controller and the bridge is there.
gadget_ready() {
    [ -d "$G" ] && [ -n "$(cat "$G/UDC" 2>/dev/null)" ] && ip link show "$BRIDGE" >/dev/null 2>&1
}

udc_name() { ls /sys/class/udc 2>/dev/null | head -n1 || true; }

UP_UNFINISHED=0
undo_failed_up() {
    local rc=$?
    [ "$UP_UNFINISHED" -eq 1 ] || return 0
    UP_UNFINISHED=0
    set +e
    say "up failed (exit $rc); removing the half-built gadget"
    cd /
    down
    exit "$([ "$rc" -ne 0 ] && echo "$rc" || echo 1)"
}

up() {
    need_root
    lock
    if gadget_ready; then
        say "already up"
        return 0
    fi
    if [ -d "$G" ]; then
        # Left behind by an `up` that died part-way (killed, or the bind failed).
        say "found a half-built gadget; removing it and starting again"
        down
        [ ! -d "$G" ] || die "can't clear $G (reboot the PET to reset the USB port)"
    fi
    # From here any failure (set -e) takes the gadget back down, so a later
    # `up` never mistakes the leftovers for a working link.
    UP_UNFINISHED=1
    trap undo_failed_up EXIT
    local udc
    udc=$(udc_name)
    [ -n "$udc" ] || die "no USB device controller (add dtoverlay=dwc2,dr_mode=peripheral to config.txt and reboot)"
    # A new package puts a newer app in $APP_DIR: rebuild the drive so the PC gets it.
    if [ ! -f "$IMAGE" ] || [ -n "$(find "$APP_DIR" -newer "$IMAGE" -print -quit 2>/dev/null)" ]; then
        make_image
    fi
    modprobe libcomposite 2>/dev/null || true
    mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config

    local serial
    serial=$(tr -d '\0' 2>/dev/null </proc/device-tree/serial-number || cat /etc/machine-id)
    serial=${serial: -12}

    mkdir -p "$G"
    cd "$G"
    echo 0x1d6b >idVendor   # Linux Foundation
    echo 0x0104 >idProduct  # Multifunction Composite Gadget
    echo 0x0100 >bcdDevice
    echo 0x0200 >bcdUSB
    echo 0xEF >bDeviceClass  # misc / IAD, so Windows groups the interfaces
    echo 0x02 >bDeviceSubClass
    echo 0x01 >bDeviceProtocol
    mkdir -p strings/0x409
    echo "$serial" >strings/0x409/serialnumber
    echo "AlicePET" >strings/0x409/manufacturer
    echo "AlicePET Jack In" >strings/0x409/product

    # Microsoft OS descriptors: Windows loads its RNDIS driver with no install.
    echo 1 >os_desc/use
    echo 0xcd >os_desc/b_vendor_code
    echo MSFT100 >os_desc/qw_sign

    mkdir -p functions/rndis.usb0
    echo RNDIS >functions/rndis.usb0/os_desc/interface.rndis/compatible_id
    echo 5162001 >functions/rndis.usb0/os_desc/interface.rndis/sub_compatible_id
    mkdir -p functions/ecm.usb1
    # Fixed MACs per PET, so the PC sees the same adapter every time.
    local h
    h=$(echo -n "$serial" | md5sum | cut -c1-10 | sed 's/\(..\)/\1:/g; s/:$//')
    echo "02:$h" >functions/rndis.usb0/host_addr
    echo "06:$h" >functions/rndis.usb0/dev_addr
    echo "0a:$h" >functions/ecm.usb1/host_addr
    echo "0e:$h" >functions/ecm.usb1/dev_addr

    mkdir -p functions/mass_storage.0
    echo 1 >functions/mass_storage.0/stall
    echo 1 >functions/mass_storage.0/lun.0/removable
    echo 1 >functions/mass_storage.0/lun.0/ro  # the PC can't change the app
    echo "$IMAGE" >functions/mass_storage.0/lun.0/file

    # One configuration only: Windows loads its composite driver (so the
    # drive shows up next to the network link) only for single-config
    # devices. Windows binds RNDIS, macOS/Linux bind ECM, both get the drive.
    mkdir -p configs/c.1/strings/0x409
    echo "RNDIS + ECM + drive" >configs/c.1/strings/0x409/configuration
    echo 250 >configs/c.1/MaxPower
    ln -s functions/rndis.usb0 configs/c.1/
    ln -s functions/ecm.usb1 configs/c.1/
    ln -s functions/mass_storage.0 configs/c.1/
    ln -s configs/c.1 os_desc/

    echo 1 >/sys/class/leds/ext_usb_gpio_fun/brightness 2>/dev/null || true  # USB-C mux -> device
    echo "$udc" >UDC
    cd /

    # One address whichever link the PC picked.
    ip link add "$BRIDGE" type bridge 2>/dev/null || true
    for _ in $(seq 1 20); do
        ip link show usb0 >/dev/null 2>&1 && ip link show usb1 >/dev/null 2>&1 && break
        sleep 0.25
    done
    for ifc in usb0 usb1; do
        ip link set "$ifc" master "$BRIDGE" 2>/dev/null || say "no $ifc"
        ip link set "$ifc" up 2>/dev/null || true
    done
    ip addr replace "$PET_IP/30" dev "$BRIDGE"
    ip link set "$BRIDGE" up

    if command -v dnsmasq >/dev/null; then
        # DHCP only: no DNS (port 0), no router, no DNS server option.
        dnsmasq --conf-file=/dev/null --interface="$BRIDGE" --bind-interfaces --except-interface=lo \
            --port=0 --dhcp-range="$PC_IP,$PC_IP,255.255.255.252,10m" --dhcp-option=3 --dhcp-option=6 \
            --dhcp-authoritative --leasefile-ro --pid-file="$PIDFILE" 9>&-  # don't hand dnsmasq the lock
    else
        say "dnsmasq missing: set the PC's address to $PC_IP/30 by hand (apt install dnsmasq-base)"
    fi
    UP_UNFINISHED=0
    trap - EXIT
    say "up on $udc: drive + network at $PET_IP"
}

down() {
    need_root
    lock
    if [ -f "$PIDFILE" ]; then
        kill "$(cat "$PIDFILE")" 2>/dev/null || true
        rm -f "$PIDFILE"
    fi
    ip link del "$BRIDGE" 2>/dev/null || true
    [ -d "$G" ] || { say "already down"; return 0; }
    cd "$G"
    echo "" >UDC 2>/dev/null || true
    echo 0 >/sys/class/leds/ext_usb_gpio_fun/brightness 2>/dev/null || true  # USB-C mux -> hub
    rm -f os_desc/c.1
    rm -f configs/c.1/rndis.usb0 configs/c.1/ecm.usb1 configs/c.1/mass_storage.0 configs/c.2/ecm.usb1 configs/c.2/mass_storage.0
    rmdir configs/c.1/strings/0x409 configs/c.2/strings/0x409 2>/dev/null || true
    rmdir configs/c.1 configs/c.2 2>/dev/null || true
    rmdir functions/rndis.usb0 functions/ecm.usb1 functions/mass_storage.0 2>/dev/null || true
    rmdir strings/0x409 2>/dev/null || true
    cd /
    rmdir "$G" 2>/dev/null || say "could not remove $G (check with ls)"
    say "down"
}

status() {
    if [ -d "$G" ] && ! gadget_ready; then
        echo "gadget: half-built (run: usb-gadget.sh down)"
    elif [ -d "$G" ]; then
        echo "gadget: up (UDC $(cat "$G/UDC" 2>/dev/null || echo ?))"
        echo "state: $(cat /sys/class/udc/*/state 2>/dev/null | head -n1)"
        ip -br addr show "$BRIDGE" 2>/dev/null || true
    else
        echo "gadget: down"
    fi
    echo "udc: $(udc_name || true)"
    [ -f "$IMAGE" ] && echo "image: $IMAGE" || echo "image: missing"
}

case "${1:-}" in
up) up ;;
down) down ;;
status) status ;;
image) need_root; lock; make_image ;;
*) echo "usage: $0 up|down|status|image" >&2; exit 2 ;;
esac
