# ROM map — Killer Instinct Gold (USA Rev 2 / v1.2)

`baserom.us.z64`: 12 MB, header `NKIE` version 0x02, sha1 `aaeb492b1e538af65a3544a97240675ef438a04f`,
entry point 0x80000400. Constants live in `tools/kig_layout.py`.

## Code

| Segment | ROM (packed) | Loaded at | Size | Text ends | BSS | Loaded by |
|---|---|---|---|---|---|---|
| boot | 0x001000 (plain) | 0x80000400 | 0x6EC0 | 0x80006560 | 0x800072C0–0x8000F950 | IPL3 (the runtime's initial 1 MB copy) |
| seg0 | 0xA94D70–0xAB5860 | 0x8000F960 | 0x3A7A0 | 0x800425F0 | 0x8004A100–0x80127B20, 0x80128240–0x801735C0 | boot `func_80000520`, then `jalr 0x8000F960` |
| seg1 | 0xAB5870–0xB00DF0 | 0x801ACB20 | 0xE8B40 | 0x801CE500 | 0x80295660–0x802D3D70 | seg0 `func_800122F4` (front end) |
| seg2 | 0xB00DF0–0xB14B40 | 0x801AD900 | 0x259C0 | 0x801C3FE0 | 0x801D32C0–0x801DA0C0 | seg0 `func_8000FAA0` (fight) |

seg1 and seg2 occupy the same RAM; only one is loaded at a time.

### Compression

Every packed blob is Rare's "1172" format: the bytes `11 72` and then a raw deflate stream. The boot
stub's `func_80000900(src, dst, scratch)` skips the magic, inflates (`func_80001D10`), and returns
the output size (kept at 0x8000BF7C). Callers DMA the blob to 0x802D4000 (or 0x802D6000) first with
`func_800007B4(rom, ram, size)`.

### Segment table

A table at 0x800071E0 (ROM 0x7DE0) lists (start, end) ROM offsets. It has 27 entries:

- entries 0–2 are code (above);
- entries 3–26 are data. Entries 3+n are fighter data, inflated to 0x801DB000 (player 1) and
  0x801E5000 (player 2) by `func_80012198`;
- entry 26 repeats entry 20.

`tools/kig_segments.py --dump` writes each entry to `build/segs/`.

## RSP microcode (seg0)

| Ucode | Text | Data | Notes |
|---|---|---|---|
| rspboot | 0x800425F0 (0xD0) | | |
| F3D (SDK 2.0E build) | 0x800426C0 (0x1410) | 0x80049640 (0x800) | Byte-identical to the copy in W.G. 3D Hockey; RT64 knows its hashes |
| aspMain | 0x80043AD0 (0xE20) | 0x80049E40 | Text identical to wg3d's; recompiled by RSPRecomp (`aspMain.toml`) |

libultra is 2.0D (the string `RSP SW Version: 2.0D, 04-01-96` is at 0x800498B0), built with IDO.
It is split between boot (threads, PI, VI, exceptions) and seg0 (SI, controller, Controller Pak,
EEPROM, audio, gu).

## TLB

KI Gold uses the TLB for fighter data. `func_8001CF5C` (seg0) and seg1 call `osMapTLB` to map:

- virtual page 0 to physical 0x3A4000;
- virtual `(id << 16) + 0x10000` onwards, 5 × 8 KB, to 0x1DB000 (player 1);
- the same for player 2, to 0x1E5000.

The game code dereferences those kuseg addresses directly. `include/recomp.h` and `src/game/tlb.cpp`
emulate this mapping.

## Handwritten code

seg2 and parts of seg0 are hand-written assembly. It has:

- functions with several entry points;
- functions that run into the next one;
- jump tables without bounds checks, including tables that point into other functions;
- data islands inside functions;
- `$sp` used as a general pointer;
- direct `mfc0 Count` reads.

How this project handles it:

- `tools/kig_funcs.py` finds functions by following control flow.
- A patch to N64Recomp (`patches/n64recomp/`) covers the cases N64Recomp can't handle by itself.
- Data islands become nops through `[[patches.instruction]]` entries, which `tools/gen_config.py` writes.
