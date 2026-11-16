# Roadmap

Ordered by how much each item adds to real-world usefulness.

## Next: validation on real systems
1. **Physical EtherCAT hardware.** Run ecatd on an x86 PC NIC (for example Intel `em`/`igc`) connected to commodity EtherCAT I/O terminals and a CiA 402 drive. Record bring-up, mapping and the safe-stop scenario.
2. **KVM-accelerated QEMU and bare-metal x86.** Repeat `target-smoke.sh` and measure cycle behaviour on a non-emulated target. Publish timing only with QNX approval, as the non-commercial license requires.
3. **aarch64le (Raspberry Pi 4/5 with QNX 8.0).** The toolchain file exists; validate the build and run with a USB or PCIe Ethernet NIC.

## Features
- **Distributed clocks (SYNC0).** Configure DC through SOEM, align the ecatd cycle to the DC reference, and add DC registers to ecatsim.
- **CoE mailbox (SDO)** in ecatsim, and an `ecatctl sdo` command, so drives with dynamic PDO mapping can be modelled.
- **Per-bit output ownership**, for bit-packed digital outputs shared between clients.
- **Adaptive partitioning.** Run ecatd in a guaranteed CPU partition so that heavy, non-real-time load cannot starve the cycle.
- **Security policies (secpol).** Ship an example policy that grants ecatd exactly the abilities it needs at boot, instead of starting as root.
- **ENI support.** Load an EtherCAT Network Information file through SOEM's ENI tooling.

## Upstream
- Propose the additive QNX platform files (`osal/qnx`, `oshw/qnx`, `cmake/QNX.cmake`) to SOEM, subject to RT-Labs' contribution terms (they dual-license GPLv3/commercial).
