"""Bench self-tests run through TL-DBG-001 (docs/hardware/bench-test-plan.md stages 1-3).

Each test returns records shaped like the firmware's diag output: {"t": name, "ok": bool, ...}.
"ok": None marks a step a person has to do (meter, PPK2), with the instruction in "manual".

    adapter      D-01..D-08: the adapter alone, slot empty
    tl-test-001  T-01..T-10: TL-TEST-001 seated (proves the fixture before it is trusted)
    tmag         C1-02/C1-03: TL-MOD-001 identity and the WAKE path, no magnet needed
    imu          C3-01..C3-04: TL-MOD-003 WHO_AM_I, gravity and the electrostatic self-test
    sht          C4-01/C4-05: TL-MOD-004 serial number with CRC and the heater rise
"""

from __future__ import annotations

import math

from . import id_block
from .adapter import AdapterError, DebugAdapter, card_addresses
from .mcp2221 import I2cNack

TEST_CARD_EXPANDER = 0x27
TMAG = 0x35
LIS2DW12 = 0x19
SHT40 = 0x44


def _rec(t: str, ok: bool | None, **extra) -> dict:
    return {"t": t, "ok": ok, **extra}


# --- stage 1: the adapter alone -------------------------------------------------------------

def adapter(a: DebugAdapter) -> list[dict]:
    out = [_rec("adapter.configured", a.is_configured(), problems=a.config_problems())]
    if not out[0]["ok"]:
        return out
    a.aon_off_and_wait()
    scl, sda = a.bus_levels()
    ln = a.lines()
    out.append(_rec("adapter.lines_aon_off", not scl and not sda and ln.alert_asserted is None,
                    scl=scl, sda=sda))
    a.set_rail("aon", True)
    scl, sda = a.bus_levels()
    ln = a.lines()
    out.append(_rec("adapter.lines_aon_on", scl and sda and ln.alert_asserted is False,
                    scl=scl, sda=sda, alert=ln.alert_asserted))
    found = a.scan() if scl and sda else []
    out.append(_rec("adapter.empty_scan", found == [], found=[hex(x) for x in found],
                    hint="" if not found else "a card is seated, or the adapter answers on its own bus"))
    try:
        discharge = a.power_cycle_aon()
        out.append(_rec("adapter.aon_discharge", True, seconds=round(discharge, 3)))
    except AdapterError as e:
        out.append(_rec("adapter.aon_discharge", False, error=str(e)))
    a.all_off()
    out.append(_rec("adapter.v3v3", None, manual="meter TP1: 3.251-3.350 V (AP2112K +/-1.5 %)"))
    out.append(_rec("adapter.baseline_current", None,
                    manual="PPK2 across JP1, AON on, slot empty: < 10 uA; subtract from card readings"))
    out.append(_rec("adapter.lamps", None,
                    manual="D-10: PWR lit on USB; AON and SW lamps follow `power aon|sw on|off`; FLT dark, lit on "
                           "a SW overload (10 R to GND) until `power sw off`; IN lit with USB where fitted"))
    return out


# --- stage 2: TL-TEST-001 -------------------------------------------------------------------

def _card_reg(a: DebugAdapter, reg: int) -> int:
    return a.read(TEST_CARD_EXPANDER, reg, 1)[0]


def _card_set(a: DebugAdapter, reg: int, value: int) -> None:
    a.write(TEST_CARD_EXPANDER, reg, bytes([value]))


def test_card(a: DebugAdapter) -> list[dict]:
    a.set_rail("aon", True)
    found = set(a.scan())
    want = card_addresses("TL-TEST-001")
    out = [_rec("card.scan", found == want, found=sorted(hex(x) for x in found),
                want=sorted(hex(x) for x in want))]
    if TEST_CARD_EXPANDER not in found:
        return out
    cid = a.card_id()
    again = a.card_id()
    out.append(_rec("card.id", cid is not None and cid.block.status == "ok" and cid.code == 0xF001,
                    id=cid.describe() if cid else None))
    eui_ok = cid is not None and again is not None and not id_block.eui_problems(cid.eui) and cid.eui == again.eui
    out.append(_rec("card.eui", eui_ok, eui=id_block.eui_text(cid.eui) if cid else None))
    # Everything released, then read what the adapter drives: CS_N, RESET_N (pull-ups), UART_TX (U6).
    _card_set(a, 0x06, 0xFF)
    _card_set(a, 0x07, 0xFF)
    port1 = _card_reg(a, 0x01)
    out.append(_rec("card.idle_inputs", (port1 & 0x13) == 0x13, cs_n=bool(port1 & 1),
                    reset_n=bool(port1 & 2), uart_tx=bool(port1 & 0x10)))
    # WAKE: the card drives it through 1k; GP1 reads it.
    levels = []
    for level in (1, 0):
        _card_set(a, 0x02, level)
        _card_set(a, 0x06, 0xFE)
        levels.append(a.lines().wake)
    out.append(_rec("card.wake", levels == [True, False], seen=levels))
    # The same through the edge detector, which catches pulses that polling misses.
    a.enter_wake_latch()
    try:
        a.take_wake_edge()  # clear anything left over
        _card_set(a, 0x02, 0x01)
        _card_set(a, 0x02, 0x00)
        out.append(_rec("card.wake_latch", a.take_wake_edge()))
    finally:
        a.leave_wake_latch()
    # ALERT_N: the card pulls it low; GP3 reads it.
    _card_set(a, 0x02, 0x00)
    _card_set(a, 0x06, 0xFC)
    asserted = a.lines().alert_asserted
    _card_set(a, 0x06, 0xFF)
    released = a.lines().alert_asserted
    out.append(_rec("card.alert", asserted is True and released is False))
    # A real AON power cycle resets the card: its config register returns to 0xFF.
    _card_set(a, 0x06, 0x00)
    discharge = a.power_cycle_aon()
    out.append(_rec("card.aon_reset", _card_reg(a, 0x06) == 0xFF, discharge_s=round(discharge, 3)))
    _card_set(a, 0x06, 0xFF)
    out.append(_rec("card.sw_current", None, manual="PPK2 across JP2, `power sw on`: 10.2-11.8 mA (330 R = 10.0 mA "
                    "+/-3 %, plus the green SW LED ~0.5-1.5 mA; PROVISIONAL until the LED part is fixed)"))
    out.append(_rec("card.aon_current", None, manual="PPK2 across JP1: < adapter baseline + 5 uA"))
    out.append(_rec("card.sw_lamp", None,
                    manual="T-11: TL-TEST-001 SW lamp dark with SW off, lit with SW on; judge the PPK2 reading with "
                           "`python3 -m talos_bench ... --measure T-11=<mA>` (10.2-11.8 mA)"))
    return out


# --- stage 3: sensor cards ------------------------------------------------------------------

def tmag(a: DebugAdapter) -> list[dict]:
    a.set_rail("aon", True)
    ids = a.read(TMAG, 0x0D, 3)  # DEVICE_ID, MANUFACTURER_ID LSB, MSB
    out = [_rec("tmag.identity", ids[1:] == b"\x49\x54" and ids[0] & 0x03 == 1,
                device_id=ids[0], manufacturer=f"0x{ids[2]:02X}{ids[1]:02X}")]
    status = a.read(TMAG, 0x1C, 1)[0]  # DEVICE_STATUS: OSC, INT, OTP CRC, VCC UV errors
    out.append(_rec("tmag.device_status", status & 0x0F == 0, status=f"0x{status:02X}"))
    # Forced threshold: two's complement (THR_HYST 0), X threshold -40 mT, "above" -> any field
    # fires. Wake-and-sleep with 1 ms sleep, latched INT on the INT pin -> card WAKE high.
    for reg, value in ((0x01, 0x00), (0x04, 0x80), (0x05, 0x00), (0x06, 0x00), (0x03, 0x00),
                       (0x02, 0x10), (0x08, 0x44), (0x01, 0x03)):
        a.write(TMAG, reg, bytes([value]))
    a._sleep(0.01)
    woke = a.lines().wake
    out.append(_rec("tmag.wake_path", woke,
                    hint="" if woke else "TMAG INT -> SN74AUP1G04 -> WAKE: probe MAG_INT_N to split the two"))
    a.write(TMAG, 0x01, bytes([0x00]))  # any access clears the latch; leave it in standby
    out.append(_rec("tmag.latch_clears", not a.lines().wake))
    out.append(_rec("tmag.distance_sweep", None, manual="magnet sweep 0-40 mm, 4 orientations (C1-06)"))
    return out


def imu(a: DebugAdapter) -> list[dict]:
    a.set_rail("aon", True)
    who = a.read(LIS2DW12, 0x0F, 1)[0]
    out = [_rec("imu.who_am_i", who == 0x44, value=f"0x{who:02X}")]
    if who != 0x44:
        return out

    def average() -> list[int]:
        sums = [0, 0, 0]
        for s in range(6):
            a._sleep(0.02)
            raw = a.read(LIS2DW12, 0x28, 6)
            if s == 0:
                continue
            for axis in range(3):
                v = int.from_bytes(raw[2 * axis:2 * axis + 2], "little", signed=True)
                sums[axis] += int((v // 4) * 0.488)
        return [x // 5 for x in sums]

    for reg, value in ((0x20, 0x44), (0x21, 0x0C), (0x25, 0x10), (0x22, 0x00)):
        a.write(LIS2DW12, reg, bytes([value]))
    a._sleep(0.1)
    base = average()
    a.write(LIS2DW12, 0x22, bytes([0x40]))
    a._sleep(0.1)
    pos = average()
    a.write(LIS2DW12, 0x22, bytes([0x80]))
    a._sleep(0.1)
    neg = average()
    a.write(LIS2DW12, 0x22, bytes([0x00]))
    a.write(LIS2DW12, 0x20, bytes([0x00]))  # power down
    norm = math.sqrt(sum(x * x for x in base))
    out.append(_rec("imu.gravity", 940 <= norm <= 1060, mg=base, norm_mg=round(norm)))
    dpos = [p - b for p, b in zip(pos, base)]
    dneg = [n - b for n, b in zip(neg, base)]
    ok = all(70 <= d <= 1500 for d in dpos) and all(-1500 <= d <= -70 for d in dneg)
    out.append(_rec("imu.self_test", ok, delta_pos_mg=dpos, delta_neg_mg=dneg))
    return out


def sht(a: DebugAdapter, min_rise_milli_c: int = 5000) -> list[dict]:
    a.set_rail("aon", True)
    a.scan()  # checks the bus first

    def command(cmd: int, wait_s: float) -> bytes | None:
        a.bridge.i2c_write(SHT40, bytes([cmd]))
        a._sleep(wait_s)
        try:
            r = a.bridge.i2c_read(SHT40, 6)
        except I2cNack:
            return None
        if id_block.crc8(r[0:2]) != r[2] or id_block.crc8(r[3:5]) != r[5]:
            return None
        return r

    def milli_c(r: bytes) -> int:
        return -45000 + 175000 * ((r[0] << 8) | r[1]) // 65535

    r = command(0x89, 0.002)
    serial = (r[0] << 24 | r[1] << 16 | r[3] << 8 | r[4]) if r else None
    out = [_rec("sht.serial", serial not in (None, 0, 0xFFFFFFFF), serial=serial)]
    before = command(0xFD, 0.01)
    after = command(0x2F, 1.11)  # 110 mW for 1 s, then a measurement of the hot die
    if before is None or after is None:
        out.append(_rec("sht.heater", False, error="no reply or CRC error"))
        return out
    t0, t1 = milli_c(before), milli_c(after)
    out.append(_rec("sht.heater", t1 - t0 >= min_rise_milli_c, before_milli_c=t0, after_milli_c=t1,
                    note="threshold provisional: calibrate to half a golden unit's rise"))
    return out


STAGES = {"adapter": adapter, "tl-test-001": test_card, "tmag": tmag, "imu": imu, "sht": sht}
