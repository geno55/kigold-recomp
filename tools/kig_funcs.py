"""Function boundaries for every code section, by control flow from known entry points.

spimdisasm (splat) handles the compiled C well, but KI Gold also has a lot of handwritten assembly
(seg2 especially: multiple entry points, functions that fall into the next one, $k1/$gp as scratch,
data islands in .text) that it emits as data. So the function list is built here:

  1. Entry points: every splat function start, every jal target, every `j` target outside its function,
     lui/addiu (or lui/ori) pairs that point into a section's text, and data words pointing into text
     that are not jump-table entries.
  2. A function starts at an entry point and may run up to the next one. Its body is what control flow
     reaches from the start (branches, jump tables run to the limit). Branches that leave it must land
     on an entry point (N64Recomp emits a tail call); other targets become entry points and the
     analysis repeats until nothing changes.
  3. The body ends at the last reachable instruction (plus delay slot). If it can run into the next
     entry point, N64Recomp's fall-through patch (patches/n64recomp/) makes it a tail call.
     Bytes between a body's end and the next entry point are data (or dead code).

seg1 and seg2 share VRAM; a reference from another section to an address inside both counts for each
section where it is a plausible function start.

Inputs: build/kig.recomp.z64, build/splat_funcs.csv, syms/overrides.toml (optional: [[entry]], [[not_entry]])
Output: build/kig_funcs.csv  section,vram,size,name,falls_through
Usage: python tools/kig_funcs.py
"""
import bisect
import csv
import struct
import sys
import tomllib
from collections import defaultdict
from pathlib import Path

import rabbitizer

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kig_layout import ROOT, SEGS, recomp_rom  # noqa: E402

UNCOND = {"j", "jr", "b", "eret"}
# Encodings rabbitizer decodes but N64Recomp doesn't translate (not VR4300, or system-only).
UNSUPPORTED = {"pref", "mfc0", "mtc0", "dmfc0", "dmtc0", "cfc0", "ctc0", "ll", "sc", "lld", "scd", "syscall", "break",
               "tlbp", "tlbr", "tlbwi", "tlbwr", "eret", "cache", "sync", "lwc2", "swc2", "ldc2", "sdc2", "mfc2",
               "mtc2", "cfc2", "ctc2", "teq", "tne", "tge", "tgeu", "tlt", "tltu", "teqi", "tnei", "tgei", "tgeiu",
               "tlti", "tltiu", "ldl", "ldr", "sdl", "sdr", "movz", "movn", "rfe",
               "bc0f", "bc0t", "bc0fl", "bc0tl", "lwc3", "swc3"}


class Section:
    def __init__(self, seg, rom: bytes):
        self.seg = seg
        self.data = rom[seg.recomp_rom:seg.recomp_rom + seg.size]
        self.text_end = seg.text_end
        self._ins = {}

    def in_text(self, v: int) -> bool:
        return self.seg.vram <= v < self.text_end and not v & 3

    def word(self, v: int) -> int:
        o = v - self.seg.vram
        return struct.unpack(">I", self.data[o:o + 4])[0]

    def ins(self, v: int):
        i = self._ins.get(v)
        if i is None:
            i = self._ins[v] = rabbitizer.Instruction(self.word(v), v)
        return i


def is_uncond(i) -> bool:
    n = i.getOpcodeName()
    return n in UNCOND or (n == "beq" and i.rs.value == 0 and i.rt.value == 0)


def plausible_start(sec: Section, v: int) -> bool:
    """The code before v ends a function: jr/j/eret plus delay slot, then nop padding (a `b` is not
    enough: switch cases end that way). The section start always counts."""
    if v == sec.seg.vram:
        return True
    if not sec.ins(v).isValid():
        return False
    prev = sec.ins(v - 4)
    if prev.isBranch() or prev.isJump():
        return False  # a delay slot
    p = v - 4
    while p >= sec.seg.vram and sec.word(p) == 0:
        p -= 4
    if p < sec.seg.vram or not sec.ins(p).isValid():
        return True  # section start, or code right after a data island
    return any(sec.ins(j).getOpcodeName() in ("jr", "j", "eret") for j in (p - 4, p) if j >= sec.seg.vram)


def references(secs):
    """(target section name or None for 'any'), vram, kind) for every code reference."""
    refs = []
    for sec in secs:
        s = sec.seg
        hi = {}
        for v in range(s.vram, sec.text_end, 4):
            i = sec.ins(v)
            n = i.getOpcodeName()
            if n == "jal":
                refs.append((sec.seg.name, i.getInstrIndexAsVram(), "jal"))
            elif n == "lui":
                hi[i.rt.value] = (i.getProcessedImmediate() << 16) & 0xFFFFFFFF
            elif n in ("addiu", "ori") and i.rs.value in hi:
                imm = i.getProcessedImmediate() if n == "addiu" else i.getImmediate()
                refs.append((sec.seg.name, (hi[i.rs.value] + imm) & 0xFFFFFFFF, "hilo"))
            if is_uncond(i) and n != "j":
                hi = {}
    return refs


def jump_tables(sec: Section):
    """Jump tables (lui .. lw .. jr pattern): table entry addresses, all targets, and targets per jr."""
    entries, targets, by_jr = set(), set(), {}
    s = sec.seg
    for v in range(s.vram, sec.text_end, 4):
        i = sec.ins(v)
        if i.getOpcodeName() != "jr" or i.rs.value == 31:
            continue
        reg, lo, base = i.rs.value, None, None
        for j in range(v - 4, max(s.vram, v - 64) - 4, -4):
            y = sec.ins(j)
            n = y.getOpcodeName()
            if n == "lw" and y.rt.value == reg and lo is None:
                lo, base = y.getProcessedImmediate(), y.rs.value
            elif lo is not None and n == "addu" and y.rd.value == base:
                # addu base, x, idx: follow whichever operand gets the lui (the other is the index)
                for cand in (y.rs.value, y.rt.value):
                    if any(sec.ins(k).getOpcodeName() in ("lui", "addiu") and sec.ins(k).rt.value == cand
                           for k in range(max(s.vram, j - 32), j, 4)):
                        base = cand
                        break
            elif lo is not None and n == "addiu" and y.rt.value == base and y.rs.value == base:
                lo += y.getProcessedImmediate()
            elif n == "lui" and lo is not None and y.rt.value == base:
                tbl = ((y.getProcessedImmediate() << 16) + lo) & 0xFFFFFFFF
                k = tbl
                if s.vram <= k < s.vram + s.size and not sec.in_text(sec.word(k)):
                    k += 4  # handwritten tables indexed from 1 (seg2 801C0E3C)
                tg = []
                while s.vram <= k < s.vram + s.size:
                    w = sec.word(k)
                    if not sec.in_text(w) or (k != tbl and k in entries):
                        break
                    entries.add(k)
                    tg.append(w)
                    k += 4
                targets.update(tg)
                by_jr[v] = tg
                break
    return entries, targets, by_jr


def main() -> int:
    rom = recomp_rom()
    secs = [Section(s, rom) for s in SEGS]
    by_name = {sec.seg.name: sec for sec in secs}
    ov_path = ROOT / "syms" / "overrides.toml"
    ov = tomllib.loads(ov_path.read_text(encoding="utf-8")) if ov_path.exists() else {}

    entries = defaultdict(set)   # primary function starts
    extra = defaultdict(set)     # other entry points: become secondary functions unless nothing covers them
    names = {}
    for r in csv.DictReader(open(ROOT / "build" / "splat_funcs.csv")):
        v = int(r["vram"], 16)
        sec = by_name[r["section"]]
        if sec.in_text(v):
            (entries if r["kind"] == "glabel" else extra)[r["section"]].add(v)
            if not r["name"].startswith(("func_", "D_", ".")):
                names[(r["section"], v)] = r["name"]
    for sec in secs:
        entries[sec.seg.name].add(sec.seg.vram)

    jt_entries, jt_targets, jt_by_jr = {}, {}, {}
    for sec in secs:
        jt_entries[sec.seg.name], jt_targets[sec.seg.name], jt_by_jr[sec.seg.name] = jump_tables(sec)

    static = {"boot", "seg0"}
    weak = defaultdict(set)  # starts known only from pointers

    def add_ref(src: str, v: int, kind: str):
        # An overlay can reference itself and the static code; static code can reference anything.
        cands = [sec for sec in secs if sec.in_text(v) and (src in static or sec.seg.name in static or sec.seg.name == src)]
        if not cands:
            return
        own = [sec for sec in cands if sec.seg.name == src]
        for sec in (own or cands):
            if kind == "hilo" and v in jt_targets[sec.seg.name]:
                continue
            if plausible_start(sec, v):
                if v not in entries[sec.seg.name] and kind != "jal":
                    weak[sec.seg.name].add(v)  # a pointer: may be data in .text
                elif kind == "jal":
                    weak[sec.seg.name].discard(v)
                entries[sec.seg.name].add(v)
            elif kind == "jal" and sec in own:
                extra[src].add(v)  # a call into the middle of a function (handwritten code)

    for src, v, kind in references(secs):
        add_ref(src, v, kind)
    # Data words pointing into text (function pointer tables), except jump table entries.
    for sec in secs:
        s = sec.seg
        for v in range(sec.text_end, s.vram + s.size, 4):
            w = sec.word(v)
            if any(o.in_text(w) for o in secs) and v not in jt_entries[s.name]:
                add_ref(s.name, w, "data")

    # Every jump table target is a possible entry (handwritten tables jump between functions, and a jr
    # can sit in code the flow analysis doesn't reach from a known start).
    for sec in secs:
        extra[sec.seg.name] |= jt_targets[sec.seg.name]

    for e in ov.get("entry", []):
        entries[e["section"]].add(e["vram"])
    for e in ov.get("not_entry", []):
        entries[e["section"]].discard(e["vram"])

    def is_data_word(sec: Section, s, a: int, end: int, v: int) -> bool:
        """An unreachable word N64Recomp would fail to translate inside function [a, end)."""
        i = sec.ins(v)
        n = i.getOpcodeName()
        if not i.isValid() or n in UNSUPPORTED:
            return True
        if n in ("j", "jal"):
            t = i.getInstrIndexAsVram()
            return not any(o.in_text(t) for o in secs) or (n == "j" and not a <= t < end and t not in entries[s.name])
        if i.isBranch():
            return not a <= i.getBranchVramGeneric() < end
        return i.isJump() and n not in ("jr", "jalr")

    def body(sec: Section, a: int, limit: int):
        """(end, falls into limit, all instructions valid, branch/j targets outside [a, limit))."""
        s = sec.seg
        reached, falls, ok, out = set(), False, True, set()
        work = [a]
        while work:
            v = work.pop()
            while v < limit and v not in reached:
                i = sec.ins(v)
                if not i.isValid():
                    ok = False
                    break
                reached.add(v)
                n = i.getOpcodeName()
                if (i.isBranch() or i.isJump()) and v + 4 >= limit:
                    reached.add(v + 4)  # delay slot past the limit (see `end` below)
                if n in ("j", "jal") and not any(o.in_text(i.getInstrIndexAsVram()) for o in secs):
                    ok = False  # a jump nowhere: this is data
                    break
                if i.isBranch() or n in ("j", "b"):
                    t = i.getBranchVramGeneric() if n != "j" else i.getInstrIndexAsVram()
                    if a <= t < limit:
                        work.append(t)
                    elif sec.in_text(t):
                        out.add(t)
                if n == "jr" and i.rs.value != 31:
                    reached.add(v + 4)
                    tg = jt_by_jr[s.name].get(v)
                    if tg is not None:  # else an indirect tail call (N64Recomp does the same)
                        work.extend(t for t in tg if a <= t < limit)
                        # handwritten tables can point at other functions' code
                        out.update(t for t in tg if not a <= t < limit)
                    break
                if is_uncond(i):
                    reached.add(v + 4)
                    break
                v += 4
            if ok and v >= limit and v - 4 in reached:
                falls = True
        # The only word at or past the limit that can be reached is a delay slot; keep it (overlapping the
        # next function): N64Recomp needs every jump's delay slot inside the function.
        end = max(reached) + 4 if reached else a + 4
        return end, falls, ok, out, reached

    # Primary functions run up to the next primary start. A branch that leaves its function for an
    # address that is not a function start gets a secondary function there, which overlaps the primary
    # that contains it and runs to that primary's end (N64Recomp compiles each function on its own, so
    # overlap is fine, and the primary keeps its loops). Targets in no body become primaries.
    secondary = defaultdict(dict)  # section -> {vram: (container start)}
    for _ in range(100):
        changed = False
        bodies = {}
        for sec in secs:
            s = sec.seg
            starts = sorted(entries[s.name])
            for k, a in enumerate(starts):
                limit = starts[k + 1] if k + 1 < len(starts) else sec.text_end
                bodies[(s.name, a)] = body(sec, a, limit)
            # A pointer-only start whose code runs into invalid instructions is data in .text.
            bad = {a for a in starts if a in weak[s.name] and not bodies[(s.name, a)][2]}
            if bad:
                entries[s.name] -= bad
                weak[s.name] -= bad
                changed = True
                break
            ends = [bodies[(s.name, a)][0] for a in starts]
            pending = {t for a in starts for t in bodies[(s.name, a)][3]} | set(secondary[s.name]) | extra[s.name]
            secondary[s.name] = {}
            done = set()
            while pending:
                t = pending.pop()
                if t in done or t in entries[s.name]:
                    continue
                done.add(t)
                k = bisect.bisect_right(starts, t) - 1
                if k < 0 or t >= ends[k]:
                    entries[s.name].add(t)  # code nothing else covers: a primary
                    changed = True
                    continue
                secondary[s.name][t] = starts[k]
                bodies[(s.name, t)] = b = body(sec, t, ends[k])
                pending |= b[3]
        if not changed:
            break

    rows, problems, dead_words = [], [], []
    for sec in secs:
        s = sec.seg
        for a in sorted(set(entries[s.name]) | set(secondary[s.name])):
            if a in secondary[s.name] and a in entries[s.name]:
                continue
            end, falls, ok, _, reached = bodies[(s.name, a)]
            name = names.get((s.name, a), f"func_{a:08X}" if s.name in ("boot", "seg0") else f"{s.name}_{a:08X}")
            if not ok:
                problems.append(f"{s.name} {a:08X}: invalid instruction reached")
            rows.append((s.name, a, end - a, name, falls))
            # Unreachable runs inside the body that contain words N64Recomp can't translate (data islands
            # in handwritten code: invalid encodings, jumps to nowhere, branches out of the function,
            # instructions the VR4300 doesn't have): the whole run is data.
            run = []
            for v in list(range(a, end, 4)) + [None]:
                if v is not None and v not in reached:
                    run.append(v)
                    continue
                if any(is_data_word(sec, s, a, end, w) for w in run):
                    dead_words.extend((s.name, a, w) for w in run)
                run = []
    with open(ROOT / "build" / "kig_funcs.csv", "w", newline="") as fh:
        wr = csv.writer(fh)
        wr.writerow(["section", "vram", "size", "name", "falls_through"])
        for r in rows:
            wr.writerow([r[0], f"0x{r[1]:08X}", f"0x{r[2]:X}", r[3], int(r[4])])
    with open(ROOT / "build" / "kig_dead_words.csv", "w", newline="") as fh:
        wr = csv.writer(fh)
        wr.writerow(["section", "func_vram", "vram"])
        for s, a, v in dead_words:
            wr.writerow([s, f"0x{a:08X}", f"0x{v:08X}"])
    print(f"{len(dead_words)} unreachable untranslatable words inside functions -> build/kig_dead_words.csv")
    for sec in secs:
        fl = [r for r in rows if r[0] == sec.seg.name]
        covered = set()
        for r in fl:
            covered.update(range(r[1], r[1] + r[2], 4))
        nsec = sum(1 for r in fl if r[1] in secondary[sec.seg.name] and r[1] not in entries[sec.seg.name])
        print(f"{sec.seg.name}: {len(fl)} functions ({nsec} secondary), {sum(r[4] for r in fl)} fall through, "
              f"{len(covered) * 4:#x} of {sec.text_end - sec.seg.vram:#x} text bytes in bodies")
    for p in problems:
        print("  problem:", p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
