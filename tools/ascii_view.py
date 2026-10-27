#!/usr/bin/env python3
"""ascii_view.py -- render generated pixel art as text, so a reviewer who
cannot look at images (a text-only agent session) can still judge silhouettes,
shading and tile tiling from a terminal.

Usage:
  python tools/ascii_view.py heroes            # both heroes, key frames
  python tools/ascii_view.py frame <tag> <dir> <idx>
  python tools/ascii_view.py mob <name> [variant]
  python tools/ascii_view.py boss <name> [variant]
  python tools/ascii_view.py tiles <region> [tile_ids...]
  python tools/ascii_view.py tile16 <path.bin> <n>   # farm tiles etc.
  python tools/ascii_view.py png <path.png> [cols]

The character ramp maps palette luminance: '@' dark outline/ink, '#%*+=' mid
fills, '."' highlights, ' ' transparent. A per-pixel letter form is available
with --letters for telling apart two fills of similar brightness.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_common as gc

RAMP = " .'-~=+*#%@"   # light -> dark? we invert below


def _lum(name):
    r, g, b = gc.rgb_of(name)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


_NAME_BY_INDEX = {}
for _n in [gc.TRANSPARENT] + gc.PALETTE_NAMES:
    _NAME_BY_INDEX[gc.PAL[_n]] = _n


def canvas_ascii(c, letters=False):
    """One char per pixel. Transparent = space."""
    out = []
    for y in range(c.h):
        row = []
        for x in range(c.w):
            v = c.get(x, y)
            if v == 0:
                row.append(" ")
                continue
            name = _NAME_BY_INDEX.get(v)
            if name is None:
                row.append("?")
                continue
            if letters:
                # first letter, uppercase for fills, lowercase for accents
                ch = name[0]
                row.append(ch.upper() if _lum(name) > 110 else ch)
            else:
                l = _lum(name)
                # ramp: index 0 = darkest ('@'), bright fill fades to '.'
                k = int(round(l / 255.0 * 9))
                row.append("@%*+=~-:.."[min(k, 9)])
        out.append("".join(row))
    return "\n".join(out)


def load_hero_frames(tag):
    import gen_heroes as gh
    for t, ch, V in gh.HEROES:
        if t == tag:
            frames, labels = gh.build_sheet(ch, V)
            return frames, labels
    raise KeyError(tag)


def cmd_heroes():
    import gen_heroes as gh
    for tag, ch, V in gh.HEROES:
        frames, labels = gh.build_sheet(ch, V)
        picks = [0, 4, 6, 9, 10, 13, 14]   # idle, walk contact/pass, bite seq, hurt
        print(f"=== {tag} ({ch}) down view: idle/walk/bite/hurt ===")
        sel = [frames[i] for i in picks]
        lab = [labels[i] for i in picks]
        # side-by-side
        n = len(sel)
        print(" | ".join(f"{l:^32}" for l in lab))
        for y in range(32):
            print(" | ".join(canvas_ascii(c).splitlines()[y] for c in sel))


def cmd_frame(tag, d, idx):
    frames, labels = load_hero_frames(tag)
    c = frames[d * 16 + int(idx)]
    print(labels[d * 16 + int(idx)])
    print(canvas_ascii(c))


def cmd_mob(name, variant="idle"):
    import gen_monsters as gm
    fn = gm.MOBS[name]
    c = fn(None, variant)
    print(f"{name} [{variant}]")
    print(canvas_ascii(c))


def cmd_boss(name, variant="idle"):
    import gen_monsters as gm
    fn, aura = gm.BOSSES[name]
    art = fn(None, variant)
    print(f"{name} [{variant}] (aura strip: art only)")
    print(canvas_ascii(art))


def cmd_tiles(region, ids):
    import gen_world as gw
    spec = gw.REGIONS[region]
    import random
    rng = random.Random(gw.stable_seed("area", region))
    tiles = gw.build_atlas(spec, rng)
    for i in ids:
        print(f"--- {region} tile {i} ---")
        print(canvas_ascii(tiles[i]))


def cmd_tile16(path, n):
    blob = open(path, "rb").read()
    off = n * 256
    c = gc.Canvas(16, 16)
    for y in range(16):
        for x in range(16):
            c.set(x, y, blob[off + y * 16 + x])
    print(f"{path} tile {n}")
    print(canvas_ascii(c))


def cmd_png(path, cols=48):
    from PIL import Image
    img = Image.open(path).convert("RGB")
    w, h = img.size
    sx = max(1, w // cols)
    sy = max(1, round(sx * 2.1))
    px = img.load()
    print(f"{path} {w}x{h} -> {w // sx}x{h // sy}")
    for y in range(0, h - 1, sy):
        row = []
        for x in range(0, w - 1, sx):
            # average the block
            r = g = b = n = 0
            for dy in range(0, min(sy, h - y)):
                for dx in range(0, min(sx, w - x)):
                    pr, pg, pb = px[x + dx, y + dy]
                    r += pr; g += pg; b += pb; n += 1
            l = (0.2126 * r + 0.7152 * g + 0.0722 * b) / n / 255.0
            k = int(round((1.0 - l) * 9))
            row.append("@%*+=~:'.!"[min(k, 9)])
        print("".join(row))


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return
    cmd = args[0]
    if cmd == "heroes":
        cmd_heroes()
    elif cmd == "frame":
        cmd_frame(args[1], int(args[2]), int(args[3]))
    elif cmd == "mob":
        cmd_mob(args[1], args[2] if len(args) > 2 else "idle")
    elif cmd == "boss":
        cmd_boss(args[1], args[2] if len(args) > 2 else "idle")
    elif cmd == "tiles":
        ids = [int(a) for a in args[2:]] or list(range(8))
        cmd_tiles(args[1], ids)
    elif cmd == "tile16":
        cmd_tile16(args[1], int(args[2]))
    elif cmd == "png":
        cmd_png(args[1], int(args[2]) if len(args) > 2 else 48)
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
