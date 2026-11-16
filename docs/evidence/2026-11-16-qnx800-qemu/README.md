# Evidence: EtherQ on QNX OS 8.0 (QEMU x86_64), 2026-11-16

## Setup

| | |
|---|---|
| Host | AWS EC2 m6i.large, Ubuntu 24.04, **no KVM**: QEMU 8.2.2 software emulation (TCG) |
| QNX | SDP 8.0 baseline `com.qnx.qnx800` 8.0.0, `qcc` GCC 12.2.0, target `gcc_ntox86_64`; image built with `mkqnximage --type=qemu` |
| Target | QNX 8.0.0 x86_64 guest started by `scripts/qnx-qemu.sh`, with the EtherCAT NIC `vtnet1` on host `tap0` |
| Segment | ecatsim on `tap0`: three **simulated** slaves (DI8, DO8, CiA 402 servo) |
| Command | `./scripts/target-smoke.sh` from a freshly booted guest |

## Result: 15/15 checks passed

See `checks.txt`:
- target is QNX 8.0;
- fresh binaries, no leftover processes;
- **upstream SOEM `slaveinfo` on QNX** finds 3 slaves with expected WKC 6;
- **`BIOCSMMWRITE` accepted by io-sock**;
- bus in OP, as seen both by ecatd and by the simulator;
- `axisdemo` enables the servo (CiA 402);
- **SIGKILL of the client → ecatd releases its 6 output bytes → servo disabled, bus stays OP**;
- second writer gets EBUSY;
- DO8 outputs zero after `blink` is killed;
- **1 s freeze of ecatd → slaves with outputs drop to SAFE-OP 0x001B → supervisor recovers them to OP**.

## Files

| File | Content |
|---|---|
| `build-summary.txt` | qcc targets, SDP baseline, 0 build warnings, QNX ELF binaries (interpreter `/usr/lib/ldqnx-64.so`), QEMU/KVM status |
| `target.txt` | `uname -a`, `vtnet1`, `/dev/bpf` on the target |
| `slaveinfo.txt` | full output of upstream SOEM `slaveinfo vtnet1` built for QNX with the EtherQ port |
| `ecatd-start.txt`, `ecatctl.txt` | daemon start-up, `ecatctl info/slaves`, `/dev/ecat` nodes |
| `axisdemo.txt`, `kill-axisdemo.txt` | servo enable/profile, release on SIGKILL |
| `ebusy.txt`, `watchdog.txt` | ownership conflict; simulated slave states during the freeze |
| `stats.txt` | ecatd statistics at the end of the run |

## How to read the numbers

- Everything in `stats.txt` is **measured on the target under QEMU software emulation**. It describes this emulated setup, not QNX real-time performance, and must not be quoted as such.
- `stats.txt` covers the whole smoke test, **including the deliberate 1-second SIGSTOP of ecatd**. The 483 missed periods, the 1.77 s maximum cycle time and most WKC errors come from that freeze and the recovery after it.
- Steady state, measured separately in a quiet 60 s window the same day: 15 258 cycles at a 4 ms period, 14 WKC errors (0.09 %), 0 missed periods, 0 watchdog trips.
- Slave states, servo position and I/O values are **simulated** (ecatsim).
