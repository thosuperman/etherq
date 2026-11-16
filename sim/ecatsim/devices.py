# SPDX-License-Identifier: GPL-3.0-or-later
"""Device models attached to simulated ESCs.

All devices are fictitious ("EtherQ Virtual", vendor ID 0x45514E58 = 'EQNX').
They are not models of any real product.
"""
from __future__ import annotations

import struct
from collections import deque
from typing import Deque, Dict, Optional, Tuple

from . import sii
from .esc import AL_OP

VENDOR_ID = 0x45514E58  # fictitious, chosen to avoid real ETG vendor IDs


class Device:
    """Base class. Subclasses describe their SII and move process data."""

    kind = "device"

    def __init__(self, name: str, product_code: int, serial: int = 1) -> None:
        self.name = name
        self.product_code = product_code
        self.serial = serial
        self.al_state = 1

    def sii_description(self) -> sii.SiiDescription:
        raise NotImplementedError

    def on_state_change(self, old: int, new: int) -> None:
        self.al_state = new

    def update_inputs(self, esc, now: float) -> None:
        """Called before each frame is processed: publish inputs to DPRAM."""

    def consume_outputs(self, esc, now: float) -> None:
        """Called after each frame: read outputs from DPRAM (only valid in OP)."""

    def snapshot(self) -> Dict:
        return {"kind": self.kind}


def _bit_pdos(base_pdo: int, base_obj: int, count: int, sm: int):
    return [sii.Pdo(index=base_pdo + i, sm=sm,
                    entries=[sii.PdoEntry(index=base_obj + 0x10 * i, subindex=1, bitlen=1, datatype=0x01)])
            for i in range(count)]


class DigitalInput8(Device):
    kind = "di8"
    SM_ADDR = 0x1000

    def __init__(self, serial: int = 1) -> None:
        super().__init__("EQ-DI8 8Ch Dig In (virtual)", 0x00000001, serial)
        self.value = 0

    def sii_description(self) -> sii.SiiDescription:
        return sii.SiiDescription(
            vendor_id=VENDOR_ID, product_code=self.product_code, revision=0x00010000,
            serial=self.serial, name=self.name, pdi_control=0x0004,
            syncms=[sii.SyncManagerDesc(self.SM_ADDR, 1, 0x00, 0x01, sii.SM_TYPE_INPUTS)],
            fmmus=[sii.FMMU_INPUTS],
            txpdos=_bit_pdos(0x1A00, 0x6000, 8, sm=0))

    def set_input(self, channel: int, on: bool) -> None:
        if on:
            self.value |= 1 << channel
        else:
            self.value &= ~(1 << channel) & 0xFF

    def update_inputs(self, esc, now: float) -> None:
        esc.mem[self.SM_ADDR] = self.value & 0xFF

    def snapshot(self) -> Dict:
        return {"kind": self.kind, "inputs": [(self.value >> i) & 1 for i in range(8)]}


class DigitalOutput8(Device):
    kind = "do8"
    SM_ADDR = 0x0F00

    def __init__(self, serial: int = 1) -> None:
        super().__init__("EQ-DO8 8Ch Dig Out (virtual)", 0x00000002, serial)
        self.value = 0

    def sii_description(self) -> sii.SiiDescription:
        return sii.SiiDescription(
            vendor_id=VENDOR_ID, product_code=self.product_code, revision=0x00010000,
            serial=self.serial, name=self.name, pdi_control=0x0004,
            syncms=[sii.SyncManagerDesc(self.SM_ADDR, 1, 0x44, 0x01, sii.SM_TYPE_OUTPUTS)],
            fmmus=[sii.FMMU_OUTPUTS],
            rxpdos=_bit_pdos(0x1600, 0x7000, 8, sm=0))

    def on_state_change(self, old: int, new: int) -> None:
        super().on_state_change(old, new)
        if new != AL_OP:
            self.value = 0  # outputs go to their safe state outside OP

    def consume_outputs(self, esc, now: float) -> None:
        if esc.al_state == AL_OP:
            self.value = esc.mem[self.SM_ADDR]

    def snapshot(self) -> Dict:
        return {"kind": self.kind, "outputs": [(self.value >> i) & 1 for i in range(8)]}


# CiA 402 drive states (statusword values with the relevant mask 0x006F)
NOT_READY = "NOT_READY_TO_SWITCH_ON"
SWITCH_ON_DISABLED = "SWITCH_ON_DISABLED"
READY = "READY_TO_SWITCH_ON"
SWITCHED_ON = "SWITCHED_ON"
OP_ENABLED = "OPERATION_ENABLED"
QUICK_STOP = "QUICK_STOP_ACTIVE"
FAULT = "FAULT"

_STATUSWORD = {
    NOT_READY: 0x0000, SWITCH_ON_DISABLED: 0x0040, READY: 0x0021,
    SWITCHED_ON: 0x0023, OP_ENABLED: 0x0027, QUICK_STOP: 0x0007, FAULT: 0x0008,
}

ERR_FOLLOWING = 0x8611
ERR_COMM_LOST = 0x8130


class ServoAxis(Device):
    """Single-axis drive with a CiA 402 style state machine in cyclic synchronous
    position mode. Fixed PDO mapping (no mailbox):

    RxPDO 0x1600 (SM2 @0x1100, 6 bytes): 0x6040 controlword u16, 0x607A target position i32
    TxPDO 0x1A00 (SM3 @0x1180, 10 bytes): 0x6041 statusword u16, 0x6064 position i32,
                                          0x606C velocity i32
    """

    kind = "servo"
    OUT_ADDR = 0x1100
    IN_ADDR = 0x1180

    def __init__(self, serial: int = 1, max_velocity: float = 400_000.0,
                 max_accel: float = 4_000_000.0, following_window: int = 100_000) -> None:
        super().__init__("EQ-AX1 Servo Axis (virtual)", 0x00000003, serial)
        self.max_velocity = max_velocity
        self.max_accel = max_accel
        self.following_window = following_window
        self.state = SWITCH_ON_DISABLED
        self.position = 0.0
        self.velocity = 0.0
        self.target = 0
        self.controlword = 0
        self.prev_controlword = 0
        self.error_code = 0
        self.last_time: Optional[float] = None
        self.history: Deque[Tuple[float, float, int]] = deque(maxlen=600)

    def sii_description(self) -> sii.SiiDescription:
        return sii.SiiDescription(
            vendor_id=VENDOR_ID, product_code=self.product_code, revision=0x00010000,
            serial=self.serial, name=self.name, pdi_control=0x0005,
            syncms=[
                sii.SyncManagerDesc(0x1000, 0, 0x26, 0x00, sii.SM_TYPE_MBX_OUT),
                sii.SyncManagerDesc(0x1080, 0, 0x22, 0x00, sii.SM_TYPE_MBX_IN),
                sii.SyncManagerDesc(self.OUT_ADDR, 6, 0x64, 0x01, sii.SM_TYPE_OUTPUTS),
                sii.SyncManagerDesc(self.IN_ADDR, 10, 0x20, 0x01, sii.SM_TYPE_INPUTS),
            ],
            fmmus=[sii.FMMU_OUTPUTS, sii.FMMU_INPUTS],
            rxpdos=[sii.Pdo(0x1600, sm=2, entries=[
                sii.PdoEntry(0x6040, 0, 16, 0x06), sii.PdoEntry(0x607A, 0, 32, 0x04)])],
            txpdos=[sii.Pdo(0x1A00, sm=3, entries=[
                sii.PdoEntry(0x6041, 0, 16, 0x06), sii.PdoEntry(0x6064, 0, 32, 0x04),
                sii.PdoEntry(0x606C, 0, 32, 0x04)])])

    @property
    def statusword(self) -> int:
        sw = _STATUSWORD[self.state] | 0x0200  # remote
        if self.state == OP_ENABLED:
            sw |= 0x1000  # CSP: drive follows the command value
        if self.state in (READY, SWITCHED_ON, OP_ENABLED):
            sw |= 0x0010  # voltage enabled
        return sw

    def _fault(self, code: int) -> None:
        self.state = FAULT
        self.error_code = code
        self.velocity = 0.0

    def on_state_change(self, old: int, new: int) -> None:
        super().on_state_change(old, new)
        if old == AL_OP and new != AL_OP:
            if self.state in (OP_ENABLED, SWITCHED_ON, QUICK_STOP):
                self._fault(ERR_COMM_LOST)
            elif self.state == READY:
                self.state = SWITCH_ON_DISABLED
            self.controlword = 0

    def _step_state_machine(self) -> None:
        cw = self.controlword
        rising_reset = (cw & 0x80) and not (self.prev_controlword & 0x80)
        self.prev_controlword = cw
        st = self.state
        if st == FAULT:
            if rising_reset:
                self.state = SWITCH_ON_DISABLED
                self.error_code = 0
            return
        if (cw & 0x02) == 0:  # disable voltage
            self.state = SWITCH_ON_DISABLED
            return
        if st == OP_ENABLED and (cw & 0x06) == 0x02:  # quick stop
            self.state = QUICK_STOP
            return
        cmd = cw & 0x0F
        if st == SWITCH_ON_DISABLED and (cw & 0x07) == 0x06:
            self.state = READY
        elif st == READY:
            if cmd == 0x07:
                self.state = SWITCHED_ON
            elif cmd == 0x0F:
                self._enable()
        elif st == SWITCHED_ON:
            if cmd == 0x0F:
                self._enable()
            elif (cw & 0x07) == 0x06:
                self.state = READY
        elif st == OP_ENABLED:
            if cmd == 0x07:
                self.state = SWITCHED_ON
            elif (cw & 0x07) == 0x06:
                self.state = READY

    def _enable(self) -> None:
        if abs(self.target - self.position) > self.following_window:
            self._fault(ERR_FOLLOWING)
        else:
            self.state = OP_ENABLED

    def _integrate(self, dt: float) -> None:
        if self.state == OP_ENABLED:
            err = self.target - self.position
            if abs(err) > self.following_window:
                self._fault(ERR_FOLLOWING)
                return
            # velocity that would close the gap in one step, limited by v and a
            desired = max(-self.max_velocity, min(self.max_velocity, err / max(dt, 1e-4)))
            dv = max(-self.max_accel * dt, min(self.max_accel * dt, desired - self.velocity))
            self.velocity += dv
            self.position += self.velocity * dt
        elif self.state == QUICK_STOP:
            dv = min(abs(self.velocity), self.max_accel * dt)
            self.velocity -= dv if self.velocity > 0 else -dv
            self.position += self.velocity * dt
            if abs(self.velocity) < 1.0:
                self.velocity = 0.0
                self.state = SWITCH_ON_DISABLED
        else:
            self.velocity = 0.0

    def update_inputs(self, esc, now: float) -> None:
        if self.last_time is not None:
            self._integrate(min(now - self.last_time, 0.1))
        self.last_time = now
        struct.pack_into("<Hii", esc.mem, self.IN_ADDR, self.statusword,
                         int(round(self.position)), int(round(self.velocity)))

    def consume_outputs(self, esc, now: float) -> None:
        if esc.al_state != AL_OP:
            return
        cw, target = struct.unpack_from("<Hi", esc.mem, self.OUT_ADDR)
        self.controlword = cw
        self.target = target
        self._step_state_machine()
        self.history.append((now, self.position, self.target))

    def snapshot(self) -> Dict:
        return {
            "kind": self.kind, "state": self.state, "statusword": self.statusword,
            "controlword": self.controlword, "position": int(self.position),
            "velocity": int(self.velocity), "target": self.target,
            "error_code": self.error_code,
            "history": [(round(t, 4), int(p), int(tg)) for t, p, tg in list(self.history)[-300:]],
        }


def default_devices():
    """The standard three-slave demo segment."""
    return [DigitalInput8(serial=101), DigitalOutput8(serial=102), ServoAxis(serial=103)]
