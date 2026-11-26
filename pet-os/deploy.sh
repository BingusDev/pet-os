#!/usr/bin/env bash
# Copy (and optionally install) the PET-os .deb on a CardputerZero.
#   SKIP_BUILD=1  use the newest dist/PET-os_*_arm64.deb as-is (e.g. built on
#                 grandworld-pi by ../bin/build-device.sh --package and copied back)
#   INSTALL=1     also run `sudo dpkg -i` on the device and restart APPLaunch
# Example: SKIP_BUILD=1 INSTALL=1 REMOTE_USER=pi REMOTE_HOST=cardputer-zero ./deploy.sh
set -euo pipefail
cd "$(dirname "$0")"

WORKFLOW_PRESET="${WORKFLOW_PRESET:-cp0-cross-package}"
REMOTE_USER="${REMOTE_USER:-}"
REMOTE_HOST="${REMOTE_HOST:-}"
REMOTE_DIR="${REMOTE_DIR:-/tmp}"
PACKAGE_GLOB="${PACKAGE_GLOB:-dist/PET-os_*_arm64.deb}"
SKIP_BUILD="${SKIP_BUILD:-0}"
INSTALL="${INSTALL:-0}"

log() {
  printf '\n==> %s\n' "$*"
}

if [[ "${SKIP_BUILD}" != "1" ]]; then
  log "Workflow: cmake --workflow --preset ${WORKFLOW_PRESET}"
  cmake --workflow --preset "${WORKFLOW_PRESET}"
fi

shopt -s nullglob
packages=( ${PACKAGE_GLOB} )
shopt -u nullglob

if (( ${#packages[@]} == 0 )); then
  printf 'No package matched: %s\n' "${PACKAGE_GLOB}" >&2
  exit 1
fi

latest_package="$(ls -t "${packages[@]}" | head -n 1)"

if [[ -z "${REMOTE_USER}" || -z "${REMOTE_HOST}" ]]; then
  printf 'Package ready: %s\n' "${latest_package}" >&2
  printf 'Set REMOTE_USER and REMOTE_HOST to deploy, for example:\n' >&2
  printf '  SKIP_BUILD=1 INSTALL=1 REMOTE_USER=pi REMOTE_HOST=192.168.1.10 ./deploy.sh\n' >&2
  exit 1
fi

remote="${REMOTE_USER}@${REMOTE_HOST}"
remote_pkg="${REMOTE_DIR}/$(basename "${latest_package}")"

log "Deploy: scp ${latest_package} ${remote}:${remote_pkg}"
scp "${latest_package}" "${remote}:${remote_pkg}"

if [[ "${INSTALL}" == "1" ]]; then
  log "Install: sudo dpkg -i ${remote_pkg} && restart APPLaunch"
  ssh -t "${remote}" "sudo dpkg -i '${remote_pkg}' && { systemctl --user restart APPLaunch.service 2>/dev/null || sudo systemctl restart APPLaunch.service; }"
fi

log "Done: ${latest_package} -> ${remote}:${remote_pkg}"
