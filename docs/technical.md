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
- 0012: save states (see below)

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

That early exit had a second effect, which crashed the game at the end of matches (issue 7, often in
Team Elimination's winning pose). The fight's per-frame routine, seg2 0x801AE460, saves every register in
its frame and its `$sp` at 0x801D9FF8. The entity loop it calls can end the frame by jumping (via
0x801BDA58) to the routine's exit at 0x801AE6B4, which restores the registers and returns with `jr $ra`;
on the N64 that leaves the routine. Recompiled, those jumps are tail calls, so the return went back into
0x801AE460 after its call to the entity loop. The rest of the frame then ran on its caller's stack,
over its own register save area, and its epilogue restored garbage, including a misaligned `$sp`. The
next frame's input code, func_80011824, then read a controller pointer through it and crashed (a crash
reporter's save state reproduced this). The issue's other crashes, through a bad display list pointer,
fit the same garbage registers but weren't reproduced. Now the routine runs under setjmp and the exit longjmps out
(`kig_fight_frame` in `src/game/kig_hooks.cpp`), the same way the fighter state restarts are handled.

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
| N64Recomp symbols | `tools/gen_syms.py` | `syms/kig.us.syms.toml` |
| Config, recompile, skipped-function declarations | `tools/gen_config.py` | `kig.us.toml`, `RecompiledFuncs/`, `include/kig_skipped.h`, `include/kig_segments.h` |

Hand decisions live in `syms/overrides.toml`, `syms/libultra_symbols.txt` and `syms/dead_ignore.txt`.

## Command-line options

`build/cmake/kig.exe [--frames N] [--speed N] [--mute] [--hidden] [--data-dir DIR] [--console]
[--load-state FILE] [rom]`. `--mute` silences the output without changing audio timing; `--hidden` never
shows the window and ignores gamepads (background test runs); `--load-state` loads a save state as soon as
the game is running.

## Save states

F5 saves the quick-save state (`states/quicksave.kigstate` in the app folder) and F7 loads it; step
scripts use `SAVE_STATE <file> [<addr> <8|16|32> <value>]` and `LOAD_STATE <file>`. The code is in
`src/game/savestate.cpp`, with N64Recomp patch 0002 and runtime patch 0012.

The difficulty is that each game thread runs as native code, so where a thread is lives in the host's
call stack. Three things make it possible:

- **Shadow call stack.** Every recompiled call site pushes its address (and, for an indirect call, the
  callee) onto a per-thread shadow stack (`RECOMP_SS_PUSH`/`POP`, `include/recomp.h`).
- **Resume dispatch.** Every function starts with a check that, while its thread is being restored,
  jumps to a dispatch at its end. The dispatch goes back into the call the function was in.
- **Safe point.** ultramodern runs one game thread at a time, so at the top of the idle thread's loop
  every other thread is blocked in the scheduler. A request waits for a moment when every other thread
  is blocked in a message queue wait and no RSP task is running. A thread blocked in a queue can be
  resumed by making the same OS call again: it rechecks the queue, as its wait loop does.

To load, the idle thread ends every other game thread, then restores everything below and starts each
saved thread again. The thread rebuilds its chain from its entry function down to the blocked OS call
and waits there until the scheduler runs it.

A state holds:

- RDRAM;
- each thread's `recomp_context` and shadow stack;
- the scheduler's running queue, event registrations and VI state;
- the TLB;
- the loaded code segments (librecomp's sections and kig's overlay list);
- the seeded Count sequence.

A state taken at a frame counter and reloaded in a fresh process reproduces the same game state at
later frames. The only differences are wall-clock values such as the timestamp at 0x80066B78.

Not covered: the audio already queued to the sound device, and RT64's GPU copies of framebuffers (the
next frame redraws them). States depend on the recompiled code's call sites, so they only load in the
build that wrote them.

## Differential testing against a reference emulator

`tools/refemu.py` runs the game on the parallel_n64 libretro core (from a RetroArch install, `--core`
elsewhere) through ctypes, headless and unthrottled: the angrylion software RDP and the cxd4 LLE RSP, so no
GPU context. mupen64plus_next was tried too; with angrylion it crashes while loading the ROM, and its
default renderer needs GL.

A scenario's `steps` (tests/scenarios/*.toml) drive both sides with the same script, synced on game state
(`WAIT_RAM`) instead of times, since gameplay drifts from run to run. The language is documented in
`tools/refemu.py`; kig.exe runs it with `KIG_STEPS` (src/game/steps.cpp), one step tick per VI on both
sides. `WRITE_RAM` pins values at a synced point (KIG_POKE's job), `DUMP_RAM` writes all of RDRAM
big-endian, and `SAVE_STATE`/`LOAD_STATE` jump straight to any point (core snapshots on the reference,
kig save states on kig; the two kinds of file don't mix). Saves are portable:
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
