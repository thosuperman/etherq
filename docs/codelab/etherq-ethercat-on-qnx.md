id: etherq-ethercat-on-qnx
title: EtherCAT on QNX 8.0 without hardware - a fieldbus as a resource manager
summary: Run an open-source EtherCAT master on QNX OS 8.0 in QEMU against a virtual EtherCAT segment, drive I/O from your own client, and see how QNX resource managers turn a client crash into a safe stop.
categories: qnx, qemu, ethercat, resource-manager, real-time
tags: intermediate
difficulty: 2
status: published
authors: EtherQ contributors
feedback_link: https://github.com/<owner>/etherq/issues


# EtherCAT on QNX 8.0 without hardware

<!-- All QNX steps verified by the author on QNX 8.0 / QEMU (software emulation), 2026-11-16. "Recorded" output is real captured output. -->

## Introduction
Duration: 3:00

EtherCAT is the real-time fieldbus inside many robots, CNC machines and motion controllers. A master sends one Ethernet frame per cycle that passes through every device on the bus. Each device reads its outputs from the frame and inserts its inputs as the frame flies by. If the master misses its cycles, every device's watchdog stops the machine.

In this codelab you run the open-source **SOEM** master on **QNX OS 8.0** in QEMU against **ecatsim**, a virtual EtherCAT segment, so no hardware is needed. Then you write a tiny client that blinks LEDs over EtherCAT and crash it on purpose.

### What you'll learn
* How a QNX program sends and receives **raw Ethernet frames** through io-sock's Berkeley Packet Filter (`/dev/bpf`), since QNX has no `AF_PACKET`.
* How a periodic real-time loop is driven by a **timer pulse** (`timer_create` + `SIGEV_PULSE_INIT` → `MsgReceivePulse`).
* How a **resource manager** turns process data into files (`/dev/ecat/in`, `/dev/ecat/out`) that any process can use with `open`, `pread`, `pwrite`, `devctl` and `poll`.
* Why a microkernel makes **client crashes safe**: QNX closes a dead process's files, the resource manager sees it, and outputs drop to zero within one cycle.

### What you'll build
```
 QNX 8.0 in QEMU                                   Linux host
 blink ─┐                                         ┌──────────────────┐
        ├─ /dev/ecat ─ ecatd ─ SOEM ─ /dev/bpf ─ vtnet1 ══ tap0 ══ ecatsim  │
 ecatctl┘   (resource manager)                    │ DI8 · DO8 · servo │
                                                  └──────────────────┘
```

### Prerequisites
* An x86_64 machine or cloud VM with **Ubuntu 22.04 or 24.04**. KVM is optional: without it QEMU emulates the CPU (TCG), which is slower but works.
* A free non-commercial **QNX SDP 8.0** license ([QNX Everywhere](https://qnx.software/en/developers/get-started/qnx-everywhere)), deployed to your myQNX account in the License Manager.
* [QNX Software Center](https://www.qnx.com/download/group.html?programid=29178) with QNX SDP 8.0 installed in `~/qnx800`.
* Basic C and shell skills.

---

## Get the code and prepare the host
Duration: 5:00

```bash
git clone https://github.com/<owner>/etherq.git
cd etherq
./scripts/provision-ubuntu.sh
```

The script installs CMake, Ninja, QEMU and Python 3, and tells you whether KVM is available:

```
KVM: not available - QEMU will use TCG software emulation (slower; timing is not representative)
cmake version 3.28.3
QEMU emulator version 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.18)
Python 3.12.3
provisioning done
```

Negative
: Timing measured inside an emulated (TCG) QNX guest describes the emulator, not QNX. In this codelab we look at **behaviour**, not microseconds.

---

## Install QNX SDP 8.0 (QNX Everywhere)
Duration: 20:00

1. Request the free non-commercial license at [QNX Everywhere](https://qnx.software/en/developers/get-started/qnx-everywhere). A new myQNX account can take a few hours to finish processing.
2. In **myQNX License Manager**, click **"Click to deploy to user/s"** next to *QNX Software Development Platform 8.0* and deploy it to yourself. Downloads stay locked until you do this.
3. Download **QNX Software Center 2.0.4 – Linux Hosts** (`qnx-setup-2.0.4-202501021438-linux.run`) and install it. You accept the QNX Development License Agreement here:

```bash
chmod +x qnx-setup-2.0.4-202501021438-linux.run
./qnx-setup-2.0.4-202501021438-linux.run     # installs into ~/qnx/qnxsoftwarecenter
```

On a server without a desktop, the graphical Software Center fails with `Could not load SWT library`. That's fine: use the command-line client. Reading your credentials with `read -s` keeps them off the screen and out of shell history:

```bash
read -p "myQNX email: " QU; read -s -p "myQNX password: " QP; echo
C=~/qnx/qnxsoftwarecenter/qnxsoftwarecenter_clt
$C -myqnx.user "$QU" -myqnx.password "$QP" -syncLicenseKeys -activateAll
$C -offline -listLicenseKeys
```

Recorded output (key masked):

```
[XXXX-XXXX-XXXX-XXXX-XXXX] - Activated - QNX Software Development Platform 8.0 ...
```

```bash
$C -myqnx.user "$QU" -myqnx.password "$QP" -installBaseline com.qnx.qnx800 -destination ~/qnx800
unset QU QP
source ~/qnx800/qnxsdp-env.sh && qcc -V
```

```
cc: targets available in /home/ubuntu/qnx800/host/linux/x86_64/etc/qcc:
	12.2.0,gcc_ntoaarch64le
	12.2.0,gcc_ntox86_64	(default)
	...
```

Negative
: QNX Software Center 2.0.4 writes the **full command line, including your password**, into `~/.qnx/swupdate/*.log`. Redact it right after installing, and never commit or screenshot those logs:
`grep -rlE -- "-myqnx\.password [^*]" ~/.qnx | xargs -r sed -i -E "s/(-myqnx\.(password|user) )[^ ]+/\1***/g"`

---

## Meet the virtual EtherCAT segment
Duration: 4:00

ecatsim behaves like a chain of three EtherCAT slaves on a network interface:

| # | Device | Process data |
|---|---|---|
| 1 | EQ-DI8, 8 digital inputs | 1 input byte |
| 2 | EQ-DO8, 8 digital outputs | 1 output byte (SyncManager watchdog enabled) |
| 3 | EQ-AX1, CiA 402 servo axis | 6 output bytes (controlword, target), 10 input bytes (statusword, position, velocity) |

Each simulated slave has the registers a master talks to:
* an SII EEPROM with its identity and PDO mapping;
* the AL state machine (INIT → PRE-OP → SAFE-OP → OP);
* FMMUs that map logical addresses to device memory;
* a watchdog that drops the slave to SAFE-OP if outputs stop arriving.

The devices are fictitious ("EtherQ Virtual", vendor `0x45514E58`).

Run its unit tests:

```bash
cd sim && python3 -m pytest -q tests && cd ..
```

```
..........................                                               [100%]
26 passed
```

---

## Check the simulator with upstream SOEM on Linux
Duration: 5:00

Before touching QNX, prove that the virtual segment speaks real EtherCAT. This step builds **unmodified** SOEM v2.0.1 for Linux and runs it against ecatsim over a veth pair:

```bash
docker build -f docker/linux-integration.Dockerfile -t etherq-linux-integration .
docker run --rm --cap-add NET_ADMIN etherq-linux-integration
```

The tail of the output (recorded on 2026-11-15):

```
PASS: slaveinfo finds 3 slaves
PASS: slave 1 name from SII strings
PASS: slave 3 name from SII strings
PASS: vendor ID read from EEPROM
PASS: simple_ng reaches OPERATIONAL
cyclic exchanges: 10000, wrong WKC: 0, late replies (>2 ms): 7
PASS: no wrong working counter in 10000 cycles
PASS: late replies within 10 per mille
```

The **working counter (WKC)** is how an EtherCAT master knows every device did its job. Each slave adds 1 when it supplies inputs and 2 when it accepts outputs. Three slaves with this mapping give an expected WKC of 6.

---

## Build EtherQ for QNX 8.0
Duration: 5:00

Load the SDP environment and cross-compile:

```bash
source ~/qnx800/qnxsdp-env.sh
./scripts/qnx-build.sh x86_64
```

CMake downloads SOEM v2.0.1 by URL with a pinned SHA-256. It then copies the QNX port from `port/soem/` on top of it. The port **only adds files**:

```
port/soem/cmake/QNX.cmake       platform file SOEM includes for CMAKE_SYSTEM_NAME=QNX
port/soem/osal/qnx/             clocks, SCHED_FIFO threads, priority-inheritance mutexes
port/soem/oshw/qnx/nicdrv.c     raw EtherCAT frames over io-sock BPF
```

Recorded output (2026-11-16, SDP 8.0, 0 compiler warnings):

```
ecatd      ELF 64-bit LSB pie executable, x86-64, ..., interpreter /usr/lib/ldqnx-64.so.
ecatctl    ELF 64-bit LSB pie executable, x86-64, ..., interpreter /usr/lib/ldqnx-64.so.
axisdemo   ELF 64-bit LSB pie executable, x86-64, ..., interpreter /usr/lib/ldqnx-64.so.
slaveinfo  ELF 64-bit LSB pie executable, x86-64, ..., interpreter /usr/lib/ldqnx-64.so.
simple_ng  ELF 64-bit LSB pie executable, x86-64, ..., interpreter /usr/lib/ldqnx-64.so.
```

`ldqnx-64.so` is the QNX dynamic loader: these are QNX binaries.

---

## How QNX sends raw Ethernet frames
Duration: 4:00

EtherCAT frames are plain Ethernet frames with EtherType `0x88A4`. They carry no IP and no UDP. On QNX you reach the wire through **BPF**, the FreeBSD packet filter that io-sock provides as `/dev/bpf`. Open `port/soem/oshw/qnx/nicdrv.c` and find `ec_bpf_open()`:

```c
fd = open("/dev/bpf", O_RDWR);
ioctl(fd, BIOCSBLEN, &blen);          /* buffer size: before BIOCSETIF   */
ioctl(fd, BIOCSETIF, &ifr);           /* attach to the NIC, e.g. vtnet1   */
ioctl(fd, BIOCIMMEDIATE, &on);        /* hand each frame over at once     */
ioctl(fd, BIOCSHDRCMPLT, &on);        /* we write the full Ethernet header */
ioctl(fd, BIOCSDIRECTION, &dir);      /* only frames coming *in*          */
ioctl(fd, BIOCSETF, &prog);           /* kernel filter: EtherType 0x88A4  */
```

The filter program runs inside io-sock, so your process only ever sees EtherCAT frames:

```c
BPF_STMT(BPF_LD + BPF_H + BPF_ABS, 12),                 /* load EtherType      */
BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, 0x88A4, 0, 1),      /* EtherCAT?           */
BPF_STMT(BPF_RET + BPF_K, (u_int)-1),                   /* yes: whole frame    */
BPF_STMT(BPF_RET + BPF_K, 0),                           /* no: drop            */
```

Two details are easy to get wrong:
* One `read()` from BPF can return **several** frames, each behind a `bpf_hdr`. The driver keeps a staging buffer and walks it with `BPF_WORDALIGN`.
* If your own transmitted frames came back to you, a request would look like a reply with WKC 0. The direction filter prevents that.

Positive
: QNX extends BPF with **multi-packet writes** (`BIOCSMMWRITE`): one `writev()` can send several frames, each prefixed by a `bpf_whdr`. EtherQ can batch all frames of a cycle this way; enable it with `ETHERQ_BPF_MMWRITE=1`.

---

## Boot QNX in QEMU with an EtherCAT NIC
Duration: 8:00

The EtherCAT segment needs its own network interface, separate from the one you use for SSH. Build the QNX image (in an **empty** directory), then boot it with the EtherQ launcher:

```bash
test -f ~/.ssh/id_ed25519 || ssh-keygen -q -t ed25519 -N "" -f ~/.ssh/id_ed25519
mkdir ~/qnx-target && cd ~/qnx-target
~/qnx800/host/common/bin/mkqnximage --type=qemu --arch=x86_64 \
    --ssh-ident=$HOME/.ssh/id_ed25519.pub --noprompt --build
cd ~/etherq
./scripts/qnx-qemu.sh start ~/qnx-target
```

The launcher gives the guest two virtio NICs:
* `vtnet0`: SSH, forwarded to `127.0.0.1:2222`.
* `vtnet1`: connected to a host tap device `tap0`. This is the EtherCAT network.

Negative
: Without KVM, QEMU's default `-cpu max` makes OpenSSL inside QNX crash `sshd` at boot ("Memory fault" on the console). The launcher picks a Skylake CPU model with AVX disabled, which works.

Copy the binaries and check the interfaces:

```bash
./scripts/qnx-qemu.sh scp build/qnx-x86_64/install/bin/*
./scripts/qnx-qemu.sh ssh "uname -a; ifconfig vtnet1 | head -1"
```

Recorded output:

```
QNX noname 8.0.0 2026/10/30-09:10:05EDT x86pc x86_64
vtnet1: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST> metric 0 mtu 1500
```

---

## Start the segment and scan it from QNX
Duration: 4:00

On the **host**, start ecatsim on `tap0` with its dashboard:

```bash
cd ~/etherq/sim
sudo python3 -m ecatsim --iface tap0 --http 127.0.0.1:8080 --master-label "EtherQ on QNX 8.0"
```

Open http://127.0.0.1:8080/. Three slaves are shown in INIT. On a cloud VM, use `ssh -L 8080:127.0.0.1:8080 ...` and open the same URL on your laptop.

On **QNX**, run plain SOEM's `slaveinfo`, now built for QNX:

```sh
cd /tmp
./slaveinfo vtnet1
```

Recorded output on QNX 8.0 (excerpt):

```
SOEM (Simple Open EtherCAT Master)
Slaveinfo
Starting slaveinfo
ecx_init on vtnet1 succeeded.
3 slaves found and configured.
Calculated workcounter 6

Slave:1
 Name:EQ-DI8 8Ch Dig In (virtual)
 ...
Slave:3
 Name:EQ-AX1 Servo Axis (virtual)
 Output size: 48bits
 Input size: 80bits
 Man: 45514e58 ID: 00000003 Rev: 00010000
```

That is unmodified SOEM code talking to the bus through the QNX BPF driver you just read.

---

## The real-time cycle: a timer that sends pulses
Duration: 5:00

Now start the EtherQ daemon. It brings the bus to OP and keeps it there:

```sh
./ecatd -i vtnet1 -c 4000 &
```

Recorded output:

```
ecatd: 3 slaves on vtnet1, 7 output bytes, 11 input bytes, expected WKC 6
ecatd: cycle 4000 us at priority 50; serving /dev/ecat/in /dev/ecat/out /dev/ecat/status
```

`-c 4000` is a 4 ms cycle, a comfortable value for an emulated target. Open `src/ecatd/master.c` and look at `cycle_thread()`:

```c
chid = ChannelCreate(_NTO_CHF_PRIVATE);
coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
SIGEV_PULSE_INIT(&ev, coid, d->cycle_prio, PULSE_CODE_TICK, 0);   /* pulse carries priority */
timer_create(CLOCK_MONOTONIC, &ev, &tid);
timer_settime(tid, TIMER_ABSTIME, &its, NULL);                    /* periodic */

while (d->running) {
    MsgReceivePulse(chid, &pulse, sizeof(pulse), NULL);           /* wait for the tick */
    /* copy outputs -> exchange frame (LRW) -> snapshot inputs -> notify clients */
}
```

Why a pulse rather than a signal or `sleep()`?
* A pulse is a tiny, non-blocking **message** that carries a priority. The receiving thread runs at the cycle priority, with no signal handler involved.
* The loop computes which period it is in from the clock. If a tick is ever missed, it is **counted** (`missed periods`), not hidden.

Look at the result:

```sh
./ecatctl slaves
cat /dev/ecat/status
```

On the dashboard all three slaves turn green: **OP**.

---

## Write your own client: blink LEDs over EtherCAT
Duration: 8:00

`/dev/ecat` is served by a **resource manager**, a normal process that registered those path names with the QNX process manager. Your program talks to it with ordinary POSIX calls, which QNX turns into messages. Here is the core of `examples/blink/blink.c`:

```c
int in  = open("/dev/ecat/in",  O_RDONLY);
int out = open("/dev/ecat/out", O_RDWR);

/* where is the DO8 in the output image? (devctl = a message to ecatd) */
ecat_slave_t s = { .index = 2 };
devctl(in, DCMD_ECAT_GET_SLAVE, &s, sizeof s, NULL);

struct pollfd pfd = { .fd = in, .events = POLLIN };
for (;;) {
    poll(&pfd, 1, 1000);             /* wakes once per completed bus cycle */
    pread(in, &b, 1, 0);             /* mark this cycle as seen            */
    if (++cycle % 25) continue;
    led = 1u << pos;                 /* move the light                     */
    pwrite(out, &led, 1, s.ooffset); /* writing claims this byte for us    */
    ...
}
```

Run it (it was built along with the rest):

```sh
./blink &
```

On the dashboard the DO8 LEDs sweep back and forth.

How does `poll()` know a cycle finished? After every cycle, ecatd's real-time thread sends a pulse to its own resource-manager channel. The handler calls `iofunc_notify_trigger()`. That wakes every client waiting in `poll()` or `select()` on `/dev/ecat/in`.

Positive
: Try a second writer: `./ecatctl hold <offset> 0xff`. It fails with **EBUSY**, because `blink` owns that byte. Output bytes have exactly one owner.

---

## Crash it on purpose
Duration: 4:00

Real machines must stay safe when software fails. Kill `blink` with no chance to clean up:

```sh
slay -s SIGKILL blink
```

The LEDs go dark within one cycle, and ecatd logs (recorded):

```
ecatd: client closed /dev/ecat/out: 1 output bytes zeroed and released
```

When a process dies, the QNX kernel closes all its connections. For every file the dead process had open on `/dev/ecat/out`, ecatd's `io_close_ocb()` handler runs:

```c
released = eq_outputs_release(&g_d->out, ocb);   /* zero + free the bytes it owned */
```

The next cycle sends zeros. No heartbeat protocol or polling is needed: the microkernel's message passing guarantees ecatd hears about the death. Do the same with the servo:

```sh
./axisdemo &            # servo enables and follows a sine wave on the dashboard
slay -s SIGKILL axisdemo
```

The controlword drops to 0, which in CiA 402 means "disable voltage". The drive goes to **SWITCH ON DISABLED**, while the rest of the bus stays in OP.

---

## Why the master must keep time
Duration: 3:00

Freeze the master for a moment:

```sh
slay -s SIGSTOP ecatd; sleep 1; slay -s SIGCONT ecatd
```

On the dashboard, slaves 2 and 3 drop to **SAFE-OP** with AL status code **0x001B** (SyncManager watchdog). Their outputs stopped arriving for more than 100 ms. After you continue ecatd, its supervisor thread acknowledges the error and brings them back to OP. Check it:

```sh
./ecatctl stats
```

This is the core of the real-time story. An EtherCAT master that misses its deadline does not just run slowly: the machine stops. That is why the master belongs on a real-time OS with fixed priorities and bounded scheduling.

---

## Verify everything automatically
Duration: 5:00

Everything you just did by hand is also an automated test. From the host:

```bash
cd ~/etherq
./scripts/target-smoke.sh
```

Recorded result (2026-11-16, QNX 8.0 in QEMU, simulated devices):

```
PASS: slaveinfo on QNX finds 3 slaves
PASS: SIGKILL: ecatd released the client's 6 output bytes
PASS: servo disabled after client crash
PASS: bus stayed in OP after client crash
PASS: second writer gets EBUSY
PASS: slaves with outputs dropped to SAFE-OP 0x001B
PASS: supervisor recovered all slaves to OP
PASS: non-root user refused (/dev/ecat/out is 0660 root)
PASS: ecatd dropped to uid 1000 with locked abilities
PASS: SIGTERM: orderly shutdown message
...
passed: 26
failed: 0
```

Negative
: What this proves and what it does not: the behaviour is **functionally** correct on QNX 8.0 against simulated devices. It says nothing about timing on real hardware: an emulated CPU is not a real-time platform. It is also not a certified safety function.

---

## Troubleshooting
Duration: 2:00

| Symptom | Fix |
|---|---|
| `QNX_HOST/QNX_TARGET not set` | `source ~/qnx800/qnxsdp-env.sh` |
| `cannot open vtnet1 on BPF` | check the name with `ifconfig -a`; run as root |
| `no EtherCAT slaves found` | is ecatsim running on `tap0`? Check on the host with `sudo tcpdump -i tap0 ether proto 0x88a4` |
| slaves bounce to SAFE-OP `0x001b` | the emulated guest is too slow for the cycle; use `-c 8000` |
| `blink: pwrite: Device or resource busy` | another client owns the DO8 byte; stop it first |

---

## Next steps
Duration: 1:00

You ran an EtherCAT master on QNX 8.0 and drove I/O from an isolated client. You also saw a crash turn into a safe stop. Where to go next:
* Read `src/ecatd/resmgr.c` to see the whole resource manager: OCBs, `io_read`, `io_write`, `io_notify` and `devctl`.
* Try `ecatd -U 100:100` to drop root after start-up with locked procmgr abilities.
* Add a device model to `sim/ecatsim/devices.py`, then write a client for it.
* Real hardware: the same binaries target a PC NIC wired to real EtherCAT slaves. This is not yet tested by the author.

EtherCAT® is a registered trademark and patented technology, licensed by Beckhoff Automation GmbH, Germany.
