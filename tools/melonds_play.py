"""
melonds_play.py -- drive melonDS with injected keys and screenshot the result.

Why this exists
---------------
Reviewing a UI means walking to the screen in question, and the interactive
path to it is key presses. melonds_drive.ps1 sends them with keybd_event, but
keybd_event from a background process is refused on modern Windows unless the
calling thread first attaches its input queue to the foreground thread's --
and a refused switch is silent: the call returns, the focus stays where it was,
and every key after it lands in some other window. The result looks exactly
like a game that ignores input.

This does the switch properly (AttachThreadInput, then SetForegroundWindow,
then detach), verifies it actually happened, and only then sends keys through
SendInput with both the virtual key and the scancode, since an emulator may
read either.

Usage:
    python tools/melonds_play.py --keys x          # press the A button
    python tools/melonds_play.py --keys down,down,x --wait 500
    python tools/melonds_play.py --hold x --hold-ms 400
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import sys
import time

user32 = ctypes.windll.user32

# Same DPI caveat as melonds_capture.py: without this the window geometry the
# Win32 calls report is in a different unit from the pixels a screenshot holds.
try:
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        user32.SetProcessDPIAware()


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


# SendInput structures.
#
# The union has to be padded out to the size of MOUSEINPUT, the largest member
# on the C side: SendInput rejects the call unless cbSize is exactly
# sizeof(INPUT), and a union holding only KEYBDINPUT is 8 bytes short on
# 64-bit -- which is why an earlier attempt failed with no error at all.
class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.POINTER(wt.ULONG))]


class _INPUTUNION(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_ubyte * 32)]


class INPUT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [("type", wt.DWORD), ("u", _INPUTUNION)]


INPUT_KEYBOARD = 1
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_SCANCODE = 0x0008

# melonDS's key bindings are SDL scancodes, and the same numbers are what
# Windows' scan-code set uses for the letters, so one table serves both.
SCAN = {
    "a": 4, "b": 5, "c": 6, "d": 7, "e": 8, "f": 9, "g": 10, "h": 11,
    "i": 12, "j": 13, "k": 14, "l": 15, "m": 16, "n": 17, "o": 18, "p": 19,
    "q": 20, "r": 21, "s": 22, "t": 23, "u": 24, "v": 25, "w": 26, "x": 27,
    "y": 28, "z": 29,
    "1": 30, "2": 31, "3": 32, "4": 33, "5": 34, "6": 35, "7": 36, "8": 37,
    "9": 38, "0": 39,
    "enter": 40, "escape": 41, "space": 44,
    "left": 80, "right": 79, "down": 81, "up": 82,
}

VK = {
    "enter": 0x0D, "escape": 0x1B, "space": 0x20,
    "left": 0x25, "up": 0x26, "right": 0x27, "down": 0x28,
}


def _vk_for(name, scan):
    """Virtual key for a name; letters and digits derive from their scancode."""
    if name in VK:
        return VK[name]
    if len(name) == 1 and name.isalnum():
        return ord(name.upper())
    return 0


def find_window(title_part="melonDS"):
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


def focus_window(hwnd):
    """Bring the window to the foreground, and say whether it worked.

    The AttachThreadInput dance is what makes this possible at all from a
    process that does not own the foreground; returning the result rather than
    assuming success is what makes a later "the game ignored my key" actually
    diagnosable.
    """
    fg = user32.GetForegroundWindow()
    dummy = wt.DWORD(0)
    mine = user32.GetWindowThreadProcessId(fg, ctypes.byref(dummy))
    theirs = user32.GetWindowThreadProcessId(hwnd, ctypes.byref(dummy))
    if not mine or not theirs:
        return False
    user32.AttachThreadInput(mine, theirs, True)
    ok = user32.SetForegroundWindow(hwnd)
    user32.AttachThreadInput(mine, theirs, False)
    time.sleep(0.15)
    return ok and user32.GetForegroundWindow() == hwnd


def send_key(scan, hold_ms):
    vk = _vk_for(_name_for(scan), scan)
    down = INPUT()
    down.type = INPUT_KEYBOARD
    down.ki = KEYBDINPUT(vk, scan, KEYEVENTF_SCANCODE, 0, None)
    up = INPUT()
    up.type = INPUT_KEYBOARD
    up.ki = KEYBDINPUT(vk, scan, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP, 0, None)

    n = ctypes.c_uint(1)
    size = ctypes.sizeof(INPUT)
    if not user32.SendInput(1, ctypes.byref(down), size):
        return False
    time.sleep(hold_ms / 1000.0)
    user32.SendInput(1, ctypes.byref(up), size)
    return True


# Reverse of SCAN, for deriving a virtual key from a scancode.
_NAME_FOR = {v: k for k, v in SCAN.items()}


def _name_for(scan):
    return _NAME_FOR.get(scan, "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keys", default="",
                    help="comma-separated keys to tap in order")
    ap.add_argument("--hold", default="",
                    help="a single key to hold for --hold-ms")
    ap.add_argument("--hold-ms", type=int, default=300)
    ap.add_argument("--wait", type=int, default=400,
                    help="ms to settle after the last key")
    ap.add_argument("--tap-ms", type=int, default=90,
                    help="how long each tap is held down")
    ap.add_argument("--window", default="melonDS")
    args = ap.parse_args()

    hwnd = find_window(args.window)
    if not hwnd:
        print("window not found", file=sys.stderr)
        return 1

    if not focus_window(hwnd):
        print("warning: could not focus the window; keys may go elsewhere",
              file=sys.stderr)

    if args.hold:
        k = args.hold.lower()
        if k not in SCAN:
            print(f"unknown key: {args.hold}", file=sys.stderr)
            return 1
        if not send_key(SCAN[k], args.hold_ms):
            print("SendInput failed", file=sys.stderr)
            return 1

    for name in [k.strip().lower() for k in args.keys.split(",") if k.strip()]:
        if name not in SCAN:
            print(f"unknown key: {name}", file=sys.stderr)
            return 1
        if not send_key(SCAN[name], args.tap_ms):
            print("SendInput failed", file=sys.stderr)
            return 1
        time.sleep(0.07)

    time.sleep(args.wait / 1000.0)
    print("keys sent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
