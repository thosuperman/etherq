#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Boot a QNX OS 8.0 x86_64 image built by mkqnximage in QEMU with two NICs:
#   vtnet0  user-mode network, guest SSH forwarded to 127.0.0.1:${SSH_PORT:-2222}
#   vtnet1  host tap device (default tap0) for the EtherCAT segment (ecatsim)
#
# This replaces mkqnximage's own runner, which expects a host bridge, so no
# bridge or qemu-bridge-helper setup is needed.
#
#   mkdir ~/qnx-target && cd ~/qnx-target
#   mkqnximage --type=qemu --arch=x86_64 --ssh-ident=$HOME/.ssh/id_ed25519.pub --noprompt --build
#   ./scripts/qnx-qemu.sh start ~/qnx-target      # boots in the background
#   ./scripts/qnx-qemu.sh ssh                      # log in as root
#   ./scripts/qnx-qemu.sh stop ~/qnx-target
set -euo pipefail

CMD="${1:-}"
IMG="${2:-$HOME/qnx-target}"
TAP="${TAP:-tap0}"
SSH_PORT="${SSH_PORT:-2222}"
RAM="${RAM:-2048}"
CPUS="${CPUS:-2}"
SSH_OPTS=(-p "$SSH_PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)

ensure_tap() {
  if ! ip link show "$TAP" >/dev/null 2>&1; then
    sudo ip tuntap add dev "$TAP" mode tap user "$USER"
  fi
  sudo ip link set "$TAP" up
}

case "$CMD" in
  start)
    [ -f "$IMG/output/ifs.bin" ] && [ -f "$IMG/output/disk-qemu.vmdk" ] || {
      echo "no mkqnximage output in $IMG/output (build the image first)" >&2; exit 2; }
    ensure_tap
    # Without KVM (TCG), OpenSSL in the guest takes AVX code paths that crash sshd
    # ("Memory fault" at boot) with -cpu max or a plain Skylake model. Verified on
    # QEMU 8.2: a Skylake model with AVX/AVX2/FMA/F16C disabled boots and sshd works.
    # Older models (Nehalem) do not boot QNX 8.0 at all.
    TCG_CPU="Skylake-Client-noTSX-IBRS,avx=off,avx2=off,fma=off,f16c=off"
    if [ -w /dev/kvm ]; then accel=(-enable-kvm -cpu host); else accel=(-cpu "${QEMU_CPU:-$TCG_CPU}"); fi
    cd "$IMG"
    rm -f output/qemu.out
    qemu-system-x86_64 \
      -smp "$CPUS" "${accel[@]}" -m "$RAM" \
      -kernel output/ifs.bin \
      -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 \
      -netdev user,id=net0,hostfwd=tcp:127.0.0.1:"$SSH_PORT"-:22 \
      -device virtio-net-pci,netdev=net0 \
      -netdev tap,id=ecat,ifname="$TAP",script=no,downscript=no \
      -device virtio-net-pci,netdev=ecat \
      -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
      -display none \
      -chardev socket,id=con0,path=output/console.sock,server=on,wait=off,logfile=output/qemu.out \
      -serial chardev:con0 \
      -pidfile output/qemu.pid -daemonize
    echo "QNX booting (console log: $IMG/output/qemu.out); waiting for SSH on port $SSH_PORT..."
    for _ in $(seq 1 120); do
      if ssh "${SSH_OPTS[@]}" -o ConnectTimeout=3 -o BatchMode=yes root@127.0.0.1 true 2>/dev/null; then
        echo "QNX is up: ./scripts/qnx-qemu.sh ssh"; exit 0
      fi
      sleep 5
    done
    echo "SSH did not come up within 10 minutes; see $IMG/output/qemu.out" >&2; exit 1
    ;;
  stop)
    if [ -f "$IMG/output/qemu.pid" ]; then
      kill "$(cat "$IMG/output/qemu.pid")" 2>/dev/null || true
      rm -f "$IMG/output/qemu.pid"
      echo stopped
    fi
    ;;
  console)
    # send one command line to the serial console; output appears in output/qemu.out
    shift 2
    printf '%s\r' "$*" | socat - UNIX-CONNECT:"$IMG/output/console.sock" >/dev/null
    ;;
  ssh)
    shift
    exec ssh "${SSH_OPTS[@]}" root@127.0.0.1 "$@"
    ;;
  scp)
    shift
    # usage: qnx-qemu.sh scp <local files...>  -> copied to /tmp on the target
    exec scp -P "$SSH_PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR "$@" root@127.0.0.1:/tmp/
    ;;
  *)
    echo "usage: $0 start|stop [image_dir] | console <image_dir> <command> | ssh [cmd] | scp <files>" >&2; exit 2 ;;
esac
