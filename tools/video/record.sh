#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Record the two real takes for the demo video on the Linux host:
#   build/video/takes/demo.mp4  + demo_marks.txt   (QNX session + dashboard)
#   build/video/takes/build.mp4 + build_marks.txt  (QNX SDP build + smoke test)
# Prerequisites: QNX target running (scripts/qnx-qemu.sh start), QNX binaries built.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="$ROOT/build/video/takes"
Q="$ROOT/scripts/qnx-qemu.sh"
mkdir -p "$T"
cd "$ROOT"

restart_sim() {
  if [ -f "$HOME/ecatsim.pid" ]; then sudo kill "$(cat "$HOME/ecatsim.pid")" 2>/dev/null || true; fi
  sleep 1
  (cd "$ROOT/sim" && sudo sh -c "nohup python3 -m ecatsim --iface tap0 --http 127.0.0.1:8080 \
     --master-label 'EtherQ on QNX 8.0' > $HOME/ecatsim.log 2>&1 & echo \$! > $HOME/ecatsim.pid")
  sleep 2
}

echo "== reset target and simulator"
"$Q" ssh 'for p in axisdemo blink ecatd; do slay -f $p >/dev/null 2>&1; done; sleep 3; rm -f /tmp/*.log; true'
"$Q" scp build/qnx-x86_64/install/bin/* >/dev/null
restart_sim

echo "== take 1: QNX demo session"
MARKS="$T/demo_marks.txt" TW=1200 COLS=104 ROWS=44 tools/video/stage.sh "$T/demo.mp4" "${DEMO_SECS:-230}" \
  "MARKS=$T/demo_marks.txt $ROOT/tools/video/demo_session.sh qnx"

echo "== take 2: engineering proof"
restart_sim
"$Q" ssh 'for p in axisdemo blink ecatd; do slay -f $p >/dev/null 2>&1; done; sleep 3; true'
MARKS="$T/build_marks.txt" TW=1920 FS=15 COLS=140 ROWS=38 tools/video/stage.sh "$T/build.mp4" "${BUILD_SECS:-330}" \
  "MARKS=$T/build_marks.txt $ROOT/tools/video/demo_session.sh build"

ls -la "$T"
