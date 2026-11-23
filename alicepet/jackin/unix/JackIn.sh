#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# JACK IN for Linux: run from the ALICEPET drive, e.g. bash /media/$USER/ALICEPET/JackIn.sh
cd "$(dirname "$0")" || exit 1
exec bash ./jackin-client.sh "$@"
