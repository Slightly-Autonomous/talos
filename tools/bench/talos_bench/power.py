"""Current measurement on the bench (HWR-009, HD-043): Nordic Power Profiler Kit II (PPK2).

    python3 -m talos_bench.power --port /dev/cu.usbmodemXXXX [--source 3000] [--seconds 120]
    python3 -m talos_bench.power --mock               # no hardware: the logic against a model

The PPK2 runs in source-meter mode and powers TL-SN-002 through the battery terminals (BT1 + / -),
standing in for 2 x AA at --source mV, with USB-C unplugged. Three measurements:
1. **Deep-sleep floor:** the median current over windows with no radio activity, after the node has
   settled (PIR warm-up done, heartbeat sent). Pass if under HWR-009's 40 uA.
2. **Average over a heartbeat cycle:** what the cells actually supply, including wakes and TX. It goes
   into the battery-life estimate.
3. **TX burst peak and duration**, plus the pair voltage the regulator sees at the cells' estimated
   internal resistance (power_budget_sn2.PAIR_R_OHM). This checks the low-battery threshold (HD-043).

Needs `pip install ppk2-api` (IRNAS, MIT). It is imported only for real hardware, so the mock and the
tests run without it. Thresholds and the analysis are pure functions, unit-tested in
tests/test_power.py.
"""

from __future__ import annotations

import argparse
import dataclasses
import math
import statistics
import sys
import time
from typing import Iterable, Protocol

SLEEP_LIMIT_UA = 40.0           # HWR-009
SAMPLE_HZ = 100_000             # PPK2 native rate
QUIET_WINDOW_S = 0.5            # a window with no sample above BURST_UA is "sleep"
BURST_UA = 1_000.0              # anything above 1 mA is a wake or a TX
TPS63900_VIN_MIN_MV = 1_800     # SLVSET3D
PAIR_R_OHM = {"25 C": 0.30, "-20 C": 0.60, "-40 C": 1.00}  # power_budget_sn2.py (EST)


class Meter(Protocol):
    def start(self, source_mv: int) -> None: ...
    def read_ua(self, seconds: float) -> list[float]: ...
    def stop(self) -> None: ...


@dataclasses.dataclass
class PowerReport:
    sleep_ua: float
    average_ua: float
    peak_ma: float
    burst_ms: float
    bursts: int
    seconds: float
    source_mv: int

    @property
    def sleep_ok(self) -> bool:
        return self.sleep_ua < SLEEP_LIMIT_UA

    def regulator_input_mv(self, r_ohm: float) -> float:
        """Pair voltage under the peak, at internal resistance r_ohm."""
        return self.source_mv - self.peak_ma * r_ohm

    def lines(self) -> list[str]:
        out = [f"deep-sleep floor: {self.sleep_ua:.1f} uA (limit {SLEEP_LIMIT_UA:.0f}): "
               f"{'PASS' if self.sleep_ok else 'FAIL'}",
               f"average over {self.seconds:.0f} s: {self.average_ua:.1f} uA",
               f"bursts: {self.bursts}, peak {self.peak_ma:.1f} mA, longest {self.burst_ms:.1f} ms"]
        for t, r in PAIR_R_OHM.items():
            v = self.regulator_input_mv(r)
            out.append(f"  regulator input at {t} ({r} ohm): {v:.0f} mV "
                       f"{'ok' if v >= TPS63900_VIN_MIN_MV else 'BELOW 1.8 V'}")
        return out


def analyse(samples_ua: Iterable[float], hz: int = SAMPLE_HZ, source_mv: int = 3_000) -> PowerReport:
    s = list(samples_ua)
    if not s:
        raise ValueError("no samples")
    win = max(1, int(QUIET_WINDOW_S * hz))
    quiet = [statistics.fmean(s[i:i + win]) for i in range(0, len(s) - win + 1, win)
             if max(s[i:i + win]) < BURST_UA]
    sleep = statistics.median(quiet) if quiet else math.inf
    bursts, longest, run = 0, 0, 0
    for v in s:
        if v >= BURST_UA:
            run += 1
        else:
            if run:
                bursts += 1
                longest = max(longest, run)
            run = 0
    if run:
        bursts += 1
        longest = max(longest, run)
    return PowerReport(sleep_ua=sleep, average_ua=statistics.fmean(s), peak_ma=max(s) / 1000.0,
                       burst_ms=longest * 1000.0 / hz, bursts=bursts, seconds=len(s) / hz,
                       source_mv=source_mv)


class Ppk2Meter:
    """PPK2 in source-meter mode (ppk2-api)."""

    def __init__(self, port: str):
        from ppk2_api.ppk2_api import PPK2_API  # imported only for real hardware
        self._ppk = PPK2_API(port)
        self._ppk.get_modifiers()

    def start(self, source_mv: int) -> None:
        self._ppk.use_source_meter()
        self._ppk.set_source_voltage(source_mv)
        self._ppk.toggle_DUT_power("ON")
        self._ppk.start_measuring()

    def read_ua(self, seconds: float) -> list[float]:
        out: list[float] = []
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            data = self._ppk.get_data()
            if data != b"":
                samples, _ = self._ppk.get_samples(data)
                out.extend(samples)
            time.sleep(0.001)
        return out

    def stop(self) -> None:
        self._ppk.stop_measuring()
        self._ppk.toggle_DUT_power("OFF")


class MockMeter:
    """A node in deep sleep at sleep_ua, waking every period_s for a burst (tests, --mock)."""

    def __init__(self, sleep_ua: float = 31.0, burst_ma: float = 45.0, burst_ms: float = 6.0,
                 period_s: float = 60.0, hz: int = 10_000):
        self.sleep_ua, self.burst_ma, self.burst_ms, self.period_s, self.hz = (
            sleep_ua, burst_ma, burst_ms, period_s, hz)
        self._t = 0

    def start(self, source_mv: int) -> None:
        self._t = 0

    def read_ua(self, seconds: float) -> list[float]:
        n = int(seconds * self.hz)
        period = int(self.period_s * self.hz)
        burst = max(1, int(self.burst_ms * self.hz / 1000))
        out = []
        for _ in range(n):
            phase = self._t % period
            out.append(self.burst_ma * 1000.0 if phase < burst else self.sleep_ua)
            self._t += 1
        return out

    def stop(self) -> None:
        pass


def measure(meter: Meter, seconds: float, source_mv: int, settle_s: float = 0.0, hz: int = SAMPLE_HZ) -> PowerReport:
    meter.start(source_mv)
    try:
        if settle_s:
            meter.read_ua(settle_s)  # PIR warm-up, first heartbeat: not part of the steady state
        return analyse(meter.read_ua(seconds), hz=hz, source_mv=source_mv)
    finally:
        meter.stop()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", help="PPK2 serial port (e.g. /dev/cu.usbmodem...)")
    ap.add_argument("--mock", action="store_true")
    ap.add_argument("--source", type=int, default=3_000, help="source voltage, mV (2 x AA)")
    ap.add_argument("--seconds", type=float, default=120.0)
    ap.add_argument("--settle", type=float, default=70.0, help="PIR warm-up + first heartbeat, s")
    a = ap.parse_args(argv)
    if a.mock:
        meter, hz = MockMeter(), 10_000
    elif a.port:
        meter, hz = Ppk2Meter(a.port), SAMPLE_HZ
    else:
        ap.error("--port or --mock")
    r = measure(meter, a.seconds, a.source, 0.0 if a.mock else a.settle, hz=hz)
    print("\n".join(r.lines()))
    return 0 if r.sleep_ok else 1


if __name__ == "__main__":
    sys.exit(main())
