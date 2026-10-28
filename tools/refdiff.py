#!/usr/bin/env python3
"""refdiff.py - compare kig.exe's outputs with the reference emulator's (tools/refemu.py).

Screenshots: RT64 is not bit-exact with the RDP (filtering, dithering, the VI's output scaling), and kig's
capture is the window, so both images are scaled to the N64's 320x240 and compared with a tolerance: a
pixel differs when a channel is off by more than `tol` after a small blur from every reference pixel
within `slack` pixels, and the pair matches when fewer than `max_frac` of the pixels differ. Regions of
random animation can be left out (`ignore` rectangles). The report gives the differing fraction and bounding box,
and a diff image (differing pixels in magenta over the reference) is written next to the kig image.

RAM: DUMP_RAM files (8 MB, big-endian) compare exactly, over the given ranges; a block map of where
they differ (64 KB blocks) helps choose ranges that hold game state rather than buffers.

Usage:
    python tools/refdiff.py img kig.png ref.png [diff.png] [--tol 40] [--max-frac 0.03] [--slack 1]
                            [--ignore x0,y0,x1,y1 ...]
    python tools/refdiff.py ram kig.bin ref.bin [--range 8000B800-8000C000 ...] [--blocks]
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

N64_SIZE = (320, 240)


def load_n64(path, crop_4_3=True):
    img = Image.open(path).convert("RGB")
    if crop_4_3:
        # kig's window can be wider or taller than 4:3 (RT64 letterboxes/pillarboxes the N64 image).
        w, h = img.size
        if w * 3 > h * 4:
            nw = h * 4 // 3
            img = img.crop(((w - nw) // 2, 0, (w - nw) // 2 + nw, h))
        elif w * 3 < h * 4:
            nh = w * 3 // 4
            img = img.crop((0, (h - nh) // 2, w, (h - nh) // 2 + nh))
    return img.resize(N64_SIZE, Image.BOX).filter(ImageFilter.BoxBlur(1))


def compare_images(kig, ref, diff_path=None, tol=40, max_frac=0.03, slack=1, ignore=()):
    """(ok, summary): see the module doc. A kig pixel matches when a reference pixel within `slack` pixels
    is within `tol` (RT64 and the RDP can place edges a pixel apart); `ignore` = [(x0, y0, x1, y1)] in
    320x240 coordinates, left out (random animation such as the menus' fire)."""
    a = np.asarray(load_n64(kig), dtype=np.int16)
    b = np.asarray(load_n64(ref), dtype=np.int16)
    best = None
    for dy in range(-slack, slack + 1):
        for dx in range(-slack, slack + 1):
            d = np.abs(a - np.roll(b, (dy, dx), axis=(0, 1))).max(axis=2)
            best = d if best is None else np.minimum(best, d)
    bad = best > tol
    for x0, y0, x1, y1 in ignore:
        bad[y0:y1, x0:x1] = False
    frac = float(bad.mean())
    ok = frac <= max_frac
    if bad.any():
        ys, xs = np.where(bad)
        box = f", bbox x {xs.min()}-{xs.max()} y {ys.min()}-{ys.max()}"
    else:
        box = ""
    if diff_path is not None:
        out = np.asarray(Image.open(ref).convert("RGB").resize(N64_SIZE, Image.BOX)).copy()
        out[bad] = (255, 0, 255)
        for x0, y0, x1, y1 in ignore:
            out[y0:y1, x0:x1] //= 3  # ignored: dimmed
        Image.fromarray(out).save(diff_path)
    return ok, f"{frac * 100:.1f}% of pixels differ (tol {tol}, slack {slack}px){box}"


def parse_range(r):
    lo, hi = (int(x, 16) & 0x1FFFFFFF for x in r.split("-"))
    return lo, hi


def compare_ram(kig, ref, ranges=None, blocks=False, ignore=()):
    """(ok, summary lines): exact comparison over ranges (default all of RDRAM), minus `ignore` ranges."""
    a = np.fromfile(kig, dtype=np.uint8)
    b = np.fromfile(ref, dtype=np.uint8).copy()
    n = min(len(a), len(b))
    for lo, hi in ignore:
        b[lo:hi] = a[lo:hi]
    ranges = ranges or [(0, n)]
    lines, total = [], 0
    for lo, hi in ranges:
        d = np.where(a[lo:hi] != b[lo:hi])[0]
        total += len(d)
        if len(d):
            shown = " ".join(f"{0x80000000 + lo + x:08X}:{a[lo + x]:02X}/{b[lo + x]:02X}" for x in d[:16])
            lines.append(f"  {0x80000000 + lo:08X}-{0x80000000 + hi:08X}: {len(d)} bytes differ (kig/ref): {shown}"
                         + (" ..." if len(d) > 16 else ""))
        else:
            lines.append(f"  {0x80000000 + lo:08X}-{0x80000000 + hi:08X}: identical")
    if blocks:
        size = 0x10000
        for blk in range(0, n, size):
            cnt = int((a[blk:blk + size] != b[blk:blk + size]).sum())
            if cnt:
                lines.append(f"    block {0x80000000 + blk:08X}: {cnt} bytes differ")
    return total == 0, lines


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    pi = sub.add_parser("img")
    pi.add_argument("kig")
    pi.add_argument("ref")
    pi.add_argument("diff", nargs="?")
    pi.add_argument("--tol", type=int, default=40)
    pi.add_argument("--max-frac", type=float, default=0.03)
    pi.add_argument("--slack", type=int, default=1)
    pi.add_argument("--ignore", action="append", default=[], metavar="X0,Y0,X1,Y1")
    pr = sub.add_parser("ram")
    pr.add_argument("kig")
    pr.add_argument("ref")
    pr.add_argument("--range", action="append", default=[])
    pr.add_argument("--blocks", action="store_true")
    a = ap.parse_args()
    if a.cmd == "img":
        ok, msg = compare_images(a.kig, a.ref, a.diff, a.tol, a.max_frac, a.slack,
                                 [tuple(int(v) for v in r.split(",")) for r in a.ignore])
        print(("match: " if ok else "DIFFER: ") + msg)
    else:
        ok, lines = compare_ram(a.kig, a.ref, [parse_range(r) for r in a.range] or None, a.blocks)
        print("identical" if ok else "DIFFER")
        print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
