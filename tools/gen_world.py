"""
World art for Capybara Kingdom: modern indie pixel-art tile atlas + large map per region.

6 Regions:
  hub:   水豚王国 · 城堡广场 (Lush kingdom plaza, cake oven, apple trees, cobblestones)
  rain:  雨林秘境 (Lush rainforest canopy, fertile soil, jungle river, fruit trees)
  farm:  金色麦田 (Golden waving wheat fields, irrigation canals, scarecrows)
  past:  青青牧场 (Soft green meadow, brook, rustic fences, grazing spotted cows)
  marsh: 甘蔗沼泽 (Sugarcane marsh, boardwalks, jointed stalks, cattail reeds)
  cacao: 可可火山 (Warm volcanic ash groves, hot spring steam vents, chocolate cottages)

Outputs:
  area_{name}_atlas.bin  48 tiles x 16x16, 8bpp, 12288 bytes per region
  area_{name}_map.bin    128x96 bytes = 12288 bytes per region
  area_{name}_prop.bin   48 bytes (solid flags) per region
  preview_atlas.png      Contact sheet of hub atlas (48 tiles)
  preview_world.png      Overview of all 6 region maps
"""

import os
import random
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_common as gc
from gen_common import Canvas, ix, preview_scale

TILE = 16
ATLAS_TILES = 48

WORLD_COLS, WORLD_ROWS = 128, 96
WORLD_W, WORLD_H = WORLD_COLS * TILE, WORLD_ROWS * TILE

RIM = "outline"

# ---------------------------------------------------------------------------
# Tile id layout contract
# ---------------------------------------------------------------------------

T_GROUND_A = 0    # 0..7    main ground, 8 speckle/tuft variants (walkable)
T_GROUND_B = 8    # 8..11   secondary ground, e.g. packed earth/moss (walkable)
T_PATH = 12       # 12..13  road / cobblestone / dirt path (walkable)
T_WATER = 14      # 14..17  water and shoreline (SOLID)
T_TREE = 18       # 18..21  canopy / tall tree (SOLID)
T_ROCK = 22       # 22..24  faceted boulders (SOLID)
T_BUSH = 25       # 25..27  plump berry shrubs (SOLID)
T_TALLGRASS = 28  # 28..29  tall swaying grass (walkable)
T_FLOWER = 30     # 30..31  small vibrant flowers (walkable)
T_STUMP = 32      # 32..33  stumps and logs with sprout (SOLID)
T_FENCE = 34      # 34..36  fences and low walls (SOLID)
T_BUILDING = 37   # 37..39  cottages / buildings (SOLID)
T_SIGNATURE = 40  # 40..42  the region's signature prop (SOLID)
T_DECOR = 43      # 43..45  regional detail, walkable
T_GROUND_C = 46   # 46..47  third ground tone (walkable)

SOLID_IDS = (set(range(T_WATER, T_WATER + 4))
             | set(range(T_TREE, T_TREE + 4))
             | set(range(T_ROCK, T_ROCK + 3))
             | set(range(T_BUSH, T_BUSH + 3))
             | set(range(T_STUMP, T_STUMP + 2))
             | set(range(T_FENCE, T_FENCE + 3))
             | set(range(T_BUILDING, T_BUILDING + 3))
             | set(range(T_SIGNATURE, T_SIGNATURE + 3)))


def stable_seed(*parts):
    return zlib.crc32("|".join(str(p) for p in parts).encode("utf-8")) & 0xFFFF


def paste_prop(dst, sub, rim=RIM):
    if rim:
        r = ix(rim)
        for y in range(TILE):
            for x in range(TILE):
                if sub.get(x, y):
                    continue
                if (sub.get(x - 1, y) or sub.get(x + 1, y)
                        or sub.get(x, y - 1) or sub.get(x, y + 1)):
                    dst.set(x, y, r)
    for y in range(TILE):
        for x in range(TILE):
            v = sub.get(x, y)
            if v:
                dst.set(x, y, v)


# ---------------------------------------------------------------------------
# Ground Painters
# ---------------------------------------------------------------------------

def ground_tile(base, speck_a, speck_b, rng, tuft=None, tuft_style=0):
    """Clean, lush ground tile with organic tufts, not random noise."""
    c = Canvas(TILE, TILE)
    c.rect(0, 0, TILE, TILE, ix(base))

    if tuft_style % 2 == 1:
        px, py = (tuft_style * 5) % 10 + 3, (tuft_style * 7) % 10 + 3
        c.ellipse(px, py, 4.0, 2.5, ix(speck_a))

    col_tuft = ix(tuft if tuft else speck_a)
    col_sh = ix(speck_b)

    num_accents = 1 + (tuft_style % 2)
    for k in range(num_accents):
        gx = (tuft_style * 7 + k * 8 + 3) % (TILE - 4) + 2
        gy = (tuft_style * 11 + k * 7 + 4) % (TILE - 4) + 2

        accent_type = (tuft_style + k) % 4
        if accent_type == 0:
            c.set(gx, gy, col_tuft)
            c.set(gx - 1, gy + 1, col_tuft)
            c.set(gx + 1, gy + 1, col_tuft)
            c.set(gx, gy + 1, col_sh)
        elif accent_type == 1:
            c.set(gx, gy, col_tuft)
            c.set(gx + 1, gy + 1, col_tuft)
            c.set(gx, gy + 1, col_sh)
        elif accent_type == 2:
            c.set(gx, gy, col_sh)
            c.set(gx + 1, gy, col_tuft)
            c.set(gx, gy + 1, col_sh)
        elif accent_type == 3:
            c.set(gx, gy, col_tuft)
            c.set(gx - 1, gy, col_tuft)
            c.set(gx, gy - 1, col_tuft)
            c.set(gx, gy + 1, col_sh)

    return c


def path_tile(base, lit, dark, rng, variant=0):
    """Charming cobblestone / flagstone path tile."""
    c = Canvas(TILE, TILE)
    c.rect(0, 0, TILE, TILE, ix(base))

    col_lit = ix(lit)
    col_dark = ix(dark)
    col_base = ix(base)

    stones = [
        (1, 1, 6, 5), (9, 2, 6, 4),
        (2, 8, 5, 6), (9, 8, 6, 6)
    ]
    if variant == 1:
        stones = [
            (2, 2, 12, 5),
            (1, 9, 6, 5), (9, 9, 6, 5)
        ]

    for sx, sy, sw, sh in stones:
        c.round_rect(sx, sy, sw, sh, 2, col_dark)
        c.round_rect(sx, sy, sw - 1, sh - 1, 2, col_base)
        c.hline(sx + 1, sx + sw - 3, sy, col_lit)
        c.vline(sx, sy + 1, sy + sh - 3, col_lit)

    c.set(7, 7, ix("grn_mid"))
    c.set(8, 7, ix("grn_light"))
    return c


def water_tile(ramp, rng, variant=0, shore=False):
    """Crystalline onsen / river water with floating yuzu and lotus pads!"""
    c = Canvas(TILE, TILE)
    dark, mid, light = ramp
    col_dark = ix(dark)
    col_mid = ix(mid)
    col_light = ix(light)

    c.rect(0, 0, TILE, TILE, col_mid)

    for y in (3, 7, 11, 15):
        offset = (y * 3) % 5
        c.hline(offset, min(TILE - 1, offset + 6), y, col_light)
        c.hline(min(TILE - 1, offset + 8), TILE - 1, y, col_dark)
        c.hline((offset + 2) % TILE, min(TILE - 1, (offset + 6) % TILE), y + 1, col_dark)

    if shore:
        c.rect(0, 0, TILE, 2, ix("white_spark"))
        c.rect(0, 2, TILE, 1, col_light)
        c.rect(0, 3, TILE, 1, col_dark)

    if variant == 1:
        # Floating lotus pad
        lx, ly = 8, 8
        c.ellipse(lx, ly, 4.0, 2.8, ix("grn_dark"))
        c.ellipse(lx, ly, 3.2, 2.2, ix("grn_light"))
        c.ellipse(lx - 0.5, ly - 0.5, 2.0, 1.2, ix("leaf_light"))
        c.set(lx + 2, ly, col_mid)
        c.set(lx + 1, ly, col_mid)
    elif variant == 2:
        if dark == "lava_dark":
            # Molten bubbling lava rock
            yx, yy = 8, 8
            c.circle(yx, yy, 3.6, ix("lava_dark"))
            c.circle(yx, yy, 2.8, ix("ember_orange"))
            c.circle(yx - 0.6, yy - 0.6, 1.6, ix("ember_yellow"))
            c.set(yx - 1, yy - 1, ix("white_spark"))
        else:
            # Floating cute golden yuzu orange bobbing in the water!
            yx, yy = 8, 8
            c.circle(yx, yy, 3.6, ix("outline"))
            c.circle(yx, yy, 2.8, ix("orange_mid"))
            c.circle(yx - 0.6, yy - 0.6, 1.6, ix("yellow_mid"))
            c.set(yx, yy - 3, ix("grn_light"))
            c.set(yx + 1, yy - 4, ix("leaf_light"))
            c.hline(yx - 4, yx + 4, yy + 3, col_light)
    elif variant == 3:
        c.set(4, 5, ix("white_spark"))
        c.set(12, 10, ix("white_spark"))
        c.set(7, 13, col_light)

    return c


# ---------------------------------------------------------------------------
# Prop Painters
# ---------------------------------------------------------------------------

def prop_tree(foliage, trunk, rng, fruit=None):
    """Lush, voluminous canopy tree with sturdy knotted trunk."""
    sub = Canvas(TILE, TILE)
    dark, mid, light = foliage

    cx = 8
    sub.rect(cx - 2, 9, 4, 6, ix(trunk[0]))
    sub.rect(cx - 1, 9, 3, 6, ix(trunk[1]))
    sub.vline(cx, 9, 14, ix(trunk[2]))
    sub.set(cx - 3, 14, ix(trunk[0]))
    sub.set(cx + 2, 14, ix(trunk[1]))

    sub.ellipse(cx, 8.5, 6.8, 5.0, ix(dark))
    sub.ellipse(cx - 3.0, 9.0, 4.0, 3.5, ix(dark))
    sub.ellipse(cx + 3.0, 9.0, 4.0, 3.5, ix(dark))

    sub.ellipse(cx, 6.5, 6.5, 5.0, ix(mid))
    sub.ellipse(cx - 2.5, 6.0, 4.5, 4.0, ix(mid))
    sub.ellipse(cx + 2.5, 6.0, 4.5, 4.0, ix(mid))

    sub.ellipse(cx - 1.5, 4.0, 4.2, 3.2, ix(light))
    sub.ellipse(cx - 3.5, 5.0, 2.5, 2.2, ix(light))
    sub.ellipse(cx + 1.5, 4.5, 2.8, 2.2, ix(light))
    sub.circle(cx - 2.0, 3.0, 1.4, ix("white_spark"))

    if fruit == "orange":
        sub.circle(cx - 3, 9, 1.6, ix("orange_mid"))
        sub.set(cx - 3, 9, ix("yellow_mid"))
        sub.circle(cx + 3, 8, 1.5, ix("orange_mid"))
    elif fruit == "cacao":
        sub.ellipse(cx - 3.5, 9.5, 1.4, 2.2, ix("orange_mid"))
        sub.ellipse(cx + 3.5, 9.0, 1.2, 2.0, ix("orange_dark"))

    return sub


def prop_rock(ramp, rng, moss=True):
    """Chiseled faceted boulder with lush moss cap."""
    sub = Canvas(TILE, TILE)
    dark, mid, light = ramp

    cx, cy = 8.0, 9.5
    sub.ellipse(cx, cy + 1.0, 6.5, 4.5, ix(dark))
    sub.ellipse(cx, cy, 6.0, 4.2, ix(mid))
    sub.ellipse(cx - 1.5, cy - 1.2, 4.0, 2.8, ix(light))
    sub.hline(int(cx - 3), int(cx + 2), int(cy - 2), ix("cream"))

    if moss:
        sub.ellipse(cx - 1.0, cy - 2.5, 3.5, 1.6, ix("grn_light"))
        sub.set(int(cx - 2), int(cy - 3), ix("leaf_light"))
        sub.set(int(cx + 1), int(cy - 2), ix("grn_mid"))

    return sub


def prop_bush(ramp, rng, berries=None):
    """Lush, plump berry bush."""
    sub = Canvas(TILE, TILE)
    dark, mid, light = ramp

    cx, cy = 8.0, 9.5
    sub.ellipse(cx, cy + 1.0, 6.5, 4.2, ix(dark))
    sub.ellipse(cx - 2.5, cy, 4.2, 3.5, ix(mid))
    sub.ellipse(cx + 2.5, cy, 4.2, 3.5, ix(mid))
    sub.ellipse(cx, cy - 1.0, 4.8, 3.8, ix(mid))
    sub.ellipse(cx - 1.5, cy - 2.0, 3.2, 2.2, ix(light))
    sub.circle(cx - 2.0, cy - 2.5, 1.2, ix("white_spark"))

    if berries:
        for bx, by in ((-3, 0), (3, -1), (0, 2), (-1, -2)):
            sub.circle(cx + bx, cy + by, 1.2, ix(berries))
            sub.set(int(cx + bx), int(cy + by), ix("white_spark"))

    return sub


def prop_stump(wood, rng):
    """Carved tree stump with visible growth rings and sprout."""
    sub = Canvas(TILE, TILE)
    dark, mid, light = wood

    sub.round_rect(3, 6, 10, 8, 2, ix(dark))
    sub.round_rect(4, 6, 8, 7, 2, ix(mid))
    sub.ellipse(8, 6.5, 4.8, 2.4, ix(light))
    sub.ellipse(8, 6.5, 3.2, 1.5, ix(mid))
    sub.ellipse(8, 6.5, 1.5, 0.8, ix(dark))

    sub.vline(10, 3, 5, ix("grn_mid"))
    sub.set(11, 3, ix("leaf_light"))
    sub.set(9, 4, ix("grn_light"))
    return sub


def prop_fence(wood, rng, post=True):
    """Rustic post-and-rail wooden fence with wood grain."""
    sub = Canvas(TILE, TILE)
    dark, mid, light = wood

    for y in (5, 10):
        sub.rect(0, y, TILE, 3, ix(dark))
        sub.rect(0, y, TILE, 2, ix(mid))
        sub.hline(0, TILE - 1, y, ix(light))

    if post:
        sub.rect(6, 2, 4, 13, ix(dark))
        sub.rect(6, 2, 3, 13, ix(mid))
        sub.vline(7, 2, 14, ix(light))
        sub.set(6, 2, ix(dark))
        sub.set(9, 2, ix(dark))
        sub.set(7, 6, ix("steel_glint"))
        sub.set(7, 11, ix("steel_glint"))

    return sub


def prop_building(wall, roof, rng, window=False):
    """Fairytale timber-framed cottage wall with shingled roof eaves."""
    sub = Canvas(TILE, TILE)
    wd, wm, wl = wall
    rd, rm, rl = roof

    sub.rect(0, 3, TILE, 13, ix(wm))
    sub.rect(0, 3, 2, 13, ix(wd))
    sub.rect(TILE - 2, 3, 2, 13, ix(wd))
    sub.vline(1, 3, 15, ix(wl))

    sub.rect(0, 0, TILE, 4, ix(rd))
    sub.rect(0, 1, TILE, 2, ix(rm))
    sub.hline(0, TILE - 1, 1, ix(rl))
    for x in range(0, TILE, 4):
        sub.set(x, 3, ix(rd))

    if window:
        sub.round_rect(5, 6, 6, 7, 2, ix(wd))
        sub.rect(6, 7, 4, 5, ix("ice_light"))
        sub.rect(6, 7, 4, 2, ix("white_spark"))
        sub.vline(7, 7, 11, ix(wd))
        sub.hline(6, 9, 9, ix(wd))
        sub.rect(4, 12, 8, 2, ix("wood_dark"))
        sub.rect(5, 12, 6, 1, ix("pink_mid"))
        sub.set(6, 11, ix("pink_light"))
        sub.set(8, 11, ix("yellow_mid"))

    return sub


def prop_cake_oven(rng):
    """The Kingdom's Cake Oven: rustic stone hearth with golden flames and cake!"""
    sub = Canvas(TILE, TILE)
    sub.round_rect(1, 2, 14, 13, 4, ix("rock_dark"))
    sub.round_rect(2, 3, 12, 12, 3, ix("rock_mid"))
    sub.hline(3, 12, 3, ix("rock_light"))

    sub.round_rect(4, 7, 8, 7, 2, ix("black"))
    sub.ellipse(8, 11, 3.2, 2.2, ix("lava_mid"))
    sub.ellipse(8, 11.5, 2.2, 1.4, ix("ember_orange"))
    sub.circle(8, 12, 1.0, ix("ember_yellow"))

    sub.round_rect(5, 4, 6, 4, 1, ix("sponge_mid"))
    sub.hline(5, 10, 4, ix("cream_light"))
    sub.circle(8, 3, 1.2, ix("cherry_mid"))
    sub.set(8, 2, ix("cherry_light"))

    sub.set(7, 1, ix("white_spark"))
    sub.set(8, 0, ix("grey_light"))
    return sub


def prop_flower(petals, rng, center="yellow_light"):
    """Vibrant 5-petal flower blossom on a cute green stem."""
    sub = Canvas(TILE, TILE)
    cx, cy = 8, 8
    for dx, dy in ((-2, 0), (2, 0), (0, -2), (0, 2), (-1, -1), (1, -1)):
        sub.circle(cx + dx, cy + dy, 1.5, ix(petals))
    sub.circle(cx, cy, 1.4, ix(center))
    sub.set(cx, cy, ix("white_spark"))
    sub.vline(cx, cy + 2, 14, ix("grn_mid"))
    sub.set(cx - 1, cy + 4, ix("grn_light"))
    sub.set(cx + 1, cy + 3, ix("leaf_light"))
    return sub


def prop_tallgrass(ramp, rng):
    """Lush swaying tall grass blades."""
    sub = Canvas(TILE, TILE)
    dark, mid, light = ramp
    stalks = [
        (3, 8, light), (5, 11, mid), (7, 13, light),
        (9, 10, mid), (11, 12, light), (13, 7, dark)
    ]
    for x, h, col in stalks:
        sub.vline(x, TILE - h, TILE - 1, ix(col))
        sub.set(x - 1, TILE - h + 1, ix(dark))
        sub.set(x, TILE - h, ix("white_spark" if col == light else light))
    return sub


def prop_wheat_sheaf(rng):
    """Golden wheat sheaf tied with dark rope."""
    sub = Canvas(TILE, TILE)
    for dx in (-3, -1, 1, 3):
        h = 12 - abs(dx)
        sub.vline(8 + dx, 14 - h, 14, ix("wheat_mid"))
        sub.set(8 + dx, 14 - h, ix("wheat_light"))
    sub.ellipse(8, 4.5, 4.5, 2.5, ix("wheat_light"))
    sub.ellipse(8, 5.5, 3.2, 1.6, ix("wheat_mid"))
    sub.rect(4, 9, 8, 2, ix("clay_dark"))
    sub.rect(5, 9, 6, 1, ix("clay_mid"))
    return sub


def prop_scarecrow(rng):
    """Charming scarecrow with floppy capybara-ear straw hat."""
    sub = Canvas(TILE, TILE)
    sub.rect(7, 5, 2, 10, ix("wood_dark"))
    sub.rect(2, 7, 12, 2, ix("wood_dark"))
    sub.rect(2, 7, 12, 1, ix("wood_mid"))
    sub.set(2, 9, ix("wheat_light"))
    sub.set(13, 9, ix("wheat_light"))

    sub.round_rect(5, 8, 6, 6, 1, ix("cloth_red"))
    sub.rect(6, 8, 4, 3, ix("cloth_red_hi"))
    sub.hline(5, 10, 11, ix("belt_dark"))

    sub.circle(8, 5, 2.8, ix("wheat_mid"))
    sub.circle(7.5, 4.5, 1.8, ix("wheat_light"))
    sub.set(7, 5, ix("black"))
    sub.set(9, 5, ix("black"))

    sub.hline(3, 12, 3, ix("wheat_dark"))
    sub.rect(5, 1, 6, 2, ix("wheat_mid"))
    sub.set(4, 2, ix("wheat_light"))
    sub.set(11, 2, ix("wheat_light"))
    return sub


def prop_cow(rng):
    """Chubby spotted dairy cow peacefully grazing."""
    sub = Canvas(TILE, TILE)
    sub.ellipse(8, 9, 6.2, 4.0, ix("cream"))
    sub.ellipse(6.0, 8.5, 2.8, 2.2, ix("brn_deep"))
    sub.ellipse(11.0, 9.5, 2.2, 1.8, ix("brn_deep"))
    sub.circle(13.0, 6.5, 2.5, ix("cream"))
    sub.circle(14.0, 6.8, 1.4, ix("pink_mid"))
    sub.set(12, 6, ix("black"))
    sub.set(12, 4, ix("wheat_light"))
    sub.set(13, 4, ix("wheat_light"))
    for lx in (4, 6, 10, 12):
        sub.rect(lx, 12, 2, 3, ix("brn_deep"))
    return sub


def prop_cane(foliage, rng):
    """Lush sugarcane stalks with joint rings."""
    sub = Canvas(TILE, TILE)
    dark, mid, light = foliage
    for x in (3, 7, 11):
        h = rng.randrange(12, 16)
        sub.rect(x, TILE - h, 2, h, ix(mid))
        sub.vline(x, TILE - h, TILE - 1, ix(light))
        for y in range(TILE - h + 3, TILE, 4):
            sub.rect(x - 1, y, 4, 1, ix(dark))
        sub.set(x - 1, TILE - h, ix(light))
        sub.set(x + 2, TILE - h, ix(light))
    return sub


def prop_reeds(foliage, rng):
    """Marsh cattail reeds with fluffy seed heads."""
    sub = Canvas(TILE, TILE)
    dark, mid, light = foliage
    for i, x in enumerate((3, 7, 12)):
        h = 10 + (i % 2) * 3
        sub.vline(x, TILE - h, TILE - 1, ix(mid))
        sub.set(x, TILE - h, ix(light))
        sub.rect(x - 1, TILE - h + 1, 3, 4, ix("brn_deep"))
        sub.vline(x, TILE - h + 1, TILE - h + 4, ix("brn_mid2"))
    return sub


def prop_steam_vent(rng):
    """Volcanic hot spring caldera vent with swirling warm steam."""
    sub = Canvas(TILE, TILE)
    sub.ellipse(8, 11.5, 6.2, 3.8, ix("rock_dark"))
    sub.ellipse(8, 11.5, 5.0, 2.8, ix("rock_mid"))
    sub.ellipse(8, 11.5, 3.6, 1.8, ix("cyan_mid"))
    sub.ellipse(8, 11.5, 2.2, 1.0, ix("white_spark"))

    for (dx, dy, r) in ((-2, -3, 2.0), (2, -6, 2.6), (-1, -9, 1.8)):
        sub.circle(8 + dx, 11 + dy, r, ix("white_spark"))
        sub.circle(8 + dx, 11 + dy, r - 0.7, ix("grey_light"))

    return sub


# ---------------------------------------------------------------------------
# Region Specs
# ---------------------------------------------------------------------------

REGIONS = {
    # The Kingdom is a wetland park rather than a town square: bright wet
    # meadow, muddy banks, reed beds and wooden boardwalks instead of paved
    # roads, with the ponds themselves added in build_cozy_hub_map() below.
    # The palette is deliberately the sunny end of the marsh family so it reads
    # as somewhere you want to come back to between runs.
    "hub": dict(
        ground=("grass_mid", "grass_light", "grass_dark"), tuft="grass_light",
        ground2=("murk_mid", "murk_light", "murk_dark"),
        path=("wood_mid", "wood_light", "wood_dark"),
        ground3=("grass_dark", "grass_mid", "grn_dark"),
        foliage=("leaf_dark", "leaf_mid", "leaf_light"),
        rock=("rock_dark", "rock_mid", "rock_light"),
        wood=("wood_dark", "wood_mid", "wood_light"),
        water=("cyan_dark", "cyan_mid", "cyan_light"),
        wall=("cream_light", "cream_mid", "cream_dark"),
        roof=("red_dark", "red_mid", "red_light"),
        bush=("leaf_dark", "leaf_mid", "leaf_light"),
        signature="oven",
        fruit="orange",
        deco=("reed", "flower", "reed"),
    ),
    "rain": dict(
        ground=("grn_dark", "grn_mid", "leaf_dark"), tuft="leaf_light",
        ground2=("brn_deep", "brn_mid2", "brn_shadow"),
        path=("brn_mid2", "brn_hi", "brn_deep"),
        ground3=("leaf_dark", "grn_mid", "pine_dark"),
        foliage=("leaf_dark", "leaf_mid", "leaf_light"),
        rock=("rock_dark", "rock_mid", "rock_light"),
        wood=("trunk_dark", "trunk_mid", "trunk_light"),
        water=("cyan_dark", "cyan_mid", "cyan_light"),
        wall=("trunk_dark", "trunk_mid", "trunk_light"),
        roof=("leaf_dark", "leaf_mid", "leaf_light"),
        bush=("grn_dark", "leaf_mid", "grn_light"),
        signature="orange_tree",
        fruit="orange",
        deco=("flower", "grass", "flower"),
    ),
    "farm": dict(
        ground=("wheat_mid", "wheat_light", "wheat_dark"), tuft="wheat_light",
        ground2=("clay_mid", "clay_dark", "sand_mid"),
        path=("clay_mid", "sand_mid", "clay_dark"),
        ground3=("wheat_dark", "wheat_mid", "clay_mid"),
        foliage=("grn_dark", "grn_mid", "grn_light"),
        rock=("rock_dark", "rock_mid", "rock_light"),
        wood=("wood_dark", "wood_mid", "wood_light"),
        water=("cyan_dark", "cyan_mid", "cyan_light"),
        wall=("red_dark", "red_mid", "red_light"),
        roof=("wood_dark", "wood_mid", "wood_light"),
        bush=("grn_dark", "grn_mid", "grn_light"),
        signature="scarecrow",
        fruit=None,
        deco=("flower", "grass", "flower"),
    ),
    "past": dict(
        ground=("grass_mid", "grass_light", "grass_dark"), tuft="grass_light",
        ground2=("sand_mid", "sand_light", "sand_dark"),
        path=("grey_mid", "grey_light", "grey_dark"),
        ground3=("grass_dark", "grass_mid", "pine_mid"),
        foliage=("pine_dark", "pine_mid", "pine_light"),
        rock=("rock_dark", "rock_mid", "rock_light"),
        wood=("wood_dark", "wood_mid", "wood_light"),
        water=("ice_dark", "ice_mid", "ice_light"),
        wall=("cream_dark", "cream_mid", "cream_light"),
        roof=("red_dark", "red_mid", "red_light"),
        bush=("pine_dark", "pine_mid", "pine_light"),
        signature="cow",
        fruit=None,
        deco=("flower", "grass", "flower"),
    ),
    "marsh": dict(
        ground=("murk_mid", "murk_light", "murk_dark"), tuft="cane_light",
        ground2=("brn_deep", "brn_shadow", "brn_mid2"),
        path=("cane_dark", "cane_mid", "cane_light"),
        ground3=("murk_dark", "murk_mid", "pine_dark"),
        foliage=("cane_dark", "cane_mid", "cane_light"),
        rock=("rock_dark", "rock_mid", "rock_light"),
        wood=("wood_dark", "wood_mid", "wood_light"),
        water=("murk_dark", "murk_mid", "murk_light"),
        wall=("cane_dark", "cane_mid", "cane_light"),
        roof=("wood_dark", "wood_mid", "wood_light"),
        bush=("cane_dark", "cane_mid", "cane_light"),
        signature="cane",
        fruit=None,
        deco=("reed", "grass", "reed"),
    ),
    "cacao": dict(
        ground=("ash_warm_mid", "ash_warm_light", "ash_warm_dark"),
        tuft="ash_warm_light",
        ground2=("brn_shadow", "brn_deep", "brn_mid2"),
        path=("ash_warm_light", "ash_warm_mid", "ash_warm_dark"),
        ground3=("ash_warm_dark", "ash_warm_mid", "rock_dark"),
        foliage=("leaf_dark", "leaf_mid", "leaf_light"),
        rock=("rock_dark", "rock_mid", "rock_light"),
        wood=("wood_dark", "wood_mid", "wood_light"),
        water=("lava_dark", "lava_mid", "lava_light"),
        wall=("choco_dark", "choco_mid", "choco_light"),
        roof=("ash_warm_dark", "ash_warm_mid", "ash_warm_light"),
        bush=("leaf_dark", "leaf_mid", "leaf_light"),
        signature="vent",
        fruit="cacao",
        deco=("flower", "vent", "grass"),
    ),
}


def build_atlas(spec, rng):
    tiles = []

    def add(c):
        assert 0 not in set(c.px), "a world tile is not fully opaque"
        tiles.append(c)

    base, sa, sb = spec["ground"]
    tuft = spec["tuft"]
    for i in range(8):
        r = random.Random(stable_seed("groundA", i))
        add(ground_tile(base, sa, sb, r, tuft=tuft, tuft_style=i))

    b2, s2a, s2b = spec["ground2"]
    for i in range(4):
        r = random.Random(stable_seed("groundB", i))
        add(ground_tile(b2, s2a, s2b, r, tuft=tuft, tuft_style=i + 8))

    p, pa, pb = spec["path"]
    for i in range(2):
        r = random.Random(stable_seed("path", i))
        add(path_tile(p, pa, pb, r, variant=i))

    wa, wb, wc = spec["water"]
    for i in range(4):
        r = random.Random(stable_seed("water", i))
        add(water_tile((wa, wb, wc), r, variant=i, shore=(i < 2)))

    for i in range(4):
        r = random.Random(stable_seed("tree", i))
        c = ground_tile(base, sa, sb, r, tuft=tuft, tuft_style=i)
        paste_prop(c, prop_tree(spec["foliage"], spec["wood"], r, fruit=spec.get("fruit")))
        add(c)

    for i in range(3):
        r = random.Random(stable_seed("rock", i))
        c = ground_tile(base, sa, sb, r, tuft=tuft, tuft_style=i + 4)
        paste_prop(c, prop_rock(spec["rock"], r, moss=(i != 2)))
        add(c)

    for i in range(3):
        r = random.Random(stable_seed("bush", i))
        c = ground_tile(base, sa, sb, r, tuft=tuft, tuft_style=i + 2)
        berries = "cherry_mid" if i == 0 else ("blue_light" if i == 1 else None)
        paste_prop(c, prop_bush(spec["bush"], r, berries=berries))
        add(c)

    for i in range(2):
        r = random.Random(stable_seed("tall", i))
        c = ground_tile(base, sa, sb, r, tuft=tuft, tuft_style=i + 1)
        paste_prop(c, prop_tallgrass(spec["foliage"], r), rim=None)
        add(c)

    for i in range(2):
        r = random.Random(stable_seed("flower", i))
        c = ground_tile(base, sa, sb, r, tuft=tuft, tuft_style=i + 3)
        cols = ("pink_mid", "yellow_mid", "cherry_mid", "purple_mid")
        paste_prop(c, prop_flower(cols[i % len(cols)], r), rim=None)
        add(c)

    for i in range(2):
        r = random.Random(stable_seed("stump", i))
        c = ground_tile(base, sa, sb, r, tuft=tuft, tuft_style=i + 5)
        paste_prop(c, prop_stump(spec["wood"], r))
        add(c)

    for i in range(3):
        r = random.Random(stable_seed("fence", i))
        c = ground_tile(base, sa, sb, r, tuft=tuft, tuft_style=i + 6)
        paste_prop(c, prop_fence(spec["wood"], r, post=(i != 1)))
        add(c)

    for i in range(3):
        r = random.Random(stable_seed("build", i))
        c = ground_tile(base, sa, sb, r, tuft=tuft, tuft_style=i + 7)
        paste_prop(c, prop_building(spec["wall"], spec["roof"], r, window=(i == 1)))
        add(c)

    sig = spec["signature"]
    for i in range(3):
        r = random.Random(stable_seed("sig", sig, i))
        c = ground_tile(base, sa, sb, r, tuft=tuft, tuft_style=i)
        sub = {
            "oven": lambda: prop_cake_oven(r),
            "wheat_sheaf": lambda: prop_wheat_sheaf(r),
            "scarecrow": lambda: prop_scarecrow(r),
            "cow": lambda: prop_cow(r),
            "cane": lambda: prop_cane(spec["foliage"], r),
            "orange_tree": lambda: prop_tree(spec["foliage"], spec["wood"], r, fruit="orange"),
            "vent": lambda: prop_steam_vent(r),
        }[sig]()
        paste_prop(c, sub)
        add(c)

    for i in range(3):
        r = random.Random(stable_seed("deco", i))
        c = ground_tile(base, sa, sb, r, tuft=tuft, tuft_style=i + 4)
        kind = spec["deco"][i]
        if kind == "flower":
            paste_prop(c, prop_flower("pink_mid" if i == 0 else "yellow_mid", r), rim=None)
        elif kind == "reed":
            paste_prop(c, prop_reeds(spec["foliage"], r), rim=None)
        elif kind == "vent":
            paste_prop(c, prop_steam_vent(r))
        else:
            paste_prop(c, prop_tallgrass(spec["foliage"], r), rim=None)
        add(c)

    b3, s3a, s3b = spec["ground3"]
    for i in range(2):
        r = random.Random(stable_seed("groundC", i))
        add(ground_tile(b3, s3a, s3b, r, tuft=tuft, tuft_style=i + 12))

    assert len(tiles) == ATLAS_TILES, f"atlas has {len(tiles)} tiles"
    return tiles


def scatter_blob(grid, rng, cx, cy, r, tile_id, jitter=0.45):
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            if not (0 <= x < WORLD_COLS and 0 <= y < WORLD_ROWS):
                continue
            d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
            if d <= r * (1.0 - jitter + rng.random() * jitter * 2.0):
                grid[y][x] = tile_id


def build_cozy_hub_map(spec, rng):
    """The Kingdom: a wetland park with a boardwalk plaza at its centre.

    The four landmarks stay exactly where the C code looks for them --
    hub_near_landmark() tests fixed pixel coordinates (1024,640 / 896,768 /
    1152,768 / 1024,850), so the tiles under those points must not move. The
    wetland is built around them rather than through them.

    Ponds are SOLID, and the asset check refuses a map that is more than 90%
    solid or that leaves less than 75% of its walkable tiles reachable from the
    spawn. The four ponds below are sized and placed to respect both: each is
    ringed by walkable mud and grass, and none of them touches the plaza, the
    four boardwalks or a house.
    """
    grid = [[T_TREE] * WORLD_COLS for _ in range(WORLD_ROWS)]
    cx, cy = WORLD_COLS // 2, WORLD_ROWS // 2  # (64, 48)

    # Outer parkland clearing: 50x38 tiles (comfortably under 90% solid limit).
    # The darker tone is scattered by a hash rather than by (x + y) % 5: a
    # periodic rule lays the variation out on a diagonal lattice, which reads as
    # tiling -- the one thing a park must not look like. Mixing two block sizes
    # gives mottling that is irregular but still clumps.
    for y in range(cy - 18, cy + 19):
        for x in range(cx - 24, cx + 25):
            h = (x * 73856093) ^ (y * 19349663) ^ ((x >> 2) * 83492791)
            v = ((h >> 7) ^ (h >> 15)) % 100
            grid[y][x] = T_GROUND_C if v < 15 else T_GROUND_A

    # Tree & bush natural perimeter around clearing
    for y in range(cy - 18, cy + 19):
        for x in range(cx - 24, cx + 25):
            dx = abs(x - cx)
            dy = abs(y - cy)
            if dx == 24 or dy == 18:
                grid[y][x] = T_BUSH

    # Town fence border (with open gates!)
    for x in range(cx - 15, cx + 16):
        if abs(x - cx) > 2:  # Gates at North and South
            grid[cy - 11][x] = T_FENCE
            grid[cy + 11][x] = T_FENCE
    for y in range(cy - 11, cy + 12):
        if abs(y - cy) > 2:  # Gates at West and East
            grid[y][cx - 15] = T_FENCE
            grid[y][cx + 15] = T_FENCE

    # Cozy houses in corners
    for (bx, by) in ((cx - 14, cy - 10), (cx + 12, cy - 10), (cx - 14, cy + 8), (cx + 12, cy + 8)):
        for dy in range(2):
            for dx in range(3):
                grid[by + dy][bx + dx] = T_BUILDING

    # Central boardwalk plaza (radius ~5.2 tiles)
    for y in range(cy - 5, cy + 6):
        for x in range(cx - 6, cx + 7):
            d = ((x - cx) ** 2 + ((y - cy) * 1.2) ** 2) ** 0.5
            if d <= 5.2:
                grid[y][x] = T_PATH

    # Boardwalks out to the 4 landmarks
    # North walk (to North Gate)
    for y in range(cy - 12, cy - 4):
        grid[y][cx] = T_PATH
        grid[y][cx - 1] = T_PATH

    # South walk (to Oven)
    for y in range(cy + 4, cy + 13):
        grid[y][cx] = T_PATH
        grid[y][cx - 1] = T_PATH

    # West walk (to Pigeon)
    for x in range(cx - 16, cx - 4):
        grid[cy][x] = T_PATH
        grid[cy - 1][x] = T_PATH

    # East walk (to Sacred Tree)
    for x in range(cx + 4, cx + 17):
        grid[cy][x] = T_PATH
        grid[cy - 1][x] = T_PATH

    # --- the wetland ---------------------------------------------------
    # One pond per quadrant. The muddy margin is walkable on purpose: it is what
    # makes the water read as a bank you could stand on rather than a hole, and
    # it is also what keeps every pond ringed by reachable ground.
    ponds = ((cx - 9, cy - 6), (cx + 9, cy - 6),
             (cx - 9, cy + 6), (cx + 9, cy + 6))
    for (px, py) in ponds:
        for y in range(py - 3, py + 4):
            for x in range(px - 3, px + 4):
                d = ((x - px) ** 2 + ((y - py) * 1.15) ** 2) ** 0.5
                if d <= 1.9:
                    grid[y][x] = T_WATER
                elif d <= 2.9:
                    grid[y][x] = T_GROUND_B

        # Reed clumps on the OUTER corners only. The inner side has to stay
        # clear or the reeds would fence the pond off from the plaza.
        step_x = -1 if px < cx else 1
        step_y = -1 if py < cy else 1
        for (ox, oy) in ((step_x * 3, step_y * 3), (step_x * 4, step_y * 1),
                         (step_x * 1, step_y * 4)):
            gx, gy = px + ox, py + oy
            if abs(gx - cx) < 24 and abs(gy - cy) < 18:
                grid[gy][gx] = T_BUSH

    # Flower and tall-grass clumps through the lawn: this is what makes the
    # clearing read as a tended park instead of an empty field. Both are
    # walkable, so they cost nothing against the solid-tile limit.
    for (fx, fy, tid) in (
        (-19, -14, T_FLOWER), (-6, -15, T_TALLGRASS), (7, -14, T_FLOWER),
        (19, -13, T_TALLGRASS), (-20, 4, T_FLOWER), (-17, 12, T_TALLGRASS),
        (16, -4, T_TALLGRASS), (19, 6, T_FLOWER), (-11, 15, T_FLOWER),
        (-3, -16, T_FLOWER), (9, 15, T_TALLGRASS), (14, 13, T_FLOWER),
        (-21, -8, T_FLOWER), (21, -9, T_FLOWER), (-21, 9, T_TALLGRASS),
        (21, 10, T_FLOWER),
    ):
        gx, gy = cx + fx, cy + fy
        if 0 <= gx < WORLD_COLS and 0 <= gy < WORLD_ROWS:
            grid[gy][gx] = tid

    # Landmarks on map
    # North Gate Pillars at (1024, 640 px) -> (cx, cy - 8)
    grid[cy - 8][cx - 2] = T_BUILDING
    grid[cy - 8][cx + 1] = T_BUILDING

    # West Pigeon Post Station at (896, 768 px) -> (cx - 8, cy)
    grid[cy][cx - 8] = T_BUILDING
    grid[cy - 1][cx - 8] = T_BUILDING
    grid[cy + 1][cx - 8] = T_FENCE

    # East Sacred Tree at (1152, 768 px) -> (cx + 8, cy)
    grid[cy][cx + 8] = T_TREE
    grid[cy - 1][cx + 8] = T_TREE
    grid[cy + 1][cx + 8] = T_FLOWER
    grid[cy - 2][cx + 8] = T_FLOWER

    # South Cake Oven at (1024, 850 px) -> (cx, cy + 5)
    grid[cy + 5][cx] = T_SIGNATURE
    grid[cy + 5][cx - 1] = T_SIGNATURE
    grid[cy + 6][cx] = T_FLOWER
    grid[cy + 6][cx - 1] = T_FLOWER

    # Flower beds along the boardwalks
    for i in range(-3, 4):
        if abs(i) > 1:
            grid[cy - 6][cx + i] = T_FLOWER
            grid[cy + 5][cx + i] = T_FLOWER

    return grid


def build_map(spec, rng, region):
    if region == "hub":
        return build_cozy_hub_map(spec, rng)

    grid = [[T_GROUND_A] * WORLD_COLS for _ in range(WORLD_ROWS)]

    for _ in range(90):
        cx = rng.randrange(WORLD_COLS)
        cy = rng.randrange(WORLD_ROWS)
        tile = rng.choice([T_GROUND_A] * 6 + [T_GROUND_B] * 2 + [T_GROUND_C])
        scatter_blob(grid, rng, cx, cy, rng.randrange(4, 13), tile)

    centre = (WORLD_COLS // 2, WORLD_ROWS // 2)
    for _ in range(rng.randrange(5, 9)):
        cx = rng.randrange(6, WORLD_COLS - 6)
        cy = rng.randrange(6, WORLD_ROWS - 6)
        if abs(cx - centre[0]) < 8 and abs(cy - centre[1]) < 8:
            continue
        scatter_blob(grid, rng, cx, cy, rng.randrange(3, 7), T_WATER, jitter=0.7)

    for _ in range(rng.randrange(55, 75)):
        cx = rng.randrange(4, WORLD_COLS - 4)
        cy = rng.randrange(4, WORLD_ROWS - 4)
        if abs(cx - centre[0]) < 6 and abs(cy - centre[1]) < 6:
            continue
        r = rng.choice([1, 1, 2, 2, 3])
        tile = rng.choice([T_TREE] * 5 + [T_ROCK] * 2 + [T_BUSH] * 3)
        scatter_blob(grid, rng, cx, cy, r, tile, jitter=0.85)

    for _ in range(320):
        x = rng.randrange(2, WORLD_COLS - 2)
        y = rng.randrange(2, WORLD_ROWS - 2)
        if grid[y][x] in (T_GROUND_A, T_GROUND_B, T_GROUND_C):
            grid[y][x] = rng.choice([T_TALLGRASS, T_FLOWER, T_DECOR])

    mid_x, mid_y = WORLD_COLS // 2, WORLD_ROWS // 2
    for x in range(2, WORLD_COLS - 2):
        if rng.random() < 0.88:
            grid[mid_y][x] = T_PATH
            if rng.random() < 0.35:
                grid[mid_y + 1][x] = T_PATH
    for y in range(2, WORLD_ROWS - 2):
        if rng.random() < 0.88:
            grid[y][mid_x] = T_PATH
            if rng.random() < 0.35:
                grid[y][mid_x + 1] = T_PATH

    for _ in range(rng.randrange(6, 11)):
        x = rng.randrange(6, WORLD_COLS - 8)
        y = rng.randrange(6, WORLD_ROWS - 8)
        for dx in range(rng.randrange(2, 4)):
            for dy in range(rng.randrange(1, 3)):
                grid[(y + dy) % WORLD_ROWS][(x + dx) % WORLD_COLS] = T_BUILDING
    for _ in range(rng.randrange(10, 16)):
        x = rng.randrange(5, WORLD_COLS - 12)
        y = rng.randrange(5, WORLD_ROWS - 5)
        run = rng.randrange(4, 10)
        for i in range(run):
            grid[y][(x + i) % WORLD_COLS] = T_FENCE

    for _ in range(rng.randrange(8, 14)):
        scatter_blob(grid, rng,
                     rng.randrange(5, WORLD_COLS - 5),
                     rng.randrange(5, WORLD_ROWS - 5),
                     rng.randrange(1, 3), T_SIGNATURE, jitter=0.9)

    for x in range(WORLD_COLS):
        for d in (0, 1):
            grid[d][x] = T_TREE if d == 0 else T_BUSH
            grid[WORLD_ROWS - 1 - d][x] = T_TREE if d == 0 else T_BUSH
    for y in range(WORLD_ROWS):
        for d in (0, 1):
            grid[y][d] = T_TREE if d == 0 else T_BUSH
            grid[y][WORLD_COLS - 1 - d] = T_TREE if d == 0 else T_BUSH

    for y in range(mid_y - 5, mid_y + 6):
        for x in range(mid_x - 5, mid_x + 6):
            if 0 <= x < WORLD_COLS and 0 <= y < WORLD_ROWS:
                if grid[y][x] in SOLID_IDS:
                    grid[y][x] = T_GROUND_A

    return grid


def reachable_area(grid, start):
    seen = set()
    stack = [start]
    while stack:
        x, y = stack.pop()
        if (x, y) in seen:
            continue
        if not (0 <= x < WORLD_COLS and 0 <= y < WORLD_ROWS):
            continue
        if grid[y][x] in SOLID_IDS:
            continue
        seen.add((x, y))
        stack.extend(((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))
    return seen


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "graphics"
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if not os.path.isabs(out):
        out = os.path.join(root, out)
    os.makedirs(out, exist_ok=True)

    previews, labels = [], []
    for name in ("hub", "rain", "farm", "past", "marsh", "cacao"):
        spec = REGIONS[name]
        rng = random.Random(stable_seed("area", name))

        tiles = build_atlas(spec, rng)
        grid = build_map(spec, rng, name)

        blob = bytearray()
        for c in tiles:
            blob += c.to_bmp8()
        gc.write_bin(os.path.join(out, f"area_{name}_atlas.bin"), bytes(blob))

        mapbytes = bytes(grid[y][x] for y in range(WORLD_ROWS)
                         for x in range(WORLD_COLS))
        gc.write_bin(os.path.join(out, f"area_{name}_map.bin"), mapbytes)

        propbytes = bytes(1 if i in SOLID_IDS else 0
                          for i in range(ATLAS_TILES))
        gc.write_bin(os.path.join(out, f"area_{name}_prop.bin"), propbytes)

        walkable = sum(1 for y in range(WORLD_ROWS) for x in range(WORLD_COLS)
                       if grid[y][x] not in SOLID_IDS)
        seen = reachable_area(grid, (WORLD_COLS // 2, WORLD_ROWS // 2))
        frac = len(seen) / max(walkable, 1)
        assert frac > 0.75, (
            f"area_{name}: only {frac:.0%} of walkable tiles are reachable "
            f"from the spawn point; the map is fragmented")
        assert grid[WORLD_ROWS // 2][WORLD_COLS // 2] not in SOLID_IDS, \
            f"area_{name}: spawn tile is solid"

        print(f"area_{name}: atlas {len(tiles)} tiles ({len(blob)} B), "
              f"map {WORLD_COLS}x{WORLD_ROWS} ({len(mapbytes)} B), "
              f"{len(SOLID_IDS)} solid ids, "
              f"walkable {frac:.0%} reachable from spawn")

        # Render real in-game DS screen (16x12 tiles = 256x192 px) around spawn
        view_w, view_h = 16, 12
        cx, cy = WORLD_COLS // 2, WORLD_ROWS // 2
        x0 = max(0, min(WORLD_COLS - view_w, cx - view_w // 2))
        y0 = max(0, min(WORLD_ROWS - view_h, cy - view_h // 2))

        screen = Canvas(view_w * TILE, view_h * TILE)
        for vy in range(view_h):
            for vx in range(view_w):
                tile_id = grid[y0 + vy][x0 + vx]
                t_canvas = tiles[tile_id]
                for py in range(TILE):
                    for px in range(TILE):
                        screen.set(vx * TILE + px, vy * TILE + py, t_canvas.get(px, py))

        previews.append(screen)
        labels.append(name)

    hub_rng = random.Random(stable_seed("area", "hub"))
    hub_tiles = build_atlas(REGIONS["hub"], hub_rng)
    preview_scale(hub_tiles, os.path.join(out, "preview_atlas.png"), scale=3,
                  cols=12, labels=[str(i) for i in range(len(hub_tiles))],
                  title="hub atlas (48 tiles)")

    preview_scale(previews, os.path.join(out, "preview_world.png"), scale=1,
                  cols=3, labels=labels,
                  title="Capybara Kingdom: 6 Regions In-Game Battle Scenery")


if __name__ == "__main__":
    main()
