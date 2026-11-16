#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Prepare an Ubuntu 22.04/24.04 x86_64 host (cloud VM or local) for EtherQ:
# build tools, QEMU, Python, and the tools to give a QEMU guest a tap NIC.
# QNX SDP 8.0 itself is NOT installed here: it needs your myQNX account and
# license. See docs/qnx-build-and-run.md, "Install QNX SDP 8.0".
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
  exec sudo -E "$0" "$@"
fi

. /etc/os-release
case "${VERSION_ID:-}" in
  22.04|24.04) ;;
  *) echo "warning: QNX SDP 8.0 supports Ubuntu 22.04/24.04 hosts; this is ${PRETTY_NAME:-unknown}" >&2 ;;
esac

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build git curl ca-certificates unzip \
  python3 python3-pytest iproute2 bridge-utils \
  qemu-system-x86 qemu-utils \
  openjdk-17-jre-headless   # QNX Software Center command-line client needs a JRE

# KVM is unavailable on most cloud VMs (no nested virtualisation); QEMU then
# falls back to TCG software emulation. Report which one we will get.
if [ -e /dev/kvm ]; then
  echo "KVM: available (/dev/kvm)"
else
  echo "KVM: not available - QEMU will use TCG software emulation (slower; timing is not representative)"
fi

cmake --version | head -1
qemu-system-x86_64 --version | head -1
python3 --version
echo "provisioning done"
