"""What the agent reads on the Pi, behind small interfaces so tests and the virtual node can fake them.

- Stream: MediaMTX's local API (/v3/paths/get/<path>): is the camera path ready, and how many bytes
  has it produced.
- Pi: `vcgencmd measure_temp` and `vcgencmd get_throttled`.
- Lid: the tamper GPIO through libgpiod (python3-libgpiod; v1 on Bookworm, v2 on Trixie).
- Clock: `timedatectl` says whether NTP has synchronised (no RTC on the Zero).
"""

from __future__ import annotations

import json
import pathlib
import re
import subprocess
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Optional, Protocol


@dataclass
class StreamSample:
    ready: bool
    bytes_produced: int  # cumulative bytes the camera source produced


class Stream(Protocol):
    def sample(self) -> Optional[StreamSample]: ...


class Pi(Protocol):
    def soc_milli_c(self) -> int: ...
    def throttled(self) -> int: ...


class Lid(Protocol):
    def raw_open(self) -> bool: ...


class Clock(Protocol):
    def synchronised(self) -> bool: ...


class MediaMtxStream:
    """MediaMTX v1 control API on localhost (api: yes, apiAddress: 127.0.0.1:9997)."""

    def __init__(self, path: str, api: str = "http://127.0.0.1:9997"):
        self.url = f"{api}/v3/paths/get/{path}"

    def sample(self) -> Optional[StreamSample]:
        try:
            with urllib.request.urlopen(self.url, timeout=2) as r:
                d = json.load(r)
        except urllib.error.HTTPError as e:
            if e.code == 404:  # path not published: the camera source is down
                return StreamSample(False, 0)
            return None
        except (OSError, ValueError):
            return None  # MediaMTX not running: unknown, reported as down by the agent
        # MediaMTX 1.21 names it inboundBytes; earlier versions bytesReceived.
        produced = d.get("inboundBytes", d.get("bytesReceived")) or 0
        return StreamSample(bool(d.get("ready")), int(produced))


_TEMP = re.compile(r"temp=([0-9.]+)'C")
_THROTTLED = re.compile(r"throttled=(0x[0-9a-fA-F]+)")


def parse_temp(text: str) -> Optional[int]:
    m = _TEMP.search(text)
    return round(float(m.group(1)) * 1000) if m else None


def parse_throttled(text: str) -> Optional[int]:
    m = _THROTTLED.search(text)
    return int(m.group(1), 16) if m else None


class VcgencmdPi:
    def _run(self, *args: str) -> str:
        try:
            return subprocess.run(["vcgencmd", *args], capture_output=True, text=True, timeout=2).stdout
        except (OSError, subprocess.SubprocessError):
            return ""

    def soc_milli_c(self) -> int:
        return parse_temp(self._run("measure_temp")) or 0

    def throttled(self) -> int:
        v = parse_throttled(self._run("get_throttled"))
        return 0 if v is None else v


class GpiodLid:
    """The lid switch on `line` of gpiochip0, with the internal pull-up enabled."""

    def __init__(self, line: int, open_level: int):
        import gpiod  # python3-libgpiod; imported only on the Pi

        self.open_level = open_level
        if hasattr(gpiod, "request_lines"):  # libgpiod v2
            from gpiod.line import Bias, Direction

            self._req = gpiod.request_lines(
                "/dev/gpiochip0",
                consumer="talos-vision",
                config={line: gpiod.LineSettings(direction=Direction.INPUT, bias=Bias.PULL_UP)},
            )
            self._line = line
            self._v2 = True
        else:  # libgpiod v1
            chip = gpiod.Chip("gpiochip0")
            self._l = chip.get_line(line)
            self._l.request(consumer="talos-vision", type=gpiod.LINE_REQ_DIR_IN,
                            flags=getattr(gpiod, "LINE_REQ_FLAG_BIAS_PULL_UP", 0))
            self._v2 = False

    def raw_open(self) -> bool:
        if self._v2:
            v = self._req.get_value(self._line)
            level = 1 if int(getattr(v, "value", v)) else 0
        else:
            level = self._l.get_value()
        return level == self.open_level


class TimedatectlClock:
    def synchronised(self) -> bool:
        try:
            out = subprocess.run(["timedatectl", "show", "-p", "NTPSynchronized", "--value"],
                                 capture_output=True, text=True, timeout=2).stdout
        except (OSError, subprocess.SubprocessError):
            return False
        return out.strip() == "yes"


# ---- Fakes for tests and the virtual Vision Node (dev on the Mac) -----------------------------------


class FileLid:
    """Lid state from a file: "1" = open. Missing file = closed. `echo 1 > lid` opens it."""

    def __init__(self, path: pathlib.Path):
        self.path = path

    def raw_open(self) -> bool:
        try:
            return self.path.read_text().strip() == "1"
        except OSError:
            return False


class FixedPi:
    def __init__(self, milli_c: int = 45_000, throttled: int = 0):
        self.milli_c, self.bits = milli_c, throttled

    def soc_milli_c(self) -> int:
        return self.milli_c

    def throttled(self) -> int:
        return self.bits


class AlwaysSynced:
    def synchronised(self) -> bool:
        return True
