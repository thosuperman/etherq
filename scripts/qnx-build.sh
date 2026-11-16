#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Cross-compile EtherQ (SOEM + QNX port, ecatd, ecatctl, axisdemo) for QNX OS 8.0.
#
#   source ~/qnx800/qnxsdp-env.sh
#   ./scripts/qnx-build.sh [x86_64|aarch64le]
#
# Output: build/qnx-<arch>/install/bin/{ecatd,ecatctl,axisdemo,slaveinfo,simple_ng}
set -euo pipefail

ARCH="${1:-x86_64}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
case "$ARCH" in
  x86_64|aarch64le) ;;
  *) echo "usage: $0 [x86_64|aarch64le]" >&2; exit 2 ;;
esac
if [ -z "${QNX_HOST:-}" ] || [ -z "${QNX_TARGET:-}" ]; then
  echo "QNX SDP environment not set. Run: source <sdp-install-dir>/qnxsdp-env.sh" >&2
  exit 2
fi

BUILD="$ROOT/build/qnx-$ARCH"
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/qnx800-$ARCH.cmake" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$BUILD/install"
cmake --build "$BUILD"
cmake --install "$BUILD"

echo
echo "QNX toolchain: $(qcc -V 2>&1 | head -1 || true)"
for b in ecatd ecatctl axisdemo slaveinfo simple_ng; do
  f="$BUILD/install/bin/$b"
  if [ -x "$f" ]; then
    printf '%-10s %s\n' "$b" "$(file -b "$f" | cut -c1-90)"
  else
    echo "missing: $b" >&2; exit 1
  fi
done
