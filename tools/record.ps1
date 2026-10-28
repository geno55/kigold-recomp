# Records what is actually on screen under the kig.exe window (desktop capture, 60 fps) with ffmpeg,
# then extracts the frames: build\rec\<Name>_NNNN.png. Starts kig.exe itself (test mode).
# Usage: tools\record.ps1 -At 13 -Seconds 4 [-Script "..."] [-Name title] [-Frames 3600]
param([double]$At = 13, [double]$Seconds = 4, [string]$Script = "", [string]$Name = "rec", [int]$Frames = 3600)
Set-Location (Split-Path $PSScriptRoot -Parent)
$ffmpeg = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter ffmpeg.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -Expand FullName
$out = "build\rec"
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out\$($Name)_*.png", "$out\$Name.mkv" -ErrorAction SilentlyContinue
if ($Script) { $env:KIG_INPUT_SCRIPT = $Script }
$p = Start-Process -FilePath build\cmake\kig.exe -ArgumentList "--frames", "$Frames", "--data-dir", "build\run\data", "baserom.us.z64" `
    -RedirectStandardError build\run\stderr.txt -RedirectStandardOutput build\run\stdout.txt -PassThru
Remove-Item Env:KIG_INPUT_SCRIPT -ErrorAction SilentlyContinue
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class RecWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[RecWin]::SetProcessDPIAware() | Out-Null
Start-Sleep -Milliseconds ([int]($At * 1000) - 600)
$p.Refresh()
$h = $p.MainWindowHandle
# (no SetForegroundWindow: focus would let keystrokes reach the game)
$r = New-Object RecWin+RECT; [RecWin]::GetClientRect($h, [ref]$r) | Out-Null
$pt = New-Object RecWin+POINT; [RecWin]::ClientToScreen($h, [ref]$pt) | Out-Null
$w = ($r.R - $r.L) -band -2; $hh = ($r.B - $r.T) -band -2
& $ffmpeg -hide_banner -loglevel error -y -f gdigrab -framerate 60 -offset_x $pt.X -offset_y $pt.Y -video_size "$($w)x$($hh)" `
    -t $Seconds -i desktop -c:v ffv1 "$out\$Name.mkv"
& $ffmpeg -hide_banner -loglevel error -y -i "$out\$Name.mkv" -vf "scale=320:240" "$out\$($Name)_%04d.png"
"recorded $((Get-ChildItem "$out\$($Name)_*.png").Count) frames ($($w)x$($hh) at $($pt.X),$($pt.Y))"
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
