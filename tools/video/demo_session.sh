#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Scripted, *real* demo session for the EtherQ video. Each command is typed on
# screen exactly as it is executed on the QNX 8.0 target (via SSH), and its
# real output is shown. Timestamps of every step go to $MARKS for editing.
#
#   tools/video/demo_session.sh qnx      # main demo on the QNX target
#   tools/video/demo_session.sh build    # engineering proof on the Linux host
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MARKS="${MARKS:-$ROOT/build/video/marks.txt}"
Q="$ROOT/scripts/qnx-qemu.sh"
mkdir -p "$(dirname "$MARKS")"

mark()  { echo "$(date +%s.%N) $1" >> "$MARKS"; }
pause() { sleep "$1"; }
show()  {                       # type a command character by character
  local prompt="$1" text="$2" i
  printf '\e[1;38;5;75m%s\e[0m' "$prompt"
  for ((i = 0; i < ${#text}; i++)); do printf '%s' "${text:$i:1}"; sleep 0.028; done
  sleep 0.35; printf '\n'
}
q()     { show "qnx# " "$1"; "$Q" ssh "cd /tmp; $1" 2>&1; }      # run on QNX
h()     { show "host\$ " "$1"; bash -c "$1" 2>&1; }              # run on the host
note()  { printf '\e[38;5;245m# %s\e[0m\n' "$1"; }

case "${1:-qnx}" in
qnx)
  clear; : > "$MARKS"; mark start
  note "QNX OS 8.0 in QEMU (software emulation) - EtherCAT NIC vtnet1 -> virtual segment"
  pause 1.5
  mark identity
  q "uname -a"
  q "ifconfig vtnet1 | head -1; ls -l /dev/bpf"
  pause 2
  mark slaveinfo
  q "./slaveinfo vtnet1 | grep -E 'succeeded|slaves found|workcounter|Name:'"
  pause 3
  mark ecatd
  q "nohup ./ecatd -i vtnet1 -c 4000 > ecatd.log 2>&1 &"
  pause 4
  q "cat ecatd.log | grep -v BIOCSMMWRITE; ls /dev/ecat"
  pause 2
  mark slaves
  q "./ecatctl slaves"
  pause 3
  mark axisdemo
  q "nohup ./axisdemo > axis.log 2>&1 &"
  pause 7
  q "tail -3 axis.log"
  pause 6
  mark kill
  q "slay -s SIGKILL axisdemo"
  pause 1.5
  q "grep released ecatd.log | tail -1"
  pause 2
  mark after_kill
  q "./ecatctl slaves | cut -c1-12"
  pause 4
  mark blink
  q "nohup ./blink 5 > /dev/null 2>&1 &"
  pause 3
  mark ebusy
  q "./ecatctl hold 0 0xff"
  pause 3
  q "slay -s SIGKILL blink"
  pause 2
  mark freeze
  q "slay -s SIGSTOP ecatd; sleep 1; slay -s SIGCONT ecatd"
  pause 4
  mark recovered
  q "grep acknowledging ecatd.log | tail -2; ./ecatctl slaves | cut -c1-12"
  pause 3
  mark unpriv
  q "slay ecatd; sleep 2; nohup ./ecatd -i vtnet1 -c 4000 -U 1000:1000 > ecatd.log 2>&1 &"
  pause 6
  q "grep running ecatd.log; pidin -p ecatd -f aU"
  pause 4
  mark end
  ;;
build)
  clear; : > "$MARKS"; mark start
  note "Engineering proof - Linux host with QNX SDP 8.0"
  pause 1
  mark toolchain
  h "source ~/qnx800/qnxsdp-env.sh >/dev/null 2>&1; qcc -V 2>&1 | grep x86_64"
  pause 2
  mark build
  h "source ~/qnx800/qnxsdp-env.sh >/dev/null 2>&1; ./scripts/qnx-build.sh x86_64 2>&1 | grep -E 'Linking|warning' | tail -6"
  pause 1
  h "file build/qnx-x86_64/install/bin/ecatd | cut -c1-118"
  pause 3
  mark code
  h "grep -n -A3 'eq_outputs_release(&g_d->out' src/ecatd/resmgr.c"
  pause 4
  mark smoke
  h "./scripts/target-smoke.sh 2>/dev/null | grep -E 'PASS|FAIL|passed|failed'"
  pause 4
  mark end
  ;;
esac
