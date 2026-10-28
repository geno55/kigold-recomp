"""Lists what a frame draws, from an RDRAM dump (KIG_RDRAM_DUMP, big-endian).

Usage: python tools/dl_dump.py build/run/shots/rdram_02345_3A0000_3A0400-1A2B30.bin [--rdp] [--rects]
           [--region x0,y0,x1,y1] [--all]
The file name gives the frame's first CPU-built RDP list (start_end) and the gfx task's F3D display list.
--rdp walks the RDP list instead of the display list. --rects prints only rectangles, --region keeps the
rectangles that overlap a screen area, --all prints every command word.
"""
import argparse
import re
import struct

FMT = ["RGBA", "YUV", "CI", "IA", "I", "?5", "?6", "?7"]
SIZ = ["4b", "8b", "16b", "32b"]
# Raw RDP command lengths in bytes (triangles depend on the shade/texture/z bits).
TRI_LEN = {0x08: 32, 0x09: 48, 0x0A: 96, 0x0B: 112, 0x0C: 96, 0x0D: 112, 0x0E: 160, 0x0F: 176}


class State:
    def __init__(self, a):
        self.a = a
        self.region = [float(v) for v in a.region.split(",")] if a.region else None
        self.timg = (0, 0, 0, 0)
        self.tiles = {}
        self.last_load = None
        self.cimg = 0

    def fmt(self, f, s):
        return FMT[f] + SIZ[s]

    def common(self, at, op, w0, w1):
        """RDP commands shared by F3D display lists and raw RDP lists. Returns True if handled."""
        if op == 0xFF:
            self.cimg = w1 & 0x7FFFFF
            if not self.a.rects:
                print(f"{at:06X} SETCIMG {self.cimg:06X} {self.fmt((w0 >> 21) & 7, (w0 >> 19) & 3)} w={(w0 & 0xFFF) + 1}")
        elif op == 0xFD:
            self.timg = (w1 & 0x7FFFFF, (w0 >> 21) & 7, (w0 >> 19) & 3, (w0 & 0xFFF) + 1)
        elif op == 0xF5:
            t = (w1 >> 24) & 7
            self.tiles[t] = (f"{self.fmt((w0 >> 21) & 7, (w0 >> 19) & 3)} line={(w0 >> 9) & 0x1FF} tmem={w0 & 0x1FF} "
                             f"pal={(w1 >> 20) & 0xF}")
        elif op in (0xF3, 0xF4, 0xF0):
            name = {0xF3: "LOADBLOCK", 0xF4: "LOADTILE", 0xF0: "LOADTLUT"}[op]
            t = self.timg
            self.last_load = f"{name} {t[0]:06X} {self.fmt(t[1], t[2])} w={t[3]}"
            if not self.a.rects:
                print(f"{at:06X} {name} timg={t[0]:06X} {self.fmt(t[1], t[2])} w={t[3]} s0={(w0 >> 12) & 0xFFF} "
                      f"t0={w0 & 0xFFF} tile={(w1 >> 24) & 7} s1={(w1 >> 12) & 0xFFF} t1/dxt={w1 & 0xFFF}")
        else:
            return False
        return True

    def rect(self, at, name, xh, yh, xl, yl, extra=""):
        r = self.region
        if r is None or (xl < r[2] and xh > r[0] and yl < r[3] and yh > r[1]):
            print(f"{at:06X} [{xl:.1f},{yl:.1f})-({xh:.1f},{yh:.1f}) cimg={self.cimg:06X} {name} {extra}")

    def texrect(self, at, op, w0, w1, st, d):
        xh, yh = ((w0 >> 12) & 0xFFF) / 4, (w0 & 0xFFF) / 4
        xl, yl = ((w1 >> 12) & 0xFFF) / 4, (w1 & 0xFFF) / 4
        tile = (w1 >> 24) & 7
        s, t = ((st >> 16) ^ 0x8000) - 0x8000, ((st & 0xFFFF) ^ 0x8000) - 0x8000
        dsdx, dtdy = ((d >> 16) ^ 0x8000) - 0x8000, ((d & 0xFFFF) ^ 0x8000) - 0x8000
        self.rect(at, "TEXRECTFLIP" if op == 0xE5 else "TEXRECT", xh, yh, xl, yl,
                  f"tile={tile} st=({s / 32},{t / 32}) d=({dsdx / 1024:.3f},{dtdy / 1024:.3f}) "
                  f"[{self.tiles.get(tile)}] load=[{self.last_load}]")

    def fillrect(self, at, w0, w1):
        self.rect(at, "FILLRECT", ((w0 >> 12) & 0xFFF) / 4, (w0 & 0xFFF) / 4, ((w1 >> 12) & 0xFFF) / 4, (w1 & 0xFFF) / 4)


def walk_dl(ram, dl, st):
    w = lambda addr: struct.unpack_from(">I", ram, addr & 0x7FFFFC)[0]
    seg = [0] * 16
    resolve = lambda x: (seg[(x >> 24) & 0xF] + (x & 0xFFFFFF)) & 0x7FFFFF
    stack = []
    pc = dl
    for _ in range(300000):
        w0, w1 = w(pc), w(pc + 4)
        at = pc
        pc += 8
        op = w0 >> 24
        if st.a.all:
            print(f"{at:06X}: {w0:08X} {w1:08X}")
        if op == 0xB8:
            if not stack:
                break
            pc = stack.pop()
        elif op == 0x06:
            if ((w0 >> 16) & 0xFF) == 0:
                stack.append(pc)
            pc = resolve(w1)
        elif op == 0xBC and (w0 & 0xFF) == 0x06:
            seg[((w0 >> 8) & 0xFFFF) // 4 & 0xF] = w1 & 0x7FFFFF
        elif op in (0xFF, 0xFD):
            st.common(at, op, w0, resolve(w1))
        elif op in (0xE4, 0xE5):
            # F3D: the texrect's s,t and dsdx,dtdy follow in RDPHALF_2 / RDPHALF_CONT.
            st.texrect(at, op, w0, w1, w(pc + 4), w(pc + 12))
        elif op == 0xF6:
            st.fillrect(at, w0, w1)
        else:
            st.common(at, op, w0, w1)


def walk_rdp(ram, start, end, st):
    w = lambda addr: struct.unpack_from(">I", ram, addr & 0x7FFFFC)[0]
    pc = start
    while pc < end:
        w0, w1 = w(pc), w(pc + 4)
        op = (w0 >> 24) & 0x3F
        if st.a.all:
            print(f"{pc:06X}: {w0:08X} {w1:08X}")
        size = 8
        if op in TRI_LEN:
            size = TRI_LEN[op]
            if not st.a.rects:
                print(f"{pc:06X} TRI{op:02X}")
        elif op in (0x24, 0x25):
            size = 16
            st.texrect(pc, op | 0xC0, w0, w1, w(pc + 8), w(pc + 12))
        elif op == 0x36:
            st.fillrect(pc, w0, w1)
        else:
            st.common(pc, op | 0xC0, w0, w1)
        pc += size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--rdp", action="store_true")
    ap.add_argument("--rects", action="store_true")
    ap.add_argument("--region", default=None)
    ap.add_argument("--all", action="store_true")
    a = ap.parse_args()
    ram = open(a.dump, "rb").read()
    m = re.search(r"_([0-9A-F]{6})_([0-9A-F]{6})-([0-9A-F]{6})\.bin$", a.dump)
    rdp_start, rdp_end, dl = (int(g, 16) for g in m.groups())
    st = State(a)
    if a.rdp:
        walk_rdp(ram, rdp_start, rdp_end, st)
    else:
        walk_dl(ram, dl, st)


if __name__ == "__main__":
    main()
