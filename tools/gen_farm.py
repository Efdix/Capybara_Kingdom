#!/usr/bin/env python3
"""Farm art: tilled soil and the five crops' growth stages.

The kingdom's field is drawn at RUNTIME rather than baked into the hub map,
because the whole point of it is that it changes -- soil dries and wets, and a
crop is a different picture on every day of its life. So this generator emits a
small sprite sheet that farm.c blits per plot, and the hub map itself is left
untouched.

Sheet layout is fixed and mirrored by source/farm.h:

    0                bare soil
    1                bare soil, freshly watered (darker, holds a sheen)
    2 + crop*3 + s   crop `crop` (0..4) at stage `s` (0 = sprout, 2 = ripe)

Five crops, one per cake ingredient the region produces. Three of them are the
obvious plant (fruit, wheat, sugarcane, cacao); the fifth is fodder, because
milk comes from a cow and a cow comes from grass -- and the kingdom already
keeps cows in the pasture. The stage drawings are deliberately not the same
plant scaled up: a sprout is two leaves, a grown plant is a silhouette, and a
ripe one is the silhouette plus the thing you actually came for, so the field
reads at a glance from across the park.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_common as gc

TILE = 16
CROP_COUNT = 5
STAGE_COUNT = 3

SOIL_DRY = 0
SOIL_WET = 1
CROP_BASE = 2
TILE_COUNT = CROP_BASE + CROP_COUNT * STAGE_COUNT    # 17

# Palette shorthand.
O = gc.ix("outline")
OS = gc.ix("outline_soft")
SHADOW = gc.ix("shadow")

# Soil is drawn in the *purple* browns (brn/tan are the same ramp the kingdom's
# boardwalk is built from -- plank_dark is (150,92,44) and brn_dark is
# (150,96,46), two RGB units apart). A bed of brn_dark furrows therefore reads
# as a patch of missing decking rather than as earth, which is exactly what the
# first cut of this looked like. trunk/choco are the darker, redder ramp the
# tree trunks and the cocoa pods use, so a tilled bed stands apart from every
# plank in the park.
SOIL_D = gc.ix("trunk_dark")
SOIL_M = gc.ix("trunk_mid")
SOIL_L = gc.ix("choco_light")
WET_D = gc.ix("choco_dark")
WET_M = gc.ix("trunk_dark")
WET_L = gc.ix("choco_mid")

G_D = gc.ix("grn_dark")
G_M = gc.ix("grn_mid")
G_L = gc.ix("grass_light")
LEAF_D = gc.ix("leaf_dark")
LEAF_M = gc.ix("leaf_mid")
LEAF_L = gc.ix("leaf_light")

W_D = gc.ix("wheat_dark")
W_M = gc.ix("wheat_mid")
W_L = gc.ix("wheat_light")

C_D = gc.ix("cane_dark")
C_M = gc.ix("cane_mid")
C_L = gc.ix("cane_light")

FR_D = gc.ix("cherry_dark")
FR_M = gc.ix("cherry_mid")
FR_L = gc.ix("red_light")

CH_D = gc.ix("choco_dark")
CH_M = gc.ix("choco_mid")
CH_L = gc.ix("choco_light")
POD = gc.ix("orange_mid")

CREAM = gc.ix("cream")
WHITE = gc.ix("white_spark")
TRUNK = gc.ix("trunk_mid")


# ---------------------------------------------------------------------------
# Soil
# ---------------------------------------------------------------------------

def soil(wet):
    """A tilled plot: clods, furrow shadows, and a rim so plots read as plots.

    Deliberately NOT drawn as straight lit/shaded rows. The kingdom's boardwalk
    is exactly that -- a flat plank tone with a lit top row and a shadowed gap
    row -- so a bed of parallel furrows in a wood-brown reads as a hole in the
    decking rather than as soil. This was the first cut's actual bug, and it was
    obvious the moment the field was on screen next to the plaza: the two were
    near-indistinguishable.

    So the texture here is *unorganised*: scattered clods of different sizes,
    broken furrow strokes rather than continuous lines, and no repeating period
    along either axis. Tilled earth is lumpy; planks are not.

    The rim matters as much as the texture. A bed is 6x4 tiles drawn edge to
    edge, so without a per-tile edge the whole field renders as one flat brown
    rectangle and the player cannot see which of the 24 plots they are about to
    act on. A two-pixel darker margin (skipped where it would double up against
    a neighbour, giving a single-line grid) is enough.
    """
    c = gc.Canvas(TILE, TILE)
    base, dark, light = (WET_M, WET_D, WET_L) if wet else (SOIL_M, SOIL_D, SOIL_L)
    c.rect(0, 0, TILE, TILE, base)

    # Clods on a hash: irregular 1-2 px lumps of both the dark and the light
    # tone, spread so no two rows line up. A hash rather than a fixed list keeps
    # the twenty-four plots of the field from being twenty-four identical tiles.
    for i in range(30):
        hx = (i * 7 + 3) % 14 + 1
        hy = (i * 11 + 5) % 14 + 1
        if (i % 3) == 0:
            c.set(hx, hy, dark)
            c.set(hx + 1, hy, dark)
            c.set(hx, hy + 1, light)
        elif (i % 3) == 1:
            c.set(hx, hy, light)
            c.set(hx, hy + 1, dark)
        else:
            c.set(hx, hy, dark)
            c.set(hx + 1, hy + 1, light)

    # Two broken furrows: short dashes rather than lines across the whole tile,
    # and offset from each other so they do not read as rows of planks.
    for (fy, x0, x1) in ((5, 2, 8), (5, 11, 15), (11, 4, 13)):
        for x in range(x0, x1):
            c.set(x, fy, dark)
            c.set(x, fy + 1, light)

    # The plot rim: the top and left edges only. Adjacent plots then share one
    # line instead of drawing a double border, which is what makes the bed read
    # as a grid of plots rather than as a pile of separate squares.
    for i in range(TILE):
        c.set(i, 0, dark)
        c.set(0, i, dark)

    if wet:
        # A sheen: isolated highlights, which is the whole read of "watered".
        # Scattered rather than in a line, so wet soil is not just a lighter
        # version of the same pattern.
        for (ox, oy) in ((4, 4), (11, 9), (2, 13), (9, 2), (14, 6)):
            c.set(ox, oy, WHITE)
            if (ox + 1) < 16:
                c.set(ox + 1, oy + 1, light)

    return c


# ---------------------------------------------------------------------------
# Crops
# ---------------------------------------------------------------------------

def _sprout(c, bud_dark, bud_mid, bud_light):
    """Stage 0, shared by every crop: two leaves and a bud.

    The leaves are green for every crop -- that is what a young plant actually
    looks like, and painting them the crop's colour made 面粉 and 牛奶 sprouts
    read as dying twigs. The crop's own colour goes into the bud at the tip
    instead, which is a single pixel but is enough to tell one plot from
    another when you are standing in front of it trying to remember what you
    planted.
    """
    c.vline(8, 9, 14, LEAF_D)
    c.set(8, 8, LEAF_D)
    c.set(6, 9, LEAF_M); c.set(7, 9, LEAF_M); c.set(7, 10, LEAF_D)
    c.set(10, 9, LEAF_M); c.set(9, 9, LEAF_M); c.set(9, 10, LEAF_D)
    c.set(6, 8, LEAF_L)
    c.set(10, 8, LEAF_L)
    # The bud: three pixels, dark rim then the crop's colour.
    c.set(7, 6, bud_dark)
    c.set(8, 5, bud_dark)
    c.set(9, 6, bud_dark)
    c.set(8, 6, bud_mid)
    c.set(8, 5, bud_mid)
    c.set(8, 5, bud_light)
    c.set(7, 15, LEAF_D); c.set(8, 15, SHADOW)


def crop_fruit(c, stage):
    """雨林的水果: a low vine that fruits red."""
    if stage == 0:
        _sprout(c, FR_D, FR_M, FR_L)
        return
    # A leafy bush. Grown it is all foliage; ripe it carries three berries.
    c.ellipse(8, 10, 5.4, 4.0, LEAF_D)
    c.ellipse(8, 9, 4.2, 3.0, LEAF_M)
    c.ellipse(7, 8, 2.4, 1.6, LEAF_L)
    c.set(3, 12, LEAF_D); c.set(12, 12, LEAF_D)
    c.vline(8, 12, 15, TRUNK)
    if stage == 2:
        for (bx, by) in ((5, 10), (10, 11), (8, 7)):
            c.set(bx, by, O)
            c.set(bx + 1, by, FR_M)
            c.set(bx, by + 1, FR_M)
            c.set(bx + 1, by + 1, FR_D)
            c.set(bx, by, FR_L)
        c.set(5, 10, FR_L)


def crop_wheat(c, stage):
    """农场的面粉: golden stalks that bow over when ripe."""
    if stage == 0:
        _sprout(c, W_D, W_L, W_M)
        return
    heads = 1 if stage == 1 else 1
    for k, bx in enumerate((4, 8, 12)):
        top = 9 if stage == 1 else 6
        c.vline(bx, top, 15, W_D if k == 1 else W_M)
        c.set(bx, top - 1, W_M)
        if stage == 2:
            # The ear: a fat head of grain, three pixels wide, tipping right
            # on the outer stalks so the row looks like it has weight.
            lean = 1 if k != 1 else 0
            for j, yy in enumerate((top - 1, top, top + 1, top + 2)):
                c.set(bx + lean + (0 if j % 2 else 1), yy, W_L)
                c.set(bx + lean - 1, yy, W_M)
            c.set(bx + lean, top - 2, W_L)
    c.hline(3, 12, 15, W_D)
    del heads


def crop_fodder(c, stage):
    """牧场的牛奶: clover and fodder, what the cows actually eat."""
    if stage == 0:
        _sprout(c, G_L, CREAM, CREAM)
        return
    # Clumps of tall grass. Ripe it flowers white, which is the only pale mark
    # on the field and so is what your eye finds from the plaza.
    for k, bx in enumerate((3, 7, 11, 13)):
        h = 5 if stage == 1 else 4
        c.vline(bx, h, 15, G_D if k % 2 else G_M)
        c.set(bx - 1, h + 2, G_M)
        c.set(bx + 1, h + 3, G_D)
        if stage == 2:
            c.set(bx, h - 1, G_L)
            c.set(bx - 1, h, G_L)
            if k % 2 == 0:
                c.set(bx, h - 1, CREAM)
                c.set(bx + 1, h, CREAM)
    c.hline(2, 13, 15, G_D)


def crop_cane(c, stage):
    """沼泽的糖: sugarcane, the tallest thing in the field."""
    if stage == 0:
        _sprout(c, C_D, C_L, C_L)
        return
    for k, bx in enumerate((4, 8, 12)):
        top = 7 if stage == 1 else 2
        c.vline(bx, top, 15, C_M if k == 1 else C_D)
        # Node bands, which is what makes a green stalk read as cane rather
        # than as a blade of grass.
        for yy in range(top + 2, 15, 4):
            c.hline(bx - 1, bx + 1, yy, C_D)
        # Leaves flicking off the top, alternating sides.
        if stage == 2:
            side = -2 if k != 1 else 2
            c.set(bx + side, top + 1, C_L)
            c.set(bx + side + (1 if side < 0 else -1), top + 2, C_L)
            c.set(bx + side // 2, top, C_L)
    c.hline(2, 13, 15, C_D)


def crop_cacao(c, stage):
    """可可火山的巧克力: a small tree that hangs pods."""
    if stage == 0:
        _sprout(c, CH_D, CH_L, POD)
        return
    c.vline(8, 7, 15, TRUNK)
    c.set(8, 6, TRUNK)
    c.ellipse(8, 6, 5.0, 3.4, LEAF_D)
    c.ellipse(7, 5, 3.4, 2.2, LEAF_M)
    if stage == 2:
        # Two pods. Ridged, in the orange of a ripe cacao pod, hanging off the
        # trunk where they are reachable.
        for (px, py) in ((5, 10), (10, 9)):
            c.rect(px, py, 3, 5, CH_D)
            c.rect(px, py + 1, 3, 3, POD)
            c.set(px + 1, py + 1, CH_L)
            c.set(px, py + 5, LEAF_D)
            c.set(px + 1, py, CH_M)


CROP_FNS = (crop_fruit, crop_wheat, crop_fodder, crop_cane, crop_cacao)
CROP_NAMES = ("水果", "面粉", "牛奶", "糖", "巧克力")


# ---------------------------------------------------------------------------

def build_sheet():
    """Exactly TILE_COUNT tiles, with no padding.

    The sheet is not padded because every tile in it has to be drawable: the
    layout is arithmetic over (crop, stage), so a blank tile would not be a
    spare but a hole -- and verify_assets.py rejects an all-empty tile for
    exactly that reason.
    """
    tiles = [soil(False), soil(True)]
    for fn in CROP_FNS:
        for stage in range(STAGE_COUNT):
            c = gc.Canvas(TILE, TILE)
            fn(c, stage)
            tiles.append(c)
    assert len(tiles) == TILE_COUNT, f"sheet has {len(tiles)} tiles"
    return tiles


def emit_header(path):
    """C constants for the sheet layout, so C never hardcodes the arithmetic."""
    lines = [
        "// Autogenerated by tools/gen_farm.py -- do not edit.",
        "// farm_tiles.bin layout: see the generator's docstring.",
        "",
        "#pragma once",
        "",
        f"#define FARM_TILE_PX     {TILE}",
        f"#define FARM_TILE_BYTES  {TILE * TILE}",
        f"#define FARM_CROP_COUNT  {CROP_COUNT}",
        f"#define FARM_STAGE_COUNT {STAGE_COUNT}",
        f"#define FARM_STAGE_RIPE  {STAGE_COUNT - 1}",
        "",
        f"#define FARM_TILE_SOIL_DRY {SOIL_DRY}",
        f"#define FARM_TILE_SOIL_WET {SOIL_WET}",
        f"#define FARM_TILE_CROP_BASE {CROP_BASE}",
        f"#define FARM_TILE_CROP(crop, stage) \\",
        f"    (FARM_TILE_CROP_BASE + (crop) * FARM_STAGE_COUNT + (stage))",
        "",
    ]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("farm header ->", path)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "graphics"
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if not os.path.isabs(out):
        out = os.path.join(root, out)
    os.makedirs(out, exist_ok=True)

    tiles = build_sheet()
    blob = bytearray()
    for c in tiles:
        blob += c.to_bmp8()
    # Plain row-major: farm.c blits 16x16 sub-tiles out of this by index.
    gc.write_bin(os.path.join(out, "farm_tiles.bin"), bytes(blob))
    print(f"farm tiles: {len(tiles)} x {TILE}x{TILE} 8bpp ({len(blob)} B)")

    gc.emit_palette(os.path.join(out, "palette.bin"))
    emit_header(os.path.join(root, "source", "farm_art.h"))

    # Contact sheet: soil (dry/wet) then five rows of three stages. Labels are
    # ASCII because the preview draws with PIL's default bitmap font, which has
    # no CJK coverage.
    labels = ["soil", "wet"]
    for name in ("fruit", "wheat", "fodder", "cane", "cacao"):
        for stage in ("sprout", "grown", "ripe"):
            labels.append(f"{name} {stage}")
    # preview_scale wants one label per canvas, and the sheet is padded with
    # blanks so farm.c can index tiles without a bounds dance.
    labels += [""] * (len(tiles) - len(labels))
    gc.preview_scale(tiles, os.path.join(out, "preview_farm.png"),
                     scale=6, cols=6, labels=labels,
                     title="farm_tiles.bin -- soil + 5 crops x 3 stages")
    print("preview ->", os.path.join(out, "preview_farm.png"))


if __name__ == "__main__":
    main()
