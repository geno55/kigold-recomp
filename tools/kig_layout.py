"""Killer Instinct Gold (USA Rev 2 / v1.2): code layout shared by the tools.

Code lives in four places (docs/rom_map.md):
  boot  uncompressed, ROM 0x1000 -> 0x80000400 (loaded by the runtime like IPL3 would)
  seg0  1172 blob, segment table entry 0, inflated by boot to 0x8000F960 (static from then on)
  seg1  entry 1, inflated by func_800122F4 to 0x801ACB20 (front end; overlay)
  seg2  entry 2, inflated by func_8000FAA0 to 0x801AD900 (fight; overlay, overlaps seg1)
The other entries (3-26) are data.

N64Recomp reads code from a ROM file, so tools/build_recomp_rom.py writes build/kig.recomp.z64: the
original ROM with seg0-seg2 appended, inflated, at RECOMP_ROM offsets. Only N64Recomp and the analysis
tools use that file; the game itself runs from the real ROM, and src/game/kig_hooks.cpp maps each
inflated segment to its section (by original ROM offset) when the game inflates it.
"""
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASEROM = ROOT / "baserom.us.z64"
RECOMP_ROM = ROOT / "build" / "kig.recomp.z64"
ROM_SIZE = 0xC00000


@dataclass(frozen=True)
class Seg:
    name: str
    table_index: int | None   # segment table entry (None: uncompressed boot code)
    orig_rom: int             # where the (packed) bytes are in baserom.us.z64
    recomp_rom: int           # where the inflated bytes are in build/kig.recomp.z64
    vram: int
    size: int                 # inflated size (text + data)
    text_end: int             # end of CPU code; RSP ucode / data follow
    bss_end: int | None = None


SEGS = [
    Seg("boot", None, 0x1000, 0x1000, 0x80000400, 0x6EC0, 0x80006560, 0x8000F950),
    Seg("seg0", 0, 0xA94D70, 0xC00000, 0x8000F960, 0x3A7A0, 0x800425F0, 0x801735C0),
    Seg("seg1", 1, 0xAB5870, 0xC3A7A0, 0x801ACB20, 0xE8B40, 0x801CE500, 0x802D3D70),
    Seg("seg2", 2, 0xB00DF0, 0xD232E0, 0x801AD900, 0x259C0, 0x801C3FE0, 0x801DA0C0),
]
BY_NAME = {s.name: s for s in SEGS}
ENTRYPOINT = 0x80000400

# RSP microcode in seg0 (tools/identify_ucode.py)
RSPBOOT_TEXT = 0x800425F0
F3D_TEXT, F3D_DATA = 0x800426C0, 0x80049640
ASPMAIN_TEXT, ASPMAIN_TEXT_SIZE, ASPMAIN_DATA = 0x80043AD0, 0xE20, 0x80049E40


def seg_of(vram: int, prefer: str | None = None) -> Seg | None:
    hits = [s for s in SEGS if s.vram <= vram < s.vram + s.size]
    if prefer:
        for s in hits:
            if s.name == prefer:
                return s
    return hits[0] if hits else None


def recomp_rom() -> bytes:
    return RECOMP_ROM.read_bytes()


def seg_bytes(rom: bytes, s: Seg) -> bytes:
    return rom[s.recomp_rom:s.recomp_rom + s.size]
