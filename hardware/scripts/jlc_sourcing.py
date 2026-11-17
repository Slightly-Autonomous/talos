#!/usr/bin/env python3
"""JLCPCB sourcing: every assembled part must be a part JLCPCB actually stocks.

    python3 hardware/scripts/jlc_sourcing.py refresh   # network: query JLCPCB, rewrite the stock snapshot
    python3 hardware/scripts/jlc_sourcing.py check     # offline (CI): every part mapped, stocked, MPN matches
    python3 hardware/scripts/jlc_sourcing.py export [BOARD ...]  # JLCPCB-format BOM + CPL into hardware/fab

Inputs:
- hardware/sourcing/jlc-parts.csv: (value, footprint) -> LCSC part, one row per distinct part.
  `assembly` is `jlc` (JLCPCB places it), `board_feature` (test pad, gold fingers: not a part) or
  `not_placed` (JLCPCB does not stock it). Every `not_placed` row must name a risk ID in
  design-risks.md; the check reports it but does not fail, so open decisions stay visible.
- hardware/sourcing/jlc-stock.json: snapshot written by `refresh` (stock, Basic/Extended, MPN).

The check fails if a part has no row, a `jlc` row's LCSC code is missing from the snapshot, the
snapshot MPN differs from the row's MPN, or stock is below the quantity for BUILD_QTY boards.
Stock moves daily: run `refresh` and `check` again right before any order. Nothing is ordered
here (HW-0005).
"""

from __future__ import annotations

import csv
import datetime
import fnmatch
import json
import os
import pathlib
import re
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_kicad import BOARDS, KICAD, ROOT  # noqa: E402

SOURCING = ROOT / "hardware" / "sourcing"
PARTS = SOURCING / "jlc-parts.csv"
STOCK = SOURCING / "jlc-stock.json"
FAB = ROOT / "hardware" / "fab"
BUILD_QTY = 5  # prototype run size the stock check must cover
LOW_STOCK_FACTOR = 10  # warn when stock covers fewer than 10 such runs
STALE_DAYS = 30
API = "https://jlcpcb.com/api/overseas-pcb-order/v1/shoppingCart/smtGood/selectSmtComponentList"
# KiCad and JLCPCB disagree on the zero orientation of some packages. JLCPCB's placement preview
# is the authority: check every part there before confirming, and record corrections here by
# footprint name (fnmatch pattern -> degrees added). None recorded yet: none verified.
ROTATION_FIX: dict[str, float] = {}


def load_rows() -> list[dict]:
    with open(PARTS, newline="") as f:
        return list(csv.DictReader(f))


def board_parts(board: str) -> dict[str, tuple[str, str]]:
    intended = json.load(open(KICAD / board / "intended-netlist.json"))
    return {ref: (p["value"], (p["footprint"] or "TBD").split(":")[-1]) for ref, p in intended["parts"].items()}


def match(rows: list[dict], value: str, fp: str) -> list[dict]:
    return [r for r in rows if fnmatch.fnmatchcase(value, r["value"]) and fnmatch.fnmatchcase(fp, r["footprint"])]


def natural(ref: str) -> tuple:
    return re.sub(r"\d", "", ref), int(re.sub(r"\D", "", ref) or 0)


def resolve(board: str, rows: list[dict]) -> tuple[dict[str, dict], list[str]]:
    out, errors = {}, []
    for ref, (value, fp) in sorted(board_parts(board).items(), key=lambda kv: natural(kv[0])):
        m = match(rows, value, fp)
        if len(m) != 1:
            errors.append(f"{board} {ref} ({value}, {fp}): {'no' if not m else len(m)} sourcing rows match")
        else:
            out[ref] = m[0]
    return out, errors


def query(code: str) -> dict | None:
    body = json.dumps({"keyword": code, "currentPage": 1, "pageSize": 10}).encode()
    req = urllib.request.Request(API, data=body, headers={"Content-Type": "application/json", "User-Agent": "Mozilla/5.0"})
    data = json.load(urllib.request.urlopen(req, timeout=30)).get("data") or {}
    for c in (data.get("componentPageInfo") or {}).get("list") or []:
        if c.get("componentCode") == code:
            return {"mpn": c["componentModelEn"], "manufacturer": c["componentBrandEn"],
                    "library": "basic" if c["componentLibraryType"] == "base" else "extended",
                    "stock": int(c["stockCount"] or 0), "package": c.get("componentSpecificationEn") or ""}
    return None


def refresh() -> int:
    codes = sorted({r["lcsc"] for r in load_rows() if r["lcsc"]})
    parts = {}
    for code in codes:
        parts[code] = query(code) or {"missing": True}
        print(f"  {code}: {parts[code]}")
        time.sleep(0.8)  # be polite to the public endpoint
    STOCK.write_text(json.dumps({"checked": datetime.date.today().isoformat(), "source": API,
                                 "parts": parts}, indent=1, sort_keys=True) + "\n")
    print(f"wrote {STOCK.relative_to(ROOT)} ({len(parts)} parts)")
    return 0


def check() -> int:
    rows = load_rows()
    snap = json.load(open(STOCK))
    errors, warnings, waived = [], [], []
    need: dict[str, int] = {}
    for board in BOARDS:
        resolved, errs = resolve(board, rows)
        values = {ref: v for ref, (v, _) in board_parts(board).items()}
        errors += errs
        for ref, r in resolved.items():
            if r["assembly"] == "jlc":
                need[r["lcsc"]] = need.get(r["lcsc"], 0) + BUILD_QTY
            elif r["assembly"] == "not_placed":
                if not r["risk"]:
                    errors.append(f"{board} {ref}: not_placed without a risk ID")
                waived.append(f"{board} {ref} {values[ref]}: {r['note']} ({r['risk']})")
            elif r["assembly"] != "board_feature":
                errors.append(f"{board} {ref}: unknown assembly '{r['assembly']}'")
    by_code = {r["lcsc"]: r for r in rows if r["assembly"] == "jlc"}
    for code, qty in sorted(need.items()):
        s = snap["parts"].get(code)
        r = by_code[code]
        if not s or s.get("missing"):
            errors.append(f"{code} ({r['mpn']}): not in the JLCPCB snapshot; run refresh or pick another part")
            continue
        if s["mpn"] != r["mpn"]:
            errors.append(f"{code}: JLCPCB lists {s['mpn']}, sourcing file says {r['mpn']}")
        if s["stock"] < qty:
            errors.append(f"{code} ({r['mpn']}): stock {s['stock']} < {qty} needed for {BUILD_QTY} of every board")
        elif s["stock"] < qty * LOW_STOCK_FACTOR:
            warnings.append(f"{code} ({r['mpn']}): low stock {s['stock']} (need {qty} per run)")
    age = (datetime.date.today() - datetime.date.fromisoformat(snap["checked"])).days
    if age > STALE_DAYS:
        warnings.append(f"snapshot is {age} days old; run refresh before ordering")
    extended = sorted({by_code[c]["mpn"] for c in need if snap["parts"].get(c, {}).get("library") == "extended"})
    print(f"JLCPCB sourcing: {len(need)} distinct assembled parts, snapshot {snap['checked']}, "
          f"{len(extended)} Extended (setup fee each)")
    for w in waived:
        print(f"  NOT PLACED BY JLCPCB: {w}")
    for w in warnings:
        print(f"  warning: {w}")
    for e in errors:
        print(f"  ERROR: {e}")
    print("PASS" if not errors else "FAIL")
    return 0 if not errors else 1


def export(boards: list[str]) -> int:
    rows = load_rows()
    for board in boards:
        resolved, errs = resolve(board, rows)
        if errs:
            raise SystemExit("\n".join(errs))
        groups: dict[tuple, list[str]] = {}
        skipped = []
        for ref, r in resolved.items():
            value, fp = board_parts(board)[ref]
            if r["assembly"] == "jlc":
                groups.setdefault((value, fp, r["lcsc"]), []).append(ref)
            elif r["assembly"] == "not_placed":
                skipped.append([ref, value, r["mpn"], r["risk"], r["note"]])
        out = FAB / board
        out.mkdir(parents=True, exist_ok=True)
        with open(out / f"{board}-jlc-bom.csv", "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
            for (value, fp, code), refs in sorted(groups.items(), key=lambda g: natural(g[1][0])):
                w.writerow([value, ",".join(refs), fp, code])
        placed = {ref for refs in groups.values() for ref in refs}
        with open(out / f"{board}-cpl.csv", newline="") as f, open(out / f"{board}-jlc-cpl.csv", "w", newline="") as g:
            w = csv.writer(g)
            w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
            for p in csv.DictReader(f):
                if p["Ref"] not in placed:
                    continue
                fix = next((d for pat, d in ROTATION_FIX.items() if fnmatch.fnmatchcase(p["Package"], pat)), 0.0)
                rot = (float(p["Rot"]) + fix) % 360
                w.writerow([p["Ref"], f"{float(p['PosX']):.4f}mm", f"{float(p['PosY']):.4f}mm",
                            "Top" if p["Side"] == "top" else "Bottom", f"{rot:g}"])
        with open(out / f"{board}-not-assembled.csv", "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["ref", "value", "mpn", "risk", "why"])
            w.writerows(skipped)
        print(f"{board}: {sum(map(len, groups.values()))} parts for JLCPCB, {len(skipped)} not placed")
    return 0


def main() -> int:
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    if cmd == "refresh":
        return refresh()
    if cmd == "check":
        return check()
    if cmd == "export":
        return export(sys.argv[2:] or BOARDS)
    raise SystemExit(__doc__)


if __name__ == "__main__":
    sys.exit(main())
