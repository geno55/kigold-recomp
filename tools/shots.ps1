# Runs kig.exe with an input script and captures screenshots, then writes a contact sheet.
# Usage (repo root, PowerShell): tools\shots.ps1 -Script "20:START;22:A" -Times 21,25,30 [-Frames 2400] [-Exe build\cmake\kig.exe]
param([string]$Script = "", [double[]]$Times = @(5, 10), [int]$Frames = 2400, [int]$Timeout = 120,
      [string]$Exe = "build\cmake\kig.exe", [int]$Cols = 3)
Set-Location (Split-Path $PSScriptRoot -Parent)
$dir = "build\run\shots"
Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $dir | Out-Null
$env:KIG_CAPTURE = ($Times | ForEach-Object { "{0}:t{1:000.000}" -f $_, $_ }) -join ","
$env:KIG_CAPTURE_DIR = $dir
if ($Script) { $env:KIG_INPUT_SCRIPT = $Script }
$p = Start-Process -FilePath $Exe -ArgumentList "--frames", "$Frames", "--data-dir", "build\run\data", "baserom.us.z64" `
    -RedirectStandardError build\run\stderr.txt -RedirectStandardOutput build\run\stdout.txt -PassThru
if (-not $p.WaitForExit($Timeout * 1000)) { $p.Kill(); "TIMEOUT (killed)" } else { "exit $($p.ExitCode)" }
Remove-Item Env:KIG_CAPTURE, Env:KIG_CAPTURE_DIR, Env:KIG_INPUT_SCRIPT -ErrorAction SilentlyContinue
python tools\contact_sheet.py "$dir\*.bmp" -o "$dir\sheet.png" --cols $Cols --width 400
Get-Content build\run\stderr.txt | Select-String -Pattern "CRASH|loaded|unloaded|error|Failed|osMapTLB|smoke" | Select-Object -First 30
