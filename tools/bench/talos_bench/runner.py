"""Bench runner for TL-SN-001 (docs/hardware/bench-test-plan.md stages 5-6, cards in slots).

Drives a node's diag console through the tests in order. Every result is one JSON line:
    {"test", "stage", "verdict": "pass"|"fail"|"blocked"|"manual", "detail", "diagnosis"}
A test whose prerequisite did not pass is "blocked", never "fail", so one fault does not
bury the report in consequences. When a test fails, its fault tree probes further and names
the most likely cause in "diagnosis".

The same runner drives firmware/sim:bench_node in CI, where every injected fault must lead
to the right diagnosis (tests/test_runner.py).
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from typing import Callable

from talos_hil.console import Console, Reply

# Module codes (firmware/portable/modules.h).
CARD_CODES = {"TL-MOD-001": "0x0103", "TL-MOD-002": "0x0104", "TL-MOD-003": "0x0202",
              "TL-MOD-004": "0x0302", "TL-TEST-001": "0xF001"}
FAULT_NAMES = {0: "none", 1: "unknown module", 2: "no ID EEPROM", 3: "init failed", 4: "bus error",
               5: "disabled for power", 6: "missing resource", 7: "mux fault", 8: "ID blank",
               9: "ID corrupt", 10: "stuck interrupt", 11: "warm-up timeout"}
# sensor_edge_v1 finger numbers for the lines the slot loopback checks.
FINGERS = {"slot.wake": "WAKE finger 9", "slot.alert": "ALERT_N finger 10",
           "slot.cs_n": "CS_N finger 14", "slot.reset_n": "RESET_N finger 17",
           "slot.present": "PRESENT_N finger 18", "slot.branch_isolation": "SDA/SCL fingers 7/8"}
# Parts on each slot's I2C branch and AON rail (TL-SN-001); tests/test_contracts.py holds this
# table to the schematic netlist, so a renumbered schematic cannot leave diagnoses pointing at
# the wrong part.
SLOT_PARTS = {s: {"aon_switch": f"U{6 + s}", "aon_enable": f"EXP_A P0{4 + s}",
                  "pullups": (f"R{34 + 2 * s}", f"R{35 + 2 * s}"), "esd": f"D{4 + s}"} for s in range(4)}
ROOT_PARTS = {32: "EXP_A U15 (0x20)", 33: "EXP_B U16 (0x21)", 106: "charger U4 BQ25186 (0x6A)",
              112: "I2C mux U14 TCA9546A (0x70)"}


@dataclass
class Result:
    test: str
    stage: str
    verdict: str
    detail: dict = field(default_factory=dict)
    diagnosis: str = ""

    def to_json(self) -> str:
        return json.dumps({"test": self.test, "stage": self.stage, "verdict": self.verdict,
                           "detail": self.detail, "diagnosis": self.diagnosis},
                          separators=(",", ":"), sort_keys=True)


@dataclass
class Test:
    id: str
    stage: str
    run: Callable[["Bench"], Result]
    requires: tuple[str, ...] = ()


class Bench:
    def __init__(self, console: Console, expect: dict[int, str], sink: Callable[[Result], None] | None = None):
        self.c = console
        self.expect = expect  # slot -> card name, from the operator
        self.results: dict[str, Result] = {}
        self.sink = sink
        self._root_found: list[int] | None = None
        self._slots: list[dict] | None = None

    # --- helpers ------------------------------------------------------------------------

    def cmd(self, text: str, timeout: float = 15.0) -> Reply:
        return self.c.command(text, timeout=timeout)

    def slots(self, refresh: bool = False) -> list[dict]:
        if self._slots is None or refresh:
            self._slots = self.cmd("slots").records
        return self._slots

    def slot_scan(self, slot: int) -> list[int]:
        rec = self.cmd(f"i2c scan {slot}").record("i2c.scan") or {}
        root = set(self._root_found or [])
        return [a for a in rec.get("found", []) if a not in root]

    # --- running ------------------------------------------------------------------------

    def run(self, tests: list[Test]) -> list[Result]:
        out = []
        for t in tests:
            blocked = [r for r in t.requires if self.results.get(r) is None or self.results[r].verdict != "pass"]
            if blocked:
                res = Result(t.id, t.stage, "blocked", {"waiting_for": blocked})
            else:
                try:
                    res = t.run(self)
                except Exception as e:  # a console failure is a result, not a crash
                    res = Result(t.id, t.stage, "fail", {"error": str(e)},
                                 "no reply from the node: check USB, reset, and the boot log")
            self.results[t.id] = res
            out.append(res)
            if self.sink:
                self.sink(res)
        return out


# --- tests ------------------------------------------------------------------------------------

def t_identity(b: Bench) -> Result:
    rec = b.cmd("id").record("id") or {}
    ok = bool(rec) and rec.get("board_ok", False)
    diag = "" if ok else "expanders or charger did not take their safe state at boot: see S-M05/S-M06"
    return Result("S-M00", "mcu", "pass" if ok else "fail", rec, diag)


def t_root_bus(b: Bench) -> Result:
    reply = b.cmd("selftest i2c", timeout=30)
    scan = reply.record("i2c.root_scan") or {}
    b._root_found = scan.get("found", [])
    detail = {r["t"]: r.get("ok") for r in reply.records}
    detail.update(found=scan.get("found"), want=scan.get("want"))
    if reply.ok:
        return Result("S-M05", "mcu", "pass", detail)
    missing = [a for a in scan.get("want", []) if a not in scan.get("found", [])]
    extra = [a for a in scan.get("found", []) if a not in scan.get("want", [])]
    if missing:
        names = ", ".join(ROOT_PARTS.get(a, hex(a)) for a in missing)
        diag = f"not answering on the root bus: {names}. Check that part's I2C pins, address straps and rotation"
    elif extra:
        diag = f"unexpected root-bus devices {[hex(a) for a in extra]}: an address strap or a bridged branch"
    elif not detail.get("i2c.mux_readback", True):
        diag = "mux U14 does not read back its control register: U14 joints or RESET_N (R33)"
    elif not detail.get("i2c.expander_outputs", True):
        diag = "an expander's output latch does not read back: TCA9535 joints, or something driving its pins"
    else:
        diag = "charger configuration did not read back: BQ25186 I2C or a register reset (see `power`)"
    return Result("S-M05", "mcu", "fail", detail, diag)


def t_power(b: Bench) -> Result:
    reply = b.cmd("power")
    p = reply.record("power") or {}
    ch = reply.record("charger") or {}
    detail = {"source": p.get("source"), "battery_mv": p.get("battery_mv"), **ch}
    if reply.ok and p.get("source") != "unknown":
        return Result("S-P09", "power", "pass", detail)
    if p.get("source") == "unknown":
        diag = "power source unreadable: EXP_B U16 (CHG_PG_N, PMUX_ST) not answering"
    else:
        diag = reply.done.get("err") or "charger status unreadable"
    return Result("S-P09", "power", "fail", detail, diag)


def slot_test(slot: int, card: str) -> Test:
    def run(b: Bench) -> Result:
        rec = next((r for r in b.slots() if r.get("slot") == slot), {})
        want = CARD_CODES[card]
        detail = {"expected": card, "code": rec.get("code"), "fault": FAULT_NAMES.get(rec.get("fault"), rec.get("fault")),
                  "present": rec.get("present"), "enabled": rec.get("enabled")}
        if rec.get("code") == want and rec.get("enabled"):
            return Result(f"slot{slot}.discovered", "slots", "pass", detail)
        return Result(f"slot{slot}.discovered", "slots", "fail", detail, diagnose_slot(b, slot, rec, card))
    return Test(f"slot{slot}.discovered", "slots", run, requires=("S-M05",))


def diagnose_slot(b: Bench, slot: int, rec: dict, card: str) -> str:
    """Fault tree: why the expected card in this slot was not discovered."""
    fault = rec.get("fault")
    if rec.get("present") == 0:
        return (f"slot {slot}: no card seen (PRESENT_N high). Card not seated, or finger 18 / "
                f"PRESENT_PROBE path open")
    if fault == 7:
        return "mux U14 does not accept the branch select: U14 or its supply"
    if fault == 8:
        return f"slot {slot}: card ID never programmed. Program it on TL-DBG-001: talos_dbg program-id --card {card}"
    if fault == 9:
        return f"slot {slot}: card ID corrupt. Read it on TL-DBG-001 (`id --raw`) and reprogram"
    if fault == 3:
        return (f"slot {slot}: card answered but its sensor did not start. Run the card's selftest "
                f"on TL-DBG-001 to split a card fault from a slot fault")
    if fault == 1:
        return f"slot {slot}: a different or unknown card (code {rec.get('code')}), expected {card}"
    if fault == 2:
        others = b.slot_scan(slot)
        if not others:
            parts = SLOT_PARTS[slot]
            return (f"slot {slot}: nothing answers on branch {slot} although a card is seated. Branch "
                    f"dead: AON_{slot} rail ({parts['aon_switch']}, {parts['aon_enable']}), branch pull-ups "
                    f"{'/'.join(parts['pullups'])}, ESD {parts['esd']}, or U14 channel {slot}")
        return (f"slot {slot}: the card's sensors answer ({[hex(a) for a in others]}) but its ID EEPROM "
                f"(0x50) does not: card fault. Check the card on TL-DBG-001")
    if rec.get("code") not in (None, "0x0000") and rec.get("code") != CARD_CODES[card]:
        return f"slot {slot}: found {rec.get('code')}, expected {card} ({CARD_CODES[card]})"
    return f"slot {slot}: not discovered (fault {FAULT_NAMES.get(fault, fault)})"


def card_selftest(slot: int, card: str) -> Test | None:
    command = {"TL-TEST-001": "slot", "TL-MOD-003": "imu", "TL-MOD-004": "sht"}.get(card)
    if command is None:
        return None
    test_id = f"slot{slot}.{command}_selftest"

    def run(b: Bench) -> Result:
        reply = b.cmd(f"selftest {command} {slot}", timeout=30)
        detail = {r["t"]: r.get("ok") for r in reply.records}
        if reply.ok:
            return Result(test_id, "slots", "pass", detail)
        failed = [t for t, ok in detail.items() if ok is False]
        if command == "slot":
            contacts = ", ".join(FINGERS.get(t, t) for t in failed)
            diag = (f"slot {slot}: {contacts} not passing. Swap the test card to another slot: if the fault "
                    f"follows the card it is the card, otherwise the slot's socket or line")
        elif command == "imu":
            diag = (f"slot {slot}: LIS2DW12 " + ("self-test response out of range (70-1500 mg): sensing "
                    "element or its supply" if "imu.self_test" in failed else "gravity vector wrong: VDD, card "
                    "mounting, or a damaged part"))
        else:
            diag = (f"slot {slot}: SHT40 " + ("heater did not warm the die: heater, VDD stiffness, or the "
                    "copper under the sensor sinking the heat" if "sht.heater" in failed else "serial number or CRC failed"))
        return Result(test_id, "slots", "fail", detail, diag)
    return Test(test_id, "slots", run, requires=(f"slot{slot}.discovered",))


def plan(expect: dict[int, str]) -> list[Test]:
    tests = [Test("S-M00", "mcu", t_identity), Test("S-M05", "mcu", t_root_bus),
             Test("S-P09", "power", t_power, requires=("S-M05",))]
    for slot, card in sorted(expect.items()):
        tests.append(slot_test(slot, card))
        st = card_selftest(slot, card)
        if st:
            tests.append(st)
    return tests


def summary(results: list[Result]) -> dict:
    counts: dict[str, int] = {}
    for r in results:
        counts[r.verdict] = counts.get(r.verdict, 0) + 1
    return {"counts": counts, "failed": [{"test": r.test, "diagnosis": r.diagnosis}
                                         for r in results if r.verdict == "fail"],
            "time": time.strftime("%Y-%m-%dT%H:%M:%S")}
