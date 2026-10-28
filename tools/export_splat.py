"""Export function bounds, jump tables and data references from the splat/spimdisasm output.

Run splat first:  python -m splat split splat/kig.us.yaml
Then:             python tools/export_splat.py

KI Gold has several code sections (tools/kig_layout.py); every row carries its section, since seg1 and
seg2 share VRAM. Writes:
  build/splat_funcs.csv    section,vram,size,name
  build/splat_jtbls.csv    section,jtbl_vram,entries,func_vram,targets(;-separated)
  build/splat_datarefs.csv vram,name,refs   (%hi/%lo-referenced data symbols)
and prints cross-checks (coverage of each section's text, jal targets, jump table targets).
"""
import csv
import re
import struct
import sys
from bisect import bisect_right
from collections import Counter
from pathlib import Path

import rabbitizer

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kig_layout import SEGS, recomp_rom  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
ASM = ROOT / "build" / "splat" / "asm"

INSN = re.compile(r"/\* ([0-9A-F]+) ([0-9A-F]{8}) ([0-9A-F]{8}) \*/")
NONMATCHING = re.compile(r"^nonmatching (\S+), 0x([0-9A-F]+)")
GLABEL = re.compile(r"^\s*glabel (\S+)")
ALABEL = re.compile(r"^\s*alabel (\S+)")  # spimdisasm: extra entry point inside a function
DLABEL = re.compile(r"^dlabel (\S+)")
WORD_LABEL = re.compile(r"\.word \.L([0-9A-F]{8})")
HILO = re.compile(r"%(?:hi|lo)\(([A-Za-z_][\w.]*)\)")
SYMADDR = re.compile(r"_([0-9A-F]{8})$")


def seg_for_rom(rom_off: int):
    for s in SEGS:
        if s.recomp_rom <= rom_off < s.recomp_rom + s.size:
            return s
    return None


def parse_text():
    funcs = []  # (seg, vram, size, name, glabel/alabel)
    datarefs = Counter()
    for f in sorted(ASM.glob("*.s")):
        if f.name == "header.s":
            continue
        pending_size = {}
        cur = None
        for line in f.read_text(encoding="utf-8", errors="replace").splitlines():
            m = NONMATCHING.match(line)
            if m:
                pending_size[m[1]] = int(m[2], 16)
                continue
            m = GLABEL.match(line)
            a = ALABEL.match(line)
            if m or a:
                cur = (m or a)[1]
                kind = "glabel" if m else "alabel"
                continue
            m = INSN.search(line)
            if m and cur:
                seg = seg_for_rom(int(m[1], 16))
                funcs.append((seg.name, int(m[2], 16), pending_size.get(cur), cur, kind))
                cur = None
            for sym in HILO.findall(line):
                datarefs[sym] += 1
    return funcs, datarefs


def parse_jtbls():
    tables = []  # (seg, vram, targets)
    for f in sorted((ASM / "data").glob("*.s")):
        seg = seg_for_rom(int(f.name.split(".")[0], 16))
        cur, targets = None, []
        for line in f.read_text(encoding="utf-8", errors="replace").splitlines():
            m = DLABEL.match(line)
            if m:
                if cur and cur.startswith("jtbl_") and targets:
                    tables.append((seg.name, int(cur[5:], 16), targets))
                cur, targets = m[1], []
                continue
            m = WORD_LABEL.search(line)
            if m and cur:
                targets.append(int(m[1], 16))
        if cur and cur.startswith("jtbl_") and targets:
            tables.append((seg.name, int(cur[5:], 16), targets))
    return tables


def main() -> int:
    rom = recomp_rom()
    funcs, datarefs = parse_text()
    funcs.sort(key=lambda f: ([s.name for s in SEGS].index(f[0]), f[1]))
    problems = []
    by_seg = {s.name: [] for s in SEGS}
    for f in funcs:
        by_seg[f[0]].append(list(f))
    for s in SEGS:
        word = lambda v: struct.unpack(">I", rom[s.recomp_rom + v - s.vram:s.recomp_rom + v - s.vram + 4])[0]
        fl = by_seg[s.name]
        # Split functions at alternate entry points (alabel): trim each function to the next start.
        for i, f in enumerate(fl):
            if f[2] is None and i:
                p = fl[i - 1]
                end = p[1] + p[2]
                p[2] = f[1] - p[1]
                f[2] = end - f[1]
                ins = rabbitizer.Instruction(word(f[1] - 8), f[1] - 8)
                if not (ins.isJump() and not ins.isJumpWithAddress() or ins.getOpcodeName() in ("j", "b")):
                    problems.append(f"{s.name}: {f[3]} splits {p[3]} after a non-jump ({ins.disassemble()})")
        cursor, gaps = s.vram, []
        for _, v, size, n, _k in fl:
            if v < cursor:
                problems.append(f"{s.name}: overlap at {v:08X} ({n})")
            elif v > cursor and any(rom[s.recomp_rom + cursor - s.vram:s.recomp_rom + v - s.vram]):
                gaps.append((cursor, v))
            cursor = v + (size or 0)
        if cursor < s.text_end and any(rom[s.recomp_rom + cursor - s.vram:s.recomp_rom + s.text_end - s.vram]):
            gaps.append((cursor, s.text_end))
        if gaps:
            problems.append(f"{s.name}: non-zero gaps: " + ", ".join(f"{a:08X}-{b:08X}" for a, b in gaps[:10]))
        starts = {f[1] for f in fl}
        bad_jal = set()
        for v in range(s.vram, s.text_end, 4):
            w = word(v)
            if w >> 26 == 3:
                t = (v & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                if s.vram <= t < s.text_end and t not in starts:
                    bad_jal.add(t)
        if bad_jal:
            problems.append(f"{s.name}: jal targets that are not function starts: " + ", ".join(f"{a:08X}" for a in sorted(bad_jal)))
        print(f"{s.name}: {len(fl)} functions, {sum(1 for f in fl if not f[3].startswith('func_'))} named, {len(gaps)} gaps")

    tables = parse_jtbls()
    for segname, a, tg in tables:
        fl = by_seg[segname]
        starts = [f[1] for f in fl]
        i = bisect_right(starts, tg[0]) - 1
        if i < 0 or not all(fl[i][1] <= x < fl[i][1] + fl[i][2] for x in tg):
            problems.append(f"{segname}: jump table {a:08X} has targets outside one function")

    out = ROOT / "build"
    with open(out / "splat_funcs.csv", "w", newline="") as fh:
        wr = csv.writer(fh)
        wr.writerow(["section", "vram", "size", "name", "kind"])
        for s in SEGS:
            for _, v, size, n, k in by_seg[s.name]:
                wr.writerow([s.name, f"0x{v:08X}", f"0x{size:X}", n, k])
    with open(out / "splat_jtbls.csv", "w", newline="") as fh:
        wr = csv.writer(fh)
        wr.writerow(["section", "jtbl_vram", "entries", "targets"])
        for segname, a, tg in tables:
            wr.writerow([segname, f"0x{a:08X}", len(tg), ";".join(f"{x:08X}" for x in tg)])
    with open(out / "splat_datarefs.csv", "w", newline="") as fh:
        wr = csv.writer(fh)
        wr.writerow(["vram", "name", "refs"])
        for name, c in sorted(datarefs.items()):
            m = SYMADDR.search(name)
            wr.writerow([f"0x{int(m[1], 16):08X}" if m else "", name, c])
    print(f"jump tables: {len(tables)}; %hi/%lo-referenced symbols: {len(datarefs)}")
    if problems:
        print("PROBLEMS:\n  " + "\n  ".join(problems))
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
