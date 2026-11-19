"""TL-DBG-001 behaviour on top of the MCP2221A: configuration, rails, lines, card identity.

Safety rules this module enforces:
- Nothing touches the card until the adapter is configured (`require_configured`). A
  factory-fresh MCP2221A drives both rail enables high (HW-0007), so card commands refuse to run
  on one.
- `configure` needs the operator to confirm no card is seated. It sets the run-time GP settings
  first (the rails turn off at once), then writes flash, then reads flash back.
- SW only comes on while AON is on, and AON only goes off after SW, so card logic on SW never
  sees I2C pull-ups (on AON) without its own supply sequencing, and AON never backfeeds through
  an SW-powered part.
- Card lines that are pulled up to AON (ALERT_N, SDA, SCL) mean nothing while AON is off: they
  are reported as unknown, never as asserted or idle.
- Card ID writes go only to the writable lower half, one 16-byte page at a time, and never
  overwrite an existing ID unless the caller quotes it back (`expect_current`).
"""

from __future__ import annotations

import json
import os
import pathlib
import re
import time
from dataclasses import dataclass

from . import id_block
from .mcp2221 import DESIGNATION_GP1_INTERRUPT, DESIGNATION_GPIO, GpSetting, I2cNack, Mcp2221
from .modules import CARDS, MODULES, manifest_for

MANIFEST = pathlib.Path(__file__).resolve().parents[1] / "tl-dbg-001.json"
MODULE_MANIFESTS = pathlib.Path(__file__).resolve().parents[3] / "contracts/manifests/modules"


class NotConfigured(Exception):
    pass


class AdapterError(Exception):
    pass


class BusHeldLow(AdapterError):
    """SDA or SCL reads low with AON on: something on the card or slot is shorting the bus."""


@dataclass(frozen=True)
class Manifest:
    product: str
    pins: dict[str, int]  # role -> GP number
    required: list[GpSetting]  # GP0..GP3 power-up settings the adapter must have
    i2c_hz: int
    id_eeprom: int
    settle_s: float

    @staticmethod
    def load(path: pathlib.Path = MANIFEST) -> "Manifest":
        m = json.loads(path.read_text())
        pins, required = {}, []
        for n in range(4):
            gp = m["gp"][f"GP{n}"]
            pins[gp["role"]] = n
            is_input = gp["direction"] == "input"
            required.append(GpSetting(DESIGNATION_GPIO, is_input, bool(gp.get("safe_value", 0))))
        return Manifest(m["usb"]["product_string"], pins, required, m["i2c_hz"],
                        int(m["id_eeprom_address"], 16), m["rail_settle_ms"] / 1000)


def card_addresses(card: str) -> set[int]:
    """I2C addresses a Rev A card answers on, from its module manifest."""
    mid = MODULES[CARDS[card]]
    text = (MODULE_MANIFESTS / f"{mid}.yaml").read_text()
    m = re.search(r"^i2c_addresses:\s*\[([^\]]*)\]", text, re.M)
    if not m:
        raise AdapterError(f"{mid}.yaml lists no i2c_addresses")
    return {int(x.strip(), 0) for x in m.group(1).split(",") if x.strip()}


@dataclass(frozen=True)
class CardId:
    block: id_block.IdBlock
    eui: bytes

    @property
    def code(self) -> int:
        return self.block.code

    @property
    def revision(self) -> int:
        return self.block.revision

    @property
    def manifest_id(self) -> str | None:
        return manifest_for(self.block.code) if self.block.status == "ok" else None

    def describe(self) -> str:
        eui = id_block.eui_text(self.eui)
        if self.block.status == "blank":
            return f"BLANK ID (never programmed)  EUI {eui}"
        if self.block.status == "corrupt":
            return f"CORRUPT ID {self.block.raw.hex()}  EUI {eui}"
        name = self.manifest_id or "UNKNOWN module code (not in the firmware registry)"
        return f"0x{self.code:04X} rev {self.revision}: {name}  EUI {eui}"


@dataclass(frozen=True)
class Lines:
    aon_on: bool
    sw_on: bool
    wake: bool | None  # None while GP1 is routed to the edge detector (watch)
    alert_asserted: bool | None  # ALERT_N is active low; None = unknown (AON off)


@dataclass(frozen=True)
class ProgramResult:
    status: str  # "written" or "already"
    card: CardId


class DebugAdapter:
    POLL_S = 0.0005

    def __init__(self, bridge: Mcp2221, manifest: Manifest | None = None, sleep=time.sleep):
        self.bridge = bridge
        self.m = manifest or Manifest.load()
        self._sleep = sleep

    # --- configuration -----------------------------------------------------------------

    def config_problems(self) -> list[str]:
        """Empty when the flash holds the GP settings this adapter needs."""
        flash = self.bridge.read_gp_flash()
        problems = []
        for n, (have, want) in enumerate(zip(flash, self.m.required)):
            if have.designation != want.designation or have.input != want.input:
                problems.append(f"GP{n}: flash has designation {have.designation:03b} "
                                f"{'input' if have.input else 'output'}, need GPIO "
                                f"{'input' if want.input else 'output'}")
            elif not want.input and have.value != want.value:
                problems.append(f"GP{n}: power-up output {int(have.value)}, need {int(want.value)}")
        return problems

    def is_configured(self) -> bool:
        return not self.config_problems()

    def require_configured(self) -> None:
        problems = self.config_problems()
        if problems:
            raise NotConfigured("adapter is not configured; run `configure` with no card seated:\n  "
                                + "\n  ".join(problems))

    def configure(self, no_card_seated: bool) -> list[str]:
        if not no_card_seated:
            raise AdapterError("configure needs confirmation that no card is seated "
                               "(a factory-fresh adapter powers the slot)")
        steps = []
        self.bridge.set_gp_sram(self.m.required)
        steps.append("run-time GP settings loaded: both rails off")
        if self.config_problems():
            self.bridge.write_gp_flash(self.m.required)
            steps.append("GP power-up settings written to flash")
        else:
            steps.append("flash GP settings already correct")
        if self.bridge.read_product() != self.m.product:
            self.bridge.write_product(self.m.product)
            steps.append(f"USB product string set to {self.m.product} (shows after replug)")
        problems = self.config_problems()
        if problems:
            raise AdapterError("flash read-back does not match:\n  " + "\n  ".join(problems))
        steps.append("flash read back and verified")
        return steps

    # --- rails and lines ---------------------------------------------------------------

    def _pin(self, role: str) -> int:
        return self.m.pins[role]

    def lines(self) -> Lines:
        self.require_configured()
        g = self.bridge.get_gpio()
        wake_pin = self._pin("wake")
        if any(p is None for n, p in enumerate(g) if n != wake_pin):
            raise NotConfigured("a GP pin is not in GPIO mode at run time; replug or run configure")

        def v(role: str) -> bool:
            return g[self._pin(role)][0]  # type: ignore[index]

        aon = v("aon_enable")
        wake = None if g[wake_pin] is None else v("wake")
        return Lines(aon, v("sw_enable"), wake, (not v("alert_n")) if aon else None)

    def bus_levels(self) -> tuple[bool, bool]:
        """(SCL, SDA) at the MCP2221A pins. Both read low whenever AON is off."""
        return self.bridge.line_levels()

    def set_rail(self, rail: str, on: bool) -> None:
        self.require_configured()
        now = self.lines()
        if rail == "aon":
            if not on and now.sw_on:
                raise AdapterError("turn SW off before AON")
            self.bridge.set_gpio(self._pin("aon_enable"), on)
        elif rail == "sw":
            if on and not now.aon_on:
                raise AdapterError("turn AON on before SW")
            self.bridge.set_gpio(self._pin("sw_enable"), on)
        else:
            raise ValueError("rail is 'aon' or 'sw'")
        self._sleep(self.m.settle_s)

    def all_off(self) -> None:
        self.require_configured()
        self.bridge.set_gpio(self._pin("sw_enable"), False)
        self.bridge.set_gpio(self._pin("aon_enable"), False)

    def power_cycle_aon(self, timeout_s: float = 2.0, margin_s: float = 0.05) -> float:
        """Turn the card fully off and on again; returns how long AON took to discharge.

        The AON switch has no quick discharge, so a fixed delay would not reset the card.
        SDA is pulled up to AON: it reads low once the rail has fallen.
        """
        waited = self.aon_off_and_wait(timeout_s)
        self._sleep(margin_s)
        self.bridge.set_gpio(self._pin("aon_enable"), True)
        self._sleep(self.m.settle_s)
        return waited

    def aon_off_and_wait(self, timeout_s: float = 2.0) -> float:
        """SW and AON off, then wait until the rail has really fallen (SDA reads low)."""
        self.require_configured()
        self.bridge.set_gpio(self._pin("sw_enable"), False)
        self.bridge.set_gpio(self._pin("aon_enable"), False)
        waited = 0.0
        while self.bus_levels()[1]:
            if waited >= timeout_s:
                raise AdapterError(f"AON did not discharge within {timeout_s:.1f} s "
                                   "(something is back-powering the card)")
            self._sleep(0.01)
            waited += 0.01
        return waited

    # --- card I2C ----------------------------------------------------------------------

    def _require_bus(self) -> None:
        if not self.lines().aon_on:
            raise AdapterError("AON is off: the card and its I2C pull-ups are unpowered")
        scl, sda = self.bus_levels()
        if not (scl and sda):
            low = " and ".join(n for n, v in (("SCL", scl), ("SDA", sda)) if not v)
            raise BusHeldLow(f"{low} held low with AON on: a card part or the slot ESD array is "
                             "shorting the bus")
        self.bridge.set_i2c_speed(self.m.i2c_hz)

    def scan(self) -> list[int]:
        self._require_bus()
        return [a for a in range(0x08, 0x78) if self.bridge.probe(a)]

    def read(self, addr: int, reg: int, n: int) -> bytes:
        self._require_bus()
        out = b""
        while len(out) < n:
            chunk = min(32, n - len(out))
            out += self.bridge.i2c_write_read(addr, bytes([reg + len(out)]), chunk)
        return out

    def write(self, addr: int, reg: int, data: bytes) -> None:
        self._require_bus()
        self.bridge.i2c_write(addr, bytes([reg]) + data)

    def card_id(self) -> CardId | None:
        """None when nothing answers at the ID EEPROM address (empty slot or dead card)."""
        self._require_bus()
        try:
            raw = self.bridge.i2c_write_read(self.m.id_eeprom, b"\x00", id_block.SIZE)
            eui = self.bridge.i2c_write_read(self.m.id_eeprom, bytes([id_block.EUI_ADDR]), 6)
        except I2cNack:
            return None
        return CardId(id_block.decode(raw), eui)

    def id_raw(self) -> tuple[bytes, bytes]:
        """The writable half (0x00-0x7F) and the EUI-48."""
        self._require_bus()
        low = self.read(self.m.id_eeprom, 0x00, id_block.WRITABLE_END)
        eui = self.read(self.m.id_eeprom, id_block.EUI_ADDR, 6)
        return low, eui

    def _wait_write_cycle(self, timeout_s: float = 0.01) -> None:
        # The EEPROM does not ACK its address during the write cycle (TWC 5 ms max).
        waited = 0.0
        while not self.bridge.probe(self.m.id_eeprom):
            if waited >= timeout_s:
                raise AdapterError("ID EEPROM still busy 10 ms after a page write")
            self._sleep(self.POLL_S)
            waited += self.POLL_S

    def program_id(self, card: str, revision: int = 1, expect_current: str | None = None,
                   log_path: pathlib.Path | None = None) -> ProgramResult:
        if card not in CARDS:
            raise AdapterError(f"unknown card {card}; one of {', '.join(sorted(CARDS))}")
        want = id_block.encode(CARDS[card], revision)
        # The card on the slot must be the kind we are about to name it.
        found = set(self.scan())
        expected = card_addresses(card)
        if found != expected:
            raise AdapterError(f"{card} answers on {sorted(hex(a) for a in expected)}, this card "
                               f"answers on {sorted(hex(a) for a in found)}: refusing to write")
        current = self.card_id()
        if current is None:
            raise AdapterError("ID EEPROM stopped answering")
        problems = id_block.eui_problems(current.eui)
        if problems:
            raise AdapterError("; ".join(problems))
        if current.block.raw == want:
            return ProgramResult("already", current)
        if current.block.status != "blank":
            if expect_current is None or expect_current.lower() != current.block.raw.hex():
                raise AdapterError(f"card already holds {current.describe()}; pass "
                                   f"--expect-current {current.block.raw.hex()} to overwrite it")
        self.bridge.i2c_write(self.m.id_eeprom, bytes([0x00]) + want)  # one page, page-aligned
        self._wait_write_cycle()
        after = self.card_id()
        if after is None or after.block.raw != want:
            raise AdapterError("read-back after the write does not match")
        self.power_cycle_aon()
        after = self.card_id()
        if after is None or after.block.raw != want:
            raise AdapterError("ID did not survive a power cycle")
        self._log(log_path, card, after)
        return ProgramResult("written", after)

    def _log(self, path: pathlib.Path | None, card: str, cid: CardId) -> None:
        if path is None:
            base = os.environ.get("TALOS_DBG_LOG_DIR") or os.path.expanduser("~/.local/share/talos-dbg")
            path = pathlib.Path(base) / "card-ids.jsonl"
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a") as f:
            f.write(json.dumps({"time": time.strftime("%Y-%m-%dT%H:%M:%S"), "card": card,
                                "eui": id_block.eui_text(cid.eui), "block": cid.block.raw.hex()}) + "\n")

    # --- WAKE edges --------------------------------------------------------------------

    def enter_wake_latch(self) -> None:
        """Route GP1 to the edge detector (SRAM only; flash is untouched), rising edges."""
        self.require_configured()
        gp = self._run_time_gp()  # rails stay as they are
        gp[self._pin("wake")] = GpSetting(DESIGNATION_GP1_INTERRUPT, True, False)
        self.bridge.set_gp_sram(gp)
        self.bridge.set_interrupt(rising=True, falling=False, clear=True)

    def take_wake_edge(self) -> bool:
        """True (and re-armed) if WAKE rose since the last call."""
        if not self.bridge.interrupt_flag():
            return False
        self.bridge.set_interrupt(rising=True, falling=False, clear=True)
        return True

    def leave_wake_latch(self) -> None:
        self.bridge.set_gp_sram(self._run_time_gp())

    def _run_time_gp(self) -> list[GpSetting]:
        # Keep the rails where they are: SRAM GP settings carry the output values.
        g = self.bridge.get_gpio()
        out = []
        for n, want in enumerate(self.m.required):
            level = bool(g[n][0]) if g[n] is not None and not want.input else want.value
            out.append(GpSetting(want.designation, want.input, level))
        return out
