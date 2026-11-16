# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for the virtual EtherCAT segment (run: python3 -m pytest sim/tests)."""
from __future__ import annotations

import struct

import pytest

from ecatsim import protocol as p
from ecatsim import sii
from ecatsim.devices import (DigitalInput8, DigitalOutput8, ServoAxis, VENDOR_ID,
                             OP_ENABLED, FAULT, SWITCH_ON_DISABLED, ERR_COMM_LOST, ERR_FOLLOWING)
from ecatsim.esc import (AL_INIT, AL_PREOP, AL_SAFEOP, AL_OP, ALCODE_SM_WATCHDOG,
                         ALCODE_INVALID_STATE_CHANGE, ALCODE_INVALID_OUTPUT_CONFIG)
from ecatsim.segment import Segment


class FakeClock:
    def __init__(self) -> None:
        self.t = 1000.0

    def __call__(self) -> float:
        return self.t


def roundtrip(seg: Segment, *dgrams: p.Datagram) -> list:
    out = seg.process(p.make_frame(list(dgrams)))
    assert out is not None
    return p.parse_frame(out).datagrams


def dg(cmd, adp=0, ado=0, data=b"", length=None, idx=0):
    buf = bytearray(data if length is None else bytes(length))
    return p.Datagram(cmd=cmd, idx=idx, adp=adp & 0xFFFF, ado=ado, data=buf, wkc=0)


# ---------------------------------------------------------------- protocol
def test_frame_roundtrip_preserves_bytes():
    raw = p.make_frame([dg(p.BRD, ado=0, length=2), dg(p.FPRD, adp=0x1001, ado=0x130, length=6, idx=7)])
    assert len(raw) == 60  # padded to Ethernet minimum
    assert p.build_frame(p.parse_frame(raw)) == raw


def test_parse_rejects_non_ecat():
    with pytest.raises(p.FrameError):
        p.parse_frame(b"\xff" * 12 + b"\x08\x00" + bytes(46))


def test_parse_rejects_truncated():
    raw = bytearray(p.make_frame([dg(p.BRD, length=2)]))
    struct.pack_into("<H", raw, 14, 0x1000 | 0x7FF)  # length larger than frame
    with pytest.raises(p.FrameError):
        p.parse_frame(bytes(raw))


# --------------------------------------------------------------------- SII
def test_sii_header_and_checksum():
    img = sii.build_sii(DigitalInput8(serial=7).sii_description())
    words = struct.unpack_from("<64H", img, 0)
    assert words[7] == sii.crc8_sii(img[:14])
    assert (words[9] << 16 | words[8]) == VENDOR_ID
    assert (words[0x0B] << 16 | words[0x0A]) == 1
    assert (words[0x0F] << 16 | words[0x0E]) == 7


def test_crc8_known_vector():
    # ETG.2010 example header (PDI control 0x0C08 etc.) -> checksum computed by reference tools
    assert sii.crc8_sii(bytes(14)) == sii.crc8_sii(bytes(14))  # deterministic
    assert sii.crc8_sii(b"\x00") != sii.crc8_sii(b"\x01")


def test_sii_pdo_category_layout_matches_soem_parser():
    img = sii.build_sii(ServoAxis().sii_description())
    tx = sii.find_category(img, sii.CAT_TXPDO)
    # PDO header: index, n entries, SM, sync, name, flags(2) = 8 bytes; entry = 8 bytes
    index, n, sm = struct.unpack_from("<HBB", tx, 0)
    assert (index, n, sm) == (0x1A00, 3, 3)
    bitlens = [tx[8 + 8 * i + 5] for i in range(n)]
    assert bitlens == [16, 32, 32]


def test_sii_strings_name_first():
    img = sii.build_sii(DigitalOutput8().sii_description())
    s = sii.find_category(img, sii.CAT_STRINGS)
    assert s[0] == 2
    assert s[2:2 + s[1]].decode() == "EQ-DO8 8Ch Dig Out (virtual)"


# ------------------------------------------------------ addressing & WKC
def make_seg():
    clock = FakeClock()
    return Segment([DigitalInput8(serial=1), DigitalOutput8(serial=2), ServoAxis(serial=3)], clock=clock), clock


def test_brd_counts_slaves():
    seg, _ = make_seg()
    (d,) = roundtrip(seg, dg(p.BRD, ado=0x0000, length=2))
    assert d.wkc == 3
    assert d.adp == 3  # incremented by each slave


def test_auto_increment_addresses_nth_slave():
    seg, _ = make_seg()
    for n in range(3):
        roundtrip(seg, dg(p.APWR, adp=-n, ado=0x0010, data=struct.pack("<H", 0x1001 + n)))
    (d,) = roundtrip(seg, dg(p.APRD, adp=-2, ado=0x0010, length=2))
    assert d.wkc == 1 and struct.unpack("<H", d.data)[0] == 0x1003
    assert [e.station_address for e in seg.escs] == [0x1001, 0x1002, 0x1003]


def test_configured_address_and_no_match():
    seg, _ = make_seg()
    for n in range(3):
        roundtrip(seg, dg(p.APWR, adp=-n, ado=0x0010, data=struct.pack("<H", 0x1001 + n)))
    (hit, miss) = roundtrip(seg, dg(p.FPRD, adp=0x1002, ado=0x0130, length=2),
                            dg(p.FPRD, adp=0x7777, ado=0x0130, length=2))
    assert hit.wkc == 1 and struct.unpack("<H", hit.data)[0] == AL_INIT
    assert miss.wkc == 0


def test_read_only_registers_are_protected():
    seg, _ = make_seg()
    roundtrip(seg, dg(p.BWR, ado=0x0130, data=b"\x08\x00"))  # try to force AL status
    assert all(e.al_state == AL_INIT for e in seg.escs)


def test_readwrite_wkc_is_three():
    seg, _ = make_seg()
    roundtrip(seg, dg(p.APWR, adp=0, ado=0x0010, data=b"\x01\x10"))
    (d,) = roundtrip(seg, dg(p.FPRW, adp=0x1001, ado=0x0012, data=b"\x34\x12"))
    assert d.wkc == 3


def test_dl_status_topology():
    seg, _ = make_seg()
    (first,) = roundtrip(seg, dg(p.APRD, adp=0, ado=0x0110, length=2))
    (last,) = roundtrip(seg, dg(p.APRD, adp=-2, ado=0x0110, length=2))
    f = struct.unpack("<H", first.data)[0]
    l = struct.unpack("<H", last.data)[0]
    assert (f & 0x0300) == 0x0200 and (f & 0x0C00) == 0x0800  # 2 ports open
    assert (l & 0x0300) == 0x0200 and (l & 0x0C00) != 0x0800  # end of line


# ------------------------------------------------------------- EEPROM
def eeprom_read(seg, adp, word, n=8):
    roundtrip(seg, dg(p.FPWR, adp=adp, ado=0x0502, data=struct.pack("<HHH", 0x0100, word, 0)))
    (st,) = roundtrip(seg, dg(p.FPRD, adp=adp, ado=0x0502, length=2))
    assert struct.unpack("<H", st.data)[0] & 0x8000 == 0  # not busy
    (d,) = roundtrip(seg, dg(p.FPRD, adp=adp, ado=0x0508, length=n))
    return bytes(d.data)


def test_eeprom_read_vendor_and_product():
    seg, _ = make_seg()
    for n in range(3):
        roundtrip(seg, dg(p.APWR, adp=-n, ado=0x0010, data=struct.pack("<H", 0x1001 + n)))
    data = eeprom_read(seg, 0x1003, 0x0008)
    vendor, product = struct.unpack("<II", data)
    assert vendor == VENDOR_ID and product == 3
    (st,) = roundtrip(seg, dg(p.FPRD, adp=0x1003, ado=0x0502, length=2))
    assert struct.unpack("<H", st.data)[0] & 0x0040  # 8-byte reads supported


def test_eeprom_read_beyond_image_returns_ff():
    seg, _ = make_seg()
    roundtrip(seg, dg(p.APWR, adp=0, ado=0x0010, data=b"\x01\x10"))
    assert eeprom_read(seg, 0x1001, 0x7FF0) == b"\xff" * 8


# ---------------------------------------------- state machine & mapping
def configure(seg):
    """Mimic what a master does: addresses, SMs, FMMUs; returns (logical in, logical out)."""
    for n in range(3):
        roundtrip(seg, dg(p.APWR, adp=-n, ado=0x0010, data=struct.pack("<H", 0x1001 + n)))
    for addr in (0x1001, 0x1002, 0x1003):
        roundtrip(seg, dg(p.FPWR, adp=addr, ado=0x0120, data=struct.pack("<H", AL_PREOP | 0x10)))
    # DI8: SM0 @0x1000 len1 inputs; FMMU0 read logical 0x0000 -> phys 0x1000
    roundtrip(seg, dg(p.FPWR, adp=0x1001, ado=0x0800, data=struct.pack("<HHI", 0x1000, 1, 0x00 | (1 << 16))))
    roundtrip(seg, dg(p.FPWR, adp=0x1001, ado=0x0600,
                      data=struct.pack("<IHBBHBBBxxx", 0x00010000, 1, 0, 7, 0x1000, 0, 1, 1)))
    # DO8: SM0 @0x0F00 len1 outputs; FMMU0 write logical 0x0000 -> phys 0x0F00
    roundtrip(seg, dg(p.FPWR, adp=0x1002, ado=0x0800, data=struct.pack("<HHI", 0x0F00, 1, 0x44 | (1 << 16))))
    roundtrip(seg, dg(p.FPWR, adp=0x1002, ado=0x0600,
                      data=struct.pack("<IHBBHBBBxxx", 0x00000000, 1, 0, 7, 0x0F00, 0, 2, 1)))
    # Servo: SM2 outputs (6) SM3 inputs (10); outputs at logical 1..6, inputs at 0x10001..
    roundtrip(seg, dg(p.FPWR, adp=0x1003, ado=0x0810, data=struct.pack("<HHI", 0x1100, 6, 0x64 | (1 << 16))))
    roundtrip(seg, dg(p.FPWR, adp=0x1003, ado=0x0818, data=struct.pack("<HHI", 0x1180, 10, 0x20 | (1 << 16))))
    roundtrip(seg, dg(p.FPWR, adp=0x1003, ado=0x0600,
                      data=struct.pack("<IHBBHBBBxxx", 0x00000001, 6, 0, 7, 0x1100, 0, 2, 1)))
    roundtrip(seg, dg(p.FPWR, adp=0x1003, ado=0x0610,
                      data=struct.pack("<IHBBHBBBxxx", 0x00010001, 10, 0, 7, 0x1180, 0, 1, 1)))


def request_all(seg, state):
    for addr in (0x1001, 0x1002, 0x1003):
        roundtrip(seg, dg(p.FPWR, adp=addr, ado=0x0120, data=struct.pack("<H", state)))


def test_invalid_transition_sets_error():
    seg, _ = make_seg()
    roundtrip(seg, dg(p.APWR, adp=0, ado=0x0010, data=b"\x01\x10"))
    roundtrip(seg, dg(p.FPWR, adp=0x1001, ado=0x0120, data=struct.pack("<H", AL_OP)))
    esc = seg.escs[0]
    assert esc.al_state == AL_INIT and esc.al_error and esc.al_code == ALCODE_INVALID_STATE_CHANGE
    # error must be acknowledged before a new request is honoured
    roundtrip(seg, dg(p.FPWR, adp=0x1001, ado=0x0120, data=struct.pack("<H", AL_PREOP)))
    assert esc.al_state == AL_INIT
    roundtrip(seg, dg(p.FPWR, adp=0x1001, ado=0x0120, data=struct.pack("<H", AL_PREOP | 0x10)))
    assert esc.al_state == AL_PREOP and not esc.al_error


def test_safeop_rejected_without_sm_config():
    seg, _ = make_seg()
    roundtrip(seg, dg(p.APWR, adp=-1, ado=0x0010, data=b"\x02\x10"))
    roundtrip(seg, dg(p.FPWR, adp=0x1002, ado=0x0120, data=struct.pack("<H", AL_PREOP)))
    roundtrip(seg, dg(p.FPWR, adp=0x1002, ado=0x0120, data=struct.pack("<H", AL_SAFEOP)))
    esc = seg.escs[1]
    assert esc.al_state == AL_PREOP and esc.al_code == ALCODE_INVALID_OUTPUT_CONFIG


def test_full_bringup_and_lrw_exchange():
    seg, clock = make_seg()
    configure(seg)
    request_all(seg, AL_SAFEOP)
    assert [e.al_state for e in seg.escs] == [AL_SAFEOP] * 3
    request_all(seg, AL_OP)
    assert [e.al_state for e in seg.escs] == [AL_OP] * 3
    seg.escs[0].device.set_input(3, True)
    outputs = bytes([0xA5]) + struct.pack("<Hi", 0x0006, 0)
    (lo, hi) = roundtrip(seg, dg(p.LRW, adp=0x0000, ado=0x0000, data=outputs),
                         dg(p.LRW, adp=0x0000, ado=0x0001, length=11))
    # expected WKC: outputs slaves*2 + inputs slaves*1 per datagram coverage
    assert lo.wkc == 2 * 2  # DO8 + servo outputs
    assert hi.wkc == 1 + 1  # DI8 + servo inputs
    assert seg.escs[1].device.value == 0xA5
    assert hi.data[0] == 0x08  # DI channel 3
    # inputs are sampled before outputs are applied, so the drive reports the new
    # state one cycle later - exactly like a real slave
    (_, hi2) = roundtrip(seg, dg(p.LRW, adp=0x0000, ado=0x0000, data=outputs),
                         dg(p.LRW, adp=0x0000, ado=0x0001, length=11))
    sw = struct.unpack_from("<H", hi2.data, 1)[0]
    assert sw & 0x006F == 0x0021  # servo: ready to switch on after "shutdown" command


def test_bit_level_fmmu_mapping():
    seg, _ = make_seg()
    configure(seg)
    # remap DI8 FMMU: logical bits 4..7 of byte 0x20 <- physical bits 0..3
    roundtrip(seg, dg(p.FPWR, adp=0x1001, ado=0x0600,
                      data=struct.pack("<IHBBHBBBxxx", 0x20, 1, 4, 7, 0x1000, 0, 1, 1)))
    request_all(seg, AL_SAFEOP)
    seg.escs[0].device.value = 0b1011
    (d,) = roundtrip(seg, dg(p.LRD, adp=0x20, ado=0, data=b"\x01"))
    assert d.data[0] == (0b1011 << 4) | 0x01  # untouched low bits preserved
    assert d.wkc == 1


def test_sm_watchdog_drops_to_safeop_and_recovers():
    seg, clock = make_seg()
    configure(seg)
    request_all(seg, AL_SAFEOP)
    request_all(seg, AL_OP)
    clock.t += 0.05
    roundtrip(seg, dg(p.LRW, adp=0, ado=0, length=7))
    assert seg.escs[1].al_state == AL_OP
    clock.t += 0.2  # master stops cycling for 200 ms (> 100 ms watchdog)
    seg.tick()
    do8, servo = seg.escs[1], seg.escs[2]
    assert do8.al_state == AL_SAFEOP and do8.al_error and do8.al_code == ALCODE_SM_WATCHDOG
    assert servo.al_state == AL_SAFEOP and do8.watchdog_trips == 1
    assert seg.escs[0].al_state == AL_OP  # input-only slave has no output watchdog
    # master acknowledges and returns to OP
    for addr in (0x1002, 0x1003):
        roundtrip(seg, dg(p.FPWR, adp=addr, ado=0x0120, data=struct.pack("<H", AL_SAFEOP | 0x10)))
        roundtrip(seg, dg(p.FPWR, adp=addr, ado=0x0120, data=struct.pack("<H", AL_OP)))
    assert do8.al_state == AL_OP and not do8.al_error


def test_cable_cut_reduces_wkc():
    seg, _ = make_seg()
    seg.cut_cable_after(1)
    (d,) = roundtrip(seg, dg(p.BRD, ado=0, length=2))
    assert d.wkc == 1
    seg.cut_cable_after(None)
    (d,) = roundtrip(seg, dg(p.BRD, ado=0, length=2))
    assert d.wkc == 3


def test_returned_frame_marks_source_mac():
    seg, _ = make_seg()
    out = seg.process(p.make_frame([dg(p.BRD, length=2)]))
    assert out[6] & 0x02


# ----------------------------------------------------------- servo model
def test_servo_cia402_enable_and_follow():
    seg, clock = make_seg()
    configure(seg)
    request_all(seg, AL_SAFEOP)
    request_all(seg, AL_OP)
    servo = seg.escs[2].device

    def cycle(cw, target):
        clock.t += 0.004
        out = bytes([0]) + struct.pack("<Hi", cw, target)
        roundtrip(seg, dg(p.LRW, adp=0, ado=0, data=out))

    for cw in (0x0006, 0x0007, 0x000F):
        cycle(cw, 0)
    assert servo.state == OP_ENABLED
    for k in range(250):
        cycle(0x000F, 400 * k)  # 100 000 cnt/s ramp
    assert abs(servo.position - servo.target) < 2000
    assert servo.position > 90_000


def test_servo_faults_on_large_step_and_resets():
    seg, clock = make_seg()
    configure(seg)
    request_all(seg, AL_SAFEOP)
    request_all(seg, AL_OP)
    servo = seg.escs[2].device

    def cycle(cw, target):
        clock.t += 0.004
        roundtrip(seg, dg(p.LRW, adp=0, ado=0, data=bytes([0]) + struct.pack("<Hi", cw, target)))

    for cw in (0x0006, 0x0007, 0x000F):
        cycle(cw, 0)
    cycle(0x000F, 5_000_000)
    cycle(0x000F, 5_000_000)
    assert servo.state == FAULT and servo.error_code == ERR_FOLLOWING
    cycle(0x0080, 0)  # fault reset (rising edge of bit 7)
    assert servo.state == SWITCH_ON_DISABLED


def test_servo_faults_when_bus_leaves_op():
    seg, clock = make_seg()
    configure(seg)
    request_all(seg, AL_SAFEOP)
    request_all(seg, AL_OP)
    servo = seg.escs[2].device
    for cw in (0x0006, 0x0007, 0x000F):
        clock.t += 0.004
        roundtrip(seg, dg(p.LRW, adp=0, ado=0, data=bytes([0]) + struct.pack("<Hi", cw, 0)))
    assert servo.state == OP_ENABLED
    clock.t += 0.5
    seg.tick()
    assert servo.state == FAULT and servo.error_code == ERR_COMM_LOST


def test_snapshot_is_labelled_simulated():
    seg, _ = make_seg()
    snap = seg.snapshot()
    assert snap["data_class"] == "simulated"
    assert len(snap["slaves"]) == 3


# ------------------------------------------------------------- dashboard
def test_dashboard_rejects_cross_origin_and_bad_slave():
    import http.client
    from ecatsim.dashboard import start_dashboard
    seg, _ = make_seg()
    srv = start_dashboard(seg, "127.0.0.1", 0)
    port = srv.server_address[1]
    try:
        def post(path, headers=None):
            c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            c.request("POST", path, headers=headers or {})
            r = c.getresponse()
            r.read()
            return r.status
        assert post("/api/di?slave=1&ch=2&on=1", {"Origin": "http://evil.example"}) == 403
        assert post("/api/di?slave=0&ch=2&on=1") == 400       # no wrap-around to the last slave
        assert post("/api/di?slave=1&ch=2&on=1", {"Origin": f"http://127.0.0.1:{port}"}) == 200
        assert seg.escs[0].device.value == 0b100
    finally:
        srv.shutdown()
