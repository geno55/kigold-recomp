#!/usr/bin/env python3
"""Regenerate KIG symbols and native C code using Splat without changing source symbol notes.

Uses existing syms/libultra_symbols.txt. Regenerating that source requires the separate
W.G. 3D Hockey reference project (tools/kig_libsyms.py).
"""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent

def run(*args):
    print('+', ' '.join(map(str,args)), flush=True)
    subprocess.run(list(map(str,args)), cwd=ROOT, check=True)

def main():
    source = ROOT / 'syms/libultra_symbols.txt'
    dest = ROOT / 'build/libultra_symbols_splat.txt'
    dest.parent.mkdir(parents=True, exist_ok=True)
    # Preserve the original symbol file, which contains valuable identification notes.
    # Splat's parser treats `manual:` as an attribute and rejects its free-text reason.
    lines = [re.sub(r' manual:.*$', ' manual', line)
             for line in source.read_text().splitlines()]
    dest.write_text('\n'.join(lines) + '\n')

    yaml = (ROOT / 'splat/kig.us.yaml').read_text(encoding='utf-8-sig')
    marker = 'syms/libultra_symbols.txt'
    if yaml.count(marker) != 1:
        raise RuntimeError('Expected one symbol path in Splat YAML')
    config = ROOT / 'splat/kig.local.yaml'
    config.write_text(yaml.replace(marker, 'build/libultra_symbols_splat.txt'))

    run(sys.executable, 'tools/build_recomp_rom.py')
    run(sys.executable, '-m', 'splat', 'split', 'splat/kig.local.yaml')
    run(sys.executable, 'tools/export_splat.py')
    run(sys.executable, 'tools/kig_funcs.py')
    run(sys.executable, 'tools/gen_syms.py')
    run(sys.executable, 'tools/gen_config.py', '--no-check', '--n64recomp',
        ROOT / 'lib/N64Recomp/build/N64Recomp')

if __name__ == '__main__':
    main()
