# Timed capture series: boots the game in ares (reference) or kig.exe and screenshots
# its window every -IntervalMs ms, -Count times (build\<Name>_NNN.png), then closes it.
# Usage: tools\capture_series.ps1 [-Target ares|kig] [-Count 30] [-IntervalMs 1000] [-Name <target>]
# kig's output goes to build\<Name>.out.txt / .err.txt; set KIG_* variables in the caller's environment.
param([string]$Target = "ares", [int]$Count = 30, [int]$IntervalMs = 1000, [string]$Name = "")
if (-not $Name) { $Name = $Target }
$root = Split-Path -Parent $PSScriptRoot
if ($Target -eq "ares") {
    $ares = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter ares.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $ares) { "[capture] ares.exe not found (winget install ares-emulator.ares)"; exit 1 }
    $rom = Join-Path $root "baserom.us.z64"
    $p = Start-Process -FilePath $ares.FullName -ArgumentList @("--kiosk", "--no-file-prompt", "--system", "`"Nintendo 64`"", "`"$rom`"") -PassThru
} else {
    $exe = Join-Path $root "build\cmake\kig.exe"
    $p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru -ArgumentList "--frames", "1000000", "`"$(Join-Path $root 'baserom.us.z64')`"" `
        -RedirectStandardOutput (Join-Path $root "build\$Name.out.txt") -RedirectStandardError (Join-Path $root "build\$Name.err.txt")
}
$start = Get-Date
# Wait (up to 15 s) for the window; the series is timed from process start.
for ($i = 0; $i -lt 150; $i++) {
    $p.Refresh()
    if ($p.MainWindowHandle -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 100
}
"[capture] $Target window after {0:N1}s" -f ((Get-Date) - $start).TotalSeconds
& (Join-Path $PSScriptRoot "shot_kig.ps1") -Process $Target -Name $Name -Count $Count -IntervalMs $IntervalMs | Select-Object -Last 1
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
