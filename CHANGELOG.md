# Changelog

All notable changes to this project are documented here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [Semantic Versioning](https://semver.org/).

## [0.1.0] - 2026-11-16

First public release, prepared for the QNX Global Hackathon 2026.

### Added
- **SOEM QNX OS 8.0 port** (`port/soem/`), an additive overlay on SOEM v2.0.1:
  - QNX OSAL (monotonic clocks, SCHED_FIFO threads created with explicit scheduling, priority-inheritance mutexes);
  - raw EtherCAT frames over io-sock **BPF**, with a kernel EtherType filter and inbound-only direction;
  - optional QNX **multi-packet writes** (`BIOCSMMWRITE`, `ETHERQ_BPF_MMWRITE=1`).
- **ecatd**, a master daemon:
  - process-data cycle driven by a QNX **timer pulse** at a fixed SCHED_FIFO priority;
  - supervisor that acknowledges and recovers slaves;
  - **resource manager** serving `/dev/ecat/in` (poll()-able once per cycle), `/dev/ecat/out` (byte ownership) and `/dev/ecat/status`;
  - **safe stop**: outputs owned by a client that closes or crashes are zeroed and released;
  - optional least-privilege mode `-U uid:gid`, using locked procmgr abilities.
- **ecatctl** CLI; **axisdemo** (CiA 402 servo client); **blink** (minimal client example).
- **ecatsim**, a virtual EtherCAT segment:
  - ESC register model, SII EEPROM, AL state machine, bit-level FMMUs, SyncManager watchdog;
  - DI8/DO8/CiA 402 servo device models;
  - live dashboard and fault injection.
- Build and test tooling:
  - CMake toolchain files for QNX 8.0 x86_64 and aarch64le;
  - `scripts/qnx-build.sh`, `scripts/qnx-qemu.sh` (two-NIC QEMU launcher), `scripts/target-smoke.sh` (on-target end-to-end test);
  - Linux integration test against unmodified upstream SOEM; GitHub Actions CI.
- Documentation: architecture, QNX build/run guide, testing methodology, limitations, codelab, demo script and evidence logs.

### Fixed (from code review, before release)
- IOmap bounds check against SOEM's required size.
- devctl state/statistics commands require `/dev/ecat/out` opened for writing.
- notify triggers run under the attribute lock.
- Supervisor: valid AL transition steps with error acknowledge, and the result of `ecx_reconfig_slave` is checked.
- Orderly, idempotent shutdown on SIGINT/SIGTERM/SIGHUP/SIGQUIT; SIGPIPE ignored.
- Supplementary groups dropped together with root.
- BPF record validation; no sleep when another thread already received the frame.
- WKC errors counted only in OP.
- Clients use `O_CLOEXEC`.
- Dashboard rejects cross-origin POSTs and invalid slave numbers.

### Verified
- QNX SDP 8.0 clean build with 0 warnings; end-to-end test on QNX 8.0 under QEMU (software emulation): 26/26 checks.
- Unmodified SOEM v2.0.1 (Linux) against ecatsim: 10 000 cycles, 0 wrong working counters.

### Known limitations
- See `docs/limitations.md`. No physical EtherCAT hardware or aarch64 target has been tested.
