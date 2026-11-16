#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Set up the demo "stage" on the Linux host: a virtual X display with
#   left : xterm running the given command (the QNX session)
#   right: Chromium showing the ecatsim dashboard
# and record it with ffmpeg (x11grab). Everything on screen is real software.
#
#   tools/video/stage.sh <out.mp4> <seconds> <command...>
set -euo pipefail

OUT="$1"; SECS="$2"; shift 2
DISP="${DISP:-:99}"
W=1920; H=1080; TW="${TW:-1150}"     # terminal width; dashboard gets the rest (TW=1920: terminal only)
URL="${URL:-http://127.0.0.1:8080/}"
FONT="${FONT:-JetBrains Mono}"; FS="${FS:-13}"
COLS="${COLS:-99}"; ROWS="${ROWS:-49}"     # JetBrains Mono 13: ~11.2 px x 21.5 px per cell

export DISPLAY="$DISP"
pkill -f "Xvfb $DISP" 2>/dev/null || true
Xvfb "$DISP" -screen 0 ${W}x${H}x24 -nolisten tcp >/dev/null 2>&1 &
XPID=$!
sleep 1

if [ "$TW" -lt "$W" ]; then
/snap/bin/chromium --app="$URL" --window-position=$TW,0 --window-size=$((W - TW)),$H \
  --user-data-dir="$HOME/.cache/eq-chromium" --no-first-run --no-default-browser-check \
  --disable-features=Translate,MediaRouter --disable-gpu --force-device-scale-factor=1 \
  --hide-scrollbars >/dev/null 2>&1 &
CPID=$!
sleep 4
else CPID=""; fi

xterm -geometry ${COLS}x${ROWS}+0+0 -fa "$FONT" -fs "$FS" -bg '#0f1216' -fg '#e7ebf0' \
  -xrm 'XTerm*cursorColor: #4aa3ff' -xrm 'XTerm*internalBorder: 14' -T "QNX 8.0" \
  -e bash -c "$*; sleep 3600" >/dev/null 2>&1 &
TPID=$!
sleep 0.5

ffmpeg -v error -y -f x11grab -draw_mouse 0 -video_size ${W}x${H} -framerate 30 -i "$DISP" -t "$SECS" \
  -c:v libx264 -preset ultrafast -crf 16 -pix_fmt yuv420p "$OUT"

kill $TPID ${CPID:-} $XPID 2>/dev/null || true
wait 2>/dev/null || true
echo "recorded $OUT"
