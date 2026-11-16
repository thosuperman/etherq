# SPDX-License-Identifier: GPL-3.0-or-later
"""Command line entry point: ``python3 -m ecatsim --iface tap0``."""
from __future__ import annotations

import argparse
import gc
import logging
import select
import signal
import sys
import time

from . import __version__
from .dashboard import start_dashboard
from .devices import DigitalInput8, DigitalOutput8, ServoAxis
from .segment import Segment
from .transport import RawTransport, UdpTransport

log = logging.getLogger("ecatsim")


def build_devices(spec: str):
    makers = {"di8": DigitalInput8, "do8": DigitalOutput8, "servo": ServoAxis}
    devices = []
    for i, name in enumerate(s.strip() for s in spec.split(",") if s.strip()):
        if name not in makers:
            raise SystemExit(f"unknown device '{name}' (choose from {', '.join(makers)})")
        devices.append(makers[name](serial=101 + i))
    return devices


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="ecatsim", description=__doc__)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--iface", help="Linux interface to serve raw EtherCAT frames on (e.g. tap0, veth1)")
    g.add_argument("--udp", metavar="HOST:PORT", help="serve EtherCAT-over-UDP instead (e.g. 0.0.0.0:34980)")
    ap.add_argument("--devices", default="di8,do8,servo",
                    help="comma-separated slave chain in wire order (default: di8,do8,servo)")
    ap.add_argument("--http", default="127.0.0.1:8080",
                    help="dashboard bind address, or 'off' (default: 127.0.0.1:8080)")
    ap.add_argument("--master-label", default="EtherCAT master",
                    help="label shown for the master in the dashboard (e.g. 'EtherQ on QNX 8.0')")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--version", action="version", version=f"ecatsim {__version__}")
    args = ap.parse_args(argv)

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(name)s %(levelname)s %(message)s")
    segment = Segment(build_devices(args.devices))
    if args.iface:
        transport = RawTransport(args.iface)
        where = f"interface {args.iface}"
    else:
        host, _, port = args.udp.rpartition(":")
        transport = UdpTransport(host or "0.0.0.0", int(port))
        where = f"UDP {args.udp}"
    log.info("virtual EtherCAT segment with %d slaves on %s", len(segment.escs), where)
    for i, esc in enumerate(segment.escs, 1):
        log.info("  slave %d: %s", i, esc.desc.name)

    server = None
    if args.http != "off":
        host, _, port = args.http.rpartition(":")
        server = start_dashboard(segment, host, int(port), args.master_label)
        log.info("dashboard: http://%s:%s/", host, port)

    stop = {"flag": False}

    def _stop(*_a):
        stop["flag"] = True

    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)

    # Fewer, shorter GC pauses: frame processing allocates many short-lived objects.
    gc.freeze()
    gc.set_threshold(50_000, 50, 100)
    last_tick = time.monotonic()
    states = {}
    while not stop["flag"]:
        ready, _, _ = select.select([transport], [], [], 0.005)
        if ready:
            raw, peer = transport.recv()
            if raw:
                reply = segment.process(raw)
                if reply is not None:
                    transport.send(reply, peer)
        now = time.monotonic()
        if now - last_tick >= 0.01:
            segment.tick()
            last_tick = now
            for i, esc in enumerate(segment.escs, 1):
                cur = (esc.state_name, esc.al_error, esc.al_code)
                if states.get(i) != cur:
                    states[i] = cur
                    log.info("slave %d -> %s%s", i, esc.state_name,
                             f" ERROR 0x{esc.al_code:04x}" if esc.al_error else "")
    transport.close()
    if server:
        server.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
