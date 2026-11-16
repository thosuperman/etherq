# Evidence: upstream SOEM v2.0.1 (Linux) against ecatsim, 2026-11-15

- Host: Docker Desktop on an Intel MacBook Pro 2019 (Linux VM). The image was built from `docker/linux-integration.Dockerfile`.
- Command: `docker run --rm --cap-add NET_ADMIN etherq-linux-integration`
- Data class: the simulated slaves are simulated. Timings in `simple_ng` output are host-side measurements by SOEM's sample on a Linux VM, not QNX data.

| File | Content |
|---|---|
| `checks.txt` | PASS/FAIL checks from `scripts/linux-integration.sh` |
| `summary.txt` | cycles, wrong WKC, late replies |
| `slaveinfo.txt` | full SOEM `slaveinfo -map` output (SII, SMs, FMMUs, PDO mapping) |
| `simple_ng-excerpt.txt` | start-up lines, first/last iterations of `simple_ng` (10 000 cycles) |
| `ecatsim.log` | simulator AL state log |
| `unit-tests.txt` | ecatsim unit test result in the same image |

The negative "Roundtrip time min" printed by SOEM's sample comes from its use of `CLOCK_REALTIME`, which the VM adjusts. It is unrelated to EtherQ.
