"""What N64Recomp skips and what the runtime implements (shared by the config and audit tools)."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def list_from_cpp(path: Path, var: str) -> set[str]:
    s = path.read_text(encoding="utf-8")
    a = s.index(f"N64Recomp::{var} {{")
    return set(re.findall(r'"([^"]+)"', s[a:s.index("};", a)]))


def skipped_funcs() -> set[str]:
    sl = ROOT / "lib/N64Recomp/src/symbol_lists.cpp"
    return list_from_cpp(sl, "reimplemented_funcs") | list_from_cpp(sl, "ignored_funcs")


def runtime_funcs() -> set[str]:
    out = set()
    for d in ("librecomp/src", "ultramodern/src"):
        for f in (ROOT / "lib/N64ModernRuntime" / d).glob("*.cpp"):
            out |= set(re.findall(r"void\s+([A-Za-z0-9_]+)_recomp\s*\(", f.read_text(encoding="utf-8", errors="replace")))
    return out
