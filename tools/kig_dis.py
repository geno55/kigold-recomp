"""Disassembles KI Gold code from the recomp ROM (build/kig.recomp.z64), any section (debug helper).

Usage: python tools/kig_dis.py NAME|0xVRAM [...]       disassemble whole functions
       python tools/kig_dis.py --callers NAME|0xVRAM     list jal call sites of a function
       python tools/kig_dis.py --refs 0xADDR             list lui/addiu|ori|load|store pairs reaching an address
"""
import struct
import sys
import tomllib
from pathlib import Path

import rabbitizer

ROOT = Path(__file__).resolve().parent.parent
rom = (ROOT / "build" / "kig.recomp.z64").read_bytes()
syms = tomllib.loads((ROOT / "syms" / "kig.us.syms.toml").read_text(encoding="utf-8"))
sections = syms["section"]
funcs = {}
for s in sections:
    for f in s["functions"]:
        funcs[(s["name"], f["vram"])] = dict(f, section=s["name"])
by_name = {f["name"]: f for f in funcs.values()}
lib = {}
for line in (ROOT / "syms" / "libultra_symbols.txt").read_text().splitlines():
    if "=" in line:
        n, v = line.split("=")[0].strip(), line.split("=")[1].split(";")[0].strip()
        lib[int(v, 16)] = n


def name_at(vram):
    for f in funcs.values():
        if f["vram"] == vram:
            return f["name"]
    return lib.get(vram, hex(vram))


def words(sec):
    base = sec["rom"]
    for i in range(0, sec["size"], 4):
        yield sec["vram"] + i, struct.unpack_from(">I", rom, base + i)[0]


def resolve(arg):
    if arg.startswith("0x"):
        v = int(arg, 16)
        hits = [f for f in funcs.values() if f["vram"] == v]
        return hits
    return [by_name[arg]] if arg in by_name else []


def dis(f):
    sec = next(s for s in sections if s["name"] == f["section"])
    print(f"--- {f['name']} @ {f['vram']:08X} size 0x{f['size']:X} ({f['section']})")
    for v in range(f["vram"], f["vram"] + f["size"], 4):
        w = struct.unpack_from(">I", rom, sec["rom"] + v - sec["vram"])[0]
        x = rabbitizer.Instruction(w, v)
        extra = ""
        if x.getOpcodeName() == "jal":
            extra = f"   ; {name_at(x.getInstrIndexAsVram())}"
        print(f"  {v:08X}: {x.disassemble()}{extra}")


def containing(sec_name, v):
    for f in funcs.values():
        if f["section"] == sec_name and f["vram"] <= v < f["vram"] + f["size"]:
            return f["name"]
    return "?"


def main():
    args = sys.argv[1:]
    if args[0] == "--callers":
        target = int(args[1], 16) if args[1].startswith("0x") else by_name[args[1]]["vram"]
        jal = 0x0C000000 | ((target >> 2) & 0x3FFFFFF)
        for s in sections:
            for v, w in words(s):
                if w == jal:
                    print(f"{s['name']} {v:08X} in {containing(s['name'], v)}")
        return
    if args[0] == "--refs":
        target = int(args[1], 16)
        for s in sections:
            his = {}
            for v, w in words(s):
                op, rs, rt, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
                if op == 0x0F:
                    his[rt] = (imm << 16, v)
                elif rs in his and op in (0x09, 0x0D, 0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B, 0x31, 0x39):
                    hi, hv = his[rs]
                    addr = (hi | imm) if op == 0x0D else (hi + ((imm ^ 0x8000) - 0x8000)) & 0xFFFFFFFF
                    if v - hv < 0x200 and addr == target:
                        print(f"{s['name']} {v:08X} in {containing(s['name'], v)}: {rabbitizer.Instruction(w, v).disassemble()}")
        return
    for a in args:
        hits = resolve(a)
        if not hits:
            print(f"?? {a}")
        for f in hits:
            dis(f)


if __name__ == "__main__":
    main()
