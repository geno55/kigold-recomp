# Smoke run: kig.exe for N VIs of game time with a throwaway data dir, then prints the log.
# Usage (repo root, PowerShell): tools\smoke.ps1 [-Frames 600] [-Timeout 90] [-Build] [-Lines 80] [-Speed 1]
param([int]$Frames = 600, [int]$Timeout = 90, [switch]$Build, [int]$Lines = 80, [int]$Speed = 1)
Set-Location (Split-Path $PSScriptRoot -Parent)
if ($Build) {
    cmd /c "tools\cmake_build.bat kig"
    if ($LASTEXITCODE) { Get-Content build\cmake_build.log | Select-String -Pattern "error" | Select-Object -First 20; exit 1 }
}
New-Item -ItemType Directory -Force build\run | Out-Null
$argv = @("--frames", "$Frames", "--data-dir", "build\run\data")
if ($Speed -gt 1) { $argv += @("--speed", "$Speed") }
$argv += "baserom.us.z64"
$p = Start-Process -FilePath build\cmake\kig.exe -ArgumentList $argv -RedirectStandardError build\run\stderr.txt `
    -RedirectStandardOutput build\run\stdout.txt -PassThru
if (-not $p.WaitForExit($Timeout * 1000)) { $p.Kill(); "TIMEOUT after $Timeout s (killed)" } else { "exit $($p.ExitCode)" }
Get-Content build\run\stderr.txt -TotalCount $Lines
