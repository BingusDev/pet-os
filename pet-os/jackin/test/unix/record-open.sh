#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Test stand-in for open / xdg-open: records the URL instead of opening it.
printf '%s\n' "$1" >>"${JACKIN_TEST_OPENED:-/dev/null}"
