# Contributing to EtherQ

Thanks for your interest. EtherQ has three parts: a QNX OS 8.0 port of SOEM, the `ecatd` resource-manager daemon, and the `ecatsim` virtual EtherCAT segment. Contributions to any of them are welcome.

## Ground rules

- **Never commit QNX SDP files**: no headers, libraries, images, license keys or QNX Software Center logs. Build against your own SDP installation. `.gitignore` excludes the usual locations; check `git status` before committing anyway.
- **Never commit credentials.** QNX Software Center 2.0.4 writes `-myqnx.password` into `~/.qnx/swupdate/*.log`. Keep those logs out of the repository and out of screenshots.
- **Keep evidence honest.**
  - Label measurements as *measured on target* (and say whether it is QEMU, KVM or hardware), *simulated* or *host-side*.
  - Make no comparative performance claims.
  - The QNX non-commercial license restricts publishing performance evaluations without approval.
- **The SOEM port stays additive.** Files under `port/soem/` add a platform; they must not require patching upstream SOEM sources.

## Development workflow

```bash
# host-side tests (no QNX needed)
cd sim && python3 -m pytest -q tests && cd ..
cmake -S . -B build/host -DCMAKE_C_FLAGS="-fsanitize=address,undefined"
cmake --build build/host && ctest --test-dir build/host --output-on-failure

# simulator against unmodified upstream SOEM (Linux, needs CAP_NET_ADMIN)
docker build -f docker/linux-integration.Dockerfile -t etherq-linux-integration .
docker run --rm --cap-add NET_ADMIN etherq-linux-integration

# QNX build and on-target end-to-end test (needs SDP 8.0, see docs/qnx-build-and-run.md)
source ~/qnx800/qnxsdp-env.sh && ./scripts/qnx-build.sh x86_64
./scripts/qnx-qemu.sh start ~/qnx-target
./scripts/target-smoke.sh
```

A change is ready for review when:
1. the QNX build has **0 warnings** with `-Wall -Wextra`;
2. host tests pass;
3. if you touched `port/`, `src/ecatd/` or `src/ecatcore/`, `target-smoke.sh` passes on a QNX target. Say which target (QEMU TCG, QEMU KVM or hardware) in the pull request.

## Style

- C11, 3-space indentation, braces on their own line, as in the existing code. No new dependencies in the daemon.
- Python: standard library only for `ecatsim` (tests may use pytest).
- Commit messages follow Conventional Commits (`fix(ecatd): ...`, `feat(sim): ...`).

## Licensing of contributions

EtherQ is GPL-3.0-or-later. By contributing you agree your contribution is licensed under it.
- Files in `port/soem/` derive from SOEM's OS layer, which RT-Labs dual-licenses (GPLv3/commercial).
- If the port is contributed upstream later, RT-Labs may ask contributors to accept their contribution terms.
