#!/usr/bin/env python3
"""Linux counterpart of tools/regen_syms.ps1: rebuilds the symbols and runs N64Recomp."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent

def run(*args, check=True):
    print('+', ' '.join(map(str,args)), flush=True)
    subprocess.run(list(map(str,args)), cwd=ROOT, check=check)

def main():
    run(sys.executable, 'tools/build_recomp_rom.py')
    run(sys.executable, '-m', 'splat', 'split', 'splat/kig.us.yaml')
    # Exits 1 whenever it lists known problems, so its exit code doesn't mean the step failed.
    run(sys.executable, 'tools/export_splat.py', check=False)
    run(sys.executable, 'tools/kig_funcs.py')
    run(sys.executable, 'tools/gen_syms.py')
    run(sys.executable, 'tools/gen_config.py', '--no-check', '--n64recomp',
        ROOT / 'lib/N64Recomp/build/N64Recomp')

if __name__ == '__main__':
    main()
