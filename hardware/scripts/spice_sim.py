"""Circuit simulation of TL-SN-001 power and bus behaviour with ngspice.

    python3 hardware/scripts/spice_sim.py          # run every bench, write hardware/results/spice.md
    python3 hardware/scripts/spice_sim.py --check  # exit 1 if a bench result differs from the committed file

ngspice runs as a shared library (libngspice), the same engine KiCad's simulator uses: KiCad's own copy on
macOS, the distribution's libngspice0 on Linux, or NGSPICE_LIB. Nothing else is needed.

These are behavioural benches, written from datasheet numbers. TI's models for the TPS63900, TPS22917 and
TPS2553 are PSpice-encrypted or absent and do not run in ngspice, so each part here is a small model of the
datasheet behaviour that matters for the question (current limit, slew rate, on-resistance). They test our
circuit around the parts, not the parts. Every parameter names its source; EST marks an assumption the bench
must replace (hardware/results/README.md, provisional rule).
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import pathlib
import platform
import sys
from dataclasses import dataclass, field

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "hardware" / "results" / "spice.md"

# --- datasheet parameters (sources in the second element) ------------------------------------------------
P = {
    "v3v3": (3.3, "TPS63900 VO set by R20 16.2k (gen_schematics.py, SLVSET3D Table 7-3)"),
    "c_core": (32e-6, "C12 22 uF + C1 10 uF on V3V3_REG/CORE; 9 x 100 nF ignored (intended-netlist.json)"),
    "reg_imax_min": (0.4, "TPS63900 IO 0.4 A for VI >= 2.0 V (SLVSET3D 6.3), guaranteed output"),
    "reg_imax_typ": (1.0, "EST: typical output before the 1.33-1.6 A peak switch limit bites (SLVSET3D 6.5)"),
    "reg_gm": (20.0, "EST: loop transconductance, A/V (50 mV droop per amp at DC)"),
    "reg_tau": (2e-6, "EST: loop response, s"),
    "vdd_min": (3.0, "ESP32-H2-MINI-1 VDD minimum 3.0 V (module datasheet, recommended operating)"),
    "i_mcu": (0.030, "EST: ESP32-H2 active, radio idle, 30 mA"),
    "i_tx": (0.123, "802.15.4 TX at 18 dBm 123 mA (power-budget.md section 4, SRC)"),
    "aon_ron": (0.090, "TPS22917 RON 90 mOhm typ at VIN 3.6 V, 25 C (SLVSDW8B 6.5)"),
    "aon_slew": (44e3, "TPS22917 SRON 44 mV/us with CT open at VIN 3.6 V (SLVSDW8B 6.6); C27-30 are DNP"),
    "aon_slew_ct": (1900e3, "TPS22917 SRON 1900 (mV/us)*pF with CT >= 100 pF at VIN 3.6 V (SLVSDW8B 6.6)"),
    "ct_fix": (1e-9, "C27-C30 1 nF from CT to VIN (fitted 2026-11-20, finding S-1)"),
    "c_slot": (1e-6, "C13-C16 1 uF on each V3V3_AON_n (intended-netlist.json)"),
    "c_mod003": (10.2e-6, "TL-MOD-003 card: C3 10 uF + C1, C2 100 nF on V3V3_AON"),
    "c_mod004": (0.2e-6, "TL-MOD-004 card: C1, C2 100 nF on V3V3_AON"),
    "sw_ios": (0.207, "TPS2553-1 IOS max with RILIM 150k: 207 mA (SLVS841G eq. 3)"),
    "sw_deglitch": (7.5e-3, "TPS2553 FAULT deglitch 7.5 ms typ (SLVS841G 6.5)"),
    "i2c_r": (4.7e3 / 2, "R5/R6 4.7k on the root bus in parallel with the selected branch's 4.7k (R34-R41)"),
    "i2c_c": (100e-12, "EST: root bus + mux + one branch + card, ~100 pF; kisi can replace with extracted copper"),
    # lamps (B6)
    "vbus_min": (4.4, "USB VBUS at the device after cable drop (USB 2.0 allows 4.4 V at the far end, EST)"),
    "vbus_max": (5.25, "USB 2.0 VBUS maximum 5.25 V"),
    "v3v3_min": (3.0, "EST: 3.3 V rail low corner incl. TPS63900 accuracy and droop"),
    "v3v3_max": (3.6, "EST: 3.3 V rail high corner"),
    "vf_lo": (1.65, "EST: red/yellow-green AlGaInP LED Vf at 0.3-1 mA (datasheets give 1.75-2.4 V at 20 mA)"),
    "vf_hi": (2.05, "EST: same, high corner"),
    "led_i_min": (0.2e-3, "EST: dimmest still clearly visible indoors behind a light pipe, 0.2 mA"),
    "led_i_max": (5e-3, "EST: ceiling: TCA9535 and MCP2221 GP pins, and a sensible indicator budget"),
    "mcp_voh": (2.6, "EST: MCP2221A GP VOH while sourcing ~0.5 mA (DS20005565E gives VOH at higher currents)"),
    # tamper loop (B7)
    "tamper_r": (1e6, "R99 1 MOhm pull-up (gen_schematics.py)"),
    "tamper_c": (10e-9, "C38 10 nF (gen_schematics.py, changed from 100 nF after this bench)"),
    "tamper_cable_c": (200e-12, "EST: 2 m of twisted pair to the lid and wall switches, ~100 pF/m"),
    "tca_vih_frac": (0.7, "TCA9535 VIH = 0.7 x VCC (SCPS201 6.5)"),
    "tamper_debounce": (50e-3, "firmware tamper debounce 50 ms (firmware/portable/indicators.h)"),
    "tamper_budget": (100e-3, "bench S-M17: P0 within 100 ms of the lid opening"),
    # SPI pull-downs (B8)
    "spi_rpd": (100e3, "R101-R103 100 kOhm pull-downs (HD-027)"),
    "esp_rpu": (45e3, "ESP32-H2 internal weak pull-up ~45 kOhm typ (ESP32-H2 datasheet DC characteristics, EST)"),
    "card_vih": (2.31, "card input VIH, 0.7 x 3.3 V (CMOS)"),
    "card_vil": (0.99, "card input VIL, 0.3 x 3.3 V (CMOS)"),
    "card_off_r": (100e3, "EST: an unpowered card's rail seen through its clamp diodes, ~100 kOhm of quiescent load"),
    # AON discharge (B9)
    "qod_r": (150.0, "EST: TPS22917 QOD resistance ~150 Ohm (SLVSDW8B 6.5 lists RQOD; replace with the bench value)"),
    "card_idle_r_mod004": (1.65e6, "EST: TL-MOD-004 idle ~2 uA at 3.3 V (SHT40 + EEPROM sleep)"),
    "card_idle_r_mod003": (66e3, "EST: TL-MOD-003 idle ~50 uA at 3.3 V (LIS2DW12 low-power wake mode)"),
    "por_v": (0.5, "EST: discharged enough for every card part to see a clean power-on reset"),
    # TL-SN-002 (ADR 0019)
    "tps2116_vref_min": (0.92, "TPS2116 PR1 VREF min (SLVSFG1A 6.5)"),
    "tps2116_vref_max": (1.08, "TPS2116 PR1 VREF max (SLVSFG1A 6.5)"),
    "tps2116_tsw": (17.7e-6, "TPS2116 break-before-make switchover, worst listed (VIN 1.8 V) (SLVSFG1A 6.6)"),
    "sn2_vsys_c": (20e-6, "TL-SN-002 C7 10 uF + C11 10 uF on VSYS/SYS_REG"),
    "sn2_vin_min": (1.8, "TPS63900 input minimum (SLVSET3D)"),
    "pir_i": (10e-6, "PIR223D-S0 supply current (NEWOPTO/LCSC listing)"),
    "pir_rc_r": (330.0, "TL-SN-002 R17 PIR supply filter (330 R; 100 R peaked at ~30 mA)"),
    "pir_rc_c": (4.7e-6, "TL-SN-002 C18 PIR supply filter"),
    "tca_pin_max": (0.05, "TCA9535 max current per I/O (SCPS201 abs max 50 mA)"),
    "pir_vmin": (2.7, "PIR223D-S0 supply minimum (listing)"),
}


def v(name: str) -> float:
    return P[name][0]


# --- libngspice -------------------------------------------------------------------------------------------
def _find_lib() -> str:
    env = os.environ.get("NGSPICE_LIB")
    if env:
        return env
    if platform.system() == "Darwin":
        for cand in ("~/Applications/KiCad/KiCad.app/Contents/Frameworks/libngspice.0.dylib",
                     "/Applications/KiCad/KiCad.app/Contents/Frameworks/libngspice.0.dylib"):
            p = os.path.expanduser(cand)
            if os.path.exists(p):
                return p
    found = ctypes.util.find_library("ngspice")
    if found:
        return found
    for cand in ("libngspice.so.0", "libngspice.so"):
        try:
            ctypes.CDLL(cand)
            return cand
        except OSError:
            pass
    raise SystemExit("libngspice not found: install KiCad, or libngspice0 (apt), or set NGSPICE_LIB")


class NgSpice:
    _CB_CHAR = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p)
    _CB_STAT = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p)
    _CB_EXIT = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int, ctypes.c_bool, ctypes.c_bool, ctypes.c_int,
                                ctypes.c_void_p)

    def __init__(self) -> None:
        self.lib = ctypes.CDLL(_find_lib())
        self.out: list[str] = []
        self._c = self._CB_CHAR(self._char)
        self._s = self._CB_STAT(lambda *_: 0)
        self._e = self._CB_EXIT(lambda *_: 0)
        self.lib.ngSpice_Init(self._c, self._s, self._e, None, None, None, None)

    def _char(self, s, _id, _p) -> int:
        self.out.append(s.decode(errors="replace"))
        return 0

    def cmd(self, c: str) -> list[str]:
        start = len(self.out)
        self.lib.ngSpice_Command(c.encode())
        return self.out[start:]

    def version(self) -> str:
        for ln in self.cmd("version -s"):
            if "ngspice-" in ln:
                return ln.split("**")[-1].strip().replace("stdout ", "")
        return "unknown"

    def run(self, netlist: str, measures: dict[str, str]) -> dict[str, float]:
        self.cmd("destroy all")
        self.cmd("remcirc")
        for line in netlist.strip().splitlines():
            self.cmd("circbyline " + line.strip())
        self.cmd("circbyline .end")
        lines = self.cmd("run")
        if any("error" in ln.lower() and "simulation" in ln.lower() for ln in lines):
            raise RuntimeError("\n".join(lines[-20:]))
        res = {}
        for name, expr in measures.items():
            got = self.cmd(f"meas tran {name} {expr}")
            val = None
            for ln in got:
                t = ln.replace("stdout", "").strip()
                if t.startswith(name) and "=" in t:
                    try:
                        val = float(t.split("=")[1].split()[0])
                    except ValueError:
                        pass
            if val is None:
                raise RuntimeError(f"measure {name} failed: {' | '.join(got)[-400:]}")
            res[name] = val
        return res


# --- benches ----------------------------------------------------------------------------------------------
def regulator(imax: float) -> str:
    """TPS63900 as a current-limited source with a first-order loop (behavioural, not TI's model)."""
    return f"""
Berr err 0 V = {{ {v('reg_gm')} * ({v('v3v3')} - V(core)) }}
Rlag err ctl 1k
Clag ctl 0 {v('reg_tau') / 1e3}
Breg 0 core I = {{ min(max(V(ctl), 0), {imax}) }}
Ccore core 0 {v('c_core')}
Rpre core 0 1e6
"""


@dataclass
class Result:
    bench: str
    case: str
    values: dict[str, float]
    verdict: str
    note: str = ""


@dataclass
class Bench:
    name: str
    question: str
    params: list[str]
    results: list[Result] = field(default_factory=list)


def slot_inrush(ng: NgSpice) -> Bench:
    b = Bench("B1 Slot AON power-on inrush",
              "Does turning on a slot's TPS22917 sag V3V3_CORE below the ESP32's 3.0 V minimum?",
              ["v3v3", "c_core", "reg_imax_min", "reg_imax_typ", "reg_gm", "reg_tau", "vdd_min", "i_mcu",
               "i_tx", "aon_ron", "aon_slew", "c_slot", "c_mod003", "c_mod004"])
    t_on = 100e-6
    cases = []
    for card, c_card in (("TL-MOD-004 (0.2 uF)", v("c_mod004")), ("TL-MOD-003 (10.2 uF)", v("c_mod003"))):
        for reg_label, imax in (("reg 0.4 A (min)", v("reg_imax_min")), ("reg 1.0 A (EST typ)", v("reg_imax_typ"))):
            for tx in (False, True):
                cases.append((card, c_card, reg_label, imax, tx, v("aon_slew"), "CT open (before S-1)"))
    slew_fix = v("aon_slew_ct") / (v("ct_fix") * 1e12)  # mV/us*pF over pF, in V/s
    for tx in (False, True):
        cases.append(("TL-MOD-003 (10.2 uF)", v("c_mod003"), "reg 0.4 A (min)", v("reg_imax_min"), tx, slew_fix,
                      "CT = 1 nF (fitted)"))
    for card, c_card, reg_label, imax, tx, slew, ct in cases:
            ramp_end = t_on + 3.6 / slew
            t_stop = ramp_end + 400e-6
            if True:
                load = (f"Itx core 0 PWL(0 0 {t_on} 0 {t_on + 1e-6} {v('i_tx')})" if tx else "")
                net = f"""
* slot inrush
{regulator(imax)}
Imcu core 0 DC {v('i_mcu')}
{load}
Vr r 0 PWL(0 0 {t_on} 0 {ramp_end} 3.6)
Vsense core swin 0
Bsw swin aon I = {{ max(min(V(r), V(swin)) - V(aon), 0) / {v('aon_ron')} }}
Caon aon 0 {v('c_slot') + c_card}
Rcard aon 0 660
.ic V(core)={v('v3v3')} V(ctl)=0.6 V(err)=0.6 V(aon)=0
.tran 0.2u {t_stop} uic
"""
                m = ng.run(net, {"vmin": f"MIN V(core) FROM=50u TO={t_stop}",
                                 "ipk": f"MAX I(Vsense) FROM=50u TO={t_stop}"})
                vmin = m["vmin"]
                ok = vmin >= v("vdd_min")
                case = f"{card}, {ct}, {reg_label}, {'TX burst at the same time' if tx else 'radio idle'}"
                b.results.append(Result(b.name, case, {"V3V3_CORE min (V)": vmin, "inrush peak (A)": m["ipk"]},
                                        "pass" if ok else "FAIL"))
    return b


def radio_burst(ng: NgSpice) -> Bench:
    b = Bench("B2 Radio TX burst", "How far does a 123 mA TX step sag V3V3_CORE?",
              ["v3v3", "c_core", "reg_imax_min", "reg_gm", "reg_tau", "i_mcu", "i_tx", "vdd_min"])
    for reg_label, imax in (("reg 0.4 A (min)", v("reg_imax_min")), ("reg 1.0 A (EST typ)", v("reg_imax_typ"))):
        net = f"""
* tx burst
{regulator(imax)}
Imcu core 0 DC {v('i_mcu')}
Itx core 0 PWL(0 0 100u 0 101u {v('i_tx')} 2.1m {v('i_tx')} 2.101m 0)
.ic V(core)={v('v3v3')} V(ctl)=0.6 V(err)=0.6
.tran 0.2u 3m uic
"""
        m = ng.run(net, {"vmin": "MIN V(core) FROM=50u TO=3m"})
        b.results.append(Result(b.name, reg_label, {"V3V3_CORE min (V)": m["vmin"]},
                                "pass" if m["vmin"] >= v("vdd_min") else "FAIL"))
    return b


def sw_short(ng: NgSpice) -> Bench:
    b = Bench("B3 SW rail into a short",
              "While a TPS2553 sits in current limit for its 7.5 ms FAULT deglitch, does V3V3_CORE hold?",
              ["v3v3", "c_core", "reg_imax_min", "reg_imax_typ", "reg_gm", "reg_tau", "i_mcu", "i_tx",
               "sw_ios", "sw_deglitch", "vdd_min"])
    for n_short in (1, 2):
        for reg_label, imax in (("reg 0.4 A (min)", v("reg_imax_min")), ("reg 1.0 A (EST typ)", v("reg_imax_typ"))):
            t_end = 1e-3 + v("sw_deglitch")
            net = f"""
* sw short
{regulator(imax)}
Imcu core 0 DC {v('i_mcu')}
Itx core 0 DC {v('i_tx')}
Ishort core 0 PWL(0 0 1m 0 1.001m {n_short * v('sw_ios')} {t_end} {n_short * v('sw_ios')} {t_end + 1e-6} 0)
.ic V(core)={v('v3v3')} V(ctl)=0.6 V(err)=0.6
.tran 1u {t_end + 1e-3} uic
"""
            m = ng.run(net, {"vmin": f"MIN V(core) FROM=0.5m TO={t_end}"})
            m["vmin"] = max(m["vmin"], 0.0)  # ideal current sinks drive a collapsed rail negative
            case = f"{n_short} slot{'s' if n_short > 1 else ''} shorted at IOS max, radio transmitting, {reg_label}"
            b.results.append(Result(b.name, case, {"V3V3_CORE min (V)": m["vmin"]},
                                    "pass" if m["vmin"] >= v("vdd_min") else
                                    ("FAIL (rail collapses)" if m["vmin"] < 0.5 else "FAIL")))
    return b


def i2c_rise(ng: NgSpice) -> Bench:
    b = Bench("B4 I2C rise time", "Do the pull-ups meet the I2C rise-time limit with one branch selected?",
              ["i2c_r", "i2c_c"])
    for c in (v("i2c_c") / 2, v("i2c_c"), v("i2c_c") * 2):
        net = f"""
* i2c rise: open-drain released at 1 us
Vdd vdd 0 3.3
Rpu vdd sda {v('i2c_r')}
Cbus sda 0 {c}
Sdrv sda 0 ctl 0 swm
Vctl ctl 0 PWL(0 1 1u 1 1.001u 0)
.model swm sw vt=0.5 ron=20 roff=1e9
.ic V(sda)=0
.tran 1n 5u uic
"""
        m = ng.run(net, {"tr": "TRIG V(sda) VAL=0.99 RISE=1 TARG V(sda) VAL=2.31 RISE=1"})
        tr_ns = m["tr"] * 1e9
        verdict = "pass" if tr_ns <= 300 else ("pass (100 kHz only)" if tr_ns <= 1000 else "FAIL")
        b.results.append(Result(b.name, f"{c * 1e12:.0f} pF", {"rise 30-70 % (ns)": tr_ns}, verdict,
                                "limit 1000 ns at 100 kHz (firmware), 300 ns at 400 kHz"))
    return b


def priority_divider() -> Bench:
    """Arithmetic, not SPICE: the TPS2121 PR1 switchover window with R13/R14 tolerance."""
    b = Bench("B5 USB priority threshold (TPS2121 PR1)",
              "At what VBUS does the power mux take USB, across reference and resistor tolerance?", [])
    for edge, vref in (("rising", (1.01, 1.06, 1.10)), ("falling", (0.99, 1.04, 1.09))):
        for tol in (0.01, 0.05):
            lo = vref[0] * (27e3 * (1 - tol) + 10e3 * (1 + tol)) / (10e3 * (1 + tol))
            hi = vref[2] * (27e3 * (1 + tol) + 10e3 * (1 - tol)) / (10e3 * (1 - tol))
            typ = vref[1] * 3.7
            ok = hi < 4.4  # USB VBUS at the device can droop to ~4.4 V (cable drop); must still win
            b.results.append(Result(b.name, f"{edge}, resistors ±{tol * 100:.0f}%",
                                    {"min (V)": lo, "typ (V)": typ, "max (V)": hi},
                                    "pass" if ok else "FAIL",
                                    "SLVSEA3F VREF 1.01/1.06/1.10 V rising, 0.99/1.04/1.09 V falling"))
    return b


def lamps() -> Bench:
    """Arithmetic: lamp current at every supply and forward-voltage corner."""
    b = Bench("B6 Lamp currents", "Does every lamp light (>= 0.2 mA) and stay within its driver's budget at every "
              "supply and Vf corner?", ["vbus_min", "vbus_max", "v3v3_min", "v3v3_max", "vf_lo", "vf_hi",
                                        "led_i_min", "led_i_max", "mcp_voh"])
    lamps_ = [("TL-SN-001 D8 PWR (USB_VBUS, 3.3k)", v("vbus_min"), v("vbus_max"), 3.3e3),
              ("TL-SN-001 D1 STAT (3V3, 1k, GPIO8 sink)", v("v3v3_min"), v("v3v3_max"), 1e3),
              ("TL-SN-001 D9-D12 S0-S3 (3V3, 2.2k, EXP_C sink)", v("v3v3_min"), v("v3v3_max"), 2.2e3),
              ("TL-DBG-001 D3 PWR / D6 FLT (AP2112 3.3 V, 2.2k)", 3.267, 3.333, 2.2e3),
              ("TL-DBG-001 D4 AON / D5 SW (MCP2221 GP VOH, 2.2k)", v("mcp_voh"), 3.333, 2.2e3),
              ("TL-TEST-001 D1 SW (slot SW rail, 2.2k)", v("v3v3_min"), v("v3v3_max"), 2.2e3)]
    for name, vs_lo, vs_hi, r in lamps_:
        i_lo, i_hi = (vs_lo - v("vf_hi")) / r, (vs_hi - v("vf_lo")) / r
        ok = i_lo >= v("led_i_min") and i_hi <= v("led_i_max")
        b.results.append(Result(b.name, name, {"I min (mA)": i_lo * 1e3, "I max (mA)": i_hi * 1e3},
                                "pass" if ok else ("FAIL (too dim)" if i_lo < v("led_i_min") else "FAIL")))
    return b


def tamper(ng: NgSpice) -> Bench:
    b = Bench("B7 Tamper loop edge", "How long after the lid opens does TAMPER_N cross the TCA9535 VIH, and does the "
              "alarm (plus the 50 ms debounce) meet the 100 ms bench budget?",
              ["tamper_r", "tamper_c", "tamper_cable_c", "tca_vih_frac", "tamper_debounce", "tamper_budget"])
    for c_label, c in (("100 nF (before)", 100e-9), ("10 nF (fitted)", v("tamper_c"))):
        for vdd in (v("v3v3_min"), v("v3v3_max")):
            net = f"""
* tamper loop: lid switch opens at 1 ms
Vdd vdd 0 {vdd}
Rpu vdd t {v('tamper_r')}
Ct t 0 {c + v('tamper_cable_c')}
Ssw t 0 ctl 0 swm
Vctl ctl 0 PWL(0 1 1m 1 1.001m 0)
.model swm sw vt=0.5 ron=1 roff=1e12
.ic V(t)=0
.tran 20u 400m uic
"""
            m = ng.run(net, {"tr": f"TRIG V(ctl) VAL=0.5 FALL=1 TARG V(t) VAL={v('tca_vih_frac') * vdd} RISE=1"})
            total = m["tr"] + v("tamper_debounce")
            b.results.append(Result(b.name, f"C38 {c_label}, VDD {vdd:.1f} V",
                                    {"to VIH (ms)": m["tr"] * 1e3, "alarm after (ms)": total * 1e3,
                                     "loop current shut (uA)": vdd / v("tamper_r") * 1e6},
                                    "pass" if total <= v("tamper_budget") else "FAIL",
                                    "a loop shorted to GND reads 'shut': single-ended loop limitation (HD-032)"))
    return b


def spi_pulldowns() -> Bench:
    """Arithmetic: SCK level with the ESP32's reset-default pull-up against the 100 k pull-down, and the
    current pushed into an unpowered card's clamp."""
    b = Bench("B8 SPI pull-downs at reset", "What level does SCK sit at while GPIO4's reset pull-up fights the 100 k "
              "pull-down, and how much current reaches an unpowered card?",
              ["spi_rpd", "esp_rpu", "card_vih", "card_vil", "card_off_r"])
    vdd = v("v3v3")
    for phase, rpu in (("reset, before firmware (GPIO4 pull-up on)", v("esp_rpu")),
                       ("deep sleep / firmware driving low (no pull-up)", None)):
        if rpu is None:
            vs, i_card = 0.0, 0.0
        else:
            rth = rpu * v("spi_rpd") / (rpu + v("spi_rpd"))
            vth = vdd * v("spi_rpd") / (rpu + v("spi_rpd"))
            vclamp = 0.4  # clamp diode into a dead rail
            i_card = max(vth - vclamp, 0) / (rth + v("card_off_r")) if vth > vclamp else 0.0
            vs = vth - i_card * rth
        state = "high" if vs >= v("card_vih") else ("low" if vs <= v("card_vil") else "undefined")
        verdict = "pass" if state == "low" else ("pass (brief, boot only)" if rpu else "FAIL")
        b.results.append(Result(b.name, phase, {"SCK level (V)": vs, "into unpowered card (uA)": i_card * 1e6},
                                verdict, "firmware drives SCK/MOSI low at boot (HD-027); the undefined window lasts "
                                         "from reset until the GPIO init, a few ms"))
    return b


def aon_discharge(ng: NgSpice) -> Bench:
    b = Bench("B9 Slot AON discharge after switch-off", "How long until a card's AON rail is below 0.5 V, with "
              "and without the TPS22917 quick discharge (QOD to VOUT, HD-028)?",
              ["qod_r", "c_slot", "c_mod003", "c_mod004", "card_idle_r_mod003", "card_idle_r_mod004", "por_v"])
    for card, c_card, r_idle in (("TL-MOD-004", v("c_mod004"), v("card_idle_r_mod004")),
                                 ("TL-MOD-003", v("c_mod003"), v("card_idle_r_mod003"))):
        for label, r in (("QOD to VOUT (fitted)", v("qod_r") * r_idle / (v("qod_r") + r_idle)),
                         ("QOD open (before)", r_idle)):
            c = v("c_slot") + c_card
            t_stop = max(5 * r * c, 1e-3)
            net = f"""
* aon discharge from 3.3 V
Caon aon 0 {c}
Rdis aon 0 {r}
.ic V(aon)={v('v3v3')}
.tran {t_stop / 2000} {t_stop} uic
"""
            m = ng.run(net, {"t": f"WHEN V(aon)={v('por_v')} FALL=1"})
            b.results.append(Result(b.name, f"{card}, {label}", {"to 0.5 V (ms)": m["t"] * 1e3},
                                    "pass" if m["t"] <= 20e-3 else "slow (firmware must wait)",
                                    "pass = below 0.5 V within a 20 ms off time (EST firmware power-cycle window)"))
    return b


def sn2_switchover() -> Bench:
    """Arithmetic: TL-SN-002 USB/battery priority threshold (TPS2116 PR1 divider 27k/10k)."""
    b = Bench("B10 TL-SN-002 USB priority threshold (TPS2116 PR1)",
              "At what VBUS does the mux pick USB over the AA cells, across VREF and resistor tolerance?",
              ["tps2116_vref_min", "tps2116_vref_max"])
    for tol in (0.01, 0.05):
        lo = v("tps2116_vref_min") * (27e3 * (1 - tol) + 10e3 * (1 + tol)) / (10e3 * (1 + tol))
        hi = v("tps2116_vref_max") * (27e3 * (1 + tol) + 10e3 * (1 - tol)) / (10e3 * (1 - tol))
        b.results.append(Result(b.name, f"resistors ±{tol * 100:.0f}%", {"min (V)": lo, "max (V)": hi},
                                "pass" if hi < 4.4 else "FAIL", "USB at the device can droop to ~4.4 V; must still win"))
    return b


def sn2_unplug() -> Bench:
    """Arithmetic: VSYS dip during the TPS2116 break-before-make when USB is pulled under a TX burst."""
    b = Bench("B11 TL-SN-002 USB unplug during a TX burst",
              "Does VSYS stay above the TPS63900's 1.8 V minimum while the mux switches from USB to the cells?",
              ["tps2116_tsw", "sn2_vsys_c", "sn2_vin_min"])
    for vbus in (4.4, 5.0):
        for v_bat in (2.2, 3.0):
            p_out = v("v3v3") * (v("i_tx") + 0.035)
            i_in = p_out / (0.85 * vbus)
            dip = i_in * v("tps2116_tsw") / v("sn2_vsys_c")
            vmin = min(vbus - dip, v_bat)  # the cells take over at their own voltage
            b.results.append(Result(b.name, f"VBUS {vbus} V, cells {v_bat} V", {"dip (V)": dip, "VSYS min (V)": vmin},
                                    "pass" if vmin >= v("sn2_vin_min") else "FAIL",
                                    "2.2 V = the firmware low-battery threshold (power-budget-sn2.md)"))
    return b


def sn2_pir_rail(ng: NgSpice) -> Bench:
    b = Bench("B12 TL-SN-002 PIR rail from an expander pin",
              "Does the TCA9535 pin power the PIR through 100 R + 4.7 uF within its current limit, and how "
              "fast does PIR_VDD reach the PIR's 2.7 V minimum?", ["pir_i", "pir_rc_r", "pir_rc_c", "tca_pin_max",
                                                                  "pir_vmin"])
    for vdd in (v("v3v3_min"), v("v3v3_max")):
        net = f"""
* expander pin (ideal + 20 R output) -> R17 -> 4.7 uF || PIR 10 uA
Vpin pin 0 PWL(0 0 10u 0 11u {vdd})
Rout pin a 20
Rf a pv {v('pir_rc_r')}
Cf pv 0 {v('pir_rc_c')}
Ipir pv 0 DC {v('pir_i')}
.ic V(pv)=0
.tran 2u 5m uic
"""
        m = ng.run(net, {"ipk": "MIN I(Vpin) FROM=0 TO=5m",  # source current flows out: negative
                         "t": f"TRIG V(pin) VAL={vdd / 2} RISE=1 TARG V(pv) VAL={v('pir_vmin')} RISE=1",
                         "vss": "AVG V(pv) FROM=4m TO=5m"})
        ipk = abs(m["ipk"])
        ok = ipk <= v("tca_pin_max") and m["vss"] >= v("pir_vmin")
        b.results.append(Result(b.name, f"VDD {vdd:.1f} V", {"pin peak (mA)": ipk * 1e3, "to 2.7 V (ms)": m["t"] * 1e3,
                                                          "PIR_VDD (V)": m["vss"]}, "pass" if ok else "FAIL",
                                "firmware waits for the PIR223D warm-up after enabling the rail anyway"))
    return b


def write(benches: list[Bench], version: str) -> str:
    lines = ["# TL-SN-001 circuit simulation (generated)", "",
             f"Generated by `hardware/scripts/spice_sim.py` with {version} (libngspice). **Behavioural models from "
             "datasheet numbers, not vendor models: a model, not a measurement.** EST parameters are assumptions "
             "the bench must replace.", ""]
    for b in benches:
        lines += [f"## {b.name}", "", b.question, ""]
        keys = list(b.results[0].values)
        lines.append("| Case | " + " | ".join(keys) + " | Result |")
        lines.append("|---|" + "---:|" * len(keys) + "---|")
        for r in b.results:
            lines.append(f"| {r.case} | " + " | ".join(f"{r.values[k]:.3f}" for k in keys) + f" | {r.verdict} |")
        notes = sorted({r.note for r in b.results if r.note})
        if notes:
            lines += [""] + [f"- {n}" for n in notes]
        lines.append("")
    lines += ["## Parameters", "", "| Name | Value | Source |", "|---|---:|---|"]
    for k, (val, src) in P.items():
        lines.append(f"| {k} | {val:g} | {src} |")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    ng = NgSpice()
    benches = [slot_inrush(ng), radio_burst(ng), sw_short(ng), i2c_rise(ng), priority_divider(), lamps(),
               tamper(ng), spi_pulldowns(), aon_discharge(ng), sn2_switchover(), sn2_unplug(), sn2_pir_rail(ng)]
    text = write(benches, ng.version())
    if "--check" in sys.argv:
        # Compare case -> verdict, not the text: ngspice versions differ in the last digits.
        def verdicts(t: str) -> list[tuple[str, str]]:
            body = t.split("## Parameters")[0]
            rows = [ln.split("|") for ln in body.splitlines() if ln.startswith("| ") and not ln.startswith("| Case")]
            return [(r[1].strip(), r[-2].strip()) for r in rows if len(r) > 3 and r[-2].strip()
                    and not r[-2].strip().startswith("-")]
        if not OUT.exists() or verdicts(OUT.read_text()) != verdicts(text):
            print(f"{OUT} verdicts differ from a fresh run: run hardware/scripts/spice_sim.py and review")
            return 1
        print(f"spice: {len(verdicts(text))} cases match the committed verdicts")
        return 0
    OUT.write_text(text)
    for b in benches:
        for r in b.results:
            print(f"{r.verdict:20s} {b.name[:3]} {r.case}: " + ", ".join(f"{k} {x:.3f}" for k, x in r.values.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
