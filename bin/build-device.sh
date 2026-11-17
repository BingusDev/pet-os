#!/bin/bash
# Build a CardputerZero app for the device (aarch64) on grandworld-pi inside debian:trixie.
# Usage: bin/build-device.sh <local-project-dir> [--package]   (e.g. sdk/Template, alicepet)
#   --package  also run CPack (cp0-cross-deb preset), list the .deb contents, and copy
#              the .deb back to <project>/dist/ on this Mac.
set -euo pipefail
SRC="${1:?project dir}"; NAME=$(basename "$SRC"); PACKAGE="${2:-}"
STEPS="cmake --preset cp0-cross && cmake --build --preset cp0-cross-rel && echo BUILD_OK"
if [[ "$PACKAGE" == "--package" ]]; then
  STEPS="$STEPS && rm -rf dist && cpack --preset cp0-cross-deb >/dev/null && for d in dist/*.deb; do echo PACKAGE_OK \$d; dpkg-deb -I \$d | sed -n '/Package:/,/Description:/p'; dpkg-deb -c \$d; done"
fi
ssh grandworld-pi "mkdir -p pet-build"
rsync -a --delete --exclude build --exclude .cache --exclude dist "$SRC"/ "grandworld-pi:pet-build/$NAME/"
ssh grandworld-pi "cd pet-build && docker run --rm -v \$PWD/$NAME:/src -w /src debian:trixie bash -c 'apt-get -o Acquire::ForceIPv4=true -o Acquire::Retries=3 update -qq && DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::ForceIPv4=true -o Acquire::Retries=3 install -y -qq build-essential cmake ninja-build git pkg-config dpkg-dev curl ca-certificates >/dev/null && $STEPS; chown -R 1000:1000 build .cache dist 2>/dev/null; true'"
echo "Built: grandworld-pi:pet-build/$NAME/build/cp0-cross/Release/"
if [[ "$PACKAGE" == "--package" ]]; then
  mkdir -p "$SRC/dist"
  rsync -a "grandworld-pi:pet-build/$NAME/dist/" "$SRC/dist/"
  echo "Package copied to: $SRC/dist/"
fi
