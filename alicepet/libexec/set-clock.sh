#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Sets the PET's clock (it has no battery clock, so after time without power
# or network it is wrong, and RETRO save sync pauses). Run through sudo by the
# AlicePET app with a time it got from a trusted Jack In PC or from RomM:
#   set-clock.sh <unix seconds, UTC>
# Refuses anything that is not a plain number in a sane range.
set -eu
t=${1:-}
case "$t" in
    ''|*[!0-9]*) echo "usage: set-clock.sh <unix seconds>" >&2; exit 2 ;;
esac
# Not before September 2026 (this was written later), not after 2100.
if [ "${#t}" -ne 10 ] || [ "$t" -lt 1790000000 ] || [ "$t" -gt 4102444800 ]; then
    echo "set-clock.sh: $t is not a believable time" >&2
    exit 2
fi
date -u -s "@$t" >/dev/null
# Keep it across a reboot where the image has fake-hwclock or systemd's clock file.
if command -v fake-hwclock >/dev/null 2>&1; then fake-hwclock save >/dev/null 2>&1 || true; fi
if [ -d /var/lib/systemd/timesync ]; then touch /var/lib/systemd/timesync/clock 2>/dev/null || true; fi
echo "clock set to $(date -u '+%Y-%m-%d %H:%M:%S') UTC"
