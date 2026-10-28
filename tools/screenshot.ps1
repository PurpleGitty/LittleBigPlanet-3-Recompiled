# Starts the game, waits, saves a screenshot of its window, then stops it.
#   .\tools\screenshot.ps1 -Seconds 25 -Out analysis\screenshots\boot.png
param([int]$Seconds = 25, [string]$Out = "analysis\screenshots\shot.png", [string]$AutoPress = "", [int]$Count = 1)

$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

if ($AutoPress) { $env:LBP3_AUTOPRESS = $AutoPress }
$p = Start-Process -FilePath "$root\build\lbp3.exe" `
    -ArgumentList "--run", "--skip-intro" `
    -WorkingDirectory $root -RedirectStandardOutput "$root\analysis\shot_stdout.txt" `
    -RedirectStandardError "$root\analysis\shot_stderr.txt" -PassThru
$env:LBP3_AUTOPRESS = $null
Start-Sleep -Seconds $Seconds
$p.Refresh()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { Write-Host "no window found"; Stop-Process -Id $p.Id -Force; exit 1 }

for ($i = 0; $i -lt $Count; $i++) {
    $path = if ($Count -gt 1) { "$root\" + [IO.Path]::ChangeExtension($Out, $null) + "_$i.png" } else { "$root\$Out" }
    $r = New-Object Win+RECT
    [Win]::GetClientRect($hwnd, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1, $r.Right)), ([Math]::Max(1, $r.Bottom))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [Win]::PrintWindow($hwnd, $hdc, 3) | Out-Null   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc)
    New-Item -ItemType Directory -Force (Split-Path "$root\$Out") | Out-Null
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Start-Sleep -Milliseconds 150
}
Stop-Process -Id $p.Id -Force
Write-Host "saved $Out ($($r.Right)x$($r.Bottom))"
