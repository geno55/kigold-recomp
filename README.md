# Killer Instinct Gold: Recompiled

A native Windows port of Killer Instinct Gold (N64, USA Rev 2 / v1.2), made by static recompilation
with [N64Recomp](https://github.com/N64Recomp/N64Recomp), [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime)
and the [RT64](https://github.com/rt64/rt64) renderer. You need your own copy of the ROM. Nothing from it is included here.

The project layout, runtime glue (launcher, settings, input, audio, crash handler, capture and trace
tools) and the runtime patches come from
[W.G. 3D Hockey: Recompiled](../wg3d-recomp). What is specific to KI Gold is described in
[docs/rom_map.md](docs/rom_map.md):

- **Compressed code.** The game ships deflate-compressed code segments. Two of them, seg1 and seg2,
  are overlays that share RAM.
- **TLB.** Fighter data is reached through TLB-mapped addresses.
- **Handwritten assembly.** Much of seg2 is hand-written.

## Status

Boots through the intro and title screen, and plays through character select, a full fight and the
Continue screen, with graphics, audio and input. The code recompiles completely (1919 functions in four
sections). Not yet verified: EEPROM saves, all characters and stages, and a comparison against ares.

Changes to the runtime are in `patches/runtime/` (applied at configure time). The ones added for KI Gold:

- 0006: raw RDP lists (`osDpSetNextBuffer`) to RT64
- 0007: tolerate COP0 Status writes
- 0008: `osViSetMode` clears the black flag
- 0009: present the current VI state (frames were sometimes shown out of order)
- 0010: synchronous gfx submission (the title fire texture was read mid-update)
- 0011: keep RDRAM allocated at exit (crash on quit)

The N64Recomp changes (handwritten code, VR4300 divide by zero, Count reads, branch labels) are in `patches/n64recomp/`.
Upstream emitted at most one label per instruction, so a branch target outside a function (below its
start) pushed every later label one instruction late. The fight's entity loop then skipped reloading
each slot's type, and every slot after the first empty one stopped updating: blood, hit sparks and
fireballs froze in place.
RT64 gets one patch (`patches/rt64/`, also applied at configure time) for raw RDP triangles. They are
drawn as the two trapezoids the RDP fills, not as one triangle, which matters because the fight sky is
sent as a rectangle. They are also clipped to the scissor before drawing, which fixes the flickering
CPU-built floors. On the renderer side
(`src/main/rt64_render_context.cpp`):

- frames are presented as the VI scans them out (RT64's Console mode), not a frame early;
- the CPU-built RDP list and the frame's gfx task form one RT64 frame;
- the DP interrupt waits until RT64 has written the frame back to RDRAM, because KI Gold reads its
  framebuffers with the CPU;
- the viewport carries over from one gfx task to the next, as it does in the RSP's DMEM. Near the end of
  the victory pose KI Gold stops loading its viewport; RT64's per-task reset (an all-zero viewport) then
  made the whole stage vanish, leaving a black floor.

## ROM

`baserom.us.z64`: Killer Instinct Gold (USA) (Rev 2), sha1 `aaeb492b1e538af65a3544a97240675ef438a04f`
(big-endian .z64; the launcher also accepts .v64/.n64).

## Building

Requirements: VS 2022 Build Tools (clang-cl, Windows SDK), CMake ≥ 3.20, Ninja, Python 3.12 with
`rabbitizer`, `xxhash`, `splat64[mips]` and `spimdisasm`. Run the commands below from PowerShell or cmd, not Git Bash.

```
git submodule update --init --recursive
git -C lib/N64Recomp apply ../../patches/n64recomp/0001-handwritten-code.patch
cmake -S lib/N64Recomp -B lib/N64Recomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build lib/N64Recomp/build --target N64RecompCLI RSPRecomp
tools\regen_syms.ps1          # syms + kig.us.toml + RecompiledFuncs/ (needs baserom.us.z64)
tools\cmake_build.bat kig     # -> build/cmake/kig.exe
```

The last two steps need the vcvars environment. `tools\cmake_build.bat` sets it up, and so does a VS developer prompt.

### Pipeline

| Step | Tool | Output |
|---|---|---|
| Inflate the code segments, append them to a copy of the ROM | `tools/build_recomp_rom.py` | `build/kig.recomp.z64` |
| Disassemble (analysis only) | `python -m splat split splat/kig.us.yaml` | `build/splat/` |
| Function starts and sizes | `tools/export_splat.py`, `tools/kig_funcs.py` | `build/splat_funcs.csv`, `build/kig_funcs.csv` |
| Library names (matched against wg3d's libultra 2.0D) | `tools/kig_libsyms.py` | `syms/libultra_symbols.txt` |
| N64Recomp symbols | `tools/gen_syms.py` | `syms/kig.us.syms.toml` |
| Config, recompile, skipped-function declarations | `tools/gen_config.py` | `kig.us.toml`, `RecompiledFuncs/`, `include/kig_skipped.h`, `include/kig_segments.h` |

Hand decisions live in `syms/overrides.toml`, `syms/libnames_manual.toml` and `syms/dead_ignore.txt`.

## Running

`build/cmake/kig.exe [--frames N] [--speed N] [--mute] [--hidden] [--data-dir DIR] [--console] [rom]`. The
first run asks for the ROM, or takes it on the command line. Settings and saves live in `%APPDATA%\KIGRecomp`.
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

## License

GPL-3.0 (see LICENSE). Some runtime glue is adapted from Quest64-Recomp, by way of wg3d-recomp. The
game's code and data belong to their owners.
