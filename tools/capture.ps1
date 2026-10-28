# Saves a screenshot of the already-running game window (doesn't start or stop it).
#   .\tools\capture.ps1 -Out analysis\screenshots\now.png [-Delay 30]
param([string]$Out = "analysis\screenshots\now.png", [int]$Delay = 0)

$root = Split-Path $PSScriptRoot -Parent
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class WinCap {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
if ($Delay -gt 0) { Start-Sleep -Seconds $Delay }
$p = Get-Process lbp3 -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p -or $p.MainWindowHandle -eq [IntPtr]::Zero) { Write-Host "game not running"; exit 1 }
$r = New-Object WinCap+RECT
[WinCap]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1, $r.Right)), ([Math]::Max(1, $r.Bottom))
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[WinCap]::PrintWindow($p.MainWindowHandle, $hdc, 3) | Out-Null
$g.ReleaseHdc($hdc)
New-Item -ItemType Directory -Force (Split-Path "$root\$Out") | Out-Null
$bmp.Save("$root\$Out", [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "saved $Out"
