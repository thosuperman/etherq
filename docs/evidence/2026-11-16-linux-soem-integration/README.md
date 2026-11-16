# Evidence: upstream SOEM v2.0.1 (Linux) against the final ecatsim, 2026-11-16

Re-run of `docker/linux-integration.Dockerfile` after the final simulator changes (compact dashboard, cross-origin protection). Docker Desktop on an Intel MacBook Pro 2019.

Result: 27 simulator unit tests passed; SOEM found 3 slaves and reached OPERATIONAL; 10 000 cycles, **0 wrong working counters**, 14 replies later than SOEM's 2 ms timeout (Python simulator host scheduling, within the 10 per mille threshold).

Simulated devices; timings are host-side and not QNX data.
