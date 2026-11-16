# EtherQ demo video: how it was made (2:37)

The video is **produced** (`EtherQ_Demo.mp4`, 1920×1080, 30 fps, 2:37, H.264/AAC, -16.4 LUFS). Captions: [`EtherQ_Demo.srt`](EtherQ_Demo.srt). It is not committed here because of its size; it is published on YouTube by the author.

## What is on screen

- **Footage:** two real screen recordings made on 2026-11-16 on a Linux host. A virtual X display shows xterm (left) and Chromium (right). There is no mock-up and no staged output.
  - **Take 1:** QNX OS 8.0 in QEMU (software emulation), driven over SSH. Every command is typed on screen exactly as it is executed on QNX. On the right is the ecatsim dashboard.
  - **Take 2:** the engineering proof on the host: the QNX SDP toolchain, the build, the safe-stop code and the automated end-to-end test (26/26).
- **Permanent labels** in the caption band: "Real QNX 8.0 · QEMU (software emulation)" and "Simulated EtherCAT devices".
- **Speed changes:** waiting time is sped up by at most 2× and labelled on screen.
- **Proof scene:** the 91 s test run is shown as its start, then a hard cut to its final lines.
- **Narration:** a synthetic voice (Kokoro TTS, voice `am_michael`, Apache-2.0 model, generated locally). The end card says it is synthetic.
- **No music, no third-party footage or logos.** "EtherCAT" is used descriptively, with the trademark notice on the end card.
- **No timing figures are presented as QNX performance.**

## Scenes

| # | Scene | Footage | Narration (from `tools/video/narration.json`) |
|---|---|---|---|
| 1 | Problem | title card | Crashing motion application; the motor must not keep acting; the machine must keep running |
| 2 | Meet EtherQ | architecture card, then the real QNX shell | SOEM on QNX, independent daemon, resource manager `/dev/ecat`; real QNX 8.0 in QEMU, simulated devices |
| 3 | Discover | `slaveinfo` on QNX (3 slaves, WKC 6) → `ecatd` → dashboard to OP | BPF port; timer-pulse cycle at a fixed real-time priority |
| 4 | Drive | `axisdemo` → servo OPERATION ENABLED, sine profile | CiA 402 enable and profile |
| 5 | **Kill** | `slay -s SIGKILL axisdemo` → "6 output bytes zeroed and released" → servo SWITCH ON DISABLED, all slaves OP (green highlights) | Why: QNX closes the connection, ecatd zeroes and releases the outputs |
| 6 | Reliability | `ecatctl hold` → Resource busy; 1 s SIGSTOP of ecatd → SAFE-OP 0x001B → recovered; `-U 1000:1000` → uid 1000 | Ownership, watchdog recovery, least privilege |
| 7 | Proof | `qcc -V`, QNX build, `file ecatd`, the `eq_outputs_release` code, smoke test 26 passed / 0 failed | Real QNX build, zero warnings, every check passes |
| 8 | Closing | end card | Contribution and next step (hardware validation); honesty notes |

## Reproduce

On the Linux host (QNX target running, binaries built):

```bash
tools/video/record.sh                      # records build/video/takes/{demo,build}.mp4 + markers
```

Then, on a machine with ffmpeg, Pillow and the Kokoro TTS environment:

```bash
python3 tools/video/build.py build/video/takes
```

- `tools/video/stage.sh`: Xvfb + xterm + Chromium + ffmpeg x11grab.
- `tools/video/demo_session.sh`: the scripted, real terminal session.
- `tools/video/build.py`: the edit (cards, speed limits, highlights, captions, narration mix).

Recording needs 4 vCPUs. On 2 vCPUs, the QEMU emulation, the browser and the encoder compete, the guest misses cycles, and the take shows spurious watchdog trips.
