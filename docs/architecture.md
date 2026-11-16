# Architecture

## Goals

1. Run the proven SOEM master on QNX OS 8.0 without forking it.
2. Make fieldbus I/O a QNX-native service: many independent processes, and no process can corrupt the cycle.
3. Make the whole stack testable without hardware, both locally and in CI.

## Components and boundaries

```
           client processes (any number, crash-isolated)
      axisdemo        ecatctl        your application
          │  open / pread / pwrite / devctl / poll (QNX message passing)
          ▼
 ┌──────────────────────────── ecatd ─────────────────────────────┐
 │ resource-manager thread pool (dispatch, iofunc)                 │
 │   /dev/ecat/in      read, notify (one wake-up per cycle)        │
 │   /dev/ecat/out     write = claim bytes (eq_outputs_*)          │
 │   /dev/ecat/status  text                                        │
 │          ▲ pulse "new cycle"                │ staged outputs    │
 │          │                                  ▼ (PI mutex)        │
 │ cycle thread  SCHED_FIFO P=50                                   │
 │   timer pulse → copy outputs → LRW exchange → snapshot inputs   │
 │ supervisor    SCHED_FIFO P=20, every 100 ms                     │
 │   AL states, SAFE-OP+ERROR ack, reconfigure/recover lost slaves │
 │ SOEM v2.0.1 core  ·  osal/qnx  ·  oshw/qnx (BPF)                │
 └───────────────────────────────┬─────────────────────────────────┘
                                 │ /dev/bpf (io-sock)
                                 ▼
                         NIC (vtnet / em) ── EtherCAT segment (real or ecatsim)
```

### SOEM port (`port/soem/`)

SOEM picks its platform layer with `include(cmake/${CMAKE_SYSTEM_NAME}.cmake)`. The port adds `cmake/QNX.cmake`, `osal/qnx/` and `oshw/qnx/`, and changes no upstream file. The top-level `CMakeLists.txt` fetches the pinned SOEM tarball and copies the overlay over it.

| Concern | Linux SOEM | QNX port | Why |
|---|---|---|---|
| Raw frames | `AF_PACKET` socket | `/dev/bpf` (io-sock BPF) | QNX has no `AF_PACKET`; BPF is the supported mechanism |
| Frame filter | bind to EtherType | kernel BPF program `ldh [12]; jeq #0x88a4` | only EtherCAT frames are copied to user space |
| Own transmissions | not looped back (ptype bind) | `BIOCSDIRECTION(BPF_D_IN)`/`BIOCSSEESENT 0`; source-MAC check as fallback | a looped-back request would look like a reply with WKC 0 |
| Batching | one `send()` per frame | optional `BIOCSMMWRITE`: frames queued and sent in one `writev()` with `bpf_whdr` headers, flushed before the first receive | fewer kernel round trips when a cycle spans several frames |
| RT thread | create, then `pthread_setschedparam` | created with `PTHREAD_EXPLICIT_SCHED` + `SCHED_FIFO` | the thread never runs at the wrong priority |
| Receive | `recv()` one frame | one `read()` may return several `bpf_hdr`-prefixed frames, staged in `ec_bpfrxt` | BPF buffer semantics |

### Cycle (`src/ecatd/master.c`)

- A `CLOCK_MONOTONIC` timer delivers a **pulse** to a private channel. The pulse carries the cycle priority, so the receiving thread runs at that priority.
- The cycle index is derived from time (`(now - start) / period`), not from counting pulses. Lost or merged pulses therefore show up as `missed_periods`.
- **Wake latency** is the actual start minus the scheduled start, and **exec** is the cycle duration. Both come from the target clock and are reported as *measured on this target*.
- When WKC is lower than expected, the cycle sets `need_check` for the supervisor. It never runs recovery itself, so its duration stays bounded.

### Resource manager (`src/ecatd/resmgr.c`)

- The custom OCB records which cycle the client last read (`seen_cycle`) for `io_notify`. `poll()`/`select()` on `/dev/ecat/in` becomes readable once per new cycle.
- **Byte ownership** is handled by `eq_outputs_write()`: all bytes or nothing, and `EBUSY` if another client owns any of them.
- **Safe stop:** QNX sends `io_close_ocb` for every open file when a process exits, including after `SIGKILL`. `eq_outputs_release()` zeroes that client's bytes, and the next cycle (at most one period later) sends zeros. For a CiA 402 drive, controlword 0 means "disable voltage".
- Data is shared with the cycle thread under one priority-inheritance mutex, held only for `memcpy` of at most 4 KiB.

### Privileges (`src/ecatd/privileges.c`)

With `-U uid:gid`, after BPF is open, paths are attached and threads exist, ecatd:
1. keeps `PROCMGR_AID_PRIORITY` (locked);
2. permits `setuid`/`setgid` only for the switch;
3. denies and locks everything else;
4. becomes an unprivileged user.

### ecatsim (`sim/ecatsim/`)

| Module | Role |
|---|---|
| `protocol.py` | Frame/datagram codec; preserves length and padding |
| `sii.py` | SII EEPROM image: header, CRC-8, STRINGS/GENERAL/FMMU/SYNCM/TXPDO/RXPDO categories |
| `esc.py` | ESC: register space, read-only ranges, EEPROM interface (8-byte reads), AL state machine with error/ack, SM configuration check on PRE-OP→SAFE-OP, bit-granular FMMU mapping, SM watchdog (`(div+2)·40 ns · time`, default 100 ms) |
| `devices.py` | DI8, DO8 (outputs drop to 0 outside OP), CiA 402 servo (state machine, CSP following with velocity/acceleration limits, following-error fault, fault on leaving OP) |
| `segment.py` | Wire-order processing: auto-increment and configured addressing, broadcast OR, logical WKC rules (LRW read +1 / write +2), cable-break simulation, stats |
| `transport.py` | Linux raw socket (bound to 0x88A4, so no self-loop); EtherCAT-over-UDP fallback |
| `dashboard.py` / `.html` | Stdlib HTTP + Server-Sent Events, fault injection endpoints |

Its fidelity is validated by running **unmodified upstream SOEM** against it (see `docs/testing.md`).
