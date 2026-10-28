#!/usr/bin/env python3
"""refemu.py - headless reference emulator for differential testing against kig.exe.

Drives the parallel_n64 libretro core through ctypes, unthrottled and without a window or GPU context:
the angrylion software RDP and the cxd4 LLE RSP (core options, below). mupen64plus_next was tried
too: its angrylion setup crashes in retro_load_game and its default renderer needs GL.

Step language (shared with kig.exe's KIG_STEPS, src/game/steps.cpp). A scenario's `steps` (tests/
scenarios/*.toml) or a .steps file; one tick per VI (one retro_run here, one VI there):
    WAIT <vis>                                   let that many VIs pass
    WAIT_RAM <addr> <8|16|32> <==|!=|&> <value> [timeout_vis]
                                                 block until the RAM condition holds (default timeout
                                                 3600 VIs: the run fails, exit code 3)
    PRESS <spec> [vis]                           hold spec for vis VIs (default 9), without blocking
    HOLD <spec> / RELEASE                        hold until RELEASE (which releases everything)
    WRITE_RAM <addr> <8|16|32> <value>           (KIG_POKE's job, at a synced point)
    SCREENSHOT <label>                           <out>/<label>.png
    DUMP_RAM <label>                             <out>/<label>.bin: all 8 MB of RDRAM, big-endian
    LOAD_RAM <file> <start> <end> [<start> <end> ...]
                                                 copy those ranges of a big-endian dump (the other side's
                                                 DUMP_RAM) into RDRAM, to start both sides from one game state
    SAVE_STATE <file> / LOAD_STATE <file>        core snapshots (this side only; kig skips them)
    EXIT [code]
  Addresses are hex KSEG0/KSEG1 (801D8806); values decimal or 0x hex. spec is kig's input spec:
  '+'-joined A B Z START L R DU DD DL DR CU CD CL CR, X=<f> / Y=<f> stick (-1..1, Y up), "2>" = port 2.
  Every executed step is logged as "[ref] step vi=<n> line <l>: ..." (kig: "[kig] step ..."), so the
  two logs line up step by step; the VI numbers show where the two drift apart.

Saves are portable to kig: --eeprom-in/--eeprom-out use kig's saves/kig.us.bin (the 4 Kbit EEPROM's 512
bytes in N64 order); --pak-in/--pak-out use kig's Controller Pak folder (saves/controllerpak_port1/,
tools/pak_convert.py) and a 32 KB pak image is built from it for the core.

Usage:
    python tools/refemu.py <scenario.toml|file.steps> [--out-dir dir] [--rom baserom.us.z64] [--core dll]
           [--eeprom-in f] [--eeprom-out f] [--pak-in dir] [--pak-out dir] [--max-vis N] [--option k=v]
           [--watch ADDR:BITS,...]
    python tools/refemu.py --calibrate      check the input mapping against the game's controller buffer
"""
import argparse
import ctypes as C
import os
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ROM = ROOT / "baserom.us.z64"
DEFAULT_CORE = Path(r"C:\RetroArch-Win64\cores\parallel_n64_libretro.dll")
RDRAM_SIZE = 0x800000
EEPROM_SIZE = 0x200      # 4 Kbit
SAVE_EEPROM_OFF = 0      # parallel_n64's save RAM: eeprom[0x800], mempack[4][0x8000], sram, flashram
SAVE_MEMPACK_OFF = 0x800
MEMPACK_SIZE = 0x8000

CORE_OPTIONS = {
    "parallel-n64-gfxplugin": "angrylion",
    "parallel-n64-rspplugin": "cxd4",
    "parallel-n64-pak1": "none",
    "parallel-n64-pak2": "none",
    "parallel-n64-pak3": "none",
    "parallel-n64-pak4": "none",
}

# libretro
ENV_GET_CAN_DUPE, ENV_SET_PIXEL_FORMAT, ENV_SET_HW_RENDER, ENV_GET_VARIABLE = 3, 10, 14, 15
ENV_GET_SYSTEM_DIRECTORY, ENV_GET_SAVE_DIRECTORY = 9, 31
PIXFMT_0RGB1555, PIXFMT_XRGB8888, PIXFMT_RGB565 = 0, 1, 2
MEM_SAVE_RAM, MEM_SYSTEM_RAM = 0, 2
DEVICE_JOYPAD, DEVICE_ANALOG = 1, 5
J_B, J_Y, J_SELECT, J_START, J_UP, J_DOWN, J_LEFT, J_RIGHT, J_A, J_X, J_L, J_R, J_L2, J_R2 = range(14)

# N64 button -> how the core reads it: ("pad", RetroPad id) or ("axis", stick index, axis, sign).
# The mupen64plus-derived default layout; tools/refemu.py --calibrate checks it against the game.
N64_INPUTS = {
    "A": ("pad", J_B), "B": ("pad", J_Y), "Z": ("pad", J_L2), "START": ("pad", J_START),
    "L": ("pad", J_L), "R": ("pad", J_R),
    "DU": ("pad", J_UP), "DD": ("pad", J_DOWN), "DL": ("pad", J_LEFT), "DR": ("pad", J_RIGHT),
    "CU": ("axis", 1, 1, -1), "CD": ("axis", 1, 1, 1), "CL": ("axis", 1, 0, -1), "CR": ("axis", 1, 0, 1),
}
N64_BITS = {"A": 0x8000, "B": 0x4000, "Z": 0x2000, "START": 0x1000, "DU": 0x0800, "DD": 0x0400,
            "DL": 0x0200, "DR": 0x0100, "L": 0x0020, "R": 0x0010, "CU": 0x0008, "CD": 0x0004,
            "CL": 0x0002, "CR": 0x0001}

ENV_CB = C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p)
VIDEO_CB = C.CFUNCTYPE(None, C.c_void_p, C.c_uint, C.c_uint, C.c_size_t)
AUDIO_CB = C.CFUNCTYPE(None, C.c_int16, C.c_int16)
AUDIO_BATCH_CB = C.CFUNCTYPE(C.c_size_t, C.c_void_p, C.c_size_t)
POLL_CB = C.CFUNCTYPE(None)
INPUT_CB = C.CFUNCTYPE(C.c_int16, C.c_uint, C.c_uint, C.c_uint, C.c_uint)


class GameInfo(C.Structure):
    _fields_ = [("path", C.c_char_p), ("data", C.c_void_p), ("size", C.c_size_t), ("meta", C.c_char_p)]


class Variable(C.Structure):
    _fields_ = [("key", C.c_char_p), ("value", C.c_char_p)]


class PortInput:
    """What one N64 port holds this VI: button bits and stick values (-1..1, Y up)."""

    def __init__(self):
        self.buttons, self.x, self.y = 0, 0.0, 0.0


class Core:
    """One parallel_n64 instance with the minimum frontend services."""

    def __init__(self, core_path, rom_path, work_dir, options):
        self.lib = C.CDLL(str(core_path))
        self.options = dict(CORE_OPTIONS, **options)
        self._answers = {}
        self.pixfmt = PIXFMT_0RGB1555
        self.frame = None
        self.ports = [PortInput() for _ in range(4)]
        self._dir = C.c_char_p(str(Path(work_dir).resolve()).encode())
        self._cbs = [ENV_CB(self._env), VIDEO_CB(self._video), AUDIO_CB(lambda l, r: None),
                     AUDIO_BATCH_CB(lambda data, frames: frames), POLL_CB(lambda: None), INPUT_CB(self._input)]
        L = self.lib
        for setter, cb in zip(("environment", "video_refresh", "audio_sample", "audio_sample_batch", "input_poll",
                               "input_state"), self._cbs):
            getattr(L, f"retro_set_{setter}")(cb)
        L.retro_init()
        rom = Path(rom_path).read_bytes()
        self._rom_buf = C.create_string_buffer(rom, len(rom))
        L.retro_load_game.restype = C.c_bool
        info = GameInfo(str(Path(rom_path).resolve()).encode(), C.cast(self._rom_buf, C.c_void_p), len(rom), None)
        if not L.retro_load_game(C.byref(info)):
            raise RuntimeError("core rejected the ROM")
        L.retro_get_memory_data.restype = C.c_void_p
        L.retro_get_memory_size.restype = C.c_size_t
        L.retro_serialize_size.restype = C.c_size_t
        L.retro_serialize.restype = C.c_bool
        L.retro_unserialize.restype = C.c_bool

    # --- frontend callbacks -----------------------------------------------------------------------
    def _env(self, cmd, data):
        cmd &= 0xFFFF
        if cmd in (ENV_GET_SYSTEM_DIRECTORY, ENV_GET_SAVE_DIRECTORY):
            C.cast(data, C.POINTER(C.c_char_p))[0] = self._dir.value
            return True
        if cmd == ENV_SET_PIXEL_FORMAT:
            self.pixfmt = C.cast(data, C.POINTER(C.c_int))[0]
            return self.pixfmt in (PIXFMT_0RGB1555, PIXFMT_XRGB8888, PIXFMT_RGB565)
        if cmd == ENV_GET_CAN_DUPE:
            C.cast(data, C.POINTER(C.c_bool))[0] = True
            return True
        if cmd == ENV_SET_HW_RENDER:
            return False  # no GL/Vulkan context: the core must render in software
        if cmd == ENV_GET_VARIABLE:
            var = C.cast(data, C.POINTER(Variable))[0]
            value = self.options.get(var.key.decode())
            if value is None:
                return False
            self._answers[var.key] = C.c_char_p(value.encode())
            C.cast(data, C.POINTER(Variable))[0].value = self._answers[var.key].value
            return True
        return False

    def _video(self, data, width, height, pitch):
        if data:
            self.frame = (C.string_at(data, pitch * height), width, height, pitch)

    def _input(self, port, device, index, id_):
        if port >= 4:
            return 0
        p = self.ports[port]
        if device == DEVICE_JOYPAD:
            return int(any(kind == "pad" and rid == id_ and p.buttons & N64_BITS[n]
                           for n, (kind, rid, *_) in N64_INPUTS.items()))
        if device == DEVICE_ANALOG:
            if index == 0:  # left stick: libretro Y grows downwards
                v = p.x if id_ == 0 else -p.y
                return int(max(-1.0, min(1.0, v)) * 32767)
            total = 0
            for n, spec in N64_INPUTS.items():
                if spec[0] == "axis" and spec[1] == index and spec[2] == id_ and p.buttons & N64_BITS[n]:
                    total += spec[3] * 32767
            return max(-32767, min(32767, total))
        return 0

    # --- services ---------------------------------------------------------------------------------
    def run_frame(self):
        self.lib.retro_run()

    def _mem(self, kind):
        ptr = self.lib.retro_get_memory_data(kind)
        size = self.lib.retro_get_memory_size(kind)
        return (C.c_uint8 * size).from_address(ptr) if ptr and size else None

    def rdram(self):
        mem = self._mem(MEM_SYSTEM_RAM)
        if mem is None or len(mem) < RDRAM_SIZE:
            raise RuntimeError("core exposes no 8 MB RDRAM")
        return mem

    @staticmethod
    def phys(addr, width):
        p = addr & 0x1FFFFFFF
        if p + width > RDRAM_SIZE:
            raise ValueError(f"address {addr:08X} outside RDRAM")
        return p

    # RDRAM is kept as host-order 32-bit words: N64 byte a is at a ^ 3 (as in kig).
    def read(self, addr, bits):
        mem, p = self.rdram(), self.phys(addr, bits // 8)
        v = 0
        for i in range(bits // 8):
            v = v << 8 | mem[(p + i) ^ 3]
        return v

    def write(self, addr, bits, value):
        mem, p = self.rdram(), self.phys(addr, bits // 8)
        for i in range(bits // 8):
            mem[(p + i) ^ 3] = (value >> (bits - 8 * (i + 1))) & 0xFF

    def rdram_be(self):
        """All of RDRAM in N64 (big-endian) byte order, kig's DUMP_RAM layout."""
        raw = bytes(self.rdram()[:RDRAM_SIZE])
        out = bytearray(RDRAM_SIZE)
        for k in range(4):
            out[k::4] = raw[3 - k::4]
        return bytes(out)

    def save_ram(self):
        mem = self._mem(MEM_SAVE_RAM)
        if mem is None:
            raise RuntimeError("core exposes no save RAM")
        return mem

    def save_state(self):
        size = self.lib.retro_serialize_size()
        buf = C.create_string_buffer(size)
        if not self.lib.retro_serialize(buf, size):
            raise RuntimeError("serialize failed")
        return buf.raw

    def load_state(self, data):
        buf = C.create_string_buffer(data, len(data))
        if not self.lib.retro_unserialize(buf, len(data)):
            raise RuntimeError("unserialize failed")

    def screenshot(self, path):
        from PIL import Image
        if not self.frame:
            raise RuntimeError("no frame rendered yet")
        data, w, h, pitch = self.frame
        if self.pixfmt == PIXFMT_XRGB8888:
            img = Image.frombuffer("RGBX", (w, h), data, "raw", "BGRX", pitch, 1).convert("RGB")
        else:
            img = Image.frombuffer("RGB", (w, h), data, "raw", "BGR;16" if self.pixfmt == PIXFMT_RGB565 else "BGR;15",
                                   pitch, 1)
        img.save(path)


# ---------------------------------------------------------------------------------------------
# Steps
# ---------------------------------------------------------------------------------------------
def load_steps(path):
    """(lines of (line_no, OP, args)) from a scenario .toml's `steps` or a .steps file."""
    path = Path(path)
    text = path.read_text(encoding="utf-8")
    first_line = 1
    if path.suffix == ".toml":
        sc = tomllib.loads(text)
        if "steps" not in sc:
            raise SystemExit(f"{path}: no `steps` (timed-only scenarios run on kig and ares)")
        text = sc["steps"]
        # Report line numbers within the steps string, as kig does (it reads the extracted file).
    out = []
    for n, line in enumerate(text.splitlines(), first_line):
        line = line.split("#", 1)[0].strip()
        if line:
            tok = line.split()
            out.append((n, tok[0].upper(), tok[1:]))
    return out


def parse_spec(spec):
    port = 0
    if len(spec) > 2 and spec[1] == ">" and spec[0] in "1234":
        port, spec = int(spec[0]) - 1, spec[2:]
    buttons, x, y = 0, None, None
    for part in spec.split("+"):
        if part.startswith("X="):
            x = float(part[2:])
        elif part.startswith("Y="):
            y = float(part[2:])
        elif part in N64_BITS:
            buttons |= N64_BITS[part]
        else:
            raise ValueError(f"unknown input '{part}'")
    return port, buttons, x, y


class Fail(Exception):
    pass


def run_steps(core, steps, out_dir, max_vis, log=print, watch=()):
    """watch: [(addr, bits)] logged whenever their values change (KIG_WATCH's job)."""
    out_dir = Path(out_dir)
    last_watch = None
    held = []  # (port, buttons, x, y, until_vi or None)
    pc, vi, wait_until, wait_since = 0, 0, 0, None
    # kig runs step tick n after the game's VI n, so tick n here also sees n frames.
    core.run_frame()
    while vi < max_vis:
        vi += 1
        held = [h for h in held if h[4] is None or vi < h[4]]
        shot = None
        while pc < len(steps) and vi >= wait_until:
            line, op, a = steps[pc]

            def logged():
                log(f"[ref] step vi={vi} t={vi / 60:.2f} line {line}: {op} {' '.join(a)}".rstrip())
            if op == "WAIT":
                wait_until = vi + int(a[0], 0)
                pc += 1
                continue
            if op == "WAIT_RAM":
                bits, want = int(a[1]), int(a[3], 0)
                got = core.read(int(a[0], 16), bits)
                ok = got == want if a[2] == "==" else got != want if a[2] == "!=" else bool(got & want)
                if not ok:
                    wait_since = wait_since or vi
                    timeout = int(a[4], 0) if len(a) > 4 else 3600
                    if vi + 1 - wait_since >= timeout:
                        raise Fail(f"line {line} WAIT_RAM timed out after {timeout} VIs (value {got:X})")
                    break
                wait_since = None
                logged()
                pc += 1
                continue
            logged()
            if op in ("PRESS", "HOLD"):
                port, buttons, x, y = parse_spec(a[0])
                until = vi + (int(a[1], 0) if len(a) > 1 else 9) if op == "PRESS" else None
                held.append((port, buttons, x, y, until))
            elif op == "RELEASE":
                held = []
            elif op == "WRITE_RAM":
                core.write(int(a[0], 16), int(a[1]), int(a[2], 0))
            elif op == "SCREENSHOT":
                shot = out_dir / f"{a[0]}.png"
            elif op == "DUMP_RAM":
                (out_dir / f"{a[0]}.bin").write_bytes(core.rdram_be())
            elif op == "LOAD_RAM":
                if len(a) < 3 or len(a) % 2 != 1:
                    raise Fail(f"line {line}: usage: LOAD_RAM <file> <start> <end> [<start> <end> ...]")
                p = Path(a[0]) if Path(a[0]).is_absolute() else out_dir / a[0]
                be = p.read_bytes()
                mem = core.rdram()
                for lo, hi in zip(a[1::2], a[2::2]):
                    for q in range(int(lo, 16) & 0x1FFFFFFF, int(hi, 16) & 0x1FFFFFFF):
                        mem[q ^ 3] = be[q]
            elif op == "SAVE_STATE":
                (out_dir / a[0]).write_bytes(core.save_state())
            elif op == "LOAD_STATE":
                p = Path(a[0]) if Path(a[0]).is_absolute() else out_dir / a[0]
                core.load_state(p.read_bytes())
            elif op == "EXIT":
                return int(a[0], 0) if a else 0
            else:
                raise Fail(f"line {line}: unknown step {op}")
            pc += 1
        if pc >= len(steps) and vi >= wait_until and not shot:
            log(f"[ref] steps: done at vi={vi}")
            return 0
        for p in core.ports:
            p.buttons, p.x, p.y = 0, 0.0, 0.0
        for port, buttons, x, y, _ in held:
            p = core.ports[port]
            p.buttons |= buttons
            if x is not None:
                p.x = x
            if y is not None:
                p.y = y
        core.run_frame()
        # A screenshot shows the frame of the VI it was requested on; kig captures while holding that VI.
        if shot:
            core.screenshot(shot)
        if watch:
            values = tuple(core.read(addr, bits) for addr, bits in watch)
            if values != last_watch:
                last_watch = values
                log(f"[ref] watch vi={vi} t={vi / 60:.2f} " + " ".join(f"{addr:08X}={v:X}" for (addr, _), v in zip(watch, values)))
    raise Fail(f"stopped after {max_vis} VIs (step {pc + 1} of {len(steps)} not reached)")


# ---------------------------------------------------------------------------------------------
# Input calibration
# ---------------------------------------------------------------------------------------------
def calibrate(core):
    """Finds the game's copy of the port 1 OSContPad (where START reads as 0x1000) and checks every
    N64 input in N64_INPUTS lands on its bit there, and the stick on the stick bytes."""
    for _ in range(900):  # past the boot logos; the game polls the pads every frame from here
        core.run_frame()
    base = core.save_state()

    def held(spec, frames=6):
        core.load_state(base)
        p = core.ports[0]
        _, p.buttons, x, y = parse_spec(spec)
        p.x, p.y = x or 0.0, y or 0.0
        for _ in range(frames):
            core.run_frame()
        snap = core.rdram_be()
        p.buttons, p.x, p.y = 0, 0.0, 0.0
        return snap

    core.load_state(base)
    for _ in range(6):
        core.run_frame()
    idle = core.rdram_be()
    start = held("START")
    cands = [i for i in range(0, RDRAM_SIZE - 4, 2)
             if start[i:i + 2] == b"\x10\x00" and idle[i:i + 2] == b"\x00\x00"]
    if not cands:
        print("no OSContPad candidate found (START=0x1000)")
        return 1
    print(f"OSContPad candidates (START reads 0x1000): {', '.join(f'{0x80000000 + c:08X}' for c in cands[:8])}")
    bad = 0
    for name, bit in N64_BITS.items():
        snap = held(name)
        got = [int.from_bytes(snap[c:c + 2], "big") for c in cands]
        ok = any(g == bit for g in got)
        bad += not ok
        print(f"  {name:6} expect {bit:04X}: {' '.join(f'{g:04X}' for g in got[:8])} {'ok' if ok else 'MISMATCH'}")
    for spec, want in (("X=1", "stick_x > 0"), ("Y=1", "stick_y > 0")):
        snap = held(spec)
        vals = [(C.c_int8(snap[c + 2]).value, C.c_int8(snap[c + 3]).value) for c in cands]
        print(f"  {spec:6} ({want}): stick (x, y) = {vals[:8]}")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("script", nargs="?")
    ap.add_argument("--rom", default=DEFAULT_ROM)
    ap.add_argument("--core", default=DEFAULT_CORE)
    ap.add_argument("--out-dir", default=".")
    ap.add_argument("--eeprom-in")
    ap.add_argument("--eeprom-out")
    ap.add_argument("--pak-in", help="kig Controller Pak folder (saves/controllerpak_port1) to insert in port 1")
    ap.add_argument("--pak-out", help="write port 1's pak back as a kig Controller Pak folder")
    ap.add_argument("--max-vis", type=int, default=60 * 600)
    ap.add_argument("--option", action="append", default=[], metavar="KEY=VALUE", help="extra core option")
    ap.add_argument("--watch", default="", metavar="ADDR:BITS,...", help="log changes of these RAM values")
    ap.add_argument("--calibrate", action="store_true")
    a = ap.parse_args()

    out_dir = Path(a.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    work = out_dir / "core"
    work.mkdir(exist_ok=True)
    options = dict(kv.split("=", 1) for kv in a.option)
    if a.pak_in or a.pak_out:
        options["parallel-n64-pak1"] = "memory"
    core = Core(a.core, a.rom, work, options)
    if a.calibrate:
        return calibrate(core)
    if not a.script:
        ap.error("a scenario or .steps file is needed")

    save = core.save_ram()
    if a.eeprom_in:
        data = Path(a.eeprom_in).read_bytes()[:EEPROM_SIZE]
        C.memmove(C.addressof(save) + SAVE_EEPROM_OFF, data, len(data))
    if a.pak_in:
        sys.path.insert(0, str(Path(__file__).parent))
        from pak_convert import folder_to_image
        img = folder_to_image(Path(a.pak_in))
        C.memmove(C.addressof(save) + SAVE_MEMPACK_OFF, img, MEMPACK_SIZE)

    code = 0
    try:
        watch = [(int(w.split(":")[0], 16), int(w.split(":")[1]) if ":" in w else 8) for w in a.watch.split(",") if w]
        code = run_steps(core, load_steps(a.script), out_dir, a.max_vis, watch=watch)
    except Fail as e:
        print(f"[ref] step FAIL {e}")
        code = 3
    if a.eeprom_out:
        Path(a.eeprom_out).parent.mkdir(parents=True, exist_ok=True)
        Path(a.eeprom_out).write_bytes(bytes(save[SAVE_EEPROM_OFF:SAVE_EEPROM_OFF + EEPROM_SIZE]))
    if a.pak_out:
        from pak_convert import image_to_folder
        image_to_folder(bytes(save[SAVE_MEMPACK_OFF:SAVE_MEMPACK_OFF + MEMPACK_SIZE]), Path(a.pak_out))
    print(f"[ref] exit {code}")
    return code


if __name__ == "__main__":
    sys.exit(main())
