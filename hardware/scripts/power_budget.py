#!/usr/bin/env python3
"""TL-SN-001 Rev A power-budget calculator (H2).

Every constant is tagged SRC (datasheet, see hardware/requirements/datasheet-register.md)
or EST (assumption a bench must replace). The output is a model, not a measurement.

Usage: python3 hardware/scripts/power_budget.py [--out hardware/results/power-budget.md]
Exits non-zero if a design limit (HWR-009) is violated.
"""

from __future__ import annotations

import argparse
import datetime
import pathlib
import sys
from dataclasses import dataclass


@dataclass(frozen=True)
class Load:
    name: str
    ua: float
    rail: str  # "3v3" (behind the TPS63900) or "bat" (direct on battery / SYS)
    tag: str  # "SRC <doc>" or "EST <why>"


V_RAIL = 3.3
V_BAT_NOM = 3.7

# ---- Deep-sleep loads (battery only, no input source) ---------------------------------
SLEEP_LOADS = [
    Load("ESP32-H2 deep sleep (LP timer + LP mem on)", 7.0, "3v3", "SRC MINI-1 v1.6 Table 16"),
    Load("TCA9546A standby", 1.0, "3v3", "SRC SCPS205B ICC standby typ @3.6 V"),
    Load("TCA9535 x2 standby", 2 * 0.9, "3v3", "SRC SCPS201F ICC standby typ @3.6 V"),
    Load("TPS22917 x4 AON enabled (IQ; worst case, firmware powers seated slots only)", 4 * 0.5, "3v3", "SRC SLVSDW8B IQ 0.5 uA typ"),
    Load("TPS2553-1 x4 SW disabled (IIN_off)", 4 * 0.100, "3v3", "SRC SLVS841G IIN_off 0.1 uA typ"),
    # W&S mode, 100 ms SLEEPTIME, one axis (door closed). EST interpolated from SLYS045C ICC_DCM:
    # 1.21 uA @5 s and 160 uA @1 ms (1 ch) -> ~1.18 uA floor + ~0.17 uC/conversion. Door open (3 axes) ~3.8 uA.
    Load("TL-MOD-001 TMAG5273A1 W&S 100 ms, 1 axis", 2.9, "3v3", "EST from SLYS045C ICC_DCM endpoints"),
    Load("TL-MOD-001 SN74AUP1G04 ICC", 0.5, "3v3", "SRC SCES571K ICC 0.5 uA max 25 C"),
    Load("TL-MOD-002 EKMB1101111 (sleep mode)", 1.0, "3v3", "SRC Panasonic spec Iw typ"),
    Load("TL-MOD-003 LIS2DW12 low-power, low ODR", 1.0, "3v3", "EST datasheet claims <1 uA active"),
    Load("TL-MOD-004 SHT4x idle", 1.0, "3v3", "EST placeholder, not sourced"),
    Load("24AA025E ID EEPROM x4 standby", 4 * 1.0, "3v3", "EST placeholder, not sourced"),
    Load("Slot TVS arrays x4 leakage", 4 * 0.1, "3v3", "EST placeholder, not sourced"),
    Load("BQ25186 IQ_BAT (battery only)", 3.0, "bat", "SRC SLUSF69 IQ_BAT typ 25 C"),
    Load("BQ25186 TS/NTC duty (60 uA x 4/200 ms)", 60.0 * 4 / 200, "bat", "SRC SLUSF69 ITSMR, tTS_DUTY"),
    Load("VBAT divider 4.7M+4.7M @3.7 V", V_BAT_NOM / 9.4, "bat", "SRC Ohm's law"),
    Load("TPS63900 IQ", 0.075, "bat", "SRC SLVSET3D 75 nA"),
]
REG_EFF_LIGHT = 0.90  # SRC SLVSET3D ">90% efficiency at 10 uA" (taken at the floor)
REG_EFF_ACTIVE = 0.88  # EST mid-load efficiency, to be read from the efficiency curve

# ---- Radio activity -------------------------------------------------------------------
# One "report": wake, MLE/CoAP exchange, back to sleep. Charge per report is an estimate.
RADIO_TX_PEAK_MA = 123.0  # SRC MINI-1 Table 14, 802.15.4 @18 dBm
RADIO_RX_MA = 29.0  # SRC MINI-1 Table 14
REPORT_ACTIVE_MS = 40.0  # EST wake + attach poll + CON exchange
REPORT_AVG_MA = 30.0  # EST blended RX/TX/CPU at moderate TX power
CHECKINS_PER_DAY = 24 * 12  # EST one supervision check-in every 5 min
EVENTS_PER_DAY = 200  # EST busy front door

LIGHT_SLEEP_UA = 85.0  # SRC MINI-1 Table 16 (alternative: stay attached as Thread SED)

# ---- Battery and solar ----------------------------------------------------------------
BATTERY_USABLE_WH = 8.4  # EST handoff §22 (3350 mAh 18650, derated)
SELF_DISCHARGE_PCT_MONTH = 2.0  # EST protected Li-ion pack incl. protection IC
PANEL_W = 2.0  # EST HD-019
SUN_HOURS_WINTER = 1.0  # EST pessimistic winter effective full-sun hours
PV_DERATE = 0.5  # EST angle, dirt, linear-charger loss, temperature

# ---- Charger thermals -----------------------------------------------------------------
BQ_RTHETA_JA = 68.3  # SRC SLUSF69 §5.3 JEDEC board; real board differs
BQ_TREG_C = 100.0  # SRC SLUSF69 TREG THERM_REG=00
T_AMBIENT_OUTDOOR_C = 45.0  # EST hot enclosure

# ---- Peak -----------------------------------------------------------------------------
TPS63900_MAX_MA = 400.0  # SRC SLVSET3D (VIN >= 2.0 V, VOUT 3.3 V)
CARD_PEAK_MA = 2.7 + 0.1 + 1.0 + 1.0  # EST worst card: 2.3 mA TMAG5273 active (SLYS045C) + INT pull-up + margin
V_BAT_MIN = 3.0

SLEEP_LIMIT_UA = 40.0  # HWR-009


def battery_ua(loads: list[Load], v_bat: float, eff: float) -> float:
    total = 0.0
    for load in loads:
        total += load.ua if load.rail == "bat" else load.ua * V_RAIL / (v_bat * eff)
    return total


def radio_avg_ua_3v3(per_day: int) -> float:
    charge_mc = per_day * REPORT_ACTIVE_MS * REPORT_AVG_MA / 1000.0  # mA*s = mC
    return charge_mc / 86400.0 * 1000.0  # uA


def life_days(i_bat_ua: float) -> float:
    p_w = i_bat_ua * 1e-6 * V_BAT_NOM
    days_load = BATTERY_USABLE_WH / p_w / 24.0
    # self-discharge as an extra constant drain on the same energy
    sd_w = BATTERY_USABLE_WH * SELF_DISCHARGE_PCT_MONTH / 100.0 / (30.0 * 24.0)
    return BATTERY_USABLE_WH / (p_w + sd_w) / 24.0, days_load


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="hardware/results/power-budget.md")
    args = ap.parse_args()

    sleep_ua = battery_ua(SLEEP_LOADS, V_BAT_NOM, REG_EFF_LIGHT)
    sleep_ua_low = battery_ua(SLEEP_LOADS, V_BAT_MIN, REG_EFF_LIGHT)
    radio_3v3 = radio_avg_ua_3v3(CHECKINS_PER_DAY + EVENTS_PER_DAY)
    radio_bat = radio_3v3 * V_RAIL / (V_BAT_NOM * REG_EFF_ACTIVE)
    deep_total = sleep_ua + radio_bat
    sed_total = battery_ua(
        [Load("H2 light sleep", LIGHT_SLEEP_UA, "3v3", "")] + SLEEP_LOADS[1:], V_BAT_NOM, REG_EFF_LIGHT
    ) + radio_bat

    peak_rail_ma = RADIO_TX_PEAK_MA + CARD_PEAK_MA + 5.0  # +5 mA EST CPU/expanders
    peak_bat_ma = peak_rail_ma * V_RAIL / (V_BAT_MIN * REG_EFF_ACTIVE)

    rows = []
    for vin in (5.0, 6.0, 7.0):
        for ichg in (100, 200, 300, 500):
            p = (vin - 3.6) * ichg / 1000.0
            tj = T_AMBIENT_OUTDOOR_C + p * BQ_RTHETA_JA
            rows.append((vin, ichg, p, tj, "throttles (TREG)" if tj > BQ_TREG_C else "ok"))

    daily_wh_deep = deep_total * 1e-6 * V_BAT_NOM * 24
    harvest_wh = PANEL_W * SUN_HOURS_WINTER * PV_DERATE

    checks = [
        ("HWR-009 deep-sleep battery current < %.0f uA" % SLEEP_LIMIT_UA, sleep_ua < SLEEP_LIMIT_UA),
        ("Peak 3V3 load < TPS63900 400 mA", peak_rail_ma < TPS63900_MAX_MA),
        ("Winter PV harvest covers daily use (deep-sleep design)", harvest_wh > daily_wh_deep),
    ]

    out = []
    out.append("# TL-SN-001 Rev A power budget (generated)\n")
    out.append(
        f"Generated by `hardware/scripts/power_budget.py` on {datetime.date.today().isoformat()}. "
        "**This is a model, not a measurement.** SRC = datasheet value (see datasheet-register.md), "
        "EST = assumption that the bench must replace.\n"
    )
    out.append("## 1. Deep-sleep current at the battery (4 reference cards, no input)\n")
    out.append("| Load | µA | Rail | Basis |\n|---|---:|---|---|")
    for load in SLEEP_LOADS:
        out.append(f"| {load.name} | {load.ua:.3f} | {load.rail} | {load.tag} |")
    out.append(
        f"\nRegulator efficiency at light load: {REG_EFF_LIGHT:.0%} (SRC floor of the '>90% at 10 µA' claim).\n"
    )
    out.append(f"- **Sleep current at VBAT = {V_BAT_NOM} V: {sleep_ua:.1f} µA**")
    out.append(f"- At VBAT = {V_BAT_MIN} V (end of discharge): {sleep_ua_low:.1f} µA")
    out.append(
        f"- Radio activity ({CHECKINS_PER_DAY} check-ins + {EVENTS_PER_DAY} events/day, "
        f"{REPORT_ACTIVE_MS:.0f} ms at {REPORT_AVG_MA:.0f} mA each, EST): {radio_bat:.1f} µA average\n"
    )
    out.append("## 2. Operating designs and battery life (8.4 Wh usable, EST)\n")
    out.append("| Design | Avg battery µA | Life incl. self-discharge (days) | Life, load only (days) |")
    out.append("|---|---:|---:|---:|")
    for name, i in (("Deep sleep between reports", deep_total), ("Thread SED in light sleep", sed_total)):
        life, life_load = life_days(i)
        out.append(f"| {name} | {i:.1f} | {life:.0f} | {life_load:.0f} |")
    out.append(
        "\nWhether ESP-IDF's OpenThread sleepy end device can use deep sleep between reports with "
        "acceptable re-attach cost is **unverified** (HD-004/HD-018). Light-sleep SED is the "
        "safe assumption until measured.\n"
    )
    out.append("## 3. Charger dissipation on PV/USB (BQ25186 is linear)\n")
    out.append(
        f"P = (VIN − VBAT) × ICHG at VBAT = 3.6 V; Tj = {T_AMBIENT_OUTDOOR_C:.0f} °C ambient (EST) "
        f"+ P × RθJA {BQ_RTHETA_JA} °C/W (SRC, JEDEC board). Above TREG {BQ_TREG_C:.0f} °C the charger "
        "reduces current itself: safe, but charging slows.\n"
    )
    out.append("| VIN (V) | ICHG (mA) | P (W) | Tj est (°C) | Result |\n|---:|---:|---:|---:|---|")
    for vin, ichg, p, tj, verdict in rows:
        out.append(f"| {vin:.1f} | {ichg} | {p:.2f} | {tj:.0f} | {verdict} |")
    out.append(
        "\n**Policy:** firmware sets ICHG ≤ 200 mA when PMUX_ST says PV is the active source, and "
        "≤ 300 mA on USB. Reconsider a switching charger if panels above ~3 W are supported.\n"
    )
    out.append("## 4. Peak current\n")
    out.append(
        f"- 3V3 peak ≈ {peak_rail_ma:.0f} mA (802.15.4 TX @18 dBm {RADIO_TX_PEAK_MA:.0f} mA SRC + cards + "
        f"logic) vs TPS63900 >{TPS63900_MAX_MA:.0f} mA (SRC)."
    )
    out.append(
        f"- Battery peak at {V_BAT_MIN} V ≈ {peak_bat_ma:.0f} mA: needs a pack protection circuit "
        "rated well above this, and a low-ESR path.\n"
    )
    out.append("## 5. Solar reserve (deep-sleep design)\n")
    out.append(f"- Daily use ≈ {daily_wh_deep * 1000:.1f} mWh; winter harvest ≈ {harvest_wh * 1000:.0f} mWh "
               f"({PANEL_W} W panel × {SUN_HOURS_WINTER} h × {PV_DERATE:.0%}, all EST).")
    out.append(
        f"- Dark-day reserve from a full pack ≈ {life_days(deep_total)[0]:.0f} days "
        "(self-discharge dominates; cold capacity loss not modelled).\n"
    )
    out.append("## 6. Checks\n")
    ok_all = True
    for name, ok in checks:
        out.append(f"- [{'x' if ok else ' '}] {name}")
        ok_all &= ok
    out.append(
        "\n## What this does not prove\nIt does not prove actual leakage (PCB, flux, TVS), regulator ripple at the radio "
        "burst, inrush on slot switches, pack protection behaviour, cold-weather capacity, or "
        "Thread poll costs. Measure IT-08/IT-09 on Rev A (interface-test-matrix.md).\n"
    )

    path = pathlib.Path(args.out)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(out))
    print(f"sleep {sleep_ua:.1f} uA, deep-sleep design {deep_total:.1f} uA, SED {sed_total:.1f} uA -> {path}")
    for name, ok in checks:
        print(("PASS " if ok else "FAIL ") + name)
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
