# Security policy

## Reporting a vulnerability

Please report security issues privately through GitHub's **"Report a vulnerability"** (Security → Advisories) on this repository, not in public issues. Include affected files, a reproduction and the impact. Expect an acknowledgement within 7 days.

## Scope and threat model

EtherQ is a research/prototype EtherCAT master. **It is not certified for functional safety** (IEC 61508, ISO 13849, IEC 61800-5-2 and similar) and must not be the only protective measure for machinery.

| Surface | Considerations |
|---|---|
| `ecatd` raw network access | It opens `/dev/bpf`, which requires root. Run it on a dedicated NIC that is connected only to the EtherCAT segment. |
| `/dev/ecat/out` | Any process with write permission can command outputs. The node is created `0660 root:root`; widen access deliberately, for example with a dedicated group. Byte ownership prevents two clients from fighting over the same output, but it is **not** an authorization mechanism. |
| Privilege drop | `ecatd -U uid:gid` drops root after start-up and locks the remaining procmgr abilities. Prefer it in deployments. |
| Safe stop on client exit | When a client dies, ecatd zeroes the output bytes it owned within one cycle (verified on QNX 8.0 under QEMU). Whether zero is a *safe* value depends on the device: for CiA 402 drives, controlword 0 disables the power stage. Review this per device. |
| ecatsim | A test tool. Its dashboard binds to `127.0.0.1` by default and has no authentication. Do not expose it on untrusted networks. |

## Handling credentials during setup

QNX Software Center 2.0.4 records its full command line, including `-myqnx.password`, in log files under `~/.qnx/swupdate/`. After using `qnxsoftwarecenter_clt` with credentials:

```bash
grep -rlE -- "-myqnx\.password [^*]" ~/.qnx | xargs -r sed -i -E "s/(-myqnx\.(password|user) )[^ ]+/\1***/g"
```

Never commit these logs, and rotate your myQNX password if a log may have been exposed.
