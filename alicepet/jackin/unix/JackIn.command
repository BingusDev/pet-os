#!/bin/bash
# SPDX-License-Identifier: MIT
# JACK IN for macOS: open this from the ALICEPET drive. Terminal runs the shared
# client in this folder. Nothing is installed and nothing is written to the drive.
cd "$(dirname "$0")" || exit 1
bash ./jackin-client.sh
status=$?
echo
read -r -p "Press Return to close this window. " _ </dev/tty 2>/dev/null || true
exit "$status"
