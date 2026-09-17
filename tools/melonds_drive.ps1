# melonds_drive.ps1 -- drive the melonDS emulator window and capture frames.
#
# Why this exists: the interactive GUI automation layer cannot deliver input to
# the emulator window (another window owns the pixel), so the play-test loop
# drives melonDS through the Win32 input API instead and grabs screenshots via
# GDI. Keys use SDL *scancodes* because that is what melonDS writes into its
# config for keyboard bindings.
#
# Usage:
#   powershell -File tools/melonds_drive.ps1 -Keys "x" -Wait 500 -Shot out.png
#   powershell -File tools/melonds_drive.ps1 -Keys "Up,Up,x" -Wait 300 -Shot out.png
#   powershell -File tools/melonds_drive.ps1 -Hold "Up" -HoldMs 1200 -Shot out.png

param(
    [string]$Keys = "",          # comma-separated key names, pressed in order
    [string]$Hold = "",          # a single key to hold down
    [int]$HoldMs = 800,
    [int]$Wait = 400,            # ms to wait before the screenshot
    [string]$Shot = "",          # output PNG path (optional)
    [switch]$ListWindows
)

Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32 {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowDC(IntPtr h);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public const uint KEYEVENTF_KEYUP = 0x0002;
    public const uint KEYEVENTF_SCANCODE = 0x0008;
}
"@

# melonDS keyboard bindings are SDL scancodes; these are the Windows virtual-key
# codes that produce the same scancodes when sent with KEYEVENTF_SCANCODE.
# (Passing the SDL scancode directly as the scan code is what SDL itself does.)
$scancodes = @{
    "a"=4; "b"=5; "c"=6; "d"=7; "e"=8; "f"=9; "g"=10; "h"=11; "i"=12; "j"=13;
    "k"=14; "l"=15; "m"=16; "n"=17; "o"=18; "p"=19; "q"=20; "r"=21; "s"=22;
    "t"=23; "u"=24; "v"=25; "w"=26; "x"=27; "y"=28; "z"=29;
    "1"=30; "2"=31; "3"=32; "4"=33; "5"=34; "6"=35; "7"=36; "8"=37;
    "9"=38; "0"=39;
    "enter"=40; "escape"=41; "backspace"=42; "tab"=43; "space"=44;
    "left"=80; "right"=79; "down"=81; "up"=82;
}

function Get-MelonWindow {
    $p = Get-Process -Name melonDS -ErrorAction SilentlyContinue |
         Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { return [IntPtr]::Zero }
    return $p.MainWindowHandle
}

if ($ListWindows) {
    Get-Process -Name melonDS -ErrorAction SilentlyContinue |
        ForEach-Object { "$($_.Id) $($_.MainWindowTitle) $($_.MainWindowHandle)" }
    exit 0
}

$hwnd = Get-MelonWindow
if ($hwnd -eq [IntPtr]::Zero) { Write-Error "melonDS window not found"; exit 1 }

# Raise the emulator before capturing or sending input. PrintWindow only
# returns real pixels for a window the compositor is actually drawing; for an
# occluded window it yields an all-black bitmap, which is indistinguishable
# from a rendering bug in the game.
[void][Win32]::ShowWindow($hwnd, 9)   # SW_RESTORE
[void][Win32]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 250

function Send-Scancode([int]$code, [int]$holdMs) {
    # Send the scancode directly, exactly as SDL reports it.
    [Win32]::keybd_event(0, [byte]$code, [Win32]::KEYEVENTF_SCANCODE, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $holdMs
    [Win32]::keybd_event(0, [byte]$code, ([Win32]::KEYEVENTF_SCANCODE -bor [Win32]::KEYEVENTF_KEYUP), [UIntPtr]::Zero)
}

if ($Hold -ne "") {
    $k = $Hold.ToLower()
    if (-not $scancodes.ContainsKey($k)) { Write-Error "unknown key: $Hold"; exit 1 }
    Send-Scancode $scancodes[$k] $HoldMs
}

if ($Keys -ne "") {
    foreach ($k0 in $Keys.Split(",")) {
        $k = $k0.Trim().ToLower()
        if ($k -eq "") { continue }
        if (-not $scancodes.ContainsKey($k)) { Write-Error "unknown key: $k"; exit 1 }
        # 90 ms is enough for the game loop to sample keysDown().
        Send-Scancode $scancodes[$k] 90
        Start-Sleep -Milliseconds 70
    }
}

Start-Sleep -Milliseconds $Wait

if ($Shot -ne "") {
    $r = New-Object Win32+RECT
    [void][Win32]::GetWindowRect($hwnd, [ref]$r)
    $w = $r.Right - $r.Left
    $h = $r.Bottom - $r.Top
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    # PW_RENDERFULLCONTENT (2) captures GPU-composited window content.
    [void][Win32]::PrintWindow($hwnd, $hdc, 2)
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    $bmp.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output "saved $Shot ($w x $h)"
} else {
    Write-Output "keys sent"
}
