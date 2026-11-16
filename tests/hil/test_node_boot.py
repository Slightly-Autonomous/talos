"""L4 hardware-in-the-loop checks for the ESP32-H2 node (ADR 0016).

These run only on a trusted self-hosted runner with a dev kit attached
(.github/workflows/hil.yml). They assert behavior the virtual board cannot
prove: the real bootloader, NVS persistence, I2C on real silicon.
"""
import os
import re

import pytest

BOOT = re.compile(rb"talos boot epoch=(\d+) version=(\d+)")
SLOT = re.compile(rb"talos slot (\d) module=0x([0-9a-f]{4}) enabled=(\d) fault=(-?\d+)")


@pytest.mark.esp32h2
def test_boots_and_reports_all_slots(dut):
    dut.expect(BOOT, timeout=30)
    seen = set()
    for _ in range(4):
        m = dut.expect(SLOT, timeout=10)
        seen.add(int(m.group(1)))
    assert seen == {0, 1, 2, 3}
    dut.expect(rb"talos alive queued=\d+ mode=0", timeout=15)


@pytest.mark.esp32h2
def test_boot_epoch_persists_and_increments_across_reset(dut):
    first = int(dut.expect(BOOT, timeout=30).group(1))
    dut.serial.hard_reset()
    second = int(dut.expect(BOOT, timeout=30).group(1))
    assert second == first + 1, "NVS must persist the boot epoch (never reuse epoch/seq)"


@pytest.mark.esp32h2
@pytest.mark.bench_rig
@pytest.mark.skipif(os.environ.get("TALOS_RIG") != "1", reason="sensor rig not wired")
def test_rig_modules_are_discovered(dut):
    """With the bring-up rig (PIR slot 0, LIS2DW12 slot 1, SHT40 slot 3)."""
    dut.expect(BOOT, timeout=30)
    modules = {}
    for _ in range(4):
        m = dut.expect(SLOT, timeout=10)
        modules[int(m.group(1))] = (m.group(2).decode(), m.group(3) == b"1")
    assert modules[0] == ("0101", True)
    assert modules[1] == ("0201", True)  # WHO_AM_I probe passed on real silicon
    assert modules[3] == ("0301", True)
