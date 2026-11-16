# SPDX-License-Identifier: GPL-3.0-or-later
"""A chain of simulated slaves that processes EtherCAT frames like a real segment."""
from __future__ import annotations

import threading
import time
from collections import Counter, deque
from typing import Deque, Dict, List, Optional

from . import protocol as p
from .esc import Esc


class SegmentStats:
    def __init__(self) -> None:
        self.frames = 0
        self.rejected = 0
        self.commands: Counter = Counter()
        self.lrw_times: Deque[float] = deque(maxlen=512)
        self.started = time.monotonic()

    def cycle_intervals_us(self) -> List[float]:
        t = list(self.lrw_times)
        return [(b - a) * 1e6 for a, b in zip(t, t[1:])]

    def summary(self) -> Dict:
        iv = self.cycle_intervals_us()
        out = {"frames": self.frames, "rejected": self.rejected,
               "commands": {p.COMMAND_NAMES.get(k, str(k)): v for k, v in self.commands.items()}}
        if iv:
            s = sorted(iv)
            out["pd_interval_us"] = {
                "n": len(s), "min": round(s[0], 1), "max": round(s[-1], 1),
                "mean": round(sum(s) / len(s), 1), "p99": round(s[min(len(s) - 1, int(len(s) * 0.99))], 1),
                "note": "host-side arrival intervals measured by the simulator (not target timing)",
            }
        return out


class Segment:
    """Processes frames through ``escs`` in wire order.

    ``link_count`` simulates a cable break after that many slaves: the last
    reachable slave closes its port and returns the frame early.
    """

    def __init__(self, devices, clock=time.monotonic) -> None:
        self.clock = clock
        self.escs: List[Esc] = [Esc(d, clock=clock) for d in devices]
        self.link_count = len(self.escs)
        self.lock = threading.RLock()
        self.stats = SegmentStats()
        self._update_links()

    def _update_links(self) -> None:
        for i, esc in enumerate(self.escs):
            esc.has_next = i + 1 < self.link_count

    def cut_cable_after(self, n: Optional[int]) -> None:
        with self.lock:
            self.link_count = len(self.escs) if n is None else max(0, min(n, len(self.escs)))
            self._update_links()

    # ------------------------------------------------------------- processing
    def process(self, raw: bytes) -> Optional[bytes]:
        """Process one frame; returns the frame to send back to the master, or None."""
        with self.lock:
            try:
                frame = p.parse_frame(raw)
            except p.FrameError:
                self.stats.rejected += 1
                return None
            now = self.clock()
            active = self.escs[:self.link_count]
            for esc in active:
                esc.check_watchdog(now)
                esc.device.update_inputs(esc, now)
            for d in frame.datagrams:
                self.stats.commands[d.cmd] += 1
                if d.cmd in (p.LRW, p.LWR):
                    self.stats.lrw_times.append(now)
                self._process_datagram(d, active)
            for esc in active:
                esc.device.consume_outputs(esc, now)
            self.stats.frames += 1
            # ESC port 0 sets the locally-administered bit of the source MAC on return
            src = bytearray(frame.src)
            src[0] |= 0x02
            frame.src = bytes(src)
            return p.build_frame(frame)

    def _process_datagram(self, d: p.Datagram, active: List[Esc]) -> None:
        cmd = d.cmd
        if cmd == p.NOP:
            return
        if cmd in p.LOGICAL:
            original = bytes(d.data)
            for esc in active:
                rh, wh = esc.process_logical(cmd, d.logical_address, d.data, original)
                if cmd == p.LRW:
                    d.wkc += (1 if rh else 0) + (2 if wh else 0)
                elif rh or wh:
                    d.wkc += 1
            return
        for esc in active:
            if cmd in p.AUTO_INCREMENT:
                addressed = d.adp == 0
                d.adp = (d.adp + 1) & 0xFFFF
            elif cmd in p.CONFIGURED:
                addressed = esc.station_address == d.adp
            else:  # broadcast
                addressed = True
                d.adp = (d.adp + 1) & 0xFFFF
            if cmd in (p.ARMW, p.FRMW):
                if addressed:
                    d.data[:] = esc.read(d.ado, len(d.data))
                else:
                    esc.write(d.ado, bytes(d.data))
                d.wkc += 1
                continue
            if not addressed:
                continue
            if cmd in (p.APRD, p.FPRD):
                d.data[:] = esc.read(d.ado, len(d.data))
                d.wkc += 1
            elif cmd in (p.APWR, p.FPWR, p.BWR):
                esc.write(d.ado, bytes(d.data))
                d.wkc += 1
            elif cmd in (p.APRW, p.FPRW):
                incoming = bytes(d.data)
                d.data[:] = esc.read(d.ado, len(d.data))
                esc.write(d.ado, incoming)
                d.wkc += 3
            elif cmd == p.BRD:
                mem = esc.read(d.ado, len(d.data))
                for i, b in enumerate(mem):
                    d.data[i] |= b
                d.wkc += 1
            elif cmd == p.BRW:
                incoming = bytes(d.data)
                mem = esc.read(d.ado, len(d.data))
                for i, b in enumerate(mem):
                    d.data[i] |= b
                esc.write(d.ado, incoming)
                d.wkc += 3

    def tick(self) -> None:
        """Periodic housekeeping (watchdogs keep running even when no frames arrive)."""
        with self.lock:
            now = self.clock()
            for esc in self.escs[:self.link_count]:
                esc.check_watchdog(now)
                esc.device.update_inputs(esc, now)

    # --------------------------------------------------------------- snapshot
    def snapshot(self) -> Dict:
        with self.lock:
            slaves = []
            for i, esc in enumerate(self.escs):
                slaves.append({
                    "position": i + 1, "name": esc.desc.name,
                    "vendor_id": f"0x{esc.desc.vendor_id:08x}",
                    "product_code": f"0x{esc.desc.product_code:08x}",
                    "station_address": f"0x{esc.station_address:04x}",
                    "al_state": esc.state_name, "al_error": esc.al_error,
                    "al_code": f"0x{esc.al_code:04x}", "watchdog_trips": esc.watchdog_trips,
                    "reachable": i < self.link_count, "device": esc.device.snapshot(),
                })
            return {"slaves": slaves, "link_count": self.link_count,
                    "stats": self.stats.summary(), "time": round(self.clock(), 4),
                    "data_class": "simulated"}
