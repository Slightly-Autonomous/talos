"""Minimal KiCad S-expression reader/writer and symbol-library helpers.

Tokens keep their original quoting so lib_symbols can be embedded verbatim.
"""

from __future__ import annotations

import math
import os
import re
from dataclasses import dataclass, field

_TOKEN = re.compile(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+')


def parse(text: str) -> list:
    stack: list[list] = [[]]
    for tok in _TOKEN.findall(text):
        if tok == "(":
            stack.append([])
        elif tok == ")":
            node = stack.pop()
            stack[-1].append(node)
        else:
            stack[-1].append(tok)
    return stack[0]


def unq(tok: str) -> str:
    if len(tok) >= 2 and tok[0] == '"' and tok[-1] == '"':
        return tok[1:-1].replace('\\"', '"').replace("\\\\", "\\")
    return tok


def q(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def dump(node, indent: int = 0) -> str:
    if not isinstance(node, list):
        return node
    pad = "\t" * indent
    if all(not isinstance(x, list) for x in node):
        return pad + "(" + " ".join(node) + ")"
    head = []
    i = 0
    while i < len(node) and not isinstance(node[i], list):
        head.append(node[i])
        i += 1
    lines = [pad + "(" + " ".join(head)]
    for child in node[i:]:
        lines.append(dump(child, indent + 1) if isinstance(child, list) else pad + "\t" + child)
    lines.append(pad + ")")
    return "\n".join(lines)


def find(node: list, key: str) -> list | None:
    for x in node:
        if isinstance(x, list) and x and x[0] == key:
            return x
    return None


def find_all(node: list, key: str) -> list[list]:
    return [x for x in node if isinstance(x, list) and x and x[0] == key]


@dataclass
class Pin:
    number: str
    name: str
    etype: str
    x: float
    y: float
    angle: float
    hidden: bool
    unit: int = 1  # 0 (shared by all units) is folded into unit 1


@dataclass
class LibSymbol:
    lib: str
    name: str
    node: list  # flattened symbol node, named "<lib>:<name>"
    pins: list[Pin] = field(default_factory=list)

    @property
    def lib_id(self) -> str:
        return f"{self.lib}:{self.name}"

    def prop(self, key: str) -> str:
        for p in find_all(self.node, "property"):
            if unq(p[1]) == key:
                return unq(p[2])
        return ""


def _pins_of(sym: list) -> list[Pin]:
    pins: list[Pin] = []
    for sub in find_all(sym, "symbol"):
        m = re.search(r"_(\d+)_(\d+)$", unq(sub[1]))
        unit, style = (int(m.group(1)), int(m.group(2))) if m else (1, 1)
        if style not in (0, 1):
            continue
        for p in find_all(sub, "pin"):
            at = find(p, "at")
            hidden = "hide" in p or any(
                isinstance(x, list) and x[0] == "hide" and (len(x) == 1 or x[1] == "yes") for x in p
            )
            pins.append(
                Pin(
                    number=unq(find(p, "number")[1]),
                    name=unq(find(p, "name")[1]),
                    etype=p[1],
                    x=float(at[1]),
                    y=float(at[2]),
                    angle=float(at[3]) if len(at) > 3 else 0.0,
                    hidden=hidden,
                    unit=max(unit, 1),
                )
            )
    return pins


class SymbolLibraries:
    """Loads symbols from KiCad's stock libraries and project libraries, flattening `extends`."""

    def __init__(self, stock_dir: str, extra: dict[str, str]):
        self.stock_dir = stock_dir
        self.extra = extra  # lib nickname -> path
        self._cache: dict[str, dict[str, list]] = {}

    def _lib(self, lib: str) -> dict[str, list]:
        if lib not in self._cache:
            path = self.extra.get(lib) or os.path.join(self.stock_dir, f"{lib}.kicad_sym")
            root = parse(open(path, encoding="utf-8").read())[0]
            self._cache[lib] = {unq(s[1]): s for s in find_all(root, "symbol")}
        return self._cache[lib]

    def get(self, lib_id: str) -> LibSymbol:
        lib, name = lib_id.split(":", 1)
        syms = self._lib(lib)
        sym = syms[name]
        ext = find(sym, "extends")
        if ext:
            parent_name = unq(ext[1])
            parent = syms[parent_name]
            child_props = {unq(p[1]): p for p in find_all(sym, "property")}
            flat = [sym[0], q(f"{lib}:{name}")]
            for item in parent[2:]:
                if isinstance(item, list) and item[0] == "property" and unq(item[1]) in child_props:
                    flat.append(child_props.pop(unq(item[1])))
                elif isinstance(item, list) and item[0] == "symbol":
                    sub = list(item)
                    sub[1] = q(unq(item[1]).replace(parent_name, name, 1))
                    flat.append(sub)
                else:
                    flat.append(item)
            flat.extend(child_props.values())
        else:
            flat = [sym[0], q(f"{lib}:{name}")] + sym[2:]
        return LibSymbol(lib, name, flat, _pins_of(flat))


def pin_end(sx: float, sy: float, pin: Pin) -> tuple[float, float]:
    """Schematic coordinates of a pin's connection point for an unrotated symbol at (sx, sy)."""
    return round(sx + pin.x, 4), round(sy - pin.y, 4)


def stub_dir(pin: Pin) -> tuple[int, int]:
    """Unit vector (schematic coords) pointing away from the symbol body."""
    a = math.radians(pin.angle)
    return int(round(-math.cos(a))), int(round(math.sin(a)))
