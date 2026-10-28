"""Generate the N64Recomp config (kig.us.toml) and run the recompiler.

Inputs come from the other tools' outputs, so nothing is hand-copied:
  syms/kig.us.syms.toml   function list (tools/gen_syms.py), rom offsets into build/kig.recomp.z64
  syms/dead_ignore.txt    dead library code to ignore (functions only reachable through skipped ones)
  HOOKS below             [[patches.hook]] entries (src/game/kig_hooks.cpp)

Functions that start inside a function N64Recomp skips (pieces of libultra's handwritten exception and
thread code that the function finder split off) are ignored too.

After recompiling it writes include/kig_skipped.h: declarations for the skipped functions the generated
code calls that funcs.h doesn't declare (N64Recomp declares reimplemented functions only). Each must be
an N64Recomp-skipped function; anything else is an error.

Usage: python tools/gen_config.py [--trace] [--no-run] [--no-check]
  --trace     enable N64Recomp trace_mode (adds trace.h calls to every function); writes
              kig.us.trace.toml -> RecompiledFuncsTrace/ instead of the normal config/output
  --no-run    only write the config
  --no-check  skip the clang-cl syntax check of the generated C
"""
import argparse
import csv
import re
import subprocess
import sys
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_recomp_rom import main as build_recomp_rom  # noqa: E402
from kig_layout import ENTRYPOINT, SEGS  # noqa: E402
from recomp_lists import list_from_cpp, runtime_funcs, skipped_funcs  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
CONFIG = ROOT / "kig.us.toml"
OUTPUT_DIR = "RecompiledFuncs"
DECLS_HEADER = ROOT / "include" / "kig_skipped.h"
# Skipped functions with live callers that the runtime doesn't implement; this project does (src/game/).
RUNTIME_GAPS = {"osPiRawReadIo", "osMapTLB", "osUnmapTLB", "__osProbeTLB", "__osDpDeviceBusy"}  # pi.cpp, tlb.cpp, kig_hooks.cpp
VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
# Generated-code noise only (same classes the reference projects silence). Implicit declarations stay on.
NOISE_FLAGS = "-Wno-unused-variable -Wno-unused-label -Wno-unused-but-set-variable"
# N64Recomp output lines that are expected (prefixes).
EXPECTED_PREFIXES = ("Function count:", "Working dir:", "[Info] Fall-through tail call", "Tail call in",
                     "[Info] Tail call in", "[Info] Indirect tail call in", "[Info] Ambiguous jal target", "[Info] Jump table at")

# [[patches.hook]] entries: (function, before_vram, text). Boot's inflater func_80000900(src, dst, scratch)
# stores its arguments, calls the inflate loop (jal at 0x8000093C) and returns the byte count it left at
# 0x8000BF7C; the hooks around that call register inflated code segments (src/game/kig_hooks.cpp).
HOOKS = [
    ("func_80000900", 0x80000900, "kig_inflate_begin(rdram, ctx); /* kig: record src/dst */"),
    ("func_80000900", 0x80000944, "kig_inflate_end(rdram, ctx); /* kig: register code segments */"),
    # osDpSetNextBuffer (renamed in syms/overrides.toml): replaced, the original body never runs.
    ("kig_osDpSetNextBuffer", 0x80003BB0, "kig_dp_set_next_buffer(rdram, ctx); return; /* kig: raw RDP list to RT64 */"),
    # Song change (func_8002A790): after alSeqpStop it spins until the audio thread has stopped the music
    # player, up to 4,000,000 iterations (s0), about 0.6s on the N64. Natively that is a few ms with no
    # preemption, so it timed out, the new song was never started and the title screen stayed silent.
    # Yield each iteration and count each yield as ~1ms of N64 iterations, keeping the timeout.
    ("func_8002A790", 0x8002A818, "kig_spin_yield(rdram); ctx->r16 += 6000; /* kig: let the audio thread stop the song */"),
    # Fighter state restarts (src/game/kig_hooks.cpp): jal'd routines such as the animation-script
    # interpreter (0x801BCA80) can jump to 0x801B20F4 and back into the entity loop instead of returning.
    # The entity loop's dispatch runs the state code under setjmp; the restart jumps (their delay slots
    # done first) longjmp back to it. Without this the abandoned frames resumed after the whole frame,
    # with $fp one past the object array (writes past it; unmapped TLB read crash in 0x801B2144).
    ("seg2_801AFC94", 0x801AFD64, "kig_dispatch_entity(rdram, ctx); return; /* kig: entity dispatch */"),
    ("seg2_801AFD44", 0x801AFD64, "kig_dispatch_entity(rdram, ctx); return; /* kig: entity dispatch */"),
    ("seg2_801BE720", 0x801BE74C, "ctx->r6 = ADD32(0, 0X22); kig_restart_state(rdram, ctx); return; /* kig: state restart */"),
    ("seg2_801BE784", 0x801BE790, "MEM_B(0X8C, ctx->r30) = ctx->r7; kig_restart_state(rdram, ctx); return; /* kig: state restart */"),]
INFLATE_OUTCNT = 0x8000BF7C
SEGMENTS_HEADER = ROOT / "include" / "kig_segments.h"


def write_segments_header() -> None:
    rows = "".join(f'    {{ "{s.name}", 0x{s.orig_rom:X}, 0x{s.recomp_rom:X}, 0x{s.vram:08X}, 0x{s.size:X}, '
                   f'{"true" if s.name in ("boot", "seg0") else "false"} }},\n'
                   for s in SEGS if s.table_index is not None)
    text = f"""// Generated by tools/gen_config.py from tools/kig_layout.py; do not edit.
// The compressed code segments: packed ROM offset, section rom address in build/kig.recomp.z64 (what
// librecomp's section table uses), load address, inflated size, and whether it stays loaded.
#ifndef KIG_SEGMENTS_H
#define KIG_SEGMENTS_H

#include <cstdint>

namespace kig {{
    struct CodeSegment {{
        const char* name;
        uint32_t orig_rom;
        uint32_t recomp_rom;
        uint32_t vram;
        uint32_t size;
        bool is_static;
    }};
    inline constexpr CodeSegment code_segments[] = {{
{rows}    }};
    // func_80000900's output byte counter.
    inline constexpr uint32_t INFLATE_OUTCNT = 0x{INFLATE_OUTCNT:08X};
}}

#endif
"""
    if not SEGMENTS_HEADER.exists() or SEGMENTS_HEADER.read_text() != text:
        SEGMENTS_HEADER.write_text(text)


def toml_list(items) -> str:
    if not items:
        return "[]"
    return "[\n" + "".join(f'    "{i}",\n' for i in items) + "]"


def inside_skipped() -> list[str]:
    syms = tomllib.loads((ROOT / "syms" / "kig.us.syms.toml").read_text(encoding="utf-8"))
    skipped = skipped_funcs()
    out = []
    for sec in syms["section"]:
        fl = sec["functions"]
        for f in fl:
            if f["name"] not in skipped:
                continue
            for g in fl:
                if g is not f and f["vram"] < g["vram"] < f["vram"] + f["size"] and g["name"] not in skipped:
                    out.append(g["name"])
    return sorted(set(out))


def write_config(trace: bool) -> None:
    dead = ROOT / "syms" / "dead_ignore.txt"
    ov = tomllib.loads((ROOT / "syms" / "overrides.toml").read_text(encoding="utf-8"))
    manual = [e["name"] for e in ov.get("ignore", [])]
    ignored = set((dead.read_text().split() if dead.exists() else []) + inside_skipped() + manual)
    present = {f["name"] for sec in tomllib.loads((ROOT / "syms" / "kig.us.syms.toml").read_text(encoding="utf-8"))["section"]
               for f in sec["functions"]}
    missing = sorted(ignored - present)
    if missing:
        print("note: ignored names not in the symbols file: " + ", ".join(missing))
    ignored = sorted((ignored & present) - skipped_funcs())
    hook_text = "".join(f'''
[[patches.hook]]
func = "{func}"
before_vram = 0x{vram:08X}
text = "{text}"
''' for func, vram, text in HOOKS)
    # Data islands inside handwritten functions (tools/kig_funcs.py): never executed, but N64Recomp
    # translates every word of a function, so they become nops.
    syms = tomllib.loads((ROOT / "syms" / "kig.us.syms.toml").read_text(encoding="utf-8"))
    fname = {(sec["name"], f["vram"]): f["name"] for sec in syms["section"] for f in sec["functions"]}
    nops = []
    with open(ROOT / "build" / "kig_dead_words.csv") as fh:
        for r in csv.DictReader(fh):
            name = fname[(r["section"], int(r["func_vram"], 16))]
            if name not in ignored:
                nops.append((name, int(r["vram"], 16)))
    hook_text += "".join(f'''
[[patches.instruction]]
func = "{func}"
vram = 0x{vram:08X}
value = 0x00000000
''' for func, vram in nops)
    text = f"""# N64Recomp config for Killer Instinct Gold (US Rev 2). Generated by tools/gen_config.py; do not edit.
# Regenerate after changing syms, the dead-code list or the hooks.

[input]
entrypoint = 0x{ENTRYPOINT:08X}
symbols_file_path = "syms/kig.us.syms.toml"
# The original ROM with the inflated code segments appended (tools/build_recomp_rom.py).
rom_file_path = "build/kig.recomp.z64"
output_func_path = "{OUTPUT_DIR}"
# kig_skipped.h declares skipped functions that funcs.h doesn't (generated; include/).
# kig_patches.h declares runtime functions used by the hooks below (hand-written; include/).
recomp_include = "#include \\"recomp.h\\"\\n#include \\"kig_skipped.h\\"\\n#include \\"kig_patches.h\\""
uses_mips3_float_mode = false
trace_mode = {'true' if trace else 'false'}

[patches]
# Dead library code that calls skipped functions with no runtime implementation, plus pieces of skipped
# functions (exception/thread code) that the function finder split off.
ignored = {toml_list(ignored)}

stubs = []
{hook_text}"""
    CONFIG.write_text(text, encoding="utf-8")


def run_recompiler() -> int:
    out = ROOT / OUTPUT_DIR
    if out.exists():
        for f in out.glob("*"):
            f.unlink()
    r = subprocess.run([str(ROOT / "lib/N64Recomp/build/N64Recomp.exe"), CONFIG.name], cwd=ROOT,
                       capture_output=True, text=True)
    log = r.stdout + r.stderr
    (ROOT / "build").mkdir(exist_ok=True)
    (ROOT / "build" / "n64recomp.log").write_text(log)
    unexpected = [l.strip() for l in log.splitlines() if l.strip() and not l.strip().startswith(EXPECTED_PREFIXES)]
    count = next((l for l in log.splitlines() if l.startswith("Function count:")), "")
    files = sorted(out.glob("*.c"))
    print(f"N64Recomp exit {r.returncode}; {count}; {len(files)} C files in {OUTPUT_DIR}/")
    for l in unexpected[:60]:
        print(f"  unexpected: {l}")
    if len(unexpected) > 60:
        print(f"  ... {len(unexpected) - 60} more (build/n64recomp.log)")
    return 0 if r.returncode == 0 and not unexpected else 1


def write_decls() -> int:
    out = ROOT / OUTPUT_DIR
    declared = set(re.findall(r"^void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\);", (out / "funcs.h").read_text(), re.M))
    called, defined = set(), set()
    for f in out.glob("funcs_*.c"):
        text = f.read_text()
        called |= set(re.findall(r"\b(\w+)\(rdram, ctx\)", text))
        defined |= set(re.findall(r"^RECOMP_FUNC void (\w+)\(", text, re.M))
    patch_decls = set(re.findall(r"^void (\w+)\(", (ROOT / "include" / "kig_patches.h").read_text(), re.M))
    missing = sorted(called - declared - defined - patch_decls - {"LOOKUP_FUNC"})
    sl = ROOT / "lib/N64Recomp/src/symbol_lists.cpp"
    skipped = list_from_cpp(sl, "reimplemented_funcs") | list_from_cpp(sl, "ignored_funcs")
    runtime = runtime_funcs()
    errors = []
    lines = ["// Generated by tools/gen_config.py; do not edit.",
             "// Declarations for N64Recomp-skipped functions that the generated code calls but funcs.h",
             "// does not declare. Each is implemented by the runtime (librecomp/ultramodern) or, for the",
             "// runtime gaps, by this project.",
             "#ifndef KIG_SKIPPED_H", "#define KIG_SKIPPED_H", "", "#ifdef __cplusplus", 'extern "C" {', "#endif", ""]
    for name in missing:
        base = name[:-len("_recomp")] if name.endswith("_recomp") else None
        if base is None or base not in skipped:
            errors.append(f"generated code calls undeclared {name}, which is not an N64Recomp-skipped function")
            continue
        if base in runtime:
            note = "runtime"
        elif base in RUNTIME_GAPS:
            note = "not in the runtime; implemented in src/game/"
        else:
            errors.append(f"{name}: skipped, called, but neither in the runtime nor a known runtime gap")
            note = "MISSING"
        lines.append(f"void {name}(uint8_t* rdram, recomp_context* ctx); // {note}")
    lines += ["", "#ifdef __cplusplus", "}", "#endif", "", "#endif", ""]
    DECLS_HEADER.parent.mkdir(exist_ok=True)
    DECLS_HEADER.write_text("\n".join(lines))
    print(f"wrote {DECLS_HEADER.relative_to(ROOT)}: " + ", ".join(missing))
    for e in errors:
        print(f"  error: {e}")
    return 1 if errors else 0


def syntax_check() -> int:
    out = ROOT / OUTPUT_DIR
    files = sorted(out.glob("funcs_*.c"))
    log_dir = ROOT / "build" / "syntax"
    log_dir.mkdir(parents=True, exist_ok=True)
    incs = f'/I "{ROOT / "lib/N64Recomp/include"}" /I "{ROOT / "include"}"'
    bat = log_dir / "check.bat"
    bat.write_text("@echo off\r\n" + f'call "{VCVARS}" >nul\r\n' + f'cd /d "{out}"\r\n' + "".join(
        f'clang-cl /nologo /Zs /TC {NOISE_FLAGS} {incs} {f.name} > "{log_dir / (f.stem + ".log")}" 2>&1\r\n'
        for f in files))
    subprocess.run(["cmd", "/c", str(bat)], capture_output=True, text=True)
    diags = []
    for f in files:
        diags += [f"{f.name}: {l.strip()}" for l in (log_dir / f"{f.stem}.log").read_text(errors="replace").splitlines() if l.strip()]
    print(f"syntax check: {len(files)} files, {len(diags)} diagnostic lines")
    for d in diags[:20]:
        print(f"  {d}")
    return 1 if diags else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", action="store_true")
    ap.add_argument("--no-run", action="store_true")
    ap.add_argument("--no-check", action="store_true")
    args = ap.parse_args()
    if args.trace:
        global CONFIG, OUTPUT_DIR
        CONFIG = ROOT / "kig.us.trace.toml"
        OUTPUT_DIR = "RecompiledFuncsTrace"
    build_recomp_rom()
    write_segments_header()
    write_config(args.trace)
    print(f"wrote {CONFIG.name}")
    if args.no_run:
        return 0
    if run_recompiler() or write_decls():
        return 1
    if args.no_check or args.trace:  # trace builds use include/trace.h instead
        return 0
    return syntax_check()


if __name__ == "__main__":
    sys.exit(main())
