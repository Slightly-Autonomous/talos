"""Search Sensor Node layouts: board outlines x placement seeds, built in parallel, scored, ranked.

    python3 hardware/scripts/layout_search.py place [--sizes 100x80,90x75,80x70] [--seeds 1,2,3] [--out DIR]

Each candidate is a copy of hardware/kicad/TL-SN-001 under DIR (default: a temp folder), placed by
gen_pcbs.py with TALOS_SN_SIZE / TALOS_SEED / TALOS_LAYOUT_OUT, so the repo's boards are never touched.
`place` scores placements with the placer's own cost terms (wirelength, decap affinity, congestion) plus
board area, and writes DIR/candidates.json. Routing the best candidates is a separate, slower step. Local
only: routing and placement search never run in CI.
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
KICAD = ROOT / "hardware" / "kicad"
BOARD = "TL-SN-001"
KPY = os.environ.get("KICAD_PYTHON", os.path.expanduser(
    "~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3"))
PLACE_RE = re.compile(r"place: legal wirelength=([\d.]+), affinity=([\d.]+), congestion=([\d.]+), "
                      r"overlap=([\d.]+), (?:group=([\d.]+), )?cost=([\d.]+)")


def candidate_dir(out: pathlib.Path, size: str, seed: int) -> pathlib.Path:
    return out / f"{size}-s{seed}"


def place_one(out: pathlib.Path, size: str, seed: int) -> dict:
    root = candidate_dir(out, size, seed)
    if root.exists():
        shutil.rmtree(root)
    shutil.copytree(KICAD / BOARD, root / BOARD)
    (root / "libraries").symlink_to(KICAD / "libraries")
    env = dict(os.environ, TALOS_SN_SIZE=size, TALOS_SEED=str(seed), TALOS_LAYOUT_OUT=str(root))
    p = subprocess.run([KPY, str(ROOT / "hardware" / "scripts" / "gen_pcbs.py"), BOARD], env=env,
                       capture_output=True, text=True)
    w, h = (float(v) for v in size.split("x"))
    res = {"size": size, "seed": seed, "area_mm2": w * h, "dir": str(root), "ok": p.returncode == 0}
    m = None
    for m in PLACE_RE.finditer(p.stdout + p.stderr):  # the last summary is the final placement
        pass
    if not res["ok"] or not m:
        res["error"] = (p.stdout + p.stderr).strip().splitlines()[-1:] or ["no placement summary"]
        res["ok"] = False
        return res
    wl, aff, cong, ovl, grp, cost = (float(v or 0) for v in m.groups())
    res.update(wirelength=wl, affinity=aff, congestion=cong, overlap=ovl, group=grp, place_cost=cost)
    return res


def cmd_place(args) -> int:
    out = pathlib.Path(args.out or tempfile.mkdtemp(prefix="talos-layout-"))
    out.mkdir(parents=True, exist_ok=True)
    jobs = [(s, int(seed)) for s in args.sizes.split(",") for seed in args.seeds.split(",")]
    workers = max(1, min(len(jobs), (os.cpu_count() or 4) // 2))
    results = []
    with cf.ProcessPoolExecutor(max_workers=workers) as ex:
        futs = {ex.submit(place_one, out, s, seed): (s, seed) for s, seed in jobs}
        for f in cf.as_completed(futs):
            r = f.result()
            results.append(r)
            tag = f"{r['size']} seed {r['seed']}"
            print(f"{tag:18s} " + (f"wl {r['wirelength']:7.0f}  cong {r['congestion']:6.1f}  grp {r['group']:6.0f}  "
                                   f"cost {r['place_cost']:7.0f}  area {r['area_mm2']:5.0f}"
                                   if r["ok"] else f"FAILED {r['error']}"), flush=True)
    ok = sorted((r for r in results if r["ok"]), key=lambda r: (r["area_mm2"], r["place_cost"]))
    (out / "candidates.json").write_text(json.dumps(results, indent=2))
    print(f"\n{len(ok)}/{len(results)} placed; candidates in {out}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    pl = sub.add_parser("place")
    pl.add_argument("--sizes", default="100x80,90x75,80x70")
    pl.add_argument("--seeds", default="1,2,3")
    pl.add_argument("--out")
    args = ap.parse_args()
    return {"place": cmd_place}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
