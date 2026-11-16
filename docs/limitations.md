# Limitations (stated plainly)

## Verification
- **Verified on QNX 8.0 under QEMU software emulation only (2026-11-16).** It has not been run on KVM-accelerated QEMU or on physical x86/ARM boards. The aarch64le build is not yet exercised.
- The `poll()`-per-cycle notification (`iofunc_notify`) is exercised by `ecatctl watch`, `blink` and `axisdemo`, but has no dedicated automated check yet.
- **No real EtherCAT hardware has been used.** Behaviour with physical slaves, real PHYs and distributed clocks is untested.

## ecatsim
- Protocol-level model. It has no PHY timing, frame CRC, distributed clocks, CoE/FoE/EoE mailboxes, redundancy ring or bootstrap state.
- Device models are fictitious and simplified. The servo uses a fixed PDO, CSP only, and a first-order kinematic model.
- Written in Python. On a busy host a reply can occasionally exceed SOEM's 2 ms default timeout (0.07 % of cycles in the recorded run). It is a test tool, not a timing reference.

## ecatd
- Single process-data group (group 0) and one process image of at most 4 KiB.
- No distributed-clock synchronisation (SYNC0) in the MVP.
- Output ownership is per byte, not per bit. Two clients cannot share bits of one byte.
- `BIOCSMMWRITE` batching works on QNX 8.0 io-sock (verified), but stays opt-in (`ETHERQ_BPF_MMWRITE=1`) because it is less exercised than single-frame writes.

## Timing
- Under QEMU TCG (no KVM, for example on cloud VMs), cycle timing reflects the emulator. Use cycle periods of 4 ms or more, and do not read the statistics as QNX performance figures.

## Licensing
- GPL-3.0-or-later, because SOEM is GPLv3. Commercial use requires an RT-Labs commercial SOEM license and a commercial QNX license.
- An upstream contribution to SOEM may need RT-Labs' contribution terms because of their dual licensing.
