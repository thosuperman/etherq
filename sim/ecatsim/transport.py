# SPDX-License-Identifier: GPL-3.0-or-later
"""Frame transports for the simulator.

* RawTransport  - Linux AF_PACKET socket on an interface (veth, tap, bridge port).
* UdpTransport  - EtherCAT-over-UDP (ETG.1000.4, UDP port 0x88A4), used when raw
                  layer-2 access is unavailable.
"""
from __future__ import annotations

import socket
import struct
from typing import Optional, Tuple

from .protocol import ETH_P_ECAT

ECAT_UDP_PORT = 0x88A4


class RawTransport:
    def __init__(self, iface: str) -> None:
        if not hasattr(socket, "AF_PACKET"):
            raise RuntimeError("raw EtherCAT transport needs Linux (AF_PACKET)")
        # Binding to the EtherCAT ethertype (not ETH_P_ALL) means the kernel does not
        # loop our own transmitted frames back to us.
        self.sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ECAT))
        self.sock.bind((iface, ETH_P_ECAT))
        self.iface = iface

    def fileno(self) -> int:
        return self.sock.fileno()

    def recv(self) -> Tuple[bytes, Optional[object]]:
        data, addr = self.sock.recvfrom(4096)
        # addr = (ifname, proto, pkttype, hatype, addr); skip frames we sent ourselves
        if len(addr) > 2 and addr[2] == socket.PACKET_OUTGOING:
            return b"", None
        return data, None

    def send(self, frame: bytes, _peer: Optional[object]) -> None:
        self.sock.send(frame)

    def close(self) -> None:
        self.sock.close()


class UdpTransport:
    """EtherCAT frames carried in UDP. The simulator answers each datagram to its sender."""

    _FAKE_ETH = b"\xff" * 6 + b"\x02\x00\x00\x00\x00\x01" + struct.pack("!H", ETH_P_ECAT)

    def __init__(self, host: str = "0.0.0.0", port: int = ECAT_UDP_PORT) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))

    def fileno(self) -> int:
        return self.sock.fileno()

    def recv(self) -> Tuple[bytes, Optional[object]]:
        data, peer = self.sock.recvfrom(4096)
        return self._FAKE_ETH + data, peer

    def send(self, frame: bytes, peer: Optional[object]) -> None:
        payload = frame[14:]
        # strip Ethernet minimum-size padding using the EtherCAT length field
        (hdr,) = struct.unpack_from("<H", payload, 0)
        payload = payload[:2 + (hdr & 0x07FF)]
        self.sock.sendto(payload, peer)

    def close(self) -> None:
        self.sock.close()
