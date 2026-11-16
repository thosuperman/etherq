# SPDX-License-Identifier: GPL-3.0-or-later
"""EtherCAT Slave Controller (ESC) model.

Models the parts of an ESC that a master observes over the wire:
register/DPRAM address space, the SII EEPROM interface, the AL state
machine, FMMU logical->physical mapping (bit granular), SyncManager
configuration checks and the process-data (SyncManager) watchdog.

It is a protocol-level model. It does not model PHY timing, distributed
clocks or frame CRCs.
"""
from __future__ import annotations

import struct
import time
from dataclasses import dataclass
from typing import List, Optional, Tuple

from . import sii as siimod
from .protocol import LRD, LRW, LWR

MEM_SIZE = 0x10000

# Register offsets
REG_TYPE = 0x0000
REG_REVISION = 0x0001
REG_BUILD = 0x0002
REG_FMMU_COUNT = 0x0004
REG_SM_COUNT = 0x0005
REG_RAM_SIZE = 0x0006
REG_PORT_DESC = 0x0007
REG_FEATURES = 0x0008
REG_STATION_ADDR = 0x0010
REG_ALIAS = 0x0012
REG_DL_CONTROL = 0x0100
REG_DL_STATUS = 0x0110
REG_AL_CONTROL = 0x0120
REG_AL_STATUS = 0x0130
REG_AL_STATUS_CODE = 0x0134
REG_PDI_CONTROL = 0x0140
REG_WD_DIVIDER = 0x0400
REG_WD_TIME_PD = 0x0420
REG_WD_COUNTER_PD = 0x0442
REG_EEP_CONFIG = 0x0500
REG_EEP_CTRL = 0x0502
REG_EEP_ADDR = 0x0504
REG_EEP_DATA = 0x0508
REG_FMMU0 = 0x0600
REG_SM0 = 0x0800

EEP_CMD_MASK = 0x0700
EEP_CMD_READ = 0x0100
EEP_CMD_WRITE = 0x0200
EEP_CMD_RELOAD = 0x0300
EEP_STAT_R64 = 0x0040

# AL states
AL_INIT = 0x01
AL_PREOP = 0x02
AL_BOOT = 0x03
AL_SAFEOP = 0x04
AL_OP = 0x08
AL_ERROR = 0x10
AL_ACK = 0x10

AL_STATE_NAMES = {AL_INIT: "INIT", AL_PREOP: "PRE-OP", AL_BOOT: "BOOT",
                  AL_SAFEOP: "SAFE-OP", AL_OP: "OP"}

# AL status codes (ETG.1000.6 Table 11 subset)
ALCODE_NONE = 0x0000
ALCODE_INVALID_STATE_CHANGE = 0x0011
ALCODE_UNKNOWN_STATE = 0x0012
ALCODE_BOOTSTRAP_NOT_SUPPORTED = 0x0013
ALCODE_SM_WATCHDOG = 0x001B
ALCODE_INVALID_OUTPUT_CONFIG = 0x001D
ALCODE_INVALID_INPUT_CONFIG = 0x001E

READ_ONLY_RANGES: List[Tuple[int, int]] = [
    (0x0000, 0x0010),  # ESC information
    (0x0110, 0x0112),  # DL status
    (0x0130, 0x0136),  # AL status + status code
    (0x0140, 0x0142),  # PDI control (loaded from EEPROM)
]

_VALID_TRANSITIONS = {
    (AL_INIT, AL_PREOP), (AL_PREOP, AL_SAFEOP), (AL_SAFEOP, AL_OP),
    (AL_OP, AL_SAFEOP), (AL_OP, AL_PREOP), (AL_SAFEOP, AL_PREOP),
}


@dataclass
class Fmmu:
    log_start: int
    length: int
    log_start_bit: int
    log_end_bit: int
    phys_start: int
    phys_start_bit: int
    ftype: int
    active: bool

    @classmethod
    def from_bytes(cls, raw: bytes) -> "Fmmu":
        ls, ln, lsb, leb, ps, psb, ft, act = struct.unpack_from("<IHBBHBBB", raw, 0)
        return cls(ls, ln, lsb & 7, leb & 7, ps, psb & 7, ft, bool(act & 1))

    def logical_bit_range(self) -> Tuple[int, int]:
        first = self.log_start * 8 + self.log_start_bit
        last = (self.log_start + self.length - 1) * 8 + self.log_end_bit
        return first, last


@dataclass
class SyncManager:
    start: int
    length: int
    control: int
    status: int
    activate: int
    pdi_control: int

    @classmethod
    def from_bytes(cls, raw: bytes) -> "SyncManager":
        return cls(*struct.unpack_from("<HHBBBB", raw, 0))

    @property
    def enabled(self) -> bool:
        return bool(self.activate & 1)

    @property
    def watchdog_enabled(self) -> bool:
        return bool(self.control & 0x40)


def _get_bit(buf: bytes, bit: int) -> int:
    return (buf[bit >> 3] >> (bit & 7)) & 1


def _set_bit(buf: bytearray, bit: int, value: int) -> None:
    if value:
        buf[bit >> 3] |= 1 << (bit & 7)
    else:
        buf[bit >> 3] &= ~(1 << (bit & 7)) & 0xFF


class Esc:
    """One simulated slave controller with an attached device model."""

    def __init__(self, device, eeprom_8byte: bool = True,
                 clock=time.monotonic) -> None:
        self.device = device
        self.clock = clock
        self.desc: siimod.SiiDescription = device.sii_description()
        self.eeprom = bytearray(siimod.build_sii(self.desc))
        self.eeprom_8byte = eeprom_8byte
        self.mem = bytearray(MEM_SIZE)
        self.al_state = AL_INIT
        self.al_error = False
        self.al_code = ALCODE_NONE
        self.has_next = False
        self.last_output_write: Optional[float] = None
        self.watchdog_trips = 0
        self.state_log: List[Tuple[float, int, bool, int]] = []
        self._sm_cache: Optional[List[SyncManager]] = None
        self._fmmu_cache: Optional[List[Fmmu]] = None
        self._expected_sms = self._compute_expected_sms()
        self._init_registers()

    # ------------------------------------------------------------------ setup
    def _init_registers(self) -> None:
        m = self.mem
        m[REG_TYPE] = 0x11
        m[REG_REVISION] = 0x01
        struct.pack_into("<H", m, REG_BUILD, 0x0001)
        m[REG_FMMU_COUNT] = 4
        m[REG_SM_COUNT] = 4
        m[REG_RAM_SIZE] = 8  # KB
        m[REG_PORT_DESC] = 0x0F  # port0 + port1 MII, ports 2/3 not implemented
        struct.pack_into("<H", m, REG_FEATURES, 0x0000)  # no DC
        struct.pack_into("<H", m, REG_PDI_CONTROL, self.desc.pdi_control & 0xFF)
        struct.pack_into("<H", m, REG_WD_DIVIDER, 2498)  # 100 us watchdog ticks
        struct.pack_into("<H", m, REG_WD_TIME_PD, 1000)  # 100 ms
        struct.pack_into("<H", m, REG_EEP_CTRL, EEP_STAT_R64 if self.eeprom_8byte else 0)
        self._refresh_status()

    # --------------------------------------------------------------- helpers
    @property
    def station_address(self) -> int:
        return struct.unpack_from("<H", self.mem, REG_STATION_ADDR)[0]

    @property
    def state_name(self) -> str:
        return AL_STATE_NAMES.get(self.al_state, f"0x{self.al_state:x}")

    def sync_managers(self) -> List[SyncManager]:
        if self._sm_cache is None:
            self._sm_cache = [SyncManager.from_bytes(self.mem[REG_SM0 + 8 * i:REG_SM0 + 8 * i + 8])
                              for i in range(self.mem[REG_SM_COUNT])]
        return self._sm_cache

    def fmmus(self) -> List[Fmmu]:
        if self._fmmu_cache is None:
            self._fmmu_cache = [Fmmu.from_bytes(self.mem[REG_FMMU0 + 16 * i:REG_FMMU0 + 16 * i + 16])
                                for i in range(self.mem[REG_FMMU_COUNT])]
        return self._fmmu_cache

    def watchdog_timeout_s(self) -> float:
        div = struct.unpack_from("<H", self.mem, REG_WD_DIVIDER)[0]
        wdt = struct.unpack_from("<H", self.mem, REG_WD_TIME_PD)[0]
        if wdt == 0:
            return 0.0
        return (div + 2) * 40e-9 * wdt  # ESC formula: (divider+2) * 40 ns per tick

    def _refresh_status(self) -> None:
        status = self.al_state | (AL_ERROR if self.al_error else 0)
        struct.pack_into("<H", self.mem, REG_AL_STATUS, status)
        struct.pack_into("<H", self.mem, REG_AL_STATUS_CODE, self.al_code)
        # DL status: PDI operational (bit0), link on port0 (bit4), port1 link if next slave
        dl = 0x0001 | 0x0010
        dl |= 0x0200  # port0: loop open, communication established
        if self.has_next:
            dl |= 0x0020 | 0x0800  # port1 link + communication
        else:
            dl |= 0x0400  # port1 loop closed
        dl |= 0x1000 | 0x4000  # ports 2/3 loop closed
        struct.pack_into("<H", self.mem, REG_DL_STATUS, dl)

    # ------------------------------------------------------- register access
    def read(self, offset: int, length: int) -> bytes:
        self._refresh_status()
        end = offset + length
        if end <= MEM_SIZE:
            return bytes(self.mem[offset:end])
        return bytes(self.mem[offset:]) + bytes(end - MEM_SIZE)

    def write(self, offset: int, data: bytes) -> None:
        length = len(data)
        end = min(offset + length, MEM_SIZE)
        saved = {}
        for lo, hi in READ_ONLY_RANGES:
            if offset < hi and end > lo:
                saved[(lo, hi)] = bytes(self.mem[lo:hi])
        self.mem[offset:end] = data[:end - offset]
        for (lo, hi), val in saved.items():
            self.mem[lo:hi] = val
        if offset < REG_SM0 + 0x80 and end > REG_SM0:
            self._sm_cache = None
        if offset < REG_FMMU0 + 0x100 and end > REG_FMMU0:
            self._fmmu_cache = None
        if offset < REG_AL_CONTROL + 2 and end > REG_AL_CONTROL:
            self._al_control_written()
        if offset < REG_EEP_CTRL + 2 and end > REG_EEP_CTRL:
            self._eeprom_command()
        self._note_output_write(offset, end)
        self._refresh_status()

    # ---------------------------------------------------------------- EEPROM
    def _eeprom_command(self) -> None:
        ctrl = struct.unpack_from("<H", self.mem, REG_EEP_CTRL)[0]
        cmd = ctrl & EEP_CMD_MASK
        addr = struct.unpack_from("<I", self.mem, REG_EEP_ADDR)[0]
        if cmd == EEP_CMD_READ:
            n = 8 if self.eeprom_8byte else 4
            start = addr * 2
            chunk = bytes(self.eeprom[start:start + n])
            chunk += b"\xff" * (n - len(chunk))
            self.mem[REG_EEP_DATA:REG_EEP_DATA + n] = chunk
        elif cmd == EEP_CMD_WRITE:
            start = addr * 2
            if start + 2 <= len(self.eeprom):
                self.eeprom[start:start + 2] = self.mem[REG_EEP_DATA:REG_EEP_DATA + 2]
        # command completes immediately: busy=0, no error bits, report read size
        struct.pack_into("<H", self.mem, REG_EEP_CTRL,
                         EEP_STAT_R64 if self.eeprom_8byte else 0)

    # ------------------------------------------------------- AL state machine
    def _al_control_written(self) -> None:
        ctrl = struct.unpack_from("<H", self.mem, REG_AL_CONTROL)[0]
        requested = ctrl & 0x0F
        if ctrl & AL_ACK:
            self.al_error = False
            self.al_code = ALCODE_NONE
        elif self.al_error and requested != AL_INIT:
            return  # error indication must be acknowledged first
        self.request_state(requested)

    def request_state(self, requested: int) -> None:
        current = self.al_state
        if requested == current:
            return
        if requested not in AL_STATE_NAMES:
            self._set_error(ALCODE_UNKNOWN_STATE)
            return
        if requested == AL_BOOT:
            self._set_error(ALCODE_BOOTSTRAP_NOT_SUPPORTED)
            return
        if requested == AL_INIT:
            self._enter(AL_INIT)
            return
        if (current, requested) not in _VALID_TRANSITIONS:
            self._set_error(ALCODE_INVALID_STATE_CHANGE)
            return
        if requested == AL_SAFEOP and current == AL_PREOP:
            code = self._check_process_data_config()
            if code:
                self._set_error(code)
                return
        self._enter(requested)

    def _set_error(self, code: int) -> None:
        self.al_error = True
        self.al_code = code
        self.state_log.append((self.clock(), self.al_state, True, code))
        self._refresh_status()

    def _enter(self, new: int) -> None:
        old = self.al_state
        self.al_state = new
        if new == AL_OP:
            self.last_output_write = self.clock()
        self.state_log.append((self.clock(), new, self.al_error, self.al_code))
        self.device.on_state_change(old, new)
        self._refresh_status()

    def expected_sms(self) -> List[Tuple[int, siimod.SyncManagerDesc, int]]:
        """(index, description, expected length) for every process-data SM."""
        return self._expected_sms

    def _compute_expected_sms(self) -> List[Tuple[int, siimod.SyncManagerDesc, int]]:
        out = []
        for i, sm in enumerate(self.desc.syncms):
            if sm.sm_type not in (siimod.SM_TYPE_OUTPUTS, siimod.SM_TYPE_INPUTS):
                continue
            pdos = self.desc.rxpdos if sm.sm_type == siimod.SM_TYPE_OUTPUTS else self.desc.txpdos
            bits = sum(e.bitlen for p in pdos if p.sm == i for e in p.entries)
            if bits:
                out.append((i, sm, (bits + 7) // 8))
        return out

    def _check_process_data_config(self) -> int:
        actual = self.sync_managers()
        for i, sm, length in self.expected_sms():
            a = actual[i]
            if a.start != sm.start or a.length != length or not a.enabled:
                return (ALCODE_INVALID_OUTPUT_CONFIG if sm.sm_type == siimod.SM_TYPE_OUTPUTS
                        else ALCODE_INVALID_INPUT_CONFIG)
        return ALCODE_NONE

    # -------------------------------------------------------------- watchdog
    def _output_ranges(self) -> List[Tuple[int, int, bool]]:
        out = []
        actual = self.sync_managers()
        for i, sm, length in self.expected_sms():
            if sm.sm_type == siimod.SM_TYPE_OUTPUTS:
                a = actual[i]
                out.append((a.start, a.start + a.length, a.watchdog_enabled))
        return out

    def _note_output_write(self, start: int, end: int) -> None:
        for lo, hi, _wd in self._output_ranges():
            if start < hi and end > lo:
                self.last_output_write = self.clock()
                return

    def check_watchdog(self, now: Optional[float] = None) -> bool:
        """Trip the SM watchdog if outputs were not refreshed in time. Returns True on trip."""
        if self.al_state != AL_OP:
            return False
        timeout = self.watchdog_timeout_s()
        if timeout <= 0:
            return False
        if not any(wd for _lo, _hi, wd in self._output_ranges()):
            return False
        now = self.clock() if now is None else now
        if self.last_output_write is not None and now - self.last_output_write > timeout:
            self.al_error = True
            self.al_code = ALCODE_SM_WATCHDOG
            self.watchdog_trips += 1
            wd = struct.unpack_from("<H", self.mem, REG_WD_COUNTER_PD)[0]
            struct.pack_into("<H", self.mem, REG_WD_COUNTER_PD, (wd + 1) & 0xFFFF)
            self._enter(AL_SAFEOP)
            return True
        return False

    # -------------------------------------------------------- process data
    def process_logical(self, cmd: int, log_addr: int, data: bytearray,
                        original: bytes) -> Tuple[bool, bool]:
        """Apply LRD/LWR/LRW to this slave. Returns (read_hit, write_hit)."""
        if self.al_state < AL_SAFEOP and self.al_state != AL_BOOT:
            return False, False
        frame_first = log_addr * 8
        frame_last = (log_addr + len(data)) * 8 - 1
        read_hit = write_hit = False
        wrote_lo, wrote_hi = None, None
        for f in self.fmmus():
            if not f.active or f.length == 0:
                continue
            lfirst, llast = f.logical_bit_range()
            lo, hi = max(lfirst, frame_first), min(llast, frame_last)
            if lo > hi:
                continue
            pbase = f.phys_start * 8 + f.phys_start_bit
            if (lo & 7) == 0 and ((hi + 1) & 7) == 0 and ((pbase + lo - lfirst) & 7) == 0:
                fo, n = (lo - frame_first) >> 3, (hi - lo + 1) >> 3
                po = (pbase + (lo - lfirst)) >> 3
                if f.ftype & 0x2 and cmd in (LWR, LRW):
                    self.mem[po:po + n] = original[fo:fo + n]
                    write_hit = True
                    wrote_lo = po if wrote_lo is None else min(wrote_lo, po)
                    wrote_hi = po + n if wrote_hi is None else max(wrote_hi, po + n)
                if f.ftype & 0x1 and cmd in (LRD, LRW):
                    data[fo:fo + n] = self.mem[po:po + n]
                    read_hit = True
                continue
            if f.ftype & 0x2 and cmd in (LWR, LRW):
                for bit in range(lo, hi + 1):
                    pbit = pbase + (bit - lfirst)
                    _set_bit(self.mem, pbit, _get_bit(original, bit - frame_first))
                write_hit = True
                pb_lo = (pbase + (lo - lfirst)) >> 3
                pb_hi = ((pbase + (hi - lfirst)) >> 3) + 1
                wrote_lo = pb_lo if wrote_lo is None else min(wrote_lo, pb_lo)
                wrote_hi = pb_hi if wrote_hi is None else max(wrote_hi, pb_hi)
            if f.ftype & 0x1 and cmd in (LRD, LRW):
                for bit in range(lo, hi + 1):
                    pbit = pbase + (bit - lfirst)
                    _set_bit(data, bit - frame_first, _get_bit(self.mem, pbit))
                read_hit = True
        if wrote_lo is not None:
            self._note_output_write(wrote_lo, wrote_hi)
        return read_hit, write_hit

    # --------------------------------------------------------------- utility
    def sm_bytes(self, sm_type: int) -> Tuple[int, int]:
        """(start, length) of the configured process-data SM of the given type."""
        actual = self.sync_managers()
        for i, sm, length in self.expected_sms():
            if sm.sm_type == sm_type:
                return actual[i].start or sm.start, length
        return 0, 0
