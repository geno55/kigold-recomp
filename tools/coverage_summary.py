#!/usr/bin/env python3
"""coverage_summary.py - how much of the recompiled code the coverage runs reached.

Inputs: one or more coverage files (function names, one per line, as a KIG_TRACE build writes them with
KIG_COVERAGE_FILE; tools/run_scenario.py --coverage puts them in build/scen/<name>/coverage.txt). The
compiled functions are syms/kig.us.syms.toml's, minus kig.us.toml's ignored (dead) functions and the
functions N64Recomp skips (libultra code the runtime replaces). Reports entered functions and bytes per
code segment, and lists the largest functions no run entered.

Usage: python tools/coverage_summary.py [coverage.txt ...] [--top N]
       (default: every build/scen/*/coverage.txt)
"""
import argparse
import re
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="*", type=Path)
    ap.add_argument("--top", type=int, default=15, help="largest unentered functions to list")
    a = ap.parse_args()
    files = a.files or sorted((ROOT / "build" / "scen").glob("*/coverage.txt"))
    if not files:
        print("no coverage files (run tools/run_scenario.py --coverage with a KIG_TRACE build)")
        return 1

    syms = tomllib.loads((ROOT / "syms" / "kig.us.syms.toml").read_text(encoding="utf-8"))
    config = tomllib.loads((ROOT / "kig.us.toml").read_text(encoding="utf-8"))
    ignored = set(config["patches"].get("ignored", []))
    skipped_h = (ROOT / "include" / "kig_skipped.h").read_text(encoding="utf-8")
    skipped = {m.removesuffix("_recomp") for m in re.findall(r"\b(\w+_recomp)\b", skipped_h)}
    # N64Recomp also skips the libultra functions it has runtime replacements for; those never appear in a
    # coverage file, so they are left out of the totals by name below (any function no recompiled body
    # exists for is reported separately).
    recompiled = set()
    for f in (ROOT / "RecompiledFuncs").glob("*.c"):
        recompiled.update(re.findall(r"^RECOMP_FUNC void (\w+)\(", f.read_text(errors="replace"), re.M))

    entered = set()
    for f in files:
        entered.update(line.strip() for line in f.read_text(errors="replace").splitlines() if line.strip())

    rows = []
    for sec in syms["section"]:
        for fn in sec.get("functions", []):
            name = fn["name"]
            if name in ignored or name in skipped or name not in recompiled:
                continue
            rows.append((sec["name"], name, fn["vram"], fn["size"], name in entered))

    # Handwritten code has many entry points into the same instructions (symbols that overlap), so code is
    # counted as unique addresses: the union of the entered functions' ranges over the union of all.
    def covered(fns):
        words = set()
        for _, _, vram, size, _ in fns:
            words.update(range(vram, vram + size, 4))
        return 4 * len(words)

    print(f"coverage from {len(files)} run(s): " + ", ".join(str(f.parent.name if f.name == 'coverage.txt' else f) for f in files))
    print(f"{'segment':8} {'entry points':>20} {'code bytes (unique)':>26}")
    for seg in list(dict.fromkeys(r[0] for r in rows)) + ["total"]:
        r = rows if seg == "total" else [x for x in rows if x[0] == seg]
        n, ne = len(r), sum(x[4] for x in r)
        b, be = covered(r), covered([x for x in r if x[4]])
        print(f"{seg:8} {ne:6}/{n:<6} {100 * ne / n:5.1f}%  {be:8}/{b:<8} {100 * be / b:5.1f}%")
    unknown = sorted(entered - {r[1] for r in rows} - ignored - skipped)
    if unknown:
        print(f"({len(unknown)} entered names are not compiled functions here, e.g. {', '.join(unknown[:5])})")
    # Largest stretches of code no entered function covers (contiguous unique addresses), with the symbols
    # that start in them.
    hit = set()
    for _, _, vram, size, h in rows:
        if h:
            hit.update(range(vram, vram + size, 4))
    starts = {}
    for seg, name, vram, size, _ in rows:
        starts.setdefault(vram, (seg, name))
    allw = sorted({w for _, _, vram, size, _ in rows for w in range(vram, vram + size, 4)} - hit)
    gaps, lo = [], None
    for i, w in enumerate(allw):
        if lo is None:
            lo = w
        if i + 1 == len(allw) or allw[i + 1] != w + 4:
            gaps.append((lo, w + 4))
            lo = None
    print(f"\nlargest code never reached:")
    for lo, hi in sorted(gaps, key=lambda g: g[0] - g[1])[:a.top]:
        names = [starts[v][1] for v in range(lo, hi, 4) if v in starts]
        seg = starts[names and next(v for v in range(lo, hi, 4) if v in starts) or lo][0] if names else "?"
        print(f"  {lo:08X}-{hi:08X} {hi - lo:#7x} {seg:5} {', '.join(names[:3])}{' ...' if len(names) > 3 else ''}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
