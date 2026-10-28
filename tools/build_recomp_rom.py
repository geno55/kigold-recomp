"""Write build/kig.recomp.z64: baserom.us.z64 with the inflated code segments appended.

N64Recomp and the analysis tools (splat, Ghidra) read code from a ROM image; KI Gold's code is
deflate-compressed, so they get this image instead. Layout: tools/kig_layout.py.

Usage: python tools/build_recomp_rom.py
"""
import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kig_layout import BASEROM, RECOMP_ROM, ROM_SIZE, SEGS  # noqa: E402
from kig_segments import inflate, table  # noqa: E402

SHA1 = "aaeb492b1e538af65a3544a97240675ef438a04f"


def build() -> bytes:
    rom = BASEROM.read_bytes()
    if hashlib.sha1(rom).hexdigest() != SHA1:
        raise SystemExit(f"{BASEROM.name}: wrong ROM (need Killer Instinct Gold USA Rev 2, sha1 {SHA1})")
    assert len(rom) == ROM_SIZE
    entries = table(rom)
    out = bytearray(rom)
    for s in SEGS:
        if s.table_index is None:
            continue
        start, end = entries[s.table_index]
        assert start == s.orig_rom, f"{s.name}: table says {start:08X}"
        data, _ = inflate(rom, start, end)
        assert len(data) == s.size, f"{s.name}: inflated {len(data):#x}, layout says {s.size:#x}"
        assert s.recomp_rom == len(out), f"{s.name}: expected at {len(out):#x}"
        out += data
        out += bytes(-len(out) % 0x10)
    return bytes(out)


def main() -> int:
    data = build()
    RECOMP_ROM.parent.mkdir(exist_ok=True)
    if not RECOMP_ROM.exists() or RECOMP_ROM.read_bytes() != data:
        RECOMP_ROM.write_bytes(data)
    for s in SEGS:
        print(f"{s.name}: rom {s.recomp_rom:08X} vram {s.vram:08X}-{s.vram + s.size:08X} text to {s.text_end:08X}")
    print(f"wrote {RECOMP_ROM.relative_to(RECOMP_ROM.parent.parent)} ({len(data):#x} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
