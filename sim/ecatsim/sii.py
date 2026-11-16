# SPDX-License-Identifier: GPL-3.0-or-later
"""Slave Information Interface (SII) EEPROM image builder (ETG.1000.6 / ETG.2010).

The layout follows the public SII specification: a 64-word fixed header
followed by category sections starting at word 0x40.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import List, Optional

CAT_STRINGS = 10
CAT_GENERAL = 30
CAT_FMMU = 40
CAT_SYNCM = 41
CAT_TXPDO = 50  # slave -> master (inputs)
CAT_RXPDO = 51  # master -> slave (outputs)
CAT_END = 0xFFFF

FMMU_UNUSED = 0x00
FMMU_OUTPUTS = 0x01
FMMU_INPUTS = 0x02
FMMU_SYNCM_STATUS = 0x03

SM_TYPE_MBX_OUT = 1
SM_TYPE_MBX_IN = 2
SM_TYPE_OUTPUTS = 3
SM_TYPE_INPUTS = 4


def crc8_sii(data: bytes) -> int:
    """CRC-8 used for the SII header checksum (poly x^8+x^2+x+1, init 0xFF)."""
    crc = 0xFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


@dataclass
class PdoEntry:
    index: int
    subindex: int
    bitlen: int
    datatype: int = 0
    name_idx: int = 0


@dataclass
class Pdo:
    index: int
    sm: int
    entries: List[PdoEntry]
    name_idx: int = 0


@dataclass
class SyncManagerDesc:
    start: int
    length: int
    control: int
    enable: int
    sm_type: int


@dataclass
class SiiDescription:
    vendor_id: int
    product_code: int
    revision: int
    serial: int
    name: str
    group: str = "EtherQ Virtual"
    pdi_control: int = 0x0005
    syncms: List[SyncManagerDesc] = field(default_factory=list)
    fmmus: List[int] = field(default_factory=list)
    txpdos: List[Pdo] = field(default_factory=list)
    rxpdos: List[Pdo] = field(default_factory=list)
    mbx_rx_offset: int = 0
    mbx_rx_size: int = 0
    mbx_tx_offset: int = 0
    mbx_tx_size: int = 0
    mbx_protocols: int = 0
    ebus_current_ma: int = 0
    extra_strings: List[str] = field(default_factory=list)


def _category(cat: int, payload: bytes) -> bytes:
    if len(payload) % 2:
        payload += b"\x00"
    return struct.pack("<HH", cat, len(payload) // 2) + payload


def _pdo_bytes(pdos: List[Pdo]) -> bytes:
    out = bytearray()
    for p in pdos:
        out += struct.pack("<HBBBBH", p.index, len(p.entries), p.sm, 0, p.name_idx, 0)
        for e in p.entries:
            out += struct.pack("<HBBBBH", e.index, e.subindex, e.name_idx, e.datatype, e.bitlen, 0)
    return bytes(out)


def build_sii(desc: SiiDescription, size_bytes: int = 2048) -> bytes:
    """Return the full EEPROM image (little-endian words) for ``desc``."""
    words = [0] * 0x40
    words[0x00] = desc.pdi_control
    # 0x01..0x06: PDI config / sync impulse / alias / reserved -> 0
    hdr = struct.pack("<7H", *words[0:7])
    words[0x07] = crc8_sii(hdr)
    words[0x08] = desc.vendor_id & 0xFFFF
    words[0x09] = desc.vendor_id >> 16
    words[0x0A] = desc.product_code & 0xFFFF
    words[0x0B] = desc.product_code >> 16
    words[0x0C] = desc.revision & 0xFFFF
    words[0x0D] = desc.revision >> 16
    words[0x0E] = desc.serial & 0xFFFF
    words[0x0F] = desc.serial >> 16
    words[0x18] = desc.mbx_rx_offset
    words[0x19] = desc.mbx_rx_size
    words[0x1A] = desc.mbx_tx_offset
    words[0x1B] = desc.mbx_tx_size
    words[0x1C] = desc.mbx_protocols
    words[0x3E] = (size_bytes * 8 // 1024) - 1  # EEPROM size in KBit - 1
    words[0x3F] = 1  # SII version
    image = bytearray(struct.pack(f"<{len(words)}H", *words))

    strings = [desc.name, desc.group] + list(desc.extra_strings)
    sbytes = bytearray([len(strings)])
    for s in strings:
        enc = s.encode("ascii")[:255]
        sbytes += bytes([len(enc)]) + enc
    image += _category(CAT_STRINGS, bytes(sbytes))

    general = bytearray(32)
    general[0] = 2  # group string index
    general[1] = 0  # image index
    general[2] = 0  # order index
    general[3] = 1  # name string index
    general[5] = 0x01 if desc.mbx_protocols & 0x04 else 0  # CoE details
    # data[11] = flags (bit1 would mean "LRW not supported"); data[12..13] = E-bus current
    struct.pack_into("<h", general, 0x0C, desc.ebus_current_ma)
    image += _category(CAT_GENERAL, bytes(general))

    if desc.fmmus:
        image += _category(CAT_FMMU, bytes(desc.fmmus))
    if desc.syncms:
        smb = bytearray()
        for sm in desc.syncms:
            smb += struct.pack("<HHBBBB", sm.start, sm.length, sm.control, 0, sm.enable, sm.sm_type)
        image += _category(CAT_SYNCM, bytes(smb))
    if desc.txpdos:
        image += _category(CAT_TXPDO, _pdo_bytes(desc.txpdos))
    if desc.rxpdos:
        image += _category(CAT_RXPDO, _pdo_bytes(desc.rxpdos))
    image += struct.pack("<H", CAT_END)

    if len(image) > size_bytes:
        raise ValueError(f"SII image {len(image)} bytes exceeds EEPROM size {size_bytes}")
    image += b"\xff" * (size_bytes - len(image))
    return bytes(image)


def find_category(image: bytes, cat: int) -> Optional[bytes]:
    """Return the payload of the first category ``cat`` (used by tests and tools)."""
    pos = 0x40 * 2
    while pos + 4 <= len(image):
        ctype, clen = struct.unpack_from("<HH", image, pos)
        if ctype == CAT_END:
            return None
        if ctype == cat:
            return image[pos + 4:pos + 4 + clen * 2]
        pos += 4 + clen * 2
    return None
