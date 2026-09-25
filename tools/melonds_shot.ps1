# melonds_shot.ps1 -- capture ONLY the two DS screens from the melonDS window.
#
# melonds_grab.ps1 captures the whole window via PrintWindow, which includes the
# title bar and menu bar. That is fine for "is the game running", but useless
# for reviewing a UI: the chrome takes more pixels than the screens do.
#
# This crops to the client area and then to the two screen rects inside it,
# writing the top screen, the bottom screen, and a combined image for review.
#
# The screens are located by scanning for the first client row that is not a
# flat UI colour: the menu band above them is a uniform theme colour, the DS
# screens never are. Assuming a fixed offset breaks the moment the emulator's
# theme or menu layout changes.
#
# Usage:
#   powershell -File tools/melonds_shot.ps1 -Out .shots -Name title -Zoom 3

param(
    [string]$Out = ".shots",
    [string]$Name = "shot",
    [int]$Zoom = 3
)

Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W7 {
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int t,bool r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref P p);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct R { public int Left,Top,Right,Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct P { public int X,Y; }
}
"@

$proc = Get-Process -Name melonDS -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { Write-Error "melonDS not running"; exit 1 }
$hwnd = $proc.MainWindowHandle

# Size the window so the client area is the DS stack plus the menu band, then
# raise it. PrintWindow only returns real pixels for a window the compositor is
# actually drawing.
[void][W7]::ShowWindow($hwnd, 9)
[void][W7]::MoveWindow($hwnd, 4, 4, 256 + 18, 384 + 120, $true)
[void][W7]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 1500

$wr = New-Object W7+R
[void][W7]::GetWindowRect($hwnd, [ref]$wr)
$ww = $wr.Right - $wr.Left
$wh = $wr.Bottom - $wr.Top

$cr = New-Object W7+R
[void][W7]::GetClientRect($hwnd, [ref]$cr)
$cw = $cr.Right
$ch = $cr.Bottom

$org = New-Object W7+P
[void][W7]::ClientToScreen($hwnd, [ref]$org)
$offX = $org.X - $wr.Left
$offY = $org.Y - $wr.Top

$bmp = New-Object System.Drawing.Bitmap -ArgumentList @([int]$ww, [int]$wh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[void][W7]::PrintWindow($hwnd, $hdc, 2)
$g.ReleaseHdc($hdc)
$g.Dispose()

# Locate the DS stack inside the client area.
#
# The emulator letterboxes the screens with black and puts a flat-coloured menu
# band above them, and PrintWindow also picks up a few pixels of window border
# past the client edge -- so the screens cannot be found by either "first
# non-black row" or a fixed offset. Instead: stay strictly inside the client
# rect, and take the largest run of rows/columns that are neither black
# letterbox nor a flat UI colour.
function Is-Flat([System.Drawing.Color]$c) {
    # Letterbox black, or the light grey of the menu band.
    if ($c.R -lt 10 -and $c.G -lt 10 -and $c.B -lt 10) { return $true }
    if ([Math]::Abs($c.R - 243) -lt 6 -and [Math]::Abs($c.G - 243) -lt 6 -and
        [Math]::Abs($c.B - 243) -lt 6) { return $true }
    return $false
}

$cx0 = $offX
$cx1 = $offX + $cw - 1
$cy0 = $offY
$cy1 = $offY + $ch - 1

# A row belongs to the screens when a good part of it is non-flat.
$firstRow = -1
$lastRow = -1
for ($y = $cy0; $y -le $cy1; $y++) {
    $n = 0
    for ($x = $cx0; $x -le $cx1; $x += 4) {
        if (-not (Is-Flat $bmp.GetPixel($x, $y))) { $n++ }
    }
    if ($n -gt (($cx1 - $cx0) / 4) * 0.6) {
        if ($firstRow -lt 0) { $firstRow = $y }
        $lastRow = $y
    }
}

# Same for columns, measured only across the rows that belong to the screens.
$firstCol = -1
$lastCol = -1
if ($firstRow -ge 0) {
    for ($x = $cx0; $x -le $cx1; $x++) {
        $n = 0
        for ($y = $firstRow; $y -le $lastRow; $y += 4) {
            if (-not (Is-Flat $bmp.GetPixel($x, $y))) { $n++ }
        }
        if ($n -gt (($lastRow - $firstRow) / 4) * 0.6) {
            if ($firstCol -lt 0) { $firstCol = $x }
            $lastCol = $x
        }
    }
}

if ($firstRow -lt 0 -or $firstCol -lt 0) {
    Write-Error "could not locate the DS screens in the melonDS window"
    $bmp.Dispose(); exit 1
}

# One DS screen is 256x192 and the stack is two of them with no gap, so the
# height is the trustworthy dimension -- it is never confused by the window
# border the way the width can be. Derive the integer scale from it and take
# the width from the scale rather than from the measured columns.
$stackH = $lastRow - $firstRow + 1
$scale = [Math]::Max([Math]::Floor(($stackH + 1) / 384.0), 1)
$sw = 256 * $scale
$sh = 192 * $scale
$sx = $firstCol
$sy = $lastRow - ($sh * 2) + 1

if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Path $Out | Out-Null }

function Save-Zoomed($src, [int]$x, [int]$y, [int]$w, [int]$h, [string]$path, [int]$z) {
    $rect = New-Object System.Drawing.Rectangle -ArgumentList @($x, $y, $w, $h)
    $crop = $src.Clone($rect, $src.PixelFormat)
    $tw = [int]($w * $z)
    $th = [int]($h * $z)
    $big = New-Object System.Drawing.Bitmap -ArgumentList @($tw, $th)
    $gg = [System.Drawing.Graphics]::FromImage($big)
    $gg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    $gg.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $gg.DrawImage($crop, 0, 0, $tw, $th)
    $gg.Dispose()
    $big.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $big.Dispose()
    $crop.Dispose()
}

Save-Zoomed $bmp $sx $sy $sw $sh (Join-Path $Out "${Name}_top.png") $Zoom
Save-Zoomed $bmp $sx ($sy + $sh) $sw $sh (Join-Path $Out "${Name}_bot.png") $Zoom

# Combined image: both screens at a 4px gap, which is what a reviewer wants to
# look at -- a UI decision is almost never about one screen alone.
$gap = 4
$tw = [int]($sw * $Zoom)
$th = [int]($sh * $Zoom)
$both = New-Object System.Drawing.Bitmap -ArgumentList @($tw, ($th * 2 + $gap))
$gg = [System.Drawing.Graphics]::FromImage($both)
$gg.Clear([System.Drawing.Color]::FromArgb(255, 18, 18, 26))
$gg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$gg.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half

$rectT = New-Object System.Drawing.Rectangle -ArgumentList @($sx, $sy, $sw, $sh)
$rectB = New-Object System.Drawing.Rectangle -ArgumentList @($sx, ($sy + $sh), $sw, $sh)
$cropT = $bmp.Clone($rectT, $bmp.PixelFormat)
$cropB = $bmp.Clone($rectB, $bmp.PixelFormat)
$gg.DrawImage($cropT, 0, 0, $tw, $th)
$gg.DrawImage($cropB, 0, ($th + $gap), $tw, $th)
$gg.Dispose()
$both.Save((Join-Path $Out "${Name}_both.png"), [System.Drawing.Imaging.ImageFormat]::Png)
$both.Dispose()
$cropT.Dispose()
$cropB.Dispose()
$bmp.Dispose()

Write-Output "saved $Out/${Name}_{top,bot,both}.png  (client ${cw}x${ch}, screen ${sw}x${sh} @${scale}x, top=$firstRow)"
