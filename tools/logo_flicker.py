"""Count title-screen frames where the KI logo is missing, in a recording (tools/record.ps1).

The logo's purple (R and B well above G) covers a few percent of the screen on every title frame; a frame
without it, between frames that have it, is the flicker.

Usage: python tools/logo_flicker.py build/rec/<name>
"""
import glob
import sys

import numpy as np
from PIL import Image

files = sorted(glob.glob(sys.argv[1] + "_*.png"))
purple = []
for f in files:
    im = np.asarray(Image.open(f).convert("RGB"), dtype=np.int16)
    r, g, b = im[..., 0], im[..., 1], im[..., 2]
    purple.append(float(((r > 50) & (b > 70) & (g < 0.6 * r) & (g < 0.6 * b)).mean()))
title = [i for i, p in enumerate(purple) if p > 0.02]
missing = [i for i in range(1, len(purple) - 1)
           if purple[i] < 0.3 * min(purple[i - 1], purple[i + 1]) and min(purple[i - 1], purple[i + 1]) > 0.02]
print(f"{len(files)} frames, {len(title)} with the logo, {len(missing)} with it missing between logo frames")
print("missing at:", missing[:40])
