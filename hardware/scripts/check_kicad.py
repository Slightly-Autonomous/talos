#!/usr/bin/env python3
"""Run KiCad ERC and a netlist round-trip on every generated board.

    python3 hardware/scripts/check_kicad.py            # writes hardware/results/kicad-check.md
    python3 hardware/scripts/check_kicad.py --pdf      # also exports hardware/results/schematics/*.pdf
    KICAD_CLI=/path/to/kicad-cli python3 ...           # override the CLI

Checks, per board:
  1. ERC (kicad-cli sch erc, all severities). Errors fail; warnings fail unless waived
     in hardware/kicad/erc-waivers.json with a justification.
  2. Netlist round-trip: KiCad's exported netlist must equal the generator's intended
     netlist pin for pin (intended-netlist.json). Intended no-connects must be unconnected.
  3. Cross-board: every card's edge contacts must agree with the main board slot pinout
     (sensor_edge_v1), and PRESENT_N must be grounded on every card.
  4. Safe defaults (HWR-007): power/reset/CS/mux control nets have a defining resistor.
  5. Footprint audit: assigned footprints exist in the KiCad library; missing ones are listed.
Exit code is non-zero if any check fails.
"""

from __future__ import annotations

import csv
import datetime
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kicad_sexpr import find, find_all, parse, unq  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
KICAD = ROOT / "hardware" / "kicad"
RESULTS = ROOT / "hardware" / "results"
BOARDS = ["TL-SN-001", "TL-MOD-001", "TL-MOD-002", "TL-MOD-003", "TL-MOD-004", "TL-TEST-001"]
CARDS = BOARDS[1:]


def kicad_cli() -> str:
    for c in (os.environ.get("KICAD_CLI"), shutil.which("kicad-cli"),
              os.path.expanduser("~/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"),
              "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"):
        if c and os.path.exists(c):
            return c
    raise SystemExit("kicad-cli not found; set KICAD_CLI")


def footprint_dir() -> pathlib.Path | None:
    for c in (os.environ.get("KICAD_FOOTPRINT_DIR"), "/usr/share/kicad/footprints",
              os.path.expanduser("~/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints"),
              "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints"):
        if c and os.path.isdir(c):
            return pathlib.Path(c)
    return None


def run(cmd: list[str]) -> str:
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.stdout + p.stderr


def erc(cli: str, board: str, tmp: str, waivers: list[dict]) -> tuple[bool, list[str], dict]:
    out = os.path.join(tmp, f"{board}-erc.json")
    run([cli, "sch", "erc", "--format", "json", "--severity-all", "-o", out,
         str(KICAD / board / f"{board}.kicad_sch")])
    data = json.load(open(out))
    lines, counts, ok = [], {"error": 0, "warning": 0, "waived": 0}, True
    for sheet in data.get("sheets", []):
        for v in sheet["violations"]:
            items = [i["description"] for i in v["items"]]
            waiver = next((w for w in waivers if w["board"] == board and w["type"] == v["type"]
                           and all(any(m in i for i in items) for m in w["match"])), None)
            if waiver:
                counts["waived"] += 1
                lines.append(f"  - waived `{v['type']}`: {v['description']} ({'; '.join(items)}). "
                             f"Justification: {waiver['justification']}")
                continue
            counts[v["severity"]] = counts.get(v["severity"], 0) + 1
            ok = False
            lines.append(f"  - **{v['severity']}** `{v['type']}`: {v['description']} ({'; '.join(items)})")
    return ok, lines, counts


def layout_status() -> dict:
    path = KICAD / "layout-status.json"
    return json.load(open(path)) if path.exists() else {}


def pcb_drc(cli: str, board: str, tmp: str) -> tuple[bool, dict, list[str]]:
    """DRC with schematic parity on the PCB. Errors fail; warnings are counted and reported.
    A board whose layout-status says routing is "partial" may have unconnected items (reported, not fatal)."""
    partial = layout_status().get(board, {}).get("routing") == "partial"
    pcb = KICAD / board / f"{board}.kicad_pcb"
    if not pcb.exists():
        return True, {}, ["- PCB: not generated"]
    out = os.path.join(tmp, f"{board}-drc.json")
    run([cli, "pcb", "drc", "--schematic-parity", "--refill-zones", "--format", "json", "--severity-all",
         "-o", out, str(pcb)])
    data = json.load(open(out))
    counts: dict[str, int] = {}
    errors = []
    for key in ("violations", "unconnected_items", "schematic_parity"):
        for v in data.get(key, []):
            counts[f"{v['severity']}:{v['type']}"] = counts.get(f"{v['severity']}:{v['type']}", 0) + 1
            if v["severity"] == "error" and not (partial and v["type"] == "unconnected_items"):
                errors.append(f"  - **{v['type']}**: {v['description']} "
                              f"({'; '.join(i['description'] for i in v['items'])})")
    summary = ", ".join(f"{k.split(':')[1]} {n}" for k, n in sorted(counts.items())) or "none"
    lines = [f"- PCB DRC (with schematic parity): {summary}"]
    if partial:
        lines.append(f"- **Routing incomplete** ({counts.get('error:unconnected_items', 0)} unconnected items): "
                     f"{layout_status()[board].get('note', '')}")
    lines += errors[:30]
    return not errors, counts, lines


def exported_netlist(cli: str, board: str, tmp: str) -> dict[tuple[str, str], str]:
    out = os.path.join(tmp, f"{board}.net")
    run([cli, "sch", "export", "netlist", "--format", "kicadsexpr", "-o", out,
         str(KICAD / board / f"{board}.kicad_sch")])
    root = parse(open(out).read())[0]
    nodes = {}
    for net in find_all(find(root, "nets"), "net"):
        name = unq(find(net, "name")[1])
        for node in find_all(net, "node"):
            nodes[(unq(find(node, "ref")[1]), unq(find(node, "pin")[1]))] = name
    return nodes


def compare(intended: dict, exported: dict) -> list[str]:
    problems = []
    seen = set()
    for ref, part in intended["parts"].items():
        for pin, net in part["pins"].items():
            seen.add((ref, pin))
            got = exported.get((ref, pin))
            if net is None:
                if got is not None and not got.startswith("unconnected-"):
                    problems.append(f"{ref}.{pin}: intended no-connect, KiCad has {got}")
            elif got != f"/{net}":
                problems.append(f"{ref}.{pin}: intended {net}, KiCad has {got}")
    for (ref, pin), name in exported.items():
        if ref.startswith("#"):
            continue
        if (ref, pin) not in seen:
            problems.append(f"{ref}.{pin}: in KiCad ({name}) but not in the intended netlist")
    return problems


def role(net: str | None) -> str | None:
    if net is None:
        return None
    for pat, r in [(r"^V3V3_AON(_\d)?$", "AON"), (r"^V3V3_SW(_\d)?$", "SW"), (r"^SLOT\d_(.+)$", None),
                   (r"^SLOT_WAKE\d$", "WAKE"), (r"^SPI_(SCK|MOSI|MISO)$", None)]:
        m = re.match(pat, net)
        if m:
            return r or m.group(1)
    return net


def cross_board(intended: dict[str, dict]) -> list[str]:
    problems = []
    main = intended["TL-SN-001"]["parts"]
    for n, j in enumerate(["J4", "J5", "J6", "J7"]):
        roles = {c: role(net) for c, net in main[j]["pins"].items()}
        if n == 0:
            slot0 = roles
        elif roles != slot0:
            problems.append(f"TL-SN-001 {j}: slot pinout differs from J4")
    for card in CARDS:
        fingers = intended[card]["parts"]["J1"]["pins"]
        for c, net in fingers.items():
            want = slot0[c]
            if c == "18":
                if net != "GND":
                    problems.append(f"{card} contact 18 PRESENT_N must be GND, is {net}")
                continue
            if net is not None and role(net) != want:
                problems.append(f"{card} contact {c}: card role {role(net)} vs main board {want}")
            if want is None and net is not None:
                problems.append(f"{card} contact {c}: reserved contact used ({net})")
    return problems


SAFE = [r"^SLOT_SW_EN\d$", r"^SLOT_AON_EN\d$", r"^SLOT\d_RESET_N$", r"^SLOT\d_CS_N$", r"^UART_MUX_EN_N$",
        r"^UART_SEL\d$", r"^MUX_RESET_N$", r"^SLOT_WAKE\d$", r"^BOOT_N$", r"^CHIP_EN$", r"^STATUS_LED_N$"]


def safe_defaults(intended: dict) -> list[str]:
    parts = intended["parts"]
    problems = []
    nets = {net for p in parts.values() for net in p["pins"].values() if net}
    for net in sorted(nets):
        if not any(re.match(p, net) for p in SAFE):
            continue
        ok = False
        for ref, p in parts.items():
            if p["lib_id"] != "Device:R":
                continue
            a, b = p["pins"]["1"], p["pins"]["2"]
            other = b if a == net else a if b == net else None
            if other in ("GND", "V3V3_CORE") or (other or "").startswith("V3V3_AON"):
                ok = True
        if not ok:
            problems.append(f"{net}: no pull-up/pull-down defining the reset state")
    return problems


def footprints(intended: dict, fpdir: pathlib.Path | None) -> tuple[list[str], list[str]]:
    missing, tbd = [], []
    for ref, p in sorted(intended["parts"].items()):
        fp = p["footprint"]
        if not fp:
            tbd.append(f"{ref} ({p['value']}): {p['note'] or 'footprint not assigned'}")
            continue
        lib, name = fp.split(":", 1)
        if "PLACEHOLDER" in name:
            tbd.append(f"{ref} ({p['value']}): placeholder land pattern {fp}")
        base = KICAD / "libraries" / "talos-footprints.pretty" if lib == "talos" else (fpdir / f"{lib}.pretty" if fpdir else None)
        if base is not None:
            if not (base / f"{name}.kicad_mod").exists():
                missing.append(f"{ref}: {fp} not found in KiCad library")
    return missing, tbd


def write_bom(board: str, intended: dict) -> None:
    rows: dict[tuple, list[str]] = {}
    for ref, p in intended["parts"].items():
        rows.setdefault((p["value"], p["lib_id"], p["footprint"]), []).append(ref)
    path = RESULTS / "bom" / f"{board}.csv"
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["qty", "refs", "value", "symbol", "footprint"])
        for (value, lib_id, fp), refs in sorted(rows.items(), key=lambda r: sorted(r[1])[0]):
            w.writerow([len(refs), " ".join(sorted(refs, key=lambda r: (re.sub(r"\d", "", r), int(re.sub(r"\D", "", r) or 0)))),
                        value, lib_id, fp or "TBD"])


def export_pdfs(cli: str) -> None:
    out = RESULTS / "schematics"
    out.mkdir(parents=True, exist_ok=True)
    for b in BOARDS:
        run([cli, "sch", "export", "pdf", "-o", str(out / f"{b}.pdf"), str(KICAD / b / f"{b}.kicad_sch")])


def write_pin_net(board: str, intended: dict) -> None:
    path = RESULTS / "pin-to-net" / f"{board}.csv"
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ref", "pin", "net", "value"])
        for ref, p in sorted(intended["parts"].items()):
            for pin, net in sorted(p["pins"].items(), key=lambda kv: (len(kv[0]), kv[0])):
                w.writerow([ref, pin, net or "NC", p["value"]])


def main() -> int:
    cli = kicad_cli()
    if "--pdf" in sys.argv:
        export_pdfs(cli)
    version = run([cli, "version"]).strip().splitlines()[-1]
    waivers = json.load(open(KICAD / "erc-waivers.json"))
    fpdir = footprint_dir()
    intended = {b: json.load(open(KICAD / b / "intended-netlist.json")) for b in BOARDS}
    report = ["# KiCad check results (generated)\n",
              f"`hardware/scripts/check_kicad.py`, kicad-cli **{version}**, {datetime.date.today().isoformat()}. "
              "This shows the schematics are self-consistent. **It is not an electrical design review.**\n",
              "| Board | Parts | ERC errors | ERC warnings | Waived | Netlist round-trip | Safe defaults | PCB DRC errors | PCB DRC warnings |",
              "|---|---:|---:|---:|---:|---|---|---:|---:|"]
    details, all_ok = [], True
    with tempfile.TemporaryDirectory() as tmp:
        for b in BOARDS:
            ok_erc, lines, counts = erc(cli, b, tmp, waivers)
            diffs = compare(intended[b], exported_netlist(cli, b, tmp))
            safe = safe_defaults(intended[b])
            missing, tbd = footprints(intended[b], fpdir)
            write_bom(b, intended[b])
            write_pin_net(b, intended[b])
            ok_drc, drc_counts, drc_lines = pcb_drc(cli, b, tmp)
            drc_err = sum(n for k, n in drc_counts.items() if k.startswith("error:"))
            drc_warn = sum(n for k, n in drc_counts.items() if k.startswith("warning:"))
            ok = ok_erc and not diffs and not safe and not missing and ok_drc
            all_ok &= ok
            report.append(f"| {b} | {len(intended[b]['parts'])} | {counts['error']} | {counts['warning']} | "
                          f"{counts['waived']} | {'match' if not diffs else f'{len(diffs)} differences'} | "
                          f"{'ok' if not safe else f'{len(safe)} missing'} | {drc_err} | {drc_warn} |")
            details.append(f"\n## {b}\n")
            details += lines or ["- ERC: no violations"]
            details += [f"- netlist: {d}" for d in diffs[:50]] or ["- Netlist: KiCad export equals the intended netlist"]
            details += [f"- safe default: {s}" for s in safe]
            details += [f"- footprint missing: {m}" for m in missing]
            details += drc_lines
            if tbd:
                details.append("- Footprints still to draw (H4):")
                details += [f"  - {t}" for t in tbd]
    xb = cross_board(intended)
    all_ok &= not xb
    report.append("\n## Cross-board connector check (sensor_edge_v1)\n")
    report += [f"- {p}" for p in xb] or [
        "- All four main-board slots share one pinout; every card's used contacts match it, reserved contacts "
        "(5, 19) are unused, and PRESENT_N (18) is grounded on all five cards."]
    report += details
    report.append(f"\n**Overall: {'PASS' if all_ok else 'FAIL'}**\n")
    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / "kicad-check.md").write_text("\n".join(report))
    print("\n".join(report[4:4 + len(BOARDS)]))
    print(f"cross-board: {'ok' if not xb else xb}")
    print("PASS" if all_ok else "FAIL")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
