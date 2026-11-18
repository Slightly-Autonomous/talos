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
"""

from __future__ import annotations

import json
import pathlib
import time
from dataclasses import dataclass

from .mcp2221 import DESIGNATION_GPIO, GpSetting, I2cNack, Mcp2221
from .modules import manifest_for

MANIFEST = pathlib.Path(__file__).resolve().parents[1] / "tl-dbg-001.json"


class NotConfigured(Exception):
    pass


class AdapterError(Exception):
    pass


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


@dataclass(frozen=True)
class CardId:
    code: int
    revision: int
    manifest_id: str | None

    def describe(self) -> str:
        name = self.manifest_id or "UNKNOWN module code (not in the firmware registry)"
        return f"0x{self.code:04X} rev {self.revision}: {name}"


@dataclass(frozen=True)
class Lines:
    aon_on: bool
    sw_on: bool
    wake: bool
    alert_asserted: bool  # ALERT_N is active low


class DebugAdapter:
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
        if any(p is None for p in g):
            raise NotConfigured("a GP pin is not in GPIO mode at run time; replug or run configure")

        def v(role: str) -> bool:
            return g[self._pin(role)][0]  # type: ignore[index]

        return Lines(v("aon_enable"), v("sw_enable"), v("wake"), not v("alert_n"))

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

    # --- card I2C ----------------------------------------------------------------------

    def _require_aon(self) -> None:
        if not self.lines().aon_on:
            raise AdapterError("AON is off: the card and its I2C pull-ups are unpowered")

    def scan(self) -> list[int]:
        self._require_aon()
        self.bridge.set_i2c_speed(self.m.i2c_hz)
        return [a for a in range(0x08, 0x78) if self.bridge.probe(a)]

    def card_id(self) -> CardId | None:
        """None when nothing answers at the ID EEPROM address (empty slot or dead card)."""
        self._require_aon()
        self.bridge.set_i2c_speed(self.m.i2c_hz)
        try:
            raw = self.bridge.i2c_write_read(self.m.id_eeprom, b"\x00", 3)
        except I2cNack:
            return None
        code = (raw[0] << 8) | raw[1]
        return CardId(code, raw[2], manifest_for(code))
