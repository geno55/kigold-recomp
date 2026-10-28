# Rebuilds the symbol pipeline end to end and runs N64Recomp:
#   build_recomp_rom -> splat -> export_splat -> kig_funcs -> kig_libsyms -> gen_syms -> gen_config
# Usage (from the repo root, PowerShell): tools\regen_syms.ps1 [-NoSplat] [-Check]
param([switch]$NoSplat, [switch]$Check)
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot -Parent)
python tools\build_recomp_rom.py | Out-Null
if (-not $NoSplat) {
    python -m splat split splat/kig.us.yaml *> build\splat.log
    if ($LASTEXITCODE) { Get-Content build\splat.log -Tail 20; exit 1 }
}
python tools\export_splat.py | Select-Object -First 4
python tools\kig_funcs.py
if ($LASTEXITCODE) { exit 1 }
python tools\kig_libsyms.py
python tools\gen_syms.py
if ($LASTEXITCODE) { exit 1 }
if ($Check) { python tools\gen_config.py } else { python tools\gen_config.py --no-check }
exit $LASTEXITCODE
