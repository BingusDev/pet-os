#!/bin/bash
# Build a CardputerZero app for the device (aarch64) on grandworld-pi inside debian:trixie.
# Usage: bin/build-device.sh <local-project-dir>   (e.g. sdk/Template)
set -euo pipefail
SRC="${1:?project dir}"; NAME=$(basename "$SRC")
ssh grandworld-pi "mkdir -p pet-build"
rsync -a --delete --exclude build --exclude .cache "$SRC"/ "grandworld-pi:pet-build/$NAME/"
ssh grandworld-pi "cd pet-build && docker run --rm -v \$PWD/$NAME:/src -w /src debian:trixie bash -c 'apt-get -o Acquire::ForceIPv4=true -o Acquire::Retries=3 update -qq && DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::ForceIPv4=true -o Acquire::Retries=3 install -y -qq build-essential cmake ninja-build git pkg-config dpkg-dev curl ca-certificates >/dev/null && cmake --preset cp0-cross && cmake --build --preset cp0-cross-rel && echo BUILD_OK; chown -R 1000:1000 build .cache'"
echo "Built: grandworld-pi:pet-build/$NAME/build/cp0-cross/Release/"
