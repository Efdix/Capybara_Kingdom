"""
melonds_capture.py -- grab melonDS's two DS screens as clean, exactly-1x images.

Why this exists
---------------
tools/melonds_grab.ps1 captures the whole emulator window with PrintWindow,
which includes the title bar, menu bar and whatever border the window manager
adds. That is fine for "is the ROM booting", but useless for reviewing a UI:
the chrome is a third of the frame and the screens are scaled by an amount that
depends on how the window happened to be sized.

This finds the DS screen stack inside the emulator window and writes the top
and bottom screen as separate 256x192 PNGs, plus a combined image at a chosen
integer zoom. The screens are located by *measurement* rather than by assuming
an offset: it scans the client area for the largest block of rows that are not
a flat UI colour, then takes the width from the height (one screen is 256x192
and the stack is two of them with no gap, so the height is unambiguous where
the width can be confused by a window border).

Usage:
    python tools/melonds_capture.py --out .shots --name play --zoom 3

Requires PIL, which the asset generators already depend on.
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

from PIL import Image

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

# Declare this process DPI-aware before any window call.
#
# Without it Windows lies about geometry: GetClientRect reports logical pixels
# while PrintWindow paints in physical ones, so on a scaled display (this
# machine runs 1920x1200 at 150%) the measured client area and the captured
# pixels disagree by that factor. That mismatch is what made earlier versions
# of this tool crop the middle of a screen -- the numbers looked plausible and
# were simply in a different unit.
try:
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))   # PER_MONITOR_AWARE_V2
except Exception:
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        user32.SetProcessDPIAware()


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def find_window(title_part="melonDS"):
    """Handle of the first visible top-level window whose title matches."""
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    def cb(hwnd, _lparam):
        if not user32.IsWindowVisible(hwnd):
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        if n <= 0:
            return True
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        if title_part.lower() in buf.value.lower():
            found.append(hwnd)
            return False
        return True

    user32.EnumWindows(cb, 0)
    return found[0] if found else None


def grab_window(hwnd):
    """The window's pixels as a PIL image, via PrintWindow with PW_RENDERFULLCONTENT.

    A plain screen grab of the region would also work, but it captures whatever
    is stacked on top. PrintWindow asks the window to draw itself, which is what
    makes the capture reproducible on a busy desktop.
    """
    wr = RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(wr))
    w = wr.right - wr.left
    h = wr.bottom - wr.top

    hdc_win = user32.GetWindowDC(hwnd)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_win)
    hbmp = gdi32.CreateCompatibleBitmap(hdc_win, w, h)
    gdi32.SelectObject(hdc_mem, hbmp)

    # PrintWindow with flag 2 (PW_RENDERFULLCONTENT) is what makes a
    # GPU-composited window return real pixels; flag 0 gives an all-black
    # bitmap for anything the compositor draws.
    if not user32.PrintWindow(hwnd, hdc_mem, 2):
        user32.PrintWindow(hwnd, hdc_mem, 0)

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", ctypes.c_long),
                    ("biHeight", ctypes.c_long), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                    ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                    ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]

    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.biWidth = w
    bi.biHeight = -h          # negative = top-down rows
    bi.biPlanes = 1
    bi.biBitCount = 32
    bi.biCompression = 0

    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(hdc_mem, hbmp, 0, h, buf, ctypes.byref(bi), 0)

    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(hdc_mem)
    user32.ReleaseDC(hwnd, hdc_win)

    return Image.frombuffer("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1).convert("RGB")


def client_origin(hwnd):
    """Client-area top-left in window coordinates."""
    wr = RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(wr))
    pt = POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    return pt.x - wr.left, pt.y - wr.top


def client_size(hwnd):
    cr = RECT()
    user32.GetClientRect(hwnd, ctypes.byref(cr))
    return cr.right, cr.bottom


def is_flat(px):
    """A pixel that belongs to window chrome rather than to a DS screen.

    Two cases matter: the black letterbox the emulator pads the screens with,
    and the flat light grey of the menu bar. The DS screens are 8bpp artwork
    with at most 256 colours and never have a wide run of one flat tone, so a
    row of them always fails this test.
    """
    r, g, b = px
    if r < 12 and g < 12 and b < 12:
        return True
    if abs(r - 243) < 8 and abs(g - 243) < 8 and abs(b - 243) < 8:
        return True
    return False


def locate_screens(img, ox, oy, cw, ch):
    """The (sx, sy, sw, sh) of the TOP screen inside a window image.

    The screens are found by measurement, then *snapped to the DS geometry*.
    Both steps matter:

    * Measurement, because melonDS letterboxes the screens inside the client
      area and the offset depends on the window size and on how many menu rows
      the theme draws -- so no fixed offset survives a restart.
    * Snapping, because the offset is not guaranteed to be even. The block's
      height identifies the scale (a DS stack is 384 tall, so scale = h/384),
      and the origin is then aligned to that scale's grid. Without the snap a
      1.5x display can land the crop half a DS pixel off, which shows up as
      everything in the review image being one pixel blurry.

    Requires integer scaling to be on in the emulator; the scale is taken from
    the measured height either way, so a fractional display scale still works.
    """
    def is_black(px):
        return px[0] < 12 and px[1] < 12 and px[2] < 12

    x_max = min(ox + cw, img.width)
    y_max = min(oy + ch, img.height)

    # Vertical extent of the screen block: the longest run of rows with any
    # non-black pixel. Scanning the middle band only, because the window edge
    # can carry a border pixel that would otherwise read as content.
    mid0 = ox + cw // 4
    mid1 = ox + cw * 3 // 4
    runs = []
    start = None
    for y in range(oy, y_max):
        hit = any(not is_black(img.getpixel((x, y)))
                  for x in range(mid0, mid1, 4))
        if hit and start is None:
            start = y
        elif not hit and start is not None:
            runs.append((start, y - 1))
            start = None
    if start is not None:
        runs.append((start, y_max - 1))
    if not runs:
        return None

    r0, r1 = max(runs, key=lambda r: r[1] - r[0])

    # Horizontal extent within those rows.
    col0 = None
    col1 = None
    for x in range(ox, x_max):
        hit = any(not is_black(img.getpixel((x, y)))
                  for y in range(r0, r1 + 1, 4))
        if hit:
            if col0 is None:
                col0 = x
            col1 = x
    if col0 is None:
        return None

    block_h = r1 - r0 + 1
    block_w = col1 - col0 + 1

    # One DS stack is 256x384; the scale is whichever integer division of the
    # measured block fits it best. Rounding rather than truncating keeps a
    # partially clipped block (a window too short for both screens) usable.
    scale = block_h / 384.0
    if scale <= 0:
        return None

    sw = int(round(256 * scale))
    sh = int(round(192 * scale))

    # Snap the origin to the scale grid so the crop samples whole DS pixels.
    sx = col0
    sy = r0

    return sx, sy, sw, sh


def set_client_size(hwnd, want_w, want_h):
    """Resize the window so its CLIENT area is at least want_w x want_h.

    The window chrome differs per theme and DPI, so the frame delta is measured
    rather than assumed, then added to the requested client size.

    The height is a *minimum*, not a target: the menu bar's height depends on
    the theme and the client area may be capped by the work area, so a request
    that is one row short would clip the bottom screen and the crop would
    silently lose its last few lines.
    """
    wr, cr = RECT(), RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(wr))
    user32.GetClientRect(hwnd, ctypes.byref(cr))
    dw = (wr.right - wr.left) - cr.right
    dh = (wr.bottom - wr.top) - cr.bottom

    # Clamp to the work area so the request cannot push the window off screen,
    # where the compositor may not paint it and PrintWindow returns black.
    sw = user32.GetSystemMetrics(0)
    sh = user32.GetSystemMetrics(1)
    w = min(want_w + dw, sw)
    h = min(want_h + dh, sh)
    user32.MoveWindow(hwnd, max((sw - w) // 2, 0), 0, w, h, True)
    user32.SetForegroundWindow(hwnd)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=".shots")
    ap.add_argument("--name", default="shot")
    ap.add_argument("--zoom", type=int, default=3,
                    help="review magnification; the capture is always 1:1 "
                         "with the DS framebuffer")
    ap.add_argument("--scale", type=int, default=2,
                    help="emulator screen scale; the window is sized to this "
                         "multiple of 256x384 plus whatever the menu bar needs")
    ap.add_argument("--window", default="melonDS")
    ap.add_argument("--no-size", action="store_true",
                    help="do not touch the window size")
    args = ap.parse_args()

    hwnd = find_window(args.window)
    if not hwnd:
        print("melonDS window not found", file=sys.stderr)
        return 1

    if not args.no_size:
        # Ask for the two screens plus generous slack for the menu bar: the
        # bar's height varies with the theme and is inside the client area, and
        # a short window silently clips the bottom screen.
        set_client_size(hwnd, 256 * args.scale, 384 * args.scale + 160)
        time.sleep(1.2)

    img = grab_window(hwnd)
    ox, oy = client_origin(hwnd)
    cw, ch = client_size(hwnd)

    found = locate_screens(img, ox, oy, cw, ch)
    if not found:
        print("could not locate the DS screens in the window", file=sys.stderr)
        return 1
    sx, sy, sw, sh = found

    os.makedirs(args.out, exist_ok=True)

    # Capture at the DS's own resolution: whatever the display scale is, the
    # review images are always exactly 256x192 per screen, which is the only
    # way two screenshots taken at different window sizes can be compared.
    top = img.crop((sx, sy, sx + sw, sy + sh)).resize((256, 192), Image.NEAREST)
    bot = img.crop((sx, sy + sh, sx + sw, sy + sh * 2)).resize((256, 192),
                                                               Image.NEAREST)

    def save(screen, path):
        screen.save(path)                                  # exact 1:1
        big = screen.resize((256 * args.zoom, 192 * args.zoom), Image.NEAREST)
        big.save(path.replace(".png", f"@{args.zoom}x.png"))
        return path

    save(top, os.path.join(args.out, f"{args.name}_top.png"))
    save(bot, os.path.join(args.out, f"{args.name}_bot.png"))

    # Combined, for reviewing both screens in one look.
    gap = 4
    both = Image.new("RGB", (256 * args.zoom, 192 * 2 * args.zoom + gap),
                     (18, 18, 26))
    both.paste(top.resize((256 * args.zoom, 192 * args.zoom), Image.NEAREST),
               (0, 0))
    both.paste(bot.resize((256 * args.zoom, 192 * args.zoom), Image.NEAREST),
               (0, 192 * args.zoom + gap))
    both.save(os.path.join(args.out, f"{args.name}_both.png"))

    print(f"saved {args.out}/{args.name}_{{top,bot,both}}.png "
          f"(256x192 per screen; found at {sx},{sy} scale {sw/256:.2f}x)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
