#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# End-to-end smoke test on a QNX 8.0 QEMU target (run on the Linux host).
# Needs: a running target (scripts/qnx-qemu.sh start), ecatsim running on tap0
# with its dashboard on 127.0.0.1:8080, and binaries in build/qnx-x86_64.
#
# Every check reads real state: QNX command output on the target, and the
# simulated slaves' state from ecatsim's /api/state. Logs go to $OUT.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$ROOT/build/target-smoke}"
Q="$ROOT/scripts/qnx-qemu.sh"
API="${API:-http://127.0.0.1:8080/api/state}"
mkdir -p "$OUT"
fail=0
pass() { echo "PASS: $*" | tee -a "$OUT/checks.txt"; }
bad()  { echo "FAIL: $*" | tee -a "$OUT/checks.txt"; fail=1; }
tq()   { "$Q" ssh "$@"; }                       # run a command on QNX
sim()  { curl -fs "$API" | python3 -c "import json,sys; s=json.load(sys.stdin); print(eval(sys.argv[1]))" "$1"; }
: > "$OUT/checks.txt"

echo "== target identity"
tq "uname -a; ifconfig vtnet1 | head -2; ls -l /dev/bpf" | tee "$OUT/target.txt"
grep -q "QNX .* 8\.0" "$OUT/target.txt" && pass "target is QNX 8.0" || bad "target is not QNX 8.0"

echo "== copy binaries"
# stop leftovers and wait until they are really gone (ecatd shuts down orderly)
tq 'for p in axisdemo blink ecatd; do slay -f $p >/dev/null 2>&1; done
    for i in 1 2 3 4 5 6 7 8 9 10; do pidin -f a | grep -qE "ecatd|blink|axisdemo" || break; sleep 1; done
    pidin -f a | grep -qE "ecatd|blink|axisdemo" && { echo "leftover process"; exit 1; }; true' \
  || { bad "could not stop leftover processes"; exit 1; }
"$Q" scp "$ROOT"/build/qnx-x86_64/install/bin/* >/dev/null || { bad "upload of binaries failed"; exit 1; }
pass "fresh binaries uploaded, no leftover processes"

echo "== SOEM slaveinfo on QNX"
tq "/tmp/slaveinfo vtnet1" > "$OUT/slaveinfo.txt" 2>&1
grep -q "3 slaves found and configured" "$OUT/slaveinfo.txt" && pass "slaveinfo on QNX finds 3 slaves" || bad "slaveinfo"
grep -q "Calculated workcounter 6" "$OUT/slaveinfo.txt" && pass "expected WKC 6" || bad "expected WKC"

echo "== ecatd (multi-packet BPF writes)"
tq "cd /tmp; ETHERQ_BPF_MMWRITE=1 ./ecatd -i vtnet1 -c 4000 > /tmp/ecatd.log 2>&1 & sleep 8; cat /tmp/ecatd.log" > "$OUT/ecatd-start.txt"
grep -q "BIOCSMMWRITE) enabled" "$OUT/ecatd-start.txt" && pass "BIOCSMMWRITE accepted by io-sock" || bad "BIOCSMMWRITE"
tq "/tmp/ecatctl info; /tmp/ecatctl slaves; ls -l /dev/ecat" > "$OUT/ecatctl.txt"
grep -q "bus state      OP" "$OUT/ecatctl.txt" && pass "bus in OP" || bad "bus not in OP"
[ "$(sim '[x["al_state"] for x in s["slaves"]]')" = "['OP', 'OP', 'OP']" ] && pass "simulator sees 3 slaves in OP" || bad "simulator states"

echo "== servo client and crash safety"
tq "cd /tmp; nohup ./axisdemo > /tmp/axis.log 2>&1 & sleep 8; head -5 /tmp/axis.log" > "$OUT/axisdemo.txt"
[ "$(sim 's["slaves"][2]["device"]["state"]')" = "OPERATION_ENABLED" ] && pass "axisdemo enabled the servo (CiA 402)" || bad "servo not enabled"
tq "n=\$(wc -l < /tmp/ecatd.log); slay -s SIGKILL axisdemo; sleep 1; tail -n +\$((n+1)) /tmp/ecatd.log" > "$OUT/kill-axisdemo.txt"
grep -q "6 output bytes zeroed and released" "$OUT/kill-axisdemo.txt" && pass "SIGKILL: ecatd released the client's 6 output bytes" || bad "release on kill"
[ "$(sim 's["slaves"][2]["device"]["state"]')" = "SWITCH_ON_DISABLED" ] && pass "servo disabled after client crash" || bad "servo still enabled"
[ "$(sim '[x["al_state"] for x in s["slaves"]]')" = "['OP', 'OP', 'OP']" ] && pass "bus stayed in OP after client crash" || bad "bus left OP"

echo "== output ownership"
tq "cd /tmp; nohup ./blink 5 > /dev/null 2>&1 & sleep 3; ./ecatctl hold 0 0xff 2>&1 | head -1" > "$OUT/ebusy.txt"
grep -q "Resource busy" "$OUT/ebusy.txt" && pass "second writer gets EBUSY" || bad "no EBUSY"
tq "slay -s SIGKILL blink; sleep 1"
[ "$(sim 'sum(s["slaves"][1]["device"]["outputs"])')" = "0" ] && pass "DO8 outputs zero after blink crash" || bad "DO8 outputs not cleared"

echo "== watchdog: freeze the master for 1 s"
tq "slay -s SIGSTOP ecatd; sleep 1"
w=$(sim '[ (x["al_state"], x["al_code"]) for x in s["slaves"]]')
echo "$w" > "$OUT/watchdog.txt"
echo "$w" | grep -q "('SAFE-OP', '0x001b')" && pass "slaves with outputs dropped to SAFE-OP 0x001B" || bad "no watchdog trip"
tq "slay -s SIGCONT ecatd"
for _ in 1 2 3 4 5 6 7 8 9 10; do
  [ "$(sim '[x["al_state"] for x in s["slaves"]]')" = "['OP', 'OP', 'OP']" ] && break; sleep 1
done
[ "$(sim '[x["al_state"] for x in s["slaves"]]')" = "['OP', 'OP', 'OP']" ] && pass "supervisor recovered all slaves to OP" || bad "no recovery"

echo "== poll() wakes once per bus cycle (io_notify)"
tq "/tmp/ecatctl watch 50 > /tmp/watch.log 2>&1; echo rc=\$?" > "$OUT/watch.txt"
grep -q "rc=0" "$OUT/watch.txt" && pass "ecatctl watch returned after 50 cycles (poll per cycle)" || bad "watch"

echo "== requested state changes (INIT, then back to OP)"
tq "/tmp/ecatctl state init; sleep 3" > "$OUT/state.txt"
[ "$(sim '[x["al_state"] for x in s["slaves"]]')" = "['INIT', 'INIT', 'INIT']" ] && pass "ecatctl state init -> all slaves INIT" || bad "state init"
tq "/tmp/ecatctl state op; sleep 6; /tmp/ecatctl info | grep 'bus state'" >> "$OUT/state.txt"
[ "$(sim '[x["al_state"] for x in s["slaves"]]')" = "['OP', 'OP', 'OP']" ] && pass "ecatctl state op -> back to OP" || bad "state op"

echo "== access control: unprivileged user cannot command the bus"
tq "on -u qnxuser /tmp/ecatctl state init 2>&1 | head -1" > "$OUT/acl.txt"
grep -qiE "permission denied|not permitted" "$OUT/acl.txt" && pass "non-root user refused (/dev/ecat/out is 0660 root)" || bad "acl: $(cat "$OUT/acl.txt")"
[ "$(sim '[x["al_state"] for x in s["slaves"]]')" = "['OP', 'OP', 'OP']" ] && pass "bus unaffected by refused request" || bad "bus changed"

echo "== stats (before restart)"
tq "/tmp/ecatctl stats; cat /tmp/ecatd.log" > "$OUT/stats.txt"

echo "== unprivileged daemon (-U 1000:1000)"
tq "slay ecatd; sleep 3; cd /tmp; ./ecatd -i vtnet1 -c 4000 -U 1000:1000 > /tmp/ecatd-u.log 2>&1 & sleep 10; cat /tmp/ecatd-u.log; pidin -p ecatd -f aU" > "$OUT/unpriv.txt"
grep -q "running as uid 1000 gid 1000" "$OUT/unpriv.txt" && pass "ecatd dropped to uid 1000 with locked abilities" || bad "privilege drop"
grep -qE "^ *[0-9]+ +1000 " "$OUT/unpriv.txt" && pass "process table shows ecatd as uid 1000" || bad "pidin uid"
[ "$(sim '[x["al_state"] for x in s["slaves"]]')" = "['OP', 'OP', 'OP']" ] && pass "bus in OP under unprivileged ecatd" || bad "unprivileged bus state"

echo "== clean shutdown (SIGTERM)"
tq "slay ecatd; sleep 3; tail -2 /tmp/ecatd-u.log; pidin -f a | grep -c ecatd; ls /dev/ecat 2>&1 | head -1" > "$OUT/shutdown.txt"
grep -q "requesting INIT and exiting" "$OUT/shutdown.txt" && pass "SIGTERM: orderly shutdown message" || bad "no orderly shutdown"
[ "$(sim '[x["al_state"] for x in s["slaves"]]')" = "['INIT', 'INIT', 'INIT']" ] && pass "slaves left in INIT after shutdown" || bad "slaves not INIT"
grep -qE "No such file|not found" "$OUT/shutdown.txt" && pass "/dev/ecat removed after exit" || bad "/dev/ecat still present"

echo "== statistics (measured under QEMU TCG emulation, not QNX performance data)"
cat "$OUT/stats.txt"
echo
grep -c PASS "$OUT/checks.txt" | xargs echo "passed:"
grep -c FAIL "$OUT/checks.txt" | xargs echo "failed:"
exit $fail
