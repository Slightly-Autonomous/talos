"""Bench plan for TL-SN-002, the self-contained sensor node (ADR 0019).

docs/hardware/bench-test-plan.md "TL-SN-002". The slot steps of TL-SN-001 (card presence,
branch isolation, per-slot rails, flipped cards, TL-DBG-001/TL-TEST-001) do not apply: the
sensors are soldered on. What stays: board identity (S-M16), tamper (S-M17, S-M19). New:
each built-in sensor answering, every lamp, the three buttons, USB/battery switchover, the
PIR rail and the AA reading.

Same result model as runner.py: pass / fail (with a diagnosis) / blocked / manual. No part
reference designators yet: the TL-SN-002 schematic is still being generated, so diagnoses name
parts and nets. PROVISIONAL like the board.

Console lock (ADR 0019 amendment 2026-11-22): TL-SN-002's USB-C port is reachable from outside
the closed enclosure, so its console is READ-ONLY while the lid is closed and every firmware lamp
(STAT included) stays dark. The plan checks that (S-M20) with the lid closed, then OPENS THE LID
(S-M21) before anything that changes state: self-tests, the PIR rail, lamps, buttons. On a real
board S-M21 asks the operator to open the lid (which raises the tamper alarm: put the node in
maintenance on the hub first) and waits for the loop to read open; on the virtual board it opens
it with `sim fault lid_open`.
"""

from __future__ import annotations

import os
import sys
import time

from .runner import Bench, Result, Test

BOARD = {"board": "TL-SN-002", "rev": "A"}
# Built-in "slots" (firmware/esp_idf/main/board_tl_sn_002.h).
SENSORS = {
    0: ("MAG", "0x0105", "TMAG5273A2 at 0x35 (INT_N through the SN74AUP1G04 to GPIO10)"),
    1: ("VIB", "0x0201", "LIS2DW12 at 0x19 (SA0 and CS high; INT1 to GPIO11)"),
    2: ("PIR", "0x0106", "PIR223D-S0 on PIR_PWR (EXP P10), output to GPIO12"),
    3: ("ENV", "0x0301", "SHT40-AD1B at 0x44"),
}
ROOT_PARTS = {0x19: "LIS2DW12 (0x19)", 0x20: "TCA9535 lamp expander (0x20, A0-A2 GND)",
              0x35: "TMAG5273A2 (0x35)", 0x44: "SHT40 (0x44)"}
LAMPS = ["LINK", "TAMP", "BATT", "FLT"]  # `led` lamp_lit order (firmware Lamp enum)
TAMPER_GPIO = 13
USER_GPIO = 14
LOCKED_ERR = "locked: open the enclosure"  # firmware/portable/console_policy.h
# How long a real-board step waits for the operator to open or close the lid.
LID_WAIT_S = float(os.environ.get("TALOS_LID_WAIT_S", "120"))


def lid(b: Bench, open_: bool, wait_s: float | None = None) -> int | None:
    """Puts the enclosure lid in the wanted state and returns `id`'s tamper reading.

    Virtual board: `sim fault lid_open|lid_closed`, past the debounce. Real board: asks the
    operator (stderr) and polls until the loop reads that way, or `wait_s` passes.
    """
    want = 1 if open_ else 0
    if b.simulated():
        b.cmd(f"sim fault {'lid_open' if open_ else 'lid_closed'}")
        b.cmd("sim run 300")
        return (b.cmd("id").record("id") or {}).get("tamper")
    print(("OPEN the enclosure lid (the console is read-only while it is closed; this raises the "
           "tamper alarm: start maintenance on the hub first)" if open_ else
           "CLOSE the enclosure lid (or fit the bench tamper jumper)"),
          file=sys.stderr, flush=True)
    deadline = time.monotonic() + (LID_WAIT_S if wait_s is None else wait_s)
    tamper = None
    while time.monotonic() < deadline:
        tamper = (b.cmd("id").record("id") or {}).get("tamper")
        if tamper == want:
            break
        time.sleep(0.5)
    return tamper


def t_identity(b: Bench) -> Result:
    rec = b.cmd("id").record("id") or {}
    ok = bool(rec) and rec.get("board_ok", False)
    return Result("S-M00", "mcu", "pass" if ok else "fail", rec,
                  "" if ok else "the lamp expander (0x20) did not take its safe state at boot: see N-05")


def t_board_identity(b: Bench) -> Result:
    rec = b.cmd("id").record("id") or {}
    detail = {k: rec.get(k) for k in ("board", "rev", "serial")}
    if not rec.get("board") or not rec.get("serial"):
        return Result("S-M16", "mcu", "fail", detail,
                      "board identity never programmed: python3 -m talos_bench.flash --port <port> "
                      "--unit-serial <label serial> (or talos_dbg program-id --node <port> --board TL-SN-002)")
    if (rec.get("board"), rec.get("rev")) != (BOARD["board"], BOARD["rev"]):
        return Result("S-M16", "mcu", "fail", detail,
                      f"board identity says {rec.get('board')} rev {rec.get('rev')}, expected TL-SN-002 rev A: "
                      "wrong firmware target (Kconfig TALOS_BOARD) or wrong board on the bench")
    return Result("S-M16", "mcu", "pass", detail)


def t_root_bus(b: Bench) -> Result:
    reply = b.cmd("selftest i2c", timeout=30)
    scan = reply.record("i2c.root_scan") or {}
    detail = {r["t"]: r.get("ok") for r in reply.records}
    detail.update(found=scan.get("found"), want=scan.get("want"))
    if reply.ok:
        return Result("N-05", "mcu", "pass", detail)
    missing = [a for a in scan.get("want", []) if a not in scan.get("found", [])]
    extra = [a for a in scan.get("found", []) if a not in scan.get("want", [])]
    if missing:
        names = ", ".join(ROOT_PARTS.get(a, hex(a)) for a in missing)
        diag = (f"not answering on the I2C bus: {names}. Check that part's SDA/SCL joints, supply and "
                "address straps (GPIO2/3, 4.7k pull-ups)")
    elif extra:
        diag = f"unexpected devices {[hex(a) for a in extra]}: an address strap (TMAG variant? SHT40 suffix?)"
    else:
        diag = "the lamp expander's output latch does not read back: TCA9535 joints"
    return Result("N-05", "mcu", "fail", detail, diag)


def sensor_test(slot: int) -> Test:
    name, code, part = SENSORS[slot]
    test_id = f"N-0{slot + 1}.{name.lower()}"

    def run(b: Bench) -> Result:
        rec = next((r for r in b.slots() if r.get("slot") == slot), {})
        detail = {"code": rec.get("code"), "enabled": rec.get("enabled"), "fault": rec.get("fault"),
                  "built_in": rec.get("built_in")}
        if rec.get("code") == code and rec.get("enabled") and rec.get("built_in"):
            return Result(test_id, "sensors", "pass", detail)
        if rec.get("code") != code or not rec.get("built_in"):
            return Result(test_id, "sensors", "fail", detail,
                          f"{name}: the firmware is not the TL-SN-002 target (built-in {code} expected)")
        return Result(test_id, "sensors", "fail", detail,
                      f"{name}: {part} did not start (fault {rec.get('fault')}). Built in: no card to "
                      "reseat; check that part's joints, its supply and its interrupt line")
    return Test(test_id, "sensors", run, requires=("S-M00",))


def selftest(command: str, slot: int) -> Test:  # needs the lid open (S-M21)
    test_id = f"N-06.{command}"

    def run(b: Bench) -> Result:
        reply = b.cmd(f"selftest {command} {slot}", timeout=30)
        detail = {r["t"]: r.get("ok") for r in reply.records}
        if reply.ok:
            return Result(test_id, "sensors", "pass", detail)
        diag = ("LIS2DW12 self-test out of range: sensing element or its supply"
                if command == "imu" else "SHT40 heater did not warm the die: heater, supply, or the "
                "thermal tongue not routed out (copper sinking the heat)")
        return Result(test_id, "sensors", "fail", detail, diag)
    return Test(test_id, "sensors", run, requires=(f"N-0{slot + 1}.{SENSORS[slot][0].lower()}", "S-M21"))


def t_pir_rail(b: Bench) -> Result:
    """N-07: PIR_PWR switches the PIR's rail; discovery leaves it on."""
    rec = next((r for r in b.slots(refresh=True) if r.get("slot") == 2), {})
    detail = {"pir_power": rec.get("aon")}
    if not rec.get("aon"):
        return Result("N-07", "power", "fail", detail,
                      "PIR_PWR is off after discovery: EXP P10, or the PIR was quarantined (see N-03)")
    off = b.cmd("rail 2 aon off").record("rail") or {}
    on = b.cmd("rail 2 aon on").record("rail") or {}
    b.cmd("rescan")
    detail.update(off=off.get("aon"), on=on.get("aon"))
    if off.get("aon") is not False or on.get("aon") is not True:
        return Result("N-07", "power", "fail", detail, "PIR_PWR does not follow: TCA9535 P10 or its RC filter")
    detail["manual"] = ("scope the PIR supply across `rail 2 aon off/on`: it falls and rises (RC filter), "
                        "and the PIR output stays low through warm-up")
    return Result("N-07", "power", "pass", detail)


def t_lamps(b: Bench) -> Result:
    """N-08: every lamp, lid open (S-M21): lit, TAMP on; service mode; the USER lamp test blinks them all.
    Dark with the lid closed is S-M20."""
    if b.simulated():
        b.cmd("sim run 300")  # let the 150 ms event flashes from the previous step (rescan) pass
    led = b.cmd("led").record("led") or {}
    detail: dict = {"external_power": led.get("external_power"), "lamps": led.get("lamp_lit"),
                    "sensor_lamps": led.get("slot_lit"), "unattended": led.get("unattended")}
    if led.get("lamp_lit") is None:
        return Result("N-08", "indicators", "fail", detail, "no board lamps reported: not the TL-SN-002 target")
    if not led.get("unattended") or led.get("lamp_lit")[1] != 1:
        return Result("N-08", "indicators", "fail", detail,
                      "lid open but TAMP is dark: TCA9535 P05, its resistor or lamp (or the lid reads closed)")
    on = b.cmd("led service on").record("led") or {}
    b.cmd("led service off")
    detail.update(service_lamps=on.get("lamp_lit"), service_sensor_lamps=on.get("slot_lit"))
    if on.get("slot_lit") != [1, 1, 1, 1] or not on.get("lamp_lit") or not on.get("lamp_lit")[0]:
        return Result("N-08", "indicators", "fail", detail,
                      "service mode did not light LINK and the four sensor lamps: TCA9535 P00-P04, "
                      "2.2k resistors or the lamps")
    detail["manual"] = ("look: each of the 8 lamps (LINK MAG VIB PIR ENV TAMP BATT FLT) blinks in the USER lamp "
                        "test, PWR is lit on USB only, STAT shows the boot code")
    return Result("N-08", "indicators", "pass", detail)


def t_buttons(b: Bench) -> Result:
    """N-09: USER (short press = test report + lamp test), BOOT (service mode), RST (manual)."""
    if not b.simulated():
        return Result("N-09", "indicators", "manual", {},
                      "press USER briefly: every lamp blinks for 10 s and `led` shows lamp_test true; hold USER "
                      "3 s: a user.button diagnostic (commissioning) reaches the hub; press BOOT: `led` shows "
                      "service true; press RST: the console reconnects with a new boot epoch")
    b.cmd("sim fault user_press")
    b.cmd("sim run 200")
    b.cmd("sim fault user_release")
    b.cmd("sim run 100")
    led = b.cmd("led").record("led") or {}
    detail = {"lamp_test": led.get("lamp_test")}
    if not led.get("lamp_test"):
        return Result("N-09", "indicators", "fail", detail,
                      f"a USER press did nothing: the button, its 100k pull-down or GPIO{USER_GPIO}")
    b.cmd("sim fault boot_press")
    b.cmd("sim run 100")
    b.cmd("sim fault boot_release")
    b.cmd("sim run 100")
    led = b.cmd("led").record("led") or {}
    detail["service"] = led.get("service")
    b.cmd("led service off")
    if not led.get("service"):
        return Result("N-09", "indicators", "fail", detail, "a BOOT press did not toggle service mode: GPIO9")
    detail["manual"] = "RST: press it, the console reconnects with a new boot epoch"
    return Result("N-09", "indicators", "pass", detail)


def t_source(b: Bench) -> Result:
    """N-10: USB/battery switchover reading (TPS2116 + USB_PRESENT on GPIO4)."""
    p = b.cmd("power").record("power") or {}
    detail = {"source": p.get("source"), "battery_mv": p.get("battery_mv")}
    if p.get("source") != "usb":
        return Result("N-10", "power", "fail", detail,
                      "on USB but USB_PRESENT reads low: the VBUS 100k/100k divider or GPIO4")
    if not b.simulated():
        detail["manual"] = ("the console runs over USB, so the switchover is checked on the meter: with cells "
                            "fitted, unplug USB and the node keeps running (PWR lamp off), plug it back: no reset")
        return Result("N-10", "power", "manual", detail,
                      "USB side reads correctly; battery side needs the meter (see detail)")
    b.cmd("sim fault usb_off")
    b.cmd("sim run 50")
    off = b.cmd("power").record("power") or {}
    b.cmd("sim fault usb_on")
    b.cmd("sim run 50")
    detail["unplugged"] = off.get("source")
    if off.get("source") != "battery":
        return Result("N-10", "power", "fail", detail, "USB unplugged but the node still reads USB: GPIO4 stuck high")
    return Result("N-10", "power", "pass", detail)


def t_battery(b: Bench) -> Result:
    """N-12: the AA pair reads, under load, with a level estimate."""
    reply = b.cmd("power")
    bat = reply.record("battery") or {}
    detail = {k: bat.get(k) for k in ("loaded_mv", "percent", "low")}
    if not bat:
        return Result("N-12", "power", "fail", detail, "no battery record: not the TL-SN-002 target")
    if bat.get("low"):
        return Result("N-12", "power", "fail", detail,
                      "cells read low or missing (< 2.2 V under load): fit two fresh AA lithium cells; if fitted, "
                      "the reverse-protection P-FET or the 4.7M/4.7M VBAT divider on GPIO1")
    return Result("N-12", "power", "pass", detail)


def t_tamper(b: Bench) -> Result:
    """S-M17 on TL-SN-002: the lid + wall loop on GPIO13 reads closed."""
    if not b.simulated():
        tamper = lid(b, False)  # the operator closes it (e.g. after flashing with it open)
    else:
        deadline = time.monotonic() + 2.0
        while True:
            tamper = (b.cmd("id").record("id") or {}).get("tamper")
            if tamper in (0, 1) or time.monotonic() > deadline:
                break
            time.sleep(0.1)
    if tamper == 0:
        return Result("S-M17", "mcu", "pass", {"tamper": tamper})
    if tamper == 1:
        return Result("S-M17", "mcu", "fail", {"tamper": tamper},
                      "tamper loop open: lid or wall switch open, the loop not fitted (bench: jumper) or cut")
    return Result("S-M17", "mcu", "fail", {"tamper": tamper}, f"tamper state unknown: GPIO{TAMPER_GPIO}")


def t_console_lock(b: Bench) -> Result:
    """S-M20: lid closed: the USB console is read-only and every firmware lamp is dark (on USB)."""
    rec = b.cmd("id").record("id") or {}
    before = rec.get("console_refusals", 0)
    detail: dict = {"console_locked": rec.get("console_locked")}
    if rec.get("console_locked") is not True:
        return Result("S-M20", "mcu", "fail", detail,
                      "console NOT locked with the lid closed: not the TL-SN-002 target or its board table "
                      "lost console_lock = kLidClosed. Do not ship: anyone with a cable could change the node")
    refused = b.cmd("led service on")
    detail["refusal"] = refused.done.get("err")
    if refused.ok or refused.done.get("err") != LOCKED_ERR:
        return Result("S-M20", "mcu", "fail", detail,
                      "a mutating command was not refused with the lid closed: console lock broken")
    led = b.cmd("led").record("led") or {}
    after = (b.cmd("id").record("id") or {}).get("console_refusals", 0)
    detail.update(external_power=led.get("external_power"), lamps=led.get("lamp_lit"),
                  sensor_lamps=led.get("slot_lit"), stat=led.get("status_lit"), refusals=after)
    if after != before + 1:
        return Result("S-M20", "mcu", "fail", detail, "the refusal was not counted (console_refusals)")
    if any(led.get("lamp_lit") or []) or any(led.get("slot_lit") or []) or led.get("status_lit"):
        return Result("S-M20", "indicators", "fail", detail,
                      "lamps lit with the lid closed: they must stay dark even on USB (LampGate::kLidOpen)")
    if not b.simulated():
        detail["manual"] = "look: with USB plugged in and the lid closed only PWR is lit; STAT and the 8 lamps dark"
    return Result("S-M20", "mcu", "pass", detail)


def t_open_lid(b: Bench) -> Result:
    """S-M21: open the lid; the console takes commands again (the rest of the plan needs it)."""
    tamper = lid(b, True)
    rec = b.cmd("id").record("id") or {}
    detail = {"tamper": tamper, "console_locked": rec.get("console_locked")}
    if tamper != 1:
        return Result("S-M21", "mcu", "fail", detail,
                      "the lid never read open: open the enclosure (timed out), or the tamper loop on "
                      f"GPIO{TAMPER_GPIO} is shorted to GND")
    if rec.get("console_locked") is not False:
        return Result("S-M21", "mcu", "fail", detail, "lid open but the console is still locked")
    return Result("S-M21", "mcu", "pass", detail)


def t_tamper_deep_sleep(b: Bench) -> Result:
    """S-M19 on TL-SN-002: the lid opened in deep sleep wakes the node on GPIO13."""
    if not b.simulated():
        return Result("S-M19", "mcu", "manual", {},
                      "deep-sleep entry is not wired in firmware yet: when it is, sleep the node, open the lid, "
                      f"the boot log shows an ext1 wake with GPIO{TAMPER_GPIO}")
    b.cmd("sim fault lid_closed")
    b.cmd("sim run 300")
    b.cmd("sim fault lid_open")
    b.cmd("sim deepsleep")
    b.cmd("sim run 300")
    rec = b.cmd("id").record("id") or {}
    lines = int(rec.get("wake_lines", "0x0"), 16)
    detail = {k: rec.get(k) for k in ("wake_cause", "wake_lines", "tamper")}
    b.cmd("sim fault lid_closed")
    b.cmd("sim run 300")
    if rec.get("wake_cause") != 1 or not lines >> TAMPER_GPIO & 1:
        return Result("S-M19", "mcu", "fail", detail,
                      f"lid opened in deep sleep but no wake on GPIO{TAMPER_GPIO}: the loop's pull-up or the line")
    if rec.get("tamper") != 1:
        return Result("S-M19", "mcu", "fail", detail, "woke on the tamper line but reads closed")
    return Result("S-M19", "mcu", "pass", detail)


def plan() -> list[Test]:
    tests = [Test("S-M00", "mcu", t_identity), Test("S-M16", "mcu", t_board_identity),
             Test("N-05", "mcu", t_root_bus)]
    tests += [sensor_test(s) for s in sorted(SENSORS)]
    # Lid closed: tamper reads closed, the console is read-only, the lamps dark. Then open it.
    tests += [Test("S-M17", "mcu", t_tamper, requires=("S-M00",)),
              Test("S-M20", "mcu", t_console_lock, requires=("S-M17",)),
              Test("S-M19", "mcu", t_tamper_deep_sleep, requires=("S-M17",)),
              Test("S-M21", "mcu", t_open_lid, requires=("S-M17",))]
    # Lid open from here: commands that change state.
    tests += [selftest("imu", 1), selftest("sht", 3),
              Test("N-07", "power", t_pir_rail, requires=("N-03.pir", "S-M21")),
              Test("N-08", "indicators", t_lamps, requires=("N-05", "S-M21")),
              Test("N-09", "indicators", t_buttons, requires=("N-08",)),
              Test("N-10", "power", t_source, requires=("S-M00",)),
              Test("N-12", "power", t_battery, requires=("S-M00",))]
    return tests
