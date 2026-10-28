# Real-time capture series: starts kig.exe (test mode, own data dir), waits -At seconds, then grabs the
# window every -IntervalMs ms, -Count times, without pausing the game (unlike KIG_CAPTURE), so timing
# problems such as flicker show up. Frames go to build\rt\<Name>_NNN.png.
# Usage: tools\rt_capture.ps1 -At 12 -Count 120 [-IntervalMs 16] [-Script "..."] [-Name title] [-Frames 3600]
param([double]$At = 12, [int]$Count = 120, [int]$IntervalMs = 16, [string]$Script = "", [string]$Name = "rt",
      [int]$Frames = 3600)
Set-Location (Split-Path $PSScriptRoot -Parent)
$out = "build\rt"
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out\$($Name)_*.png" -ErrorAction SilentlyContinue
if ($Script) { $env:KIG_INPUT_SCRIPT = $Script }
$p = Start-Process -FilePath build\cmake\kig.exe -ArgumentList "--frames", "$Frames", "--data-dir", "build\run\data", "baserom.us.z64" `
    -RedirectStandardError build\run\stderr.txt -RedirectStandardOutput build\run\stdout.txt -PassThru
Remove-Item Env:KIG_INPUT_SCRIPT -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds ([int]($At * 1000))
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class RtCap {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[RtCap]::SetProcessDPIAware() | Out-Null
$p.Refresh()
$h = $p.MainWindowHandle
$r = New-Object RtCap+RECT
[RtCap]::GetClientRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hh = $r.B - $r.T
$sw = [Diagnostics.Stopwatch]::StartNew()
for ($i = 0; $i -lt $Count; $i++) {
    $bmp = New-Object Drawing.Bitmap $w, $hh
    $g = [Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [RtCap]::PrintWindow($h, $hdc, 3) | Out-Null
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save(("{0}\{1}_{2:000}.png" -f $out, $Name, $i), [Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $next = ($i + 1) * $IntervalMs
    $wait = $next - $sw.ElapsedMilliseconds
    if ($wait -gt 0) { Start-Sleep -Milliseconds $wait }
}
"captured $Count frames in $($sw.ElapsedMilliseconds) ms"
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
