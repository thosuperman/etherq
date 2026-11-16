# Build and run EtherQ on QNX OS 8.0 (QEMU x86_64)

| | |
|---|---|
| QNX version | QNX OS 8.0 (QNX SDP 8.0 baseline `com.qnx.qnx800`, free non-commercial license via QNX Everywhere) |
| Target | QEMU x86_64 guest built with `mkqnximage --type=qemu` (aarch64le toolchain file included, not yet exercised) |
| Toolchain | `qcc` (GCC 12.2.0), target `gcc_ntox86_64`; CMake ≥ 3.28 with `cmake/qnx800-x86_64.cmake` |
| Host | Ubuntu 22.04/24.04 x86_64. The author used AWS EC2 `m6i.large`, Ubuntu 24.04, **no KVM** (QEMU software emulation). |

> **Validation status (2026-11-16):** every step below was run end to end by the author on that host. Steps that need your myQNX account are marked **(you)**.

## 1. Prepare the host

```bash
git clone https://github.com/thosuperman/etherq.git etherq && cd etherq
./scripts/provision-ubuntu.sh
sudo apt-get install -y socat     # used for the QEMU serial console helper
```

## 2. License (you)

1. Request a QNX Everywhere license at https://qnx.software/en/developers/get-started/qnx-everywhere.
2. Wait for your myQNX account to finish processing (it can take a few hours).
3. In **myQNX License Manager**, click "Click to deploy to user/s" and deploy the SDP 8.0 license to yourself.

## 3. Install QNX Software Center and SDP 8.0 (you)

Download *QNX Software Center 2.0.4 – Linux Hosts* (`qnx-setup-2.0.4-202501021438-linux.run`) from the QNX download center. Run it and accept the Development License Agreement:

```bash
./qnx-setup-2.0.4-202501021438-linux.run      # installs into ~/qnx/qnxsoftwarecenter
```

On a server without a desktop, the graphical Software Center fails to start (`Could not load SWT library`). That is expected; use the command-line client instead. Read your credentials into variables so they don't land in shell history:

```bash
read -p "myQNX email: " QU; read -s -p "myQNX password: " QP; echo
C=~/qnx/qnxsoftwarecenter/qnxsoftwarecenter_clt
$C -myqnx.user "$QU" -myqnx.password "$QP" -syncLicenseKeys -activateAll
$C -offline -listLicenseKeys                     # must show the key as "Activated"
$C -myqnx.user "$QU" -myqnx.password "$QP" -installBaseline com.qnx.qnx800 -destination ~/qnx800
unset QU QP
```

**Security note:** QNX Software Center 2.0.4 writes the full command line, **including `-myqnx.password`**, into its log files under `~/.qnx/swupdate/`. After installing, redact them:

```bash
grep -rlE -- "-myqnx\.password [^*]" ~/.qnx | xargs -r sed -i -E "s/(-myqnx\.(password|user) )[^ ]+/\1***/g"
```

## 4. Build EtherQ for QNX

```bash
source ~/qnx800/qnxsdp-env.sh
./scripts/qnx-build.sh x86_64
```

This downloads SOEM v2.0.1 (SHA-256 pinned), overlays `port/soem/` on every configure, and cross-compiles. Expected result: a clean build with `-Wall -Wextra` and **0 warnings**. The binaries are in `build/qnx-x86_64/install/bin/`: `ecatd ecatctl axisdemo blink slaveinfo simple_ng`.

## 5. Create and boot the QEMU target

```bash
test -f ~/.ssh/id_ed25519 || ssh-keygen -q -t ed25519 -N "" -f ~/.ssh/id_ed25519
mkdir ~/qnx-target && cd ~/qnx-target          # must be an empty directory
~/qnx800/host/common/bin/mkqnximage --type=qemu --arch=x86_64 \
    --ssh-ident=$HOME/.ssh/id_ed25519.pub --noprompt --build
cd ~/etherq
./scripts/qnx-qemu.sh start ~/qnx-target       # boots in the background, waits for SSH
./scripts/qnx-qemu.sh ssh "uname -a"
```

`scripts/qnx-qemu.sh` boots the `mkqnximage` kernel and disk with two virtio NICs:

| Guest NIC | Host side | Purpose |
|---|---|---|
| `vtnet0` | QEMU user network, guest SSH forwarded to `127.0.0.1:2222` | management |
| `vtnet1` | `tap0` (created by the script) | EtherCAT segment |

The script replaces `mkqnximage --run`, which needs a host bridge (`virbr0`/`br0`).

**Software-emulation (no KVM) CPU model.**
- With `-cpu max` or a plain Skylake model, OpenSSL inside the guest takes AVX code paths that crash `sshd` and `ssh-keygen` at boot. The console shows `Memory fault (core dumped)` after "Starting sshd".
- An older model (Nehalem) does not boot QNX 8.0 at all.
- The script therefore uses `Skylake-Client-noTSX-IBRS,avx=off,avx2=off,fma=off,f16c=off` when `/dev/kvm` is unavailable. Override it with `QEMU_CPU=...`.

Console access when SSH is down:

```bash
./scripts/qnx-qemu.sh console ~/qnx-target "pidin -f an"    # output in ~/qnx-target/output/qemu.out
```

## 6. Start the virtual EtherCAT segment

```bash
cd ~/etherq/sim
sudo python3 -m ecatsim --iface tap0 --http 127.0.0.1:8080 --master-label "EtherQ on QNX 8.0"
```

The dashboard is at http://127.0.0.1:8080/. From a laptop, use `ssh -L 8080:127.0.0.1:8080 ubuntu@<host>`.

## 7. Run on QNX

```bash
./scripts/qnx-qemu.sh scp build/qnx-x86_64/install/bin/*     # -> /tmp on the target
./scripts/qnx-qemu.sh ssh
```

```sh
cd /tmp
./slaveinfo vtnet1                    # upstream SOEM on QNX: 3 slaves, expected WKC 6
./ecatd -i vtnet1 -c 4000 &           # 4 ms cycle (emulated target)
./ecatctl slaves
cat /dev/ecat/status
./axisdemo                            # servo enables and follows a sine profile
```

### Automated end-to-end check

```bash
./scripts/target-smoke.sh            # logs and PASS/FAIL in build/target-smoke/
```

It verifies:
- QNX 8.0 identity;
- `slaveinfo` (3 slaves, WKC 6);
- `BIOCSMMWRITE` accepted;
- bus in OP;
- servo enabled by `axisdemo`;
- SIGKILL of the client → its outputs zeroed and the servo disabled while the bus stays OP;
- EBUSY for a second writer;
- the watchdog trip on a 1 s freeze, and recovery to OP.

### Scenarios

| Scenario | Command (QNX) | Result observed on 2026-11-16 |
|---|---|---|
| Hold an output | `./ecatctl hold 0 0xA5` | byte owned until Ctrl-C |
| Crash safety | `slay -s SIGKILL axisdemo` | `6 output bytes zeroed and released`; servo SWITCH ON DISABLED; bus stays OP |
| Ownership | `./ecatctl hold 0 0xff` while `blink` runs | `pwrite: Resource busy` |
| Watchdog | `slay -s SIGSTOP ecatd; sleep 1; slay -s SIGCONT ecatd` | slaves 2+3 SAFE-OP 0x001B, back to OP within 2 s |
| Least privilege | `./ecatd -i vtnet1 -c 4000 -U 1000:1000 &` | runs as uid 1000 with locked abilities and keeps cycling |
| Multi-packet writes | `ETHERQ_BPF_MMWRITE=1 ./ecatd -i vtnet1 -c 4000 &` | `BIOCSMMWRITE enabled on vtnet1`; bus OP, WKC 6 |

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `License key is not activated` | `qnxsoftwarecenter_clt ... -syncLicenseKeys -activateAll` |
| GUI Software Center: `Could not load SWT library` | headless host: use `qnxsoftwarecenter_clt` |
| `mkqnximage`: directory "neither ... nor an empty directory" | start in an empty directory (don't write logs into it) |
| SSH to the guest: `Connection reset` / banner timeout | `sshd` crashed (CPU model, see step 5) or the guest is wedged: check the console, then `qnx-qemu.sh stop` + `start` |
| `cannot open vtnet1 on BPF` | check `ifconfig -a`; run ecatd as root |
| `no EtherCAT slaves found` | is ecatsim running on `tap0`? `sudo tcpdump -i tap0 ether proto 0x88a4` |
| Occasional WKC errors | the emulated guest sometimes misses SOEM's 2 ms reply timeout (about 0.1 % of cycles observed); no effect on state |
