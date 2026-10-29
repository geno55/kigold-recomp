# Technical notes

Details moved out of the README. For the ROM layout itself see [rom_map.md](rom_map.md).

The project layout, runtime glue (launcher, settings, input, audio, crash handler, capture and trace
tools) and the runtime patches come from W.G. 3D Hockey: Recompiled. What is specific to KI Gold:

- **Compressed code.** The game ships deflate-compressed code segments. Two of them, seg1 and seg2,
  are overlays that share RAM.
- **TLB.** Fighter data is reached through TLB-mapped addresses.
- **Handwritten assembly.** Much of seg2 is hand-written.

The code recompiles completely (1919 functions in four sections).

## Patches

Changes to the runtime are in `patches/runtime/` (applied at configure time). The ones added for KI Gold:

- 0006: raw RDP lists (`osDpSetNextBuffer`) to RT64
- 0007: tolerate COP0 Status writes
- 0008: `osViSetMode` clears the black flag
- 0009: present the current VI state (frames were sometimes shown out of order)
- 0010: synchronous gfx submission (the title fire texture was read mid-update)
- 0011: keep RDRAM allocated at exit (crash on quit)

The N64Recomp changes (handwritten code, VR4300 divide by zero, Count reads, branch labels, `$ra` on
calls) are in `patches/n64recomp/`.
Upstream emitted at most one label per instruction, so a branch target outside a function (below its
start) pushed every later label one instruction late. The fight's entity loop then skipped reloading
each slot's type, and every slot after the first empty one stopped updating: blood, hit sparks and
fireballs froze in place.

Upstream also compiled `jal`/`jalr` to plain C calls without setting `$ra`, so code that saves `$ra`
saved whatever an earlier return left there. The fight's per-frame routine can exit early and later
restore `$fp` from a stack slot that seg0's drawing code had reused for its saved `$ra`; the game then
stores through `$fp`. On hardware that slot holds a return address into code, and the stray store
changes nothing visible. Here it held the stale 0x800495A0, the static viewport display list, which the
store blanked for good: the stage vanished late in the victory pose (a black floor), and in the next
fight the fireplace flames were drawn beside the fireplace. Calls now set `$ra` as the hardware does.

RT64 gets one patch (`patches/rt64/`, also applied at configure time) for raw RDP triangles. They are
drawn as the two trapezoids the RDP fills, not as one triangle, which matters because the fight sky is
sent as a rectangle. They are also clipped to the scissor before drawing, which fixes the flickering
CPU-built floors. On the renderer side (`src/main/rt64_render_context.cpp`):

- frames are presented as the VI scans them out (RT64's Console mode), not a frame early;
- the CPU-built RDP list and the frame's gfx task form one RT64 frame;
- the DP interrupt waits until RT64 has written the frame back to RDRAM, because KI Gold reads its
  framebuffers with the CPU.

## Build pipeline

| Step | Tool | Output |
|---|---|---|
| Inflate the code segments, append them to a copy of the ROM | `tools/build_recomp_rom.py` | `build/kig.recomp.z64` |
| Disassemble (analysis only) | `python -m splat split splat/kig.us.yaml` | `build/splat/` |
| Function starts and sizes | `tools/export_splat.py`, `tools/kig_funcs.py` | `build/splat_funcs.csv`, `build/kig_funcs.csv` |
| Library names (matched against wg3d's libultra 2.0D) | `tools/kig_libsyms.py` | `syms/libultra_symbols.txt` |
| N64Recomp symbols | `tools/gen_syms.py` | `syms/kig.us.syms.toml` |
| Config, recompile, skipped-function declarations | `tools/gen_config.py` | `kig.us.toml`, `RecompiledFuncs/`, `include/kig_skipped.h`, `include/kig_segments.h` |

Hand decisions live in `syms/overrides.toml`, `syms/libnames_manual.toml` and `syms/dead_ignore.txt`.

## Command-line options

`build/cmake/kig.exe [--frames N] [--speed N] [--mute] [--hidden] [--data-dir DIR] [--console] [rom]`.
`--mute` silences the output without changing audio timing; `--hidden` never shows the window and ignores
gamepads (background test runs).

## Differential testing against a reference emulator

`tools/refemu.py` runs the game on the parallel_n64 libretro core (from a RetroArch install, `--core`
elsewhere) through ctypes, headless and unthrottled: the angrylion software RDP and the cxd4 LLE RSP, so no
GPU context. mupen64plus_next was tried too; with angrylion it crashes while loading the ROM, and its
default renderer needs GL.

A scenario's `steps` (tests/scenarios/*.toml) drive both sides with the same script, synced on game state
(`WAIT_RAM`) instead of times, since gameplay drifts from run to run. The language is documented in
`tools/refemu.py`; kig.exe runs it with `KIG_STEPS` (src/game/steps.cpp), one step tick per VI on both
sides. `WRITE_RAM` pins values at a synced point (KIG_POKE's job), `DUMP_RAM` writes all of RDRAM
big-endian, and `SAVE_STATE`/`LOAD_STATE` (reference only) jump straight to any point. Saves are portable:
`--eeprom-in/--eeprom-out` use kig's `saves/kig.us.bin`, and `--pak-in/--pak-out` kig's Controller Pak
folder (`tools/pak_convert.py` converts to and from a 32 KB pak image).

```powershell
python tools\run_scenario.py quick --target kig+ref   # both sides, then the comparison
python tools\refemu.py --calibrate                    # check the input mapping against the game's pad buffer
```

`tools/refdiff.py` compares screenshots with a tolerance (RT64 isn't bit-exact with the RDP: a channel
tolerance, one pixel of slack, ignore rectangles for random animation) and RAM checkpoints exactly over the
scenario's `ram_compare` ranges (game state: the code segments' data and BSS). The report lists the VI at
which each synced step ran on both sides.
