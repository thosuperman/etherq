#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs unmodified upstream SOEM (Linux build) against ecatsim over a veth pair and
# checks the results. Needs root / CAP_NET_ADMIN (CI runner with sudo, or a
# container started with --cap-add NET_ADMIN).
#
#   SOEM_BUILD=/path/to/soem/build ./scripts/linux-integration.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SOEM_BUILD="${SOEM_BUILD:-/opt/soem-build}"
OUT="${OUT:-$ROOT/build/integration}"
mkdir -p "$OUT"

SLAVEINFO="$(find "$SOEM_BUILD" -type f -name slaveinfo -perm -u+x | head -1)"
SIMPLE_NG="$(find "$SOEM_BUILD" -type f -name simple_ng -perm -u+x | head -1)"
[ -x "$SLAVEINFO" ] && [ -x "$SIMPLE_NG" ] || { echo "SOEM samples not found under $SOEM_BUILD" >&2; exit 2; }

cleanup() {
  [ -n "${SIM_PID:-}" ] && kill "$SIM_PID" 2>/dev/null || true
  ip link del eq-m 2>/dev/null || true
}
trap cleanup EXIT

ip link del eq-m 2>/dev/null || true
ip link add eq-m type veth peer name eq-s
ip link set eq-m up
ip link set eq-s up

echo "== unit tests"
( cd "$ROOT/sim" && python3 -m pytest -q tests ) | tee "$OUT/unit-tests.txt"

echo "== starting ecatsim on eq-s"
( cd "$ROOT/sim" && exec python3 -m ecatsim --iface eq-s --http off ) >"$OUT/ecatsim.log" 2>&1 &
SIM_PID=$!
sleep 1

echo "== SOEM slaveinfo (upstream v2.0.1, unmodified)"
timeout 60 "$SLAVEINFO" eq-m -map | tee "$OUT/slaveinfo.txt"

echo "== SOEM simple_ng (upstream v2.0.1, unmodified)"
timeout 120 "$SIMPLE_NG" eq-m | tr '\r' '\n' | tee "$OUT/simple_ng.txt" >/dev/null
tail -n 8 "$OUT/simple_ng.txt"

fail=0
check() { if grep -qE "$2" "$1"; then echo "PASS: $3"; else echo "FAIL: $3"; fail=1; fi; }
check "$OUT/slaveinfo.txt" "3 slaves found" "slaveinfo finds 3 slaves"
check "$OUT/slaveinfo.txt" "EQ-DI8 8Ch Dig In \(virtual\)" "slave 1 name from SII strings"
check "$OUT/slaveinfo.txt" "EQ-AX1 Servo Axis \(virtual\)" "slave 3 name from SII strings"
check "$OUT/slaveinfo.txt" "Man: 45514e58" "vendor ID read from EEPROM"
check "$OUT/simple_ng.txt" "all slaves are now operational" "simple_ng reaches OPERATIONAL"
# A WKC that comes back but is wrong is a protocol error -> hard failure.
# WKC -1 (EC_NOFRAME) means the reply missed SOEM's 2 ms EC_TIMEOUTRET; that is host
# scheduling latency of the Python simulator, reported as a rate with a threshold.
cycles=$(grep -cE "^Iteration +[0-9]+:" "$OUT/simple_ng.txt" || true)
bad_wkc=$(grep -cE "WKC [0-9]+ wrong" "$OUT/simple_ng.txt" || true)
late=$(grep -cE "WKC -1 wrong" "$OUT/simple_ng.txt" || true)
echo "cyclic exchanges: $cycles, wrong WKC: $bad_wkc, late replies (>2 ms): $late"
if [ "$bad_wkc" -eq 0 ]; then echo "PASS: no wrong working counter in $cycles cycles"; else echo "FAIL: $bad_wkc wrong working counters"; fail=1; fi
MAX_LATE_PERMILLE="${MAX_LATE_PERMILLE:-10}"
if [ "$cycles" -gt 0 ] && [ $((late * 1000 / cycles)) -le "$MAX_LATE_PERMILLE" ]; then
  echo "PASS: late replies within ${MAX_LATE_PERMILLE} per mille"
else
  echo "FAIL: too many late replies ($late of $cycles)"; fail=1
fi
{ echo "cycles=$cycles"; echo "wrong_wkc=$bad_wkc"; echo "late_replies=$late"; } > "$OUT/summary.txt"
echo "== ecatsim log (tail)"; tail -n 20 "$OUT/ecatsim.log"
exit $fail
