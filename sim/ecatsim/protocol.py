# SPDX-License-Identifier: GPL-3.0-or-later
"""EtherCAT frame and datagram codec (ETG.1000.4 layout).

Only what a master-facing slave simulator needs: parsing a frame into
datagrams, mutating datagram payload / working counter / address, and
re-serialising to the exact original length.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import List

ETH_P_ECAT = 0x88A4
ETH_HEADER_LEN = 14
ECAT_HEADER_LEN = 2
DGRAM_HEADER_LEN = 10
WKC_LEN = 2
ECAT_TYPE_COMMANDS = 0x1

# Command codes
NOP, APRD, APWR, APRW, FPRD, FPWR, FPRW, BRD, BWR, BRW, LRD, LWR, LRW, ARMW, FRMW = range(15)

COMMAND_NAMES = {
    NOP: "NOP", APRD: "APRD", APWR: "APWR", APRW: "APRW", FPRD: "FPRD",
    FPWR: "FPWR", FPRW: "FPRW", BRD: "BRD", BWR: "BWR", BRW: "BRW",
    LRD: "LRD", LWR: "LWR", LRW: "LRW", ARMW: "ARMW", FRMW: "FRMW",
}

AUTO_INCREMENT = {APRD, APWR, APRW, ARMW}
CONFIGURED = {FPRD, FPWR, FPRW, FRMW}
BROADCAST = {BRD, BWR, BRW}
LOGICAL = {LRD, LWR, LRW}
READS = {APRD, FPRD, BRD, LRD}
WRITES = {APWR, FPWR, BWR, LWR}
READWRITES = {APRW, FPRW, BRW, LRW}


class FrameError(ValueError):
    """Raised for frames that are not well-formed EtherCAT command frames."""


@dataclass
class Datagram:
    cmd: int
    idx: int
    adp: int  # position / configured address (or low word of logical address)
    ado: int  # register offset (or high word of logical address)
    data: bytearray
    wkc: int
    irq: int = 0
    circulating: bool = False
    more: bool = False
    reserved_bits: int = 0

    @property
    def logical_address(self) -> int:
        return (self.ado << 16) | self.adp

    @logical_address.setter
    def logical_address(self, value: int) -> None:
        self.adp = value & 0xFFFF
        self.ado = (value >> 16) & 0xFFFF

    @property
    def name(self) -> str:
        return COMMAND_NAMES.get(self.cmd, f"CMD{self.cmd}")


@dataclass
class Frame:
    dst: bytes
    src: bytes
    ecat_type: int
    datagrams: List[Datagram] = field(default_factory=list)
    trailer: bytes = b""  # Ethernet padding after the EtherCAT payload

    @property
    def ecat_length(self) -> int:
        return sum(DGRAM_HEADER_LEN + len(d.data) + WKC_LEN for d in self.datagrams)


def parse_frame(raw: bytes) -> Frame:
    if len(raw) < ETH_HEADER_LEN + ECAT_HEADER_LEN:
        raise FrameError("frame too short")
    (etype,) = struct.unpack_from("!H", raw, 12)
    if etype != ETH_P_ECAT:
        raise FrameError(f"not an EtherCAT frame (ethertype 0x{etype:04x})")
    (hdr,) = struct.unpack_from("<H", raw, ETH_HEADER_LEN)
    length = hdr & 0x07FF
    ecat_type = (hdr >> 12) & 0xF
    if ecat_type != ECAT_TYPE_COMMANDS:
        raise FrameError(f"unsupported EtherCAT frame type {ecat_type}")
    pos = ETH_HEADER_LEN + ECAT_HEADER_LEN
    end = pos + length
    if end > len(raw):
        raise FrameError("EtherCAT length exceeds frame")
    frame = Frame(dst=raw[0:6], src=raw[6:12], ecat_type=ecat_type)
    while True:
        if pos + DGRAM_HEADER_LEN > end:
            raise FrameError("truncated datagram header")
        cmd, idx, adp, ado, lenf, irq = struct.unpack_from("<BBHHHH", raw, pos)
        dlen = lenf & 0x07FF
        more = bool(lenf & 0x8000)
        circ = bool(lenf & 0x4000)
        rbits = (lenf >> 11) & 0x7
        dstart = pos + DGRAM_HEADER_LEN
        if dstart + dlen + WKC_LEN > end:
            raise FrameError("truncated datagram payload")
        data = bytearray(raw[dstart:dstart + dlen])
        (wkc,) = struct.unpack_from("<H", raw, dstart + dlen)
        frame.datagrams.append(Datagram(cmd, idx, adp, ado, data, wkc, irq, circ, more, rbits))
        pos = dstart + dlen + WKC_LEN
        if not more:
            break
    frame.trailer = bytes(raw[pos:])
    return frame


def build_frame(frame: Frame) -> bytes:
    out = bytearray()
    out += frame.dst + frame.src + struct.pack("!H", ETH_P_ECAT)
    out += struct.pack("<H", (frame.ecat_length & 0x07FF) | (frame.ecat_type << 12))
    n = len(frame.datagrams)
    for i, d in enumerate(frame.datagrams):
        lenf = (len(d.data) & 0x07FF) | ((d.reserved_bits & 0x7) << 11)
        if d.circulating:
            lenf |= 0x4000
        if i < n - 1:
            lenf |= 0x8000
        out += struct.pack("<BBHHHH", d.cmd, d.idx, d.adp & 0xFFFF, d.ado & 0xFFFF, lenf, d.irq)
        out += d.data
        out += struct.pack("<H", d.wkc & 0xFFFF)
    out += frame.trailer
    if len(out) < 60:
        out += bytes(60 - len(out))
    return bytes(out)


def make_frame(datagrams: List[Datagram], src: bytes = b"\x01\x01\x01\x01\x01\x01") -> bytes:
    """Helper for tests and tools: build a broadcast EtherCAT frame."""
    for i, d in enumerate(datagrams):
        d.more = i < len(datagrams) - 1
    return build_frame(Frame(dst=b"\xff" * 6, src=src, ecat_type=ECAT_TYPE_COMMANDS,
                             datagrams=datagrams))
