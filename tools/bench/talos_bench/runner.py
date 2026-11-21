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
           "slot.present": "PRESENT_N finger 19", "slot.branch_isolation": "SDA/SCL fingers 7/8"}
# sensor_edge_v1 after HD-025: PRESENT_N on contact 19, contact 18 reserved (ANALOG_RSVD).
# Parts on each slot's I2C branch and AON rail (TL-SN-001); tests/test_contracts.py holds this
# table to the schematic netlist, so a renumbered schematic cannot leave diagnoses pointing at
# the wrong part.
SLOT_PARTS = {s: {"aon_switch": f"U{6 + s}", "aon_enable": f"EXP_A P0{4 + s}",
                  "pullups": (f"R{34 + 2 * s}", f"R{35 + 2 * s}"), "esd": f"D{4 + s}"} for s in range(4)}
ROOT_PARTS = {32: "EXP_A U15 (0x20)", 33: "EXP_B U16 (0x21)", 34: "EXP_C U18 (0x22, slot LEDs and tamper)",
              106: "charger U4 BQ25186 (0x6A)", 112: "I2C mux U14 TCA9546A (0x70)"}
# D-09: what `talos_dbg program-id --node` writes into the node's talos_id partition.
BOARD = {"board": "TL-SN-001", "rev": "A"}


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


# T-07/T-11: TL-TEST-001 SW load, 330 R (10.0 mA +/-3 %) plus its green SW LED (~0.5-1.5 mA).
SW_CURRENT_MA = (10.2, 11.8)
EXP_C = 0x22
EXP_C_UNUSED = 0xFFE0  # P05-P07, P10-P17: outputs driven low (TI guidance)
EXP_C_LEDS = 0x000F  # P00-P03: slot LEDs S0-S3, active low


class Bench:
    def __init__(self, console: Console, expect: dict[int, str], sink: Callable[[Result], None] | None = None,
                 flipped: dict[int, str] | None = None, measure: dict[str, float] | None = None):
        self.c = console
        self.expect = expect  # slot -> card name, from the operator
        self.flipped = flipped or {}  # slot -> card the operator inserted upside down (C0-01)
        self.measure = measure or {}  # test id -> value the operator measured (T-11: mA)
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

    def simulated(self) -> bool:
        """True on firmware/sim:bench_node, which takes "sim ..." lines; a real node refuses them."""
        if not hasattr(self, "_sim"):
            self._sim = self.cmd("sim run 0").done.get("cmd") == "sim"
        return self._sim

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


def t_board_identity(b: Bench) -> Result:
    """D-09: board name, revision and serial, written once at program time, read back."""
    rec = b.cmd("id").record("id") or {}
    detail = {k: rec.get(k) for k in ("board", "rev", "serial")}
    if not rec.get("board") or not rec.get("serial"):
        return Result("S-M16", "mcu", "fail", detail,
                      "board identity never programmed: talos_dbg program-id --node <port> "
                      f"--board {BOARD['board']} --board-revision {BOARD['rev']} --unit-serial <label serial>")
    if rec.get("board") != BOARD["board"] or rec.get("rev") != BOARD["rev"]:
        return Result("S-M16", "mcu", "fail", detail,
                      f"board identity says {rec.get('board')} rev {rec.get('rev')}, expected "
                      f"{BOARD['board']} rev {BOARD['rev']}: wrong firmware target or wrong board on the bench")
    return Result("S-M16", "mcu", "pass", detail)


def t_tamper(b: Bench) -> Result:
    """HD-032: the J9 lid loop reads closed with the lid on (or the bench jumper fitted)."""
    # The node debounces the loop for 50 ms after boot: unknown (-1) until then.
    deadline = time.monotonic() + 2.0
    while True:
        rec = b.cmd("id").record("id") or {}
        tamper = rec.get("tamper")
        if tamper in (0, 1) or time.monotonic() > deadline:
            break
        time.sleep(0.1)
    detail = {"tamper": tamper}
    if tamper == 0:
        return Result("S-M17", "mcu", "pass", detail)
    if tamper == 1:
        return Result("S-M17", "mcu", "fail", detail,
                      "TAMPER_N high: lid switch open, J9 loop not fitted (bench: jumper J9) or the wire cut")
    return Result("S-M17", "mcu", "fail", detail,
                  "tamper state unknown: EXP_C U18 (0x22) not answering, see S-M05")


def t_lamps_exp_c(b: Bench) -> Result:
    """S-M18: EXP_C unused pins driven low; slot lamps dark on battery; service mode lights them."""
    reply = b.cmd(f"exp {EXP_C}")
    rec = reply.record("exp") or {}
    detail: dict = {"exp_c": rec}
    if not reply.ok:
        return Result("S-M18", "mcu", "fail", detail, "EXP_C U18 (0x22) not answering: see S-M05")
    cfg, outs, ins = (int(rec[k], 16) for k in ("config", "outputs", "inputs"))
    if cfg & EXP_C_UNUSED:
        return Result("S-M18", "mcu", "fail", detail,
                      "EXP_C unused pins are inputs, not driven low: firmware board table (unused_pins) "
                      "or EXP_C did not take its configuration (power-on reset after boot?)")
    if outs & EXP_C_UNUSED:
        return Result("S-M18", "mcu", "fail", detail, "EXP_C unused pins latched high: firmware safe state")
    if ins & EXP_C_UNUSED:
        pins = [f"P{p // 8}{p % 8}" for p in range(16) if (ins & EXP_C_UNUSED) >> p & 1]
        return Result("S-M18", "mcu", "fail", detail,
                      f"EXP_C {', '.join(pins)} read high while driven low: solder bridge to a neighbour "
                      "or to V3V3 (TSSOP-24 U18, check under the loupe)")
    led = b.cmd("led").record("led") or {}
    seated = {r["slot"] for r in b.slots() if r.get("enabled")}
    want = [1 if (led.get("external_power") and s in seated) else 0 for s in range(4)]
    detail.update(external_power=led.get("external_power"), lit=led.get("slot_lit"), want=want)
    if led.get("slot_lit") != want:
        diag = ("slot lamps lit on battery: they must stay dark (an intruder must not see them react)"
                if not led.get("external_power") else "slot lamps do not show the seated cards on external power")
        return Result("S-M18", "mcu", "fail", detail, diag)
    on = b.cmd("led service on").record("led") or {}
    b.cmd("led service off")
    want_on = [1 if s in seated else 0 for s in range(4)]
    detail["service_lit"] = on.get("slot_lit")
    if on.get("slot_lit") != want_on:
        return Result("S-M18", "mcu", "fail", detail,
                      "service mode did not light the seated slots: EXP_C P00-P03, the 2.2k resistors or LEDs")
    detail["manual"] = ("look: every lamp lights in service mode and is visible through the enclosure; "
                        "unplug USB: all four go dark")
    return Result("S-M18", "mcu", "pass", detail)


def t_tamper_deep_sleep(b: Bench) -> Result:
    """S-M19: a lid opened during deep sleep wakes the node through EXP_WAKE and is reported."""
    if not b.simulated():
        return Result("S-M19", "mcu", "manual", {},
                      "real board: deep-sleep entry is not wired in firmware yet (main.cc light-polls). "
                      "When it is: sleep the node, open J9, check the boot log shows ext1 wake with "
                      "GPIO14 (lines bit 14) and a P0 tamper reaches the hub")
    b.cmd("sim fault lid_closed")
    b.cmd("sim run 300")
    b.cmd("sim fault lid_open")  # the lid comes off while the node sleeps
    b.cmd("sim deepsleep")
    b.cmd("sim run 300")
    rec = b.cmd("id").record("id") or {}
    lines = int(rec.get("wake_lines", "0x0"), 16)
    detail = {k: rec.get(k) for k in ("wake_cause", "wake_lines", "resumed", "tamper", "exp_wake_masked")}
    b.cmd("sim fault lid_closed")
    b.cmd("sim run 300")
    if rec.get("wake_cause") != 1 or not lines >> 14 & 1:
        diag = ("lid opened during deep sleep but the node did not wake on EXP_WAKE: EXP_WAKE (TP45, GPIO14) "
                "never rose. Check U19 SN74AUP1G04 (supply, orientation), the EXP_INT_N pull-up, and that EXP_C "
                "U18 INT reaches EXP_INT_N (TP19)")
        if rec.get("exp_wake_masked"):
            diag = "EXP_WAKE is masked as a wake source (held high for 30 s): an expander INT is stuck, see TP19"
        return Result("S-M19", "mcu", "fail", detail, diag)
    if rec.get("tamper") != 1:
        return Result("S-M19", "mcu", "fail", detail,
                      "woke on EXP_WAKE but TAMPER_N reads closed: J9 wiring or EXP_C P04")
    return Result("S-M19", "mcu", "pass", detail)


def flipped_card_test(slot: int, card: str) -> Test:
    """C0-01 (HD-025): a card inserted upside down must not trip a rail or reset the node."""
    def run(b: Bench) -> Result:
        epoch = (b.results["S-M00"].detail or {}).get("boot_epoch")
        rec = next((r for r in b.slots(refresh=True) if r.get("slot") == slot), {})
        now = b.cmd("id").record("id") or {}
        detail = {"card": card, "present": rec.get("present"), "fault": FAULT_NAMES.get(rec.get("fault"), rec.get("fault")),
                  "boot_epoch": now.get("boot_epoch")}
        if epoch is not None and now.get("boot_epoch") != epoch:
            return Result(f"C0-01.slot{slot}", "slots", "fail", detail,
                          f"slot {slot}: the node reset with the flipped card powered (brown-out): the card shorts "
                          "AON when reversed. HD-025 swap missing on this card or board revision")
        if rec.get("fault") in (12, 13):
            return Result(f"C0-01.slot{slot}", "slots", "fail", detail,
                          f"slot {slot}: the flipped card loaded a rail (current-limit trip or quarantine): "
                          "HD-025 swap missing on this card or board revision")
        if rec.get("present") != 1:
            return Result(f"C0-01.slot{slot}", "slots", "fail", detail,
                          f"slot {slot}: PRESENT reads empty with the card flipped: the card predates the HD-025 "
                          "swap (PRESENT_N still on contact 18) or contact 19 does not make")
        return Result(f"C0-01.slot{slot}", "slots", "pass", detail)
    return Test(f"C0-01.slot{slot}", "slots", run, requires=("S-M00", "S-M05"))


def t_flipped_manual(b: Bench) -> Result:
    return Result("C0-01", "slots", "manual", {},
                  "insert a card upside down in TL-DBG-001, `power aon on`: FLT stays dark, USB stays up, no fault, "
                  "PRESENT reads seated; rerun with --flipped SLOT=CARD to check it in a node slot")


def t_dbg_lamps(b: Bench) -> Result:
    """D-10: TL-DBG-001 lamps. The MCP2221 can't see its own LEDs, so this one is the operator's."""
    return Result("D-10", "dbg", "manual", {},
                  "TL-DBG-001: PWR lit on USB; `talos_dbg power aon on/off` -> AON follows; `power sw on/off` -> "
                  "SW follows; FLT dark, lights on a SW overload (10 R to GND, PPK2 limiting) and clears after "
                  "`power sw off`; IN lit with USB where fitted. Wrong lamp: that LED, its resistor, or its net")


def t_test_card_sw_lamp(b: Bench) -> Result:
    """T-11: TL-TEST-001 SW lamp; its current must keep the SW load inside T-07's window."""
    lo, hi = SW_CURRENT_MA
    ma = b.measure.get("T-11")
    if ma is None:
        return Result("T-11", "dbg", "manual", {"window_ma": [lo, hi]},
                      "PPK2 across JP2, `talos_dbg power sw on`: read the current and rerun with --measure "
                      "T-11=<mA>; the SW lamp must be lit with SW on and dark with SW off")
    return Result("T-11", "dbg", *judge_sw_current(ma))


def judge_sw_current(ma: float) -> tuple[str, dict, str]:
    lo, hi = SW_CURRENT_MA
    detail = {"measured_ma": ma, "window_ma": [lo, hi]}
    if lo <= ma <= hi:
        return "pass", detail, ""
    if ma < 9.0:
        return "fail", detail, "far below 10 mA: the 330 R load is open, JP2 not fitted, or SW is off"
    if ma < lo:
        return "fail", detail, ("about the 330 R alone: the SW lamp is missing, reversed, or its resistor is open "
                                "(is the lamp lit?)")
    if ma <= 14.0:
        return "fail", detail, "SW lamp draws too much: wrong (too small) series resistor for the lamp"
    return "fail", detail, "far above the window: something else loads V3V3_SW (a short on TL-TEST-001)"


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
        return (f"slot {slot}: no card seen (PRESENT_N high). Card not seated, or finger 19 / "
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


def plan(expect: dict[int, str], flipped: dict[int, str] | None = None) -> list[Test]:
    tests = [Test("S-M00", "mcu", t_identity), Test("S-M16", "mcu", t_board_identity),
             Test("S-M05", "mcu", t_root_bus),
             Test("S-M17", "mcu", t_tamper, requires=("S-M05",)),
             Test("S-M18", "mcu", t_lamps_exp_c, requires=("S-M05",)),
             Test("S-M19", "mcu", t_tamper_deep_sleep, requires=("S-M05", "S-M17")),
             Test("S-P09", "power", t_power, requires=("S-M05",))]
    for slot, card in sorted(expect.items()):
        tests.append(slot_test(slot, card))
        st = card_selftest(slot, card)
        if st:
            tests.append(st)
    for slot, card in sorted((flipped or {}).items()):
        tests.append(flipped_card_test(slot, card))
    if not flipped:
        tests.append(Test("C0-01", "slots", t_flipped_manual))
    tests += [Test("D-10", "dbg", t_dbg_lamps), Test("T-11", "dbg", t_test_card_sw_lamp)]
    return tests


def summary(results: list[Result]) -> dict:
    counts: dict[str, int] = {}
    for r in results:
        counts[r.verdict] = counts.get(r.verdict, 0) + 1
    return {"counts": counts, "failed": [{"test": r.test, "diagnosis": r.diagnosis}
                                         for r in results if r.verdict == "fail"],
            "time": time.strftime("%Y-%m-%dT%H:%M:%S")}
