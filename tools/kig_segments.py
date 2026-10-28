"""Killer Instinct Gold: list and inflate the compressed ROM segments.

The boot stub (ROM 0x1000 -> VRAM 0x80000400) keeps a table of (start, end) ROM offsets at
VRAM 0x800071E0 (ROM 0x7DE0). Each entry is a Rare "1172" blob: the two bytes 11 72 followed by a
raw deflate stream. The boot stub inflates entry 0 to 0x8000F960 and jumps there.

Usage: python tools/kig_segments.py [--dump]   (--dump writes build/segs/segNN.bin)
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROM = ROOT / "baserom.us.z64"
BOOT_ROM, BOOT_VRAM = 0x1000, 0x80000400
TABLE_VRAM = 0x800071E0
TABLE_ROM = BOOT_ROM + TABLE_VRAM - BOOT_VRAM
MAGIC = b"\x11\x72"


def rom() -> bytes:
    return ROM.read_bytes()


def inflate(data: bytes, start: int, end: int | None = None) -> tuple[bytes, int]:
    """Inflates the 1172 blob at `start`; returns (data, compressed length incl. magic)."""
    if data[start:start + 2] != MAGIC:
        raise ValueError(f"no 1172 magic at {start:08X}")
    src = data[start + 2:end if end else start + 2 + 0x400000]
    d = zlib.decompressobj(-15)
    out = d.decompress(src)
    if not d.eof:
        raise ValueError(f"truncated deflate stream at {start:08X}")
    return out, 2 + len(src) - len(d.unused_data)


def table(data: bytes) -> list[tuple[int, int]]:
    """(start, end) pairs: consecutive words until an entry is no longer a valid blob."""
    out = []
    off = TABLE_ROM
    while True:
        s, e = struct.unpack(">II", data[off:off + 8])
        if not (0x1000 <= s < e <= len(data)) or data[s:s + 2] != MAGIC:
            break
        out.append((s, e))
        off += 8
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", action="store_true")
    args = ap.parse_args()
    data = rom()
    entries = table(data)
    outdir = ROOT / "build" / "segs"
    if args.dump:
        outdir.mkdir(parents=True, exist_ok=True)
    for i, (s, e) in enumerate(entries):
        blob, clen = inflate(data, s, e)
        print(f"seg{i:02d} rom {s:08X}-{e:08X} (packed {e - s:#x}, stream {clen:#x}) -> {len(blob):#x} bytes")
        if args.dump:
            (outdir / f"seg{i:02d}.bin").write_bytes(blob)
    return 0


if __name__ == "__main__":
    sys.exit(main())
