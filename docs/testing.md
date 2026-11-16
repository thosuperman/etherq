# Testing and measurement methodology

## Data classes (never mixed)

| Class | Meaning | Where it appears |
|---|---|---|
| **Measured on target** | Measured by ecatd on the QNX target with `CLOCK_MONOTONIC` (wake latency, cycle exec, counters) | `ecatctl stats`, `/dev/ecat/status`. Under QEMU TCG these numbers describe the emulator, not QNX real-time performance. |
| **Simulated** | Produced by ecatsim device models (servo position, I/O values, AL states) | dashboard (badged "Simulated slaves"), `/api/state` (`"data_class": "simulated"`) |
| **Host-side measurement** | Measured on the simulator host from frame arrival times | dashboard "process-data interval", labelled as such |

EtherQ makes **no comparative performance claims** (for example QNX vs Linux). The QNX non-commercial license restricts publishing performance evaluations without approval, and emulated timing would be misleading anyway.

## Test levels

### 1. ecatsim unit tests (Python, 26 tests)

```bash
cd sim && python3 -m pytest -q tests
```

Covered:
- frame codec round trip and rejection of malformed frames;
- SII header CRC and PDO layout, checked byte for byte against SOEM's parser;
- addressing: auto-increment, configured, broadcast OR;
- WKC rules, including read-write = 3;
- read-only register protection and DL status topology;
- EEPROM 8-byte reads, including out-of-range reads;
- invalid AL transition plus acknowledge, and SAFE-OP refused without SM configuration;
- full bring-up with LRW exchange;
- bit-level FMMU mapping;
- SM watchdog trip and recovery, and cable cut;
- CiA 402 enable/follow, following-error fault and reset, and fault when the bus leaves OP.

### 2. Portable core (C, host)

```bash
cmake -S . -B build/host -DCMAKE_C_FLAGS="-fsanitize=address,undefined"
cmake --build build/host && ctest --test-dir build/host --output-on-failure
```

Covered:
- output ownership: all-or-nothing writes, `EBUSY`, range checks, release and zeroing, reclaiming;
- statistics;
- deadline and missed-period arithmetic.

### 3. Fidelity: unmodified SOEM against ecatsim (Linux)

`scripts/linux-integration.sh` (Docker image or CI):

1. Create a veth pair (`eq-m` ↔ `eq-s`) and start ecatsim on `eq-s`.
2. Run SOEM v2.0.1 `slaveinfo eq-m -map`, then `simple_ng eq-m` (10 000 cycles).
3. Run the checks:
   - 3 slaves found;
   - names, vendor and product read from SII;
   - OPERATIONAL reached;
   - **wrong working counter = hard failure**;
   - late replies (`WKC -1`, more than 2 ms) tolerated up to 10 ‰ and reported.

**Result on 2026-11-15** (Docker Desktop on an Intel MacBook Pro 2019):

| Metric | Value |
|---|---|
| Slaves found | 3 |
| PDO mapping | 7 output + 11 input bytes, expected WKC 6 |
| Cyclic exchanges | 10 000 |
| Wrong WKC | 0 |
| Late replies | 7 |

Logs are in `docs/evidence/2026-11-15-linux-soem-integration/`.

Notes:
- SOEM's own "roundtrip min" line can show a negative value. SOEM's sample measures with `CLOCK_REALTIME`, which the Docker VM adjusts; this is unrelated to EtherQ.
- The late replies come from the Python simulator's host scheduling, measured before the optimisation (30) and after it (7).

### 4. Manual scenario checks (Linux, 2026-11-15)

- **Dashboard input toggle:** clicking DI channel 3 changed the input byte read by SOEM to `08`.
- **Cable cut after slave 2, then restore:** SOEM printed `Slave 3 lost`, then `Slave 3 is in SAFE_OP+ERROR, attempting ACK` and `change to OPERATIONAL`. The SM watchdog had tripped while disconnected.

### 5. On target: QNX 8.0 under QEMU (2026-11-16)

`scripts/target-smoke.sh` runs on the Linux host. It drives the QNX guest over SSH and reads the simulated slaves' state from ecatsim's API. **Result: 26/26 checks passed** from a clean rebuild and a freshly booted guest, after the code-review fixes. Evidence: [`docs/evidence/2026-11-16-qnx800-qemu-final/`](evidence/2026-11-16-qnx800-qemu-final/README.md). The first on-target run (15/15, before the fixes) is kept in `evidence/2026-11-16-qnx800-qemu/`.

Steady-state figures, **measured on the target under QEMU software emulation** (not QNX performance data), from a quiet 60 s window at a 4 ms period:

| Metric | Value |
|---|---|
| Cycles | 15 258 |
| WKC errors | 14 (0.09 %) |
| Missed periods | 0 |
| Watchdog trips | 0 |

The occasional WKC error is a reply that missed SOEM's 2 ms receive timeout in the emulated guest. It is the same order of magnitude as the Linux reference run.

Problems found and fixed while bringing the stack up on QNX:
- CMake does not define `QNX` for QNX targets (it defines `QNXNTO`), so the daemon targets were silently skipped. Fixed with an explicit `CMAKE_SYSTEM_NAME` check.
- `resmgr.c` type warnings: the custom OCB/attribute types must be declared before `<sys/dispatch.h>` (`RESMGR_HANDLE_T`/`RESMGR_OCB_T`/`THREAD_POOL_PARAM_T`).
- The SOEM port overlay was copied only when SOEM was first downloaded, so later port edits never reached the build. It is now copied on every configure.
- Without KVM, `-cpu max` makes OpenSSL in the guest crash `sshd`. The launcher now uses a Skylake model with AVX disabled.
- A BPF record check that compared `bh_hdrlen` with `sizeof(struct bpf_hdr)` dropped every frame (`bh_hdrlen` is the *unpadded* size). The on-target test caught it at once (18 failures), and the check was corrected.

Code-review fixes verified by this run:
- the IOmap size is bounds-checked;
- `SET_STATE`/`RESET_STATS` require write access;
- notify triggers run under the attribute lock;
- the supervisor steps through valid AL transitions with ACK and checks the result of reconfiguration;
- signals (SIGHUP, SIGQUIT, ignored SIGPIPE) lead to an orderly shutdown, and the shutdown is idempotent;
- supplementary groups are dropped;
- clients open `/dev/ecat` with `O_CLOEXEC`;
- the dashboard rejects cross-origin POSTs.
