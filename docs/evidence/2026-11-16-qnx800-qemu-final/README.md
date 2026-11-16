# Evidence: final verification on QNX OS 8.0 (QEMU x86_64), 2026-11-16

This run was made after the code-review fixes (IOmap bounds check, devctl permissions, notify locking, supervisor state handling, shutdown/signal handling, BPF record validation). The QNX binaries were rebuilt from scratch, the guest was freshly booted, and ecatsim was restarted with its slaves in INIT.

## Setup

| | |
|---|---|
| Host | AWS EC2 m6i.large, Ubuntu 24.04, **no KVM**: QEMU 8.2.2 software emulation |
| QNX | SDP 8.0 (`com.qnx.qnx800` 8.0.0), `qcc` GCC 12.2.0 `gcc_ntox86_64`, clean rebuild with **0 warnings** (`build-summary.txt`) |
| Target | QNX 8.0.0 x86_64 guest (`scripts/qnx-qemu.sh`), EtherCAT NIC `vtnet1` on host `tap0` |
| Segment | ecatsim, three **simulated** slaves |
| Command | `./scripts/target-smoke.sh` |

## Result: 26/26 checks passed (`checks.txt`)

| Area | Checks |
|---|---|
| Platform | QNX 8.0 target; fresh binaries; no leftover processes |
| Port | upstream SOEM `slaveinfo` on QNX finds 3 slaves, WKC 6; `BIOCSMMWRITE` accepted by io-sock |
| Daemon | bus in OP (ecatd and simulator agree) |
| Crash safety | axisdemo enables the servo; **SIGKILL → its 6 output bytes released and zeroed → servo disabled → bus stays OP** |
| Ownership | second writer gets EBUSY; DO8 cleared after blink is killed |
| Watchdog | 1 s freeze → SAFE-OP 0x001B → recovered to OP |
| Notification | `poll()` on `/dev/ecat/in` wakes once per cycle (50 cycles) |
| State control | `ecatctl state init` → all INIT; `ecatctl state op` → all back to OP |
| Access control | non-root user cannot command the bus (`/dev/ecat/out` is 0660 root); bus unaffected |
| Least privilege | ecatd runs as uid 1000 with locked abilities; process table confirms; bus in OP |
| Shutdown | SIGTERM → orderly shutdown; slaves left in INIT; `/dev/ecat` removed |

## Notes on the numbers

- `stats.txt` covers the whole run, including the deliberate 1 s freeze, the INIT → OP state test and two daemon restarts. Missed periods, WKC errors and recoveries come mostly from those events, not from steady-state operation.
- **Timing values (wake latency, cycle execution time) are withheld from the published copy.** The QNX non-commercial license restricts publishing performance evaluations without approval, and these values come from a software-emulated CPU, so they would say nothing about QNX real-time behaviour anyway.
- Slave states, servo position and I/O values are **simulated** (ecatsim).
