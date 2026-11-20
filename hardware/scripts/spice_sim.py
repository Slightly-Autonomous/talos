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
    benches = [slot_inrush(ng), radio_burst(ng), sw_short(ng), i2c_rise(ng), priority_divider()]
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
