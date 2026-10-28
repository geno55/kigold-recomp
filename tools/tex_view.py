"""Renders texture data from an RDRAM dump (KIG_RDRAM_DUMP, big-endian) to a PNG, scaled up.

Usage: python tools/tex_view.py dump.bin ADDR FMT WIDTH HEIGHT out.png [--scale N]
FMT: i8, i4, ia8, ia16, rgba16, rgba32.
"""
import argparse

from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("addr")
    ap.add_argument("fmt")
    ap.add_argument("width", type=int)
    ap.add_argument("height", type=int)
    ap.add_argument("out")
    ap.add_argument("--scale", type=int, default=4)
    a = ap.parse_args()
    ram = open(a.dump, "rb").read()
    base = int(a.addr, 16) & 0x7FFFFF
    w, h = a.width, a.height
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            if a.fmt == "i8":
                v = ram[base + y * w + x]
                c = (v, v, v, 255)
            elif a.fmt == "i4":
                b = ram[base + (y * w + x) // 2]
                v = (b >> 4 if x % 2 == 0 else b & 0xF) * 17
                c = (v, v, v, 255)
            elif a.fmt == "ia8":
                b = ram[base + y * w + x]
                c = ((b >> 4) * 17,) * 3 + ((b & 0xF) * 17,)
            elif a.fmt == "ia16":
                o = base + (y * w + x) * 2
                c = (ram[o],) * 3 + (ram[o + 1],)
            elif a.fmt == "rgba16":
                o = base + (y * w + x) * 2
                v = ram[o] << 8 | ram[o + 1]
                c = ((v >> 11) * 255 // 31, ((v >> 6) & 31) * 255 // 31, ((v >> 1) & 31) * 255 // 31, 255 if v & 1 else 0)
            else:
                o = base + (y * w + x) * 4
                c = tuple(ram[o:o + 4])
            px[x, y] = c
    bg = Image.new("RGBA", im.size, (255, 0, 255, 255))
    bg.alpha_composite(im)
    bg.convert("RGB").resize((w * a.scale, h * a.scale), Image.NEAREST).save(a.out)


if __name__ == "__main__":
    main()
