"""
burst_capture.py -- grab many screenshots in quick succession from melonDS.

melonds_capture.py starts a fresh Python process per shot, which costs a couple
of seconds each; a duel's interesting frames (the intro card, a telegraph, a
stagger) last well under a second, so that cadence misses the very things worth
photographing. This keeps one process alive and takes a shot on a fixed
interval.

Usage:
    python tools/burst_capture.py --count 40 --interval 0.35 --prefix duel
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

try:
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        user32.SetProcessDPIAware()


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", wt.WORD),
                ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


def find_window():
    """The melonDS window, by title. Its title carries the fps counter."""
    found = []

    def cb(h, _):
        if not user32.IsWindowVisible(h):
            return True
        n = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(h, n, 256)
        if "melonDS" in n.value:
            found.append(h)
        return True

    CB = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    user32.EnumWindows(CB(cb), 0)
    return found[0] if found else None


def client_rect(hwnd):
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    return r.right, r.bottom


def grab(hwnd, w, h, out):
    """PrintWindow straight into a DIB, then write it out as a PNG."""
    hdc = user32.GetDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    old = gdi32.SelectObject(mem, bmp)
    # PW_CLIENTONLY: the client area is the two DS screens, no chrome.
    user32.PrintWindow(hwnd, mem, 1)
    gdi32.SelectObject(mem, old)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)

    # Re-grab through a DIB section so the pixels can be read directly; the
    # compatible bitmap above is fine for PrintWindow but not for GetDIBits.
    hdc = user32.GetDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmi = BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.biWidth = w
    bmi.biHeight = -h      # top-down
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    ppv = ctypes.c_void_p()
    dib = gdi32.CreateDIBSection(hdc, ctypes.byref(bmi), 0,
                                 ctypes.byref(ppv), None, 0)
    old = gdi32.SelectObject(mem, dib)
    user32.PrintWindow(hwnd, mem, 1)
    buf = ctypes.create_string_buffer(w * h * 4)
    ctypes.memmove(buf, ppv, w * h * 4)
    gdi32.SelectObject(mem, old)
    gdi32.DeleteObject(dib)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)

    from PIL import Image
    img = Image.frombuffer("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1)
    img.convert("RGB").save(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=".shots")
    ap.add_argument("--prefix", default="burst")
    ap.add_argument("--count", type=int, default=30)
    ap.add_argument("--interval", type=float, default=0.35)
    ap.add_argument("--delay", type=float, default=0.0,
                    help="seconds to wait before the first shot")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    hwnd = find_window()
    if not hwnd:
        print("no melonDS window found")
        return 1
    w, h = client_rect(hwnd)
    print(f"window {hwnd}: client {w}x{h}")

    if args.delay:
        time.sleep(args.delay)

    for i in range(args.count):
        out = os.path.join(args.out, f"{args.prefix}{i:03d}.png")
        grab(hwnd, w, h, out)
        time.sleep(args.interval)
    print(f"captured {args.count} frames into {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
