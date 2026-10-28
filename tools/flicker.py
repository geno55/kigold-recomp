"""Find flicker in a capture series (one capture per VI).

Per pixel, a frame "flickers" when it differs strongly from both neighbours while the neighbours agree
(A-B-A). Reports frames where many pixels do that, and writes a heat map of where it happens.

Usage: python tools/flicker.py build/run/shots [--min-pixels 200]
"""
import argparse
import glob

import numpy as np
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("dir")
ap.add_argument("--min-pixels", type=int, default=200)
a = ap.parse_args()
files = sorted(glob.glob(a.dir + "/t*.bmp"))
ims = [np.asarray(Image.open(f).convert("RGB").resize((320, 240)), dtype=np.int16) for f in files]
heat = np.zeros((240, 320), dtype=np.int32)
hits = 0
for i in range(1, len(ims) - 1):
    p, c, n = ims[i - 1], ims[i], ims[i + 1]
    dpc = np.abs(p - c).max(axis=2)
    dcn = np.abs(c - n).max(axis=2)
    dpn = np.abs(p - n).max(axis=2)
    mask = (dpc > 48) & (dcn > 48) & (dpn < 16)
    k = int(mask.sum())
    heat += mask
    if k >= a.min_pixels:
        hits += 1
        ys, xs = np.nonzero(mask)
        print(f"{files[i][-14:]}: {k} flickering pixels, bbox x {xs.min()}-{xs.max()} y {ys.min()}-{ys.max()}")
print(f"checked {len(ims)} frames, {hits} with flicker")
if heat.max() > 0:
    img = (255 * heat / heat.max()).astype(np.uint8)
    Image.fromarray(img).resize((640, 480)).save(a.dir + "/flicker_heat.png")
