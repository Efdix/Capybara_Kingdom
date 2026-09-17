# Capture melonDS at 1x and upscale, so BOTH DS screens always fit: the emulator
# letterboxes to the DS aspect ratio and silently clips a window that is too
# tall, which is what made earlier captures lose the bottom screen.
param([string]$Shot = "shot.png", [int]$Zoom = 3)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W5 {
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int t,bool r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct R { public int Left,Top,Right,Bottom; }
}
"@
$p = Get-Process -Name melonDS -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Error "melonDS not running"; exit 1 }
$h = $p.MainWindowHandle
[void][W5]::ShowWindow($h, 9)
[void][W5]::MoveWindow($h, 5, 5, 256+18, 192*2+96, $true)
[void][W5]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 1400
$r = New-Object W5+R
[void][W5]::GetClientRect($h, [ref]$r)
$w = $r.Right; $ht = $r.Bottom
$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
[void][W5]::PrintWindow($h, $dc, 2)
$g.ReleaseHdc($dc); $g.Dispose()
$big = New-Object System.Drawing.Bitmap(($w*$Zoom), ($ht*$Zoom))
$g2 = [System.Drawing.Graphics]::FromImage($big)
$g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g2.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$g2.DrawImage($bmp, 0, 0, $w*$Zoom, $ht*$Zoom)
$g2.Dispose()
$big.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png)
$big.Dispose(); $bmp.Dispose()
Write-Output "saved $Shot (${w}x${ht} x$Zoom)"
