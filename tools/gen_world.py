"""
World art for Capybara Kingdom: modern indie pixel-art tile atlas + large map per region.

6 Regions:
  hub:   水豚王国 · 湿地公园 (Lush wetland park: boardwalks, ponds, dense civic buildings)
  rain:  雨林秘境 (Lush rainforest canopy, fertile soil, jungle river, fruit trees)
  farm:  金色麦田 (Golden waving wheat fields, irrigation canals, scarecrows)
  past:  青青牧场 (Soft green meadow, brook, rustic fences, grazing spotted cows)
  marsh: 甘蔗沼泽 (Sugarcane marsh, boardwalks, jointed stalks, cattail reeds)
  cacao: 可可火山 (Warm volcanic ash groves, hot spring steam vents, chocolate cottages)

Outputs:
  area_{name}_atlas.bin  64 tiles x 16x16, 8bpp, 16384 bytes per region
  area_{name}_map.bin    COLSxROWS bytes (hub is small: see HUB_COLS/HUB_ROWS)
  area_{name}_prop.bin   64 bytes (solid flags) per region
  hub_layout.h           landmark world-pixel anchors, read by source/game.c
  preview_atlas.png      Contact sheet of hub atlas
  preview_world.png      Overview of all 6 region maps

Art direction
-------------
Every prop is built the same way so the atlas reads as one hand:

  1. a soft cast-shadow blob on the ground at the prop's base,
  2. the prop's own silhouette in a 3-stop ramp (dark / mid / light),
  3. a rim light along the top-left edges, because the sun is up-left,
  4. a 1px `outline` rim grown around the whole silhouette by paste_prop().

Step 4 is what gives the chunky storybook look in the reference art, and it is
also why every painter leaves a one-pixel margin: the rim grows outward.
"""

import os
import random
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_common as gc
from gen_common import Canvas, ix, preview_scale

TILE = 16
ATLAS_TILES = 64

WORLD_COLS, WORLD_ROWS = 128, 96
WORLD_W, WORLD_H = WORLD_COLS * TILE, WORLD_ROWS * TILE

# The kingdom is deliberately small: it is a place you cross in a few seconds
# between runs, not a second game. 48x40 tiles is 768x640 px, which is 3x3.3
# screens -- enough that the camera still moves and the park still feels like a
# place, small enough that every service is a short walk from the plaza.
HUB_COLS, HUB_ROWS = 48, 40
HUB_W, HUB_H = HUB_COLS * TILE, HUB_ROWS * TILE

RIM = "outline"

# ---------------------------------------------------------------------------
# Tile id layout contract
#
# Ids 0..47 are the original contract and must not move: existing maps and the
# C-side collision code are written against them. Everything the shops,
# landmarks and roofs needed was appended at 48..63.
# ---------------------------------------------------------------------------

T_GROUND_A = 0    # 0..7    main ground, 8 speckle/tuft variants (walkable)
T_GROUND_B = 8    # 8..11   secondary ground, e.g. packed earth/moss (walkable)
T_PATH = 12       # 12..13  road / boardwalk / dirt path (walkable)
T_WATER = 14      # 14..17  water and shoreline (SOLID)
T_TREE = 18       # 18..21  canopy / tall tree (SOLID)
T_ROCK = 22       # 22..24  faceted boulders (SOLID)
T_BUSH = 25       # 25..27  plump berry shrubs (SOLID)
T_TALLGRASS = 28  # 28..29  tall swaying grass (walkable)
T_FLOWER = 30     # 30..31  small vibrant flowers (walkable)
T_STUMP = 32      # 32..33  stumps and logs with sprout (SOLID)
T_FENCE = 34      # 34..36  fences and low walls (SOLID)
T_BUILDING = 37   # 37..39  building WALL row: plain / window / door (SOLID)
T_SIGNATURE = 40  # 40..42  the region's signature prop (SOLID)
T_DECOR = 43      # 43..45  regional detail, walkable
T_GROUND_C = 46   # 46..47  third ground tone (walkable)

# --- appended for the kingdom -------------------------------------------------
T_ROOF_T = 48     # 48..50  terracotta roof: left gable / ridge / right gable
T_ROOF_S = 51     # 51..53  glazed teal roof, same three pieces
T_TOWER = 54      # 54      bell tower (pigeon post)
T_GATE_L = 55     # 55..56  the town gate, left half and right half
T_GATE_R = 56
T_MARKET = 57     # 57      market stall with a striped awning
T_WELL = 58       # 58      stone well with a little roof
T_LAMP = 59       # 59      lamp post (lit)
T_FOUNTAIN = 60   # 60      tiered stone fountain
T_BRIDGE = 61     # 61      plank bridge over water (WALKABLE)
T_HEDGE = 62      # 62      trimmed hedge
T_SIGN = 63       # 63      wooden signpost

SOLID_IDS = (set(range(T_WATER, T_WATER + 4))
             | set(range(T_TREE, T_TREE + 4))
             | set(range(T_ROCK, T_ROCK + 3))
             | set(range(T_BUSH, T_BUSH + 3))
             | set(range(T_STUMP, T_STUMP + 2))
             | set(range(T_FENCE, T_FENCE + 3))
             | set(range(T_BUILDING, T_BUILDING + 3))
             | set(range(T_SIGNATURE, T_SIGNATURE + 3))
             | set(range(T_ROOF_T, T_ROOF_T + 6))
             | {T_TOWER, T_GATE_L, T_GATE_R, T_MARKET, T_WELL, T_LAMP,
                T_FOUNTAIN, T_HEDGE, T_SIGN})

WALKABLE_IDS = set(range(ATLAS_TILES)) - SOLID_IDS

# Landmark anchors, in world pixels. game.c reads these from hub_layout.h so the
# map art and the interaction points can never drift apart.
HUB_LANDMARKS = [
    ("GATE", 24, 7),        # 出征之门 -> world map
    ("POST", 14, 11),       # 信鸽塔 -> wireless link
    ("TREE", 34, 11),       # 神树 -> passive skill tree
    ("OVEN", 24, 28),       # 蛋糕烤箱 -> bake the cake
    ("MARKET", 14, 29),     # 集市 -> progress board
    ("SPRING", 34, 29),     # 温泉 -> flavour
]


def stable_seed(*parts):
    return zlib.crc32("|".join(str(p) for p in parts).encode("utf-8")) & 0xFFFF


def paste_prop(dst, sub, rim=RIM, shadow=True):
    """Stamp a prop onto its ground tile: cast shadow, then outline, then ink.

    The order matters. The shadow must go down before the outline, or the
    outline's rim would sit *under* the shadow and the prop would look like it
    is floating; and the outline must be computed from the prop alone, so it
    wraps the silhouette rather than the shadow.
    """
    if shadow:
        # A soft blob offset down-right: the light is up-left, so the shadow
        # falls down-right onto the ground the prop is standing on.
        for y in range(TILE):
            for x in range(TILE):
                if sub.get(x - 1, y - 1) or sub.get(x, y - 1) or sub.get(x - 1, y):
                    if dst.get(x, y) and not sub.get(x, y):
                        dst.set(x, y, ix("shadow"))

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
    """Ground in the GBA Pokemon manner: one flat fill carrying a scatter of
    small hard-edged marks.

    No soft ellipse mottling -- blurred patches at 16px read as dirt smudges
    once tiled, and tiled dirt smudges are wallpaper. Instead the tile gets:
      * short 1-2px dashes of the darker speckle (cast by grass blades),
      * a few single lighter pixels (caught light),
      * on the richest variants, one classic 3-blade grass tuft: two dark
        dashes and a light pixel above them, the little 'w' mark every GBA
        route uses.
    Density varies BY VARIANT so a field settles into an uneven rhythm of
    busy and quiet ground.
    """
    c = Canvas(TILE, TILE)
    c.rect(0, 0, TILE, TILE, ix(base))

    col_a = ix(speck_a)
    col_b = ix(speck_b)
    col_tuft = ix(tuft if tuft else speck_a)

    # How much detail this variant carries. Variants 0-1 are nearly bare,
    # 2-4 light, 5-6 busier, 7 the busiest; `tuft_style` runs past 7 for the
    # secondary grounds, so it wraps and those stay naturally calm.
    density = (1, 1, 2, 2, 2, 3, 3, 5)[tuft_style % 8]

    # Dark dashes: the shadow side of blades of grass. One or two pixels
    # long, placed by rng so no two tiles share a layout.
    for k in range(density):
        dx = rng.randrange(1, TILE - 2)
        dy = rng.randrange(1, TILE - 1)
        if rng.random() < 0.6:
            c.set(dx, dy, col_b)
            c.set(dx + 1, dy, col_b)
        else:
            c.set(dx, dy, col_b)

    # Light dots: caught light, one per two dark marks.
    for k in range(max(1, density // 2)):
        c.set(rng.randrange(1, TILE - 1), rng.randrange(1, TILE - 1), col_a)

    # The classic tuft, only on the richest variants: a light pixel over
    # two dark ones -- the GBA 'w' of grass.
    if tuft is not None and tuft_style % 8 >= 6:
        tx = rng.randrange(2, TILE - 4)
        ty = rng.randrange(3, TILE - 3)
        c.set(tx, ty - 1, col_tuft)
        c.set(tx + 2, ty - 1, col_tuft)
        c.set(tx + 1, ty - 2, col_tuft)
        c.set(tx, ty, col_b)
        c.set(tx + 2, ty, col_b)

    return c


def path_tile(base, lit, dark, rng, variant=0):
    """Boardwalk planks (0) and flagstone (1).

    The kingdom is built on timber, so variant 0 is the one the player sees
    most. A deck is drawn as *boards*, not as a brown mass: the two outer rows
    of every four are the lit top and the shadowed gap of a plank, so the eye
    gets a rhythm of lines even at a glance. Keep the base close to the lit
    tone -- a dark base turns a large plaza into a slab.
    """
    c = Canvas(TILE, TILE)
    c.rect(0, 0, TILE, TILE, ix(base))
    col_lit = ix(lit)
    col_dark = ix(dark)

    if variant == 0:
        for i, py in enumerate((0, 4, 8, 12)):
            # Every other board sits a hair darker: a deck of identical
            # boards reads as wallpaper, a deck with tonal variation reads
            # as timber that has weathered unevenly.
            board = ix(base) if (i & 1) == 0 else ix(dark)
            c.rect(0, py, TILE, 4, board)
            c.hline(0, TILE - 1, py, col_lit)          # bevel along the top
            c.hline(0, TILE - 1, py + 1, board)
            c.hline(0, TILE - 1, py + 3, col_dark)     # gap between boards
            # Stagger the board ends so the deck does not look like a ladder,
            # and nail each board down where it ends: two dark pins with a
            # lit head, the detail that says "built" rather than "painted".
            end = (i * 5 + 3) % TILE
            c.set(end, py + 2, col_dark)
            c.set((end + 9) % TILE, py + 2, col_dark)
            c.set(end, py + 1, col_lit)
            c.set((end + 9) % TILE, py + 1, col_lit)
            # A grain dash or two per board.
            g = (i * 7 + 2) % (TILE - 6)
            c.hline(g, g + 3, py + 2, col_dark)
        c.set(4, 6, col_dark)                          # a knot in the timber
        c.set(11, 10, col_dark)
        c.set(5, 5, col_lit)
    else:
        stones = [(0, 0, 7, 6), (8, 1, 8, 5),
                  (1, 7, 6, 5), (8, 7, 8, 7), (1, 13, 6, 3)]
        for sx, sy, sw, sh in stones:
            c.round_rect(sx, sy, sw, sh, 2, col_dark)
            c.round_rect(sx, sy, sw - 1, sh - 1, 2, ix(base))
            # Two-plane bevel: lit up and left, shaded down and right, so
            # each stone reads as a slab with thickness.
            c.hline(sx + 1, sx + sw - 3, sy, col_lit)
            c.vline(sx, sy + 1, sy + sh - 3, col_lit)
            c.vline(sx + sw - 2, sy + 2, sy + sh - 2, col_dark)
            # A joint speck between stones keeps big slabs from reading as
            # one poured surface.
            c.set(sx + sw // 2, sy + sh // 2, col_lit)
    return c


def water_tile(ramp, rng, variant=0, shallow=False):
    """Water: deep (0/1/3) and shallow (2, via `shallow`).

    Shallow water is a *lighter* tile rather than one with a foam stripe along
    its top edge. A stripe only looks like a shoreline on the tile that happens
    to face up; laid around a whole pond, three quarters of the ring would show
    the stripe on the wrong side and the pond would read as mis-registered.
    A lighter tone works on every side, which is what an edge tile has to do.
    """
    c = Canvas(TILE, TILE)
    dark, mid, light = ramp
    col_dark = ix(dark)
    col_mid = ix(mid)
    col_light = ix(light)

    if variant == 2 and dark == "lava_dark":
        # Molten crust: the only tile in the game that is genuinely emissive,
        # so it gets the full hot ramp rather than a tinted water ripple.
        c.rect(0, 0, TILE, TILE, ix("lava_dark"))
        c.circle(8, 8, 4.2, ix("lava_dark"))
        c.circle(8, 8, 3.2, ix("ember_orange"))
        c.circle(7.4, 7.4, 1.8, ix("ember_yellow"))
        c.set(7, 7, ix("white_spark"))
        c.set(12, 12, ix("lava_mid"))
        c.set(3, 12, ix("lava_mid"))
        return c

    if shallow:
        # Sand showing up through the water: the mid tone with pale highlight
        # patches on it, so the tile sits just one step lighter than the deep
        # water rather than reading as white paint around the pond.
        c.rect(0, 0, TILE, TILE, col_mid)
        for (sx, sy) in ((3, 4), (9, 3), (6, 9), (12, 11), (2, 12), (14, 6)):
            c.ellipse(sx, sy, 2.8, 2.0, col_light)
        for i, y in enumerate((3, 8, 13)):
            offset = (y * 3) % 5
            c.hline(offset, min(TILE - 1, offset + 6), y, col_light)
        return c

    c.rect(0, 0, TILE, TILE, col_mid)

    # GBA water: the fill does the depth work, and ripples are short light
    # dashes with nothing under them. (The old version underdrew every dash
    # with a dark line; tiled, that read as a persian rug.) One dash in four
    # carries a dark echo instead.
    for i, y in enumerate((2, 6, 10, 14)):
        offset = (y * 3 + variant) % 5
        c.hline(offset, min(TILE - 1, offset + 4), y, col_light)
        if i % 3 == 1:
            c.hline(offset + 2, min(TILE - 1, offset + 6), y, col_dark)
        elif i % 3 == 2:
            c.hline(offset + 3, min(TILE - 1, offset + 7), y, col_dark)

    # Two catchlights: the sparkle the sun leaves on moving water.
    c.set(4, 3, col_light)
    c.set(12, 9, col_light)

    if variant == 1:
        # Lily pads: two discs at different sizes so the cluster has depth.
        for (lx, ly, r) in ((6, 8, 3.4), (10, 9, 2.6)):
            c.ellipse(lx, ly, r, r * 0.72, ix("grn_dark"))
            c.ellipse(lx, ly - 0.5, r - 0.6, r * 0.72 - 0.5, ix("grn_light"))
            c.set(lx + 1, ly, col_mid)
            c.set(lx + 2, ly, col_mid)
            c.set(lx - 1, ly - 2, ix("pink_mid"))
            c.set(lx - 1, ly - 3, ix("pink_light"))
        c.circle(10, 6, 1.2, ix("pink_mid"))
        c.set(10, 5, ix("pink_light"))
    elif variant == 3:
        c.set(4, 5, ix("white_spark"))
        c.set(12, 10, ix("white_spark"))
        c.set(7, 13, col_light)
    return c


# ---------------------------------------------------------------------------
# Prop Painters
# ---------------------------------------------------------------------------

def prop_tree(foliage, trunk, rng, fruit=None):
    """A canopy tree in the GBA Pokemon manner: the crown is one wide,
    bumpy mass that fills the top of the tile and overhangs the trunk on
    both sides, read through three hard tones -- a dark under-skirt, the
    mid body, and a light cap on the up-left -- plus sparse leaf marks.
    The trunk is short and thick, flares at the roots, and falls into
    shadow where the crown sits on it.
    """
    sub = Canvas(TILE, TILE)
    dark, mid, light = foliage
    td, tm, tl = trunk

    # Trunk: 3px shaft, flared roots, lit left edge.
    sub.rect(6, 10, 4, 5, ix(td))
    sub.rect(6, 10, 3, 5, ix(tm))
    sub.vline(6, 11, 14, ix(tl))          # lit edge, key light from up-left
    sub.vline(9, 10, 14, ix(td))
    sub.set(5, 14, ix(td)); sub.set(4, 14, ix(td))    # root flare, left
    sub.set(10, 14, ix(td)); sub.set(11, 14, ix(td))  # root flare, right
    sub.set(8, 12, ix(td))                            # bark notch

    # Canopy: three lobes (left, right, crown) fused into one mass. Each
    # lobe draws dark under, mid over, and a light cap up-left; the fused
    # silhouette is bumpy, which is what separates a tree crown from a ball.
    for (cx, cy, rx, ry) in ((5.2, 7.2, 4.6, 3.8),
                             (10.8, 7.2, 4.6, 3.8),
                             (8.0, 4.6, 5.0, 3.6)):
        sub.blob(cx, cy + 1.0, rx, ry, ix(dark), n=3.0)
        sub.blob(cx, cy, rx - 0.3, ry - 0.3, ix(mid), n=3.0)
        sub.blob(cx - rx * 0.30, cy - ry * 0.34,
                 (rx - 0.3) * 0.60, (ry - 0.3) * 0.58, ix(light), n=3.0)

    # Leaf texture: dark dashes along lobe undersides, light dots on the
    # caps. Sparse on purpose -- five marks per tile.
    for (x, y) in ((3, 11), (7, 12), (12, 11), (5, 8), (10, 9)):
        sub.set(x, y, ix(dark))
        sub.set(x + 1, y, ix(dark))
    for (x, y) in ((4, 4), (8, 2), (12, 5)):
        sub.set(x, y, ix(light))

    # Shadow band where canopy meets trunk.
    sub.hline(6, 9, 10, ix(td))

    if fruit == "orange":
        for (fx, fy) in ((4.2, 9.4), (11.8, 9.0)):
            sub.circle(fx, fy, 1.6, ix("orange_dark"))
            sub.circle(fx - 0.2, fy - 0.2, 1.1, ix("orange_mid"))
            sub.set(int(fx) - 1, int(fy) - 1, ix("yellow_mid"))
            sub.set(int(fx), int(fy) - 2, ix("grn_dark"))   # stem
    elif fruit == "cacao":
        for (fx, fy) in ((3.6, 10.2), (12.2, 9.4)):
            sub.ellipse(fx, fy, 1.4, 2.2, ix("orange_dark"))
            sub.ellipse(fx - 0.2, fy, 1.0, 1.7, ix("orange_mid"))
            sub.vline(int(fx), int(fy) - 2, int(fy) - 1, ix("grn_dark"))

    return sub


def prop_palm(trunk, rng):
    """A palm whose fronds are arcs, not radiating dots.

    Each frond is a two-pixel rib that curves out and down: light on top,
    dark beneath, drooping at the tip. Six of them around a coconuts pair
    reads as a palm from any of the game's camera angles.
    """
    sub = Canvas(TILE, TILE)
    td, tm, tl = trunk

    # Trunk leans right with ring marks every other row.
    for i, y in enumerate(range(6, 16)):
        x = 5 + i // 4
        sub.rect(x, y, 3, 1, ix(tm))
        sub.set(x, y, ix(tl))
        sub.set(x + 2, y, ix(td))
        if i % 2 == 0:
            sub.set(x + 1, y, ix(td))
    sub.set(5, 15, ix(td)); sub.set(8, 15, ix(td))   # root flare

    # Fronds: arcs from the crown at (9, 5). Each rib walks out and dips.
    fronds = ((-1, -1, -6), (-1, 0, -7), (0, -1, -8),
              (1, -1, -6), (1, 0, -7), (0, 1, -8))
    for (sx, sy, span) in fronds:
        cx, cy = 9, 5
        for s in range(1, 7):
            px = cx + (sx * s * 5) // 6
            py = cy + (sy * s * 4) // 6 + (s * s) // 9   # the droop
            if 0 <= px < TILE and 0 <= py < TILE:
                sub.set(px, py, ix("leaf_dark"))
                sub.set(px, py - 1, ix("leaf_mid" if s > 2 else "leaf_light"))
    sub.set(8, 4, ix("leaf_light"))

    # Coconut pair under the crown.
    sub.circle(7.6, 6.4, 1.5, ix("brn_deep"))
    sub.set(7, 6, ix("brn_mid2"))
    sub.circle(10.4, 6.6, 1.4, ix("brn_deep"))
    sub.set(10, 6, ix("brn_mid2"))
    return sub


def prop_rock(ramp, rng, moss=True):
    """A boulder with three real facets and a crack.

    The silhouette is one superellipse; the facets are a straight-edged lit
    plane up-left, a dark plane right, and a crack that runs down the mid
    face. A tuft of grass at the base grounds it.
    """
    sub = Canvas(TILE, TILE)
    dark, mid, light = ramp

    sub.blob(8.0, 10.2, 6.6, 4.8, ix(mid), n=2.6)
    # Lit facet: clipped by the silhouette, edge running down-right.
    sub.blob(6.4, 8.8, 4.0, 2.8, ix(light), n=2.4)
    # Dark facet: right side plane.
    sub.blob(11.2, 11.0, 3.2, 2.6, ix(dark), n=2.4)
    # Crack: a two-segment dark polyline down the mid face.
    sub.vline(8, 7, 9, ix(dark))
    sub.set(9, 10, ix(dark))
    sub.set(9, 11, ix(dark))
    sub.hline(5, 7, 7, ix("cream"))       # catchlight on the lit plane
    sub.hline(4, 11, 14, ix(dark))        # ground contact

    if moss:
        sub.ellipse(6.0, 6.8, 3.0, 1.4, ix("grn_light"))
        sub.set(5, 6, ix("leaf_light"))
        sub.set(9, 7, ix("grn_mid"))
    else:
        sub.set(4, 13, ix("grn_mid"))
        sub.set(12, 14, ix("grn_mid"))
    return sub


def prop_bush(ramp, rng, berries=None):
    """A shrub: two overlapping lobes, dark under-skirt, lit caps."""
    sub = Canvas(TILE, TILE)
    dark, mid, light = ramp

    sub.blob(5.6, 9.6, 4.4, 3.6, ix(dark), n=3.0)
    sub.blob(10.6, 9.8, 4.2, 3.4, ix(dark), n=3.0)
    sub.blob(5.8, 9.0, 4.0, 3.2, ix(mid), n=3.0)
    sub.blob(10.4, 9.2, 3.8, 3.0, ix(mid), n=3.0)
    sub.blob(4.6, 7.6, 2.6, 2.0, ix(light), n=3.0)
    sub.blob(9.8, 7.8, 2.4, 1.8, ix(light), n=3.0)
    sub.hline(4, 12, 13, ix(dark))        # ground contact shadow row

    if berries:
        for (bx, by) in ((5, 8), (11, 9), (8, 11), (7, 7)):
            sub.circle(bx, by, 1.3, ix(berries))
            sub.set(bx - 1, by - 1, ix("white_spark"))
            sub.set(bx, by - 2, ix("grn_dark"))
    return sub


def prop_stump(wood, rng):
    """A cut stump: a ringed top face and a bark side."""
    sub = Canvas(TILE, TILE)
    dark, mid, light = wood

    sub.round_rect(3, 7, 10, 8, 2, ix(dark))
    sub.round_rect(4, 7, 8, 7, 2, ix(mid))
    sub.ellipse(8, 7.0, 4.8, 2.6, ix(light))
    sub.ellipse(8, 7.0, 3.2, 1.6, ix(mid))
    sub.ellipse(8, 7.0, 1.4, 0.8, ix(dark))

    sub.vline(10, 3, 6, ix("grn_mid"))
    sub.set(11, 3, ix("leaf_light"))
    sub.set(9, 4, ix("grn_light"))
    return sub


def prop_fence(wood, rng, post=True):
    """Post-and-rail fence with a shadow gap under the rails."""
    sub = Canvas(TILE, TILE)
    dark, mid, light = wood

    for y in (5, 10):
        sub.rect(0, y + 1, TILE, 1, ix(dark))     # gap under the rail
        sub.rect(0, y, TILE, 2, ix(mid))
        sub.hline(0, TILE - 1, y, ix(light))

    if post:
        sub.rect(6, 2, 4, 13, ix(dark))
        sub.rect(6, 2, 3, 13, ix(mid))
        sub.vline(7, 3, 14, ix(light))
        sub.set(6, 2, ix(dark))
        sub.set(9, 2, ix(dark))
    return sub


# --- the building kit ------------------------------------------------------
#
# A house is a ROOF row stamped above a WALL row (see stamp_house). Splitting
# the two is what makes a varied village affordable: two roof colours x three
# wall finishes gives six houses out of nine tiles, and they line up when
# placed side by side because the wall pieces are designed to butt together.

def prop_wall(wall, rng, kind=0):
    """One 16x16 piece of a building's front wall, GBA-town style.

    The wall is plaster between dark corner posts, on a stone footing. The
    window is the classic Pokemon town-house read: a deep blue frame, mid
    blue glass, a white cross divider and a light sheen in the top-left
    pane -- warm lamp-glass windows made every house look like it was
    always dusk inside, and blue is what the memory holds onto.

    kind: 0 plain, 1 lit window, 2 door.
    """
    sub = Canvas(TILE, TILE)
    wd, wm, wl = wall
    frame = ix("wood_dark")
    footing = ix("stone_dark")

    # Wall face between the eave shadow (rows 0-1) and the footing (rows 14-15).
    sub.rect(0, 2, TILE, 12, ix(wm))
    # Corner posts and the top/bottom plates of the frame.
    sub.rect(0, 2, 2, 12, frame)
    sub.rect(TILE - 2, 2, 2, 12, frame)
    sub.hline(0, TILE - 1, 2, ix("shadow"))
    sub.hline(2, TILE - 3, 3, ix(wl))
    sub.hline(2, TILE - 3, 12, ix(wd))
    sub.hline(0, TILE - 1, 13, ix(wd))
    # Stone footing, so the house sits on the ground instead of in it.
    sub.rect(0, 14, TILE, 2, footing)
    sub.hline(0, TILE - 1, 14, ix("stone_mid"))

    if kind == 1:
        sub.round_rect(4, 5, 8, 8, 1, frame)
        sub.rect(5, 6, 6, 6, ix("blue_dark"))
        sub.rect(5, 6, 6, 5, ix("blue_mid"))
        sub.rect(5, 6, 2, 2, ix("blue_light"))      # the sheen pane
        sub.vline(7, 6, 11, ix("white"))            # cross divider
        sub.hline(5, 10, 8, ix("white"))
        sub.vline(8, 6, 11, ix("blue_dark"))
        sub.hline(4, 11, 13, ix(wd))                # sill
    elif kind == 2:
        sub.round_rect(4, 4, 8, 10, 1, frame)
        sub.rect(5, 5, 6, 9, ix("trunk_dark"))
        sub.rect(5, 5, 5, 9, ix("trunk_mid"))
        sub.vline(6, 6, 13, ix("trunk_light"))
        sub.set(9, 9, ix("brass_mid"))
        sub.set(9, 10, ix("brass_shadow"))
        sub.hline(5, 10, 4, ix("plank_light"))      # lintel
    else:
        # A stud and a brace: the cheapest way to keep a blank wall from
        # reading as an untextured plane.
        sub.vline(8, 4, 12, ix(wd))
        sub.set(4, 5, ix(wd))
        sub.set(4, 6, ix(wd))
        sub.set(11, 6, ix(wd))
        sub.set(11, 5, ix(wd))
    return sub


def prop_roof(roof, rng, part=1):
    """One 16x16 piece of a pitched roof, seen from above.

    part 0/2 are the left and right gable ends, 1 continues the ridge. The
    ridge highlight runs along the top third and the eave shadow along the
    bottom, which is what gives a row of these tiles a slope.
    """
    sub = Canvas(TILE, TILE)
    rd, rm, rl = roof

    sub.rect(0, 0, TILE, 14, ix(rd))
    sub.rect(0, 2, TILE, 9, ix(rm))
    sub.hline(0, TILE - 1, 3, ix(rl))          # the ridge catchlight
    sub.hline(0, TILE - 1, 1, ix(rm))
    sub.hline(0, TILE - 1, 13, ix(rd))         # eave shadow
    sub.hline(0, TILE - 1, 14, ix("shadow"))   # overhang onto the wall

    # Tile courses: staggered so the roof is shingled, not striped.
    for i, y in enumerate((6, 9, 12)):
        for x in range((i * 5) % 6, TILE, 6):
            sub.set(x, y, ix(rd))
            sub.set(x + 1, y, ix(rd))
            sub.set(x, y - 1, ix(rl))

    if part == 0:
        sub.vline(0, 0, 13, ix(rd))
        sub.vline(1, 1, 13, ix(rm))
        for y in range(0, 14, 3):
            sub.set(0, y, ix(rl))
    elif part == 2:
        sub.vline(TILE - 1, 0, 13, ix(rd))
        sub.vline(TILE - 2, 1, 13, ix(rm))
        for y in range(0, 14, 3):
            sub.set(TILE - 1, y, ix(rl))
    return sub


# --- civic landmarks -------------------------------------------------------

def prop_tower(rng):
    """Bell tower: the pigeon post.

    Read at a glance it has to say "tower" and not "grey lump", so the three
    things that carry that are exaggerated: a base wider than the shaft, a
    belfry arch tall enough to see the light inside, and a spire that fills the
    whole top of the tile. Masonry courses and a lit course of windows finish it.
    """
    sub = Canvas(TILE, TILE)

    # Spire first, so the shaft overlaps its base and the join is hidden.
    sub.ellipse(8, 4.0, 6.2, 4.6, ix("slate_dark"))
    sub.ellipse(8, 3.2, 4.8, 3.6, ix("slate_mid"))
    sub.ellipse(7.0, 2.4, 2.6, 2.0, ix("slate_light"))
    sub.vline(8, 0, 2, ix("ui_gold"))
    sub.set(8, 0, ix("ui_gold_light"))
    sub.hline(3, 13, 6, ix("slate_dark"))     # eave over the belfry

    # Shaft.
    sub.rect(3, 7, 10, 8, ix("stone_dark"))
    sub.rect(4, 7, 8, 8, ix("stone_mid"))
    sub.vline(4, 8, 14, ix("stone_light"))
    sub.vline(11, 8, 14, ix("stone_shadow"))
    sub.hline(4, 11, 10, ix("stone_shadow"))
    sub.hline(4, 11, 13, ix("stone_shadow"))

    # Belfry arch, lit from inside -- the one warm note on the whole prop.
    sub.round_rect(6, 7, 5, 6, 2, ix("stone_shadow"))
    sub.round_rect(7, 8, 3, 5, 1, ix("lamp_glow"))
    sub.rect(7, 9, 2, 3, ix("lamp_core"))

    # Base.
    sub.rect(2, 14, 12, 2, ix("stone_shadow"))
    sub.hline(2, 13, 14, ix("stone_mid"))
    return sub


def prop_gate_left(rng):
    """Left half of the town gate: a stone pier under a tiled roof."""
    sub = Canvas(TILE, TILE)
    sub.rect(1, 3, 15, 13, ix("stone_dark"))
    sub.rect(2, 4, 13, 12, ix("stone_mid"))
    sub.vline(3, 5, 15, ix("stone_light"))
    # Masonry courses.
    for y in (7, 11):
        sub.hline(2, 14, y, ix("stone_shadow"))
    for x in (6, 10):
        sub.vline(x, 5, 6, ix("stone_shadow"))
        sub.vline(x + 2, 9, 10, ix("stone_shadow"))
    # Roof cap, overhanging to the left.
    sub.ellipse(9, 2.5, 8.0, 3.0, ix("tile_dark"))
    sub.ellipse(9, 1.8, 6.6, 2.2, ix("tile_mid"))
    sub.hline(3, 15, 1, ix("tile_light"))
    sub.hline(1, 15, 4, ix("tile_dark"))
    return sub


def prop_gate_right(rng):
    """Right half of the town gate, plus the banner that marks the exit."""
    sub = Canvas(TILE, TILE)
    sub.rect(0, 3, 15, 13, ix("stone_dark"))
    sub.rect(1, 4, 13, 12, ix("stone_mid"))
    sub.vline(2, 5, 15, ix("stone_light"))
    for y in (7, 11):
        sub.hline(1, 13, y, ix("stone_shadow"))
    for x in (5, 9):
        sub.vline(x, 5, 6, ix("stone_shadow"))

    sub.ellipse(7, 2.5, 8.0, 3.0, ix("tile_dark"))
    sub.ellipse(7, 1.8, 6.6, 2.2, ix("tile_mid"))
    sub.hline(0, 12, 1, ix("tile_light"))
    sub.hline(0, 14, 4, ix("tile_dark"))

    # A hanging banner on the pier face: gold ground, red chevron.
    sub.rect(4, 6, 7, 9, ix("outline"))
    sub.rect(5, 7, 5, 7, ix("ui_gold"))
    sub.rect(5, 7, 5, 2, ix("ui_gold_light"))
    sub.hline(5, 9, 10, ix("cloth_red"))
    sub.hline(5, 9, 12, ix("cloth_red"))
    sub.set(4, 15, ix("ui_gold"))
    sub.set(10, 15, ix("ui_gold"))
    return sub


def prop_market(rng):
    """Market stall: a striped awning over a crate of produce."""
    sub = Canvas(TILE, TILE)
    # Crate.
    sub.rect(3, 10, 10, 5, ix("plank_shadow"))
    sub.rect(3, 10, 10, 4, ix("plank_mid"))
    sub.hline(3, 12, 10, ix("plank_light"))
    sub.set(5, 12, ix("orange_mid"))
    sub.set(7, 12, ix("cherry_mid"))
    sub.set(9, 12, ix("yellow_mid"))
    sub.set(6, 11, ix("leaf_light"))
    sub.set(8, 11, ix("leaf_light"))
    # Posts.
    sub.rect(2, 5, 2, 6, ix("plank_dark"))
    sub.rect(12, 5, 2, 6, ix("plank_dark"))
    # Awning: alternating red and cream stripes, scalloped along the front.
    for i, x in enumerate(range(0, 16, 4)):
        col = ix("cloth_red") if i % 2 == 0 else ix("cream")
        sub.rect(x, 2, 4, 5, col)
        sub.hline(x, x + 3, 2, ix("cloth_red_hi") if i % 2 == 0 else ix("white"))
    sub.hline(0, 15, 6, ix("shadow"))
    for x in range(1, 16, 4):
        sub.set(x, 7, ix("cloth_red"))
        sub.set(x + 1, 7, ix("cloth_red"))
    return sub


def prop_well(rng):
    """Stone well: a lit opening, a thick ring of masonry, a rope and a roof.

    The dark hole is what identifies it. Everything else -- the ring, the roof,
    the rope -- is arranged around that one shape so the eye lands on it first.
    """
    sub = Canvas(TILE, TILE)

    # Masonry ring: an outer ellipse with a raised lip on the light side.
    sub.ellipse(8, 11.5, 6.6, 4.0, ix("stone_shadow"))
    sub.ellipse(8, 11.0, 5.8, 3.4, ix("stone_dark"))
    sub.ellipse(8, 10.6, 4.8, 2.8, ix("stone_mid"))
    sub.ellipse(6.4, 9.8, 2.6, 1.4, ix("stone_light"))

    # The hole: near-black at the bottom, water catchlight at the top.
    sub.ellipse(8, 10.8, 3.0, 1.7, ix("black"))
    sub.ellipse(8, 11.2, 2.4, 1.2, ix("murk_dark"))
    sub.hline(6, 9, 10, ix("sea_mid"))

    # Two posts, a cross beam, a rope and a bucket on it.
    sub.rect(2, 4, 2, 7, ix("plank_shadow"))
    sub.rect(12, 4, 2, 7, ix("plank_shadow"))
    sub.rect(2, 4, 1, 7, ix("plank_mid"))
    sub.rect(12, 4, 1, 7, ix("plank_mid"))
    sub.rect(2, 5, 12, 2, ix("plank_dark"))
    sub.hline(3, 12, 5, ix("plank_light"))
    sub.vline(8, 7, 9, ix("belt_mid"))
    sub.rect(7, 9, 3, 2, ix("brass_dark"))
    sub.hline(7, 9, 9, ix("brass_mid"))

    # Roof.
    sub.ellipse(8, 2.8, 6.6, 3.2, ix("tile_dark"))
    sub.ellipse(8, 2.0, 5.2, 2.4, ix("tile_mid"))
    sub.hline(2, 14, 1, ix("tile_light"))
    return sub


def prop_lamp(rng):
    """Lamp post: a warm pool of light on the ground under a lantern."""
    sub = Canvas(TILE, TILE)
    sub.ellipse(8, 14.0, 5.0, 2.0, ix("lamp_glow"))   # the pool of light
    sub.ellipse(8, 14.0, 3.0, 1.2, ix("lamp_core"))

    sub.rect(7, 7, 2, 8, ix("gun_dark"))
    sub.vline(7, 8, 14, ix("gun_hi"))
    sub.rect(6, 14, 4, 2, ix("gun_dark"))
    sub.hline(6, 9, 14, ix("gun_hi"))

    sub.round_rect(5, 3, 7, 5, 1, ix("gun_dark"))
    sub.rect(6, 4, 5, 3, ix("lamp_glow"))
    sub.rect(6, 4, 4, 2, ix("lamp_core"))
    sub.ellipse(8, 2.0, 3.6, 1.6, ix("brass_dark"))
    sub.ellipse(8, 1.5, 2.6, 1.1, ix("brass_mid"))
    return sub


def prop_fountain(rng):
    """Tiered fountain: a broad basin of water with a lifted stone rim, a
    pedestal and a jet. The white water is the subject; the stone is a frame."""
    sub = Canvas(TILE, TILE)

    # Basin: shadow, outer wall, then the water disc inset inside it.
    sub.ellipse(8, 12.0, 7.6, 4.2, ix("stone_shadow"))
    sub.ellipse(8, 11.4, 7.2, 3.9, ix("stone_mid"))
    sub.ellipse(8, 11.6, 5.6, 2.8, ix("sea_deep"))
    sub.ellipse(8, 11.4, 5.0, 2.4, ix("sea_mid"))
    sub.ellipse(8, 11.2, 3.4, 1.5, ix("sea_light"))
    sub.hline(2, 6, 9, ix("stone_light"))
    sub.hline(10, 14, 12, ix("stone_shadow"))

    # Pedestal and upper bowl.
    sub.rect(7, 7, 2, 5, ix("stone_shadow"))
    sub.vline(7, 7, 11, ix("stone_light"))
    sub.ellipse(8, 6.6, 3.6, 1.6, ix("stone_dark"))
    sub.ellipse(8, 6.2, 3.0, 1.2, ix("stone_mid"))
    sub.ellipse(8, 6.0, 2.2, 0.8, ix("sea_light"))

    # Jet: a tall white column with a spray crown.
    sub.vline(8, 1, 5, ix("foam"))
    sub.set(7, 3, ix("cyan_light"))
    sub.set(9, 3, ix("cyan_light"))
    sub.set(7, 1, ix("white_spark"))
    sub.set(9, 1, ix("white_spark"))
    sub.set(8, 0, ix("white_spark"))
    sub.set(5, 6, ix("foam"))
    sub.set(11, 6, ix("foam"))
    return sub


def prop_bridge(rng):
    """Plank bridge. Walkable, so it must not read as an obstacle: it is drawn
    as a flat deck with side rails, no outline rim, and the same plank rhythm
    as the boardwalk path tiles so the two connect."""
    sub = Canvas(TILE, TILE)
    for py in (0, 4, 8, 12):
        sub.rect(0, py, TILE, 3, ix("plank_dark"))
        sub.hline(0, TILE - 1, py, ix("plank_mid"))
        sub.hline(0, TILE - 1, py + 3, ix("plank_shadow"))
        for x in (2, 9):
            sub.set(x, py + 1, ix("plank_light"))
    return sub


def prop_hedge(rng):
    """A trimmed hedge block: flat top, square sides, dense leaf texture."""
    sub = Canvas(TILE, TILE)
    sub.round_rect(0, 4, TILE, 12, 2, ix("grn_dark"))
    sub.round_rect(1, 5, 14, 10, 2, ix("leaf_mid"))
    sub.hline(2, 13, 6, ix("leaf_light"))
    sub.hline(2, 11, 5, ix("leaf_light"))
    for x in range(2, 14, 3):
        sub.set(x, 9, ix("grn_dark"))
        sub.set(x + 1, 12, ix("grn_dark"))
        sub.set(x, 8, ix("leaf_light"))
    return sub


def prop_sign(rng):
    """A wooden signpost with two boards, for the hub's directions."""
    sub = Canvas(TILE, TILE)
    sub.rect(7, 6, 2, 9, ix("plank_shadow"))
    sub.vline(7, 7, 14, ix("plank_mid"))

    sub.round_rect(1, 3, 12, 4, 1, ix("plank_shadow"))
    sub.round_rect(2, 3, 10, 3, 1, ix("plank_mid"))
    sub.hline(2, 11, 3, ix("plank_light"))
    sub.set(10, 4, ix("plank_dark"))

    sub.round_rect(5, 8, 11, 4, 1, ix("plank_shadow"))
    sub.round_rect(6, 8, 9, 3, 1, ix("plank_mid"))
    sub.hline(6, 14, 8, ix("plank_light"))
    sub.set(6, 9, ix("plank_dark"))
    return sub


def prop_flower(petals, rng, center="yellow_light"):
    """Five-petal flower with a two-leaf stem and a lit corolla."""
    sub = Canvas(TILE, TILE)
    cx, cy = 8, 7
    sub.vline(cx, cy + 2, 14, ix("grn_mid"))
    sub.vline(cx, cy + 2, 14, ix("grn_light"))
    sub.set(cx, cy + 14, ix("grn_dark"))
    # Two leaves off the stem, one each side, lit edge up.
    for (lx, ly, flip) in ((cx - 2, cy + 5, -1), (cx + 2, cy + 8, 1)):
        sub.set(lx, ly, ix("grn_mid"))
        sub.set(lx + flip, ly, ix("grn_light"))
        sub.set(lx + flip, ly - 1, ix("leaf_light"))
    for (dx, dy) in ((-2, 0), (2, 0), (0, -2), (0, 2), (-1, -1), (1, -1)):
        sub.circle(cx + dx, cy + dy, 1.6, ix(petals))
        sub.set(int(cx + dx) - 1, int(cy + dy) - 1,
                ix("white_spark" if dx + dy < 0 else petals))
    sub.circle(cx, cy, 1.5, ix(center))
    sub.set(cx - 1, cy - 1, ix("white_spark"))
    return sub


def prop_tallgrass(ramp, rng):
    """A clump of tall grass with curved blades.

    The blades are two-pixel ribs that lean outward and tip back in at the
    top -- straight vertical bars read as a picket fence once tiled, arcs
    read as grass. Blade tips take the light tone, never white.
    """
    sub = Canvas(TILE, TILE)
    dark, mid, light = ramp
    sub.ellipse(8, 13.2, 6.4, 2.8, ix(dark))
    sub.ellipse(8, 12.4, 5.0, 2.0, ix(mid))
    for (x, h, col, lean) in ((3, 9, light, -1), (5, 12, mid, -1),
                              (7, 14, light, 0), (9, 11, mid, 1),
                              (11, 13, light, 1), (13, 8, dark, 1)):
        for k, y in enumerate(range(TILE - h, 13)):
            px = x + (lean * k) // max(1, h // 3)
            sub.set(px, y, ix(col))
            sub.set(px + 1, y, ix(dark if k > h // 2 else mid))
        sub.set(x + lean * (h // 3) // max(1, h // 3), TILE - h, ix(light))
    sub.ellipse(8, 9.0, 4.4, 1.6, ix(mid))
    return sub


def prop_wheat_sheaf(rng):
    """A tied sheaf of wheat."""
    sub = Canvas(TILE, TILE)
    for dx in (-3, -1, 1, 3):
        h = 12 - abs(dx)
        sub.vline(8 + dx, 14 - h, 14, ix("wheat_mid"))
        sub.set(8 + dx, 14 - h, ix("wheat_light"))
        sub.set(8 + dx, 14 - h + 2, ix("wheat_dark"))
    sub.ellipse(8, 4.5, 4.5, 2.6, ix("wheat_light"))
    sub.ellipse(8, 5.5, 3.2, 1.7, ix("wheat_mid"))
    sub.rect(4, 9, 8, 2, ix("clay_dark"))
    sub.rect(5, 9, 6, 1, ix("clay_mid"))
    return sub


def prop_scarecrow(rng):
    """A scarecrow in a straw hat."""
    sub = Canvas(TILE, TILE)
    sub.rect(7, 5, 2, 10, ix("wood_dark"))
    sub.rect(2, 7, 12, 2, ix("wood_dark"))
    sub.rect(2, 7, 12, 1, ix("wood_mid"))
    sub.set(2, 9, ix("wheat_light"))
    sub.set(13, 9, ix("wheat_light"))

    sub.round_rect(5, 8, 6, 6, 1, ix("cloth_red"))
    sub.rect(6, 8, 4, 3, ix("cloth_red_hi"))
    sub.hline(5, 10, 11, ix("belt_dark"))

    sub.circle(8, 5, 2.9, ix("wheat_mid"))
    sub.circle(7.4, 4.4, 1.9, ix("wheat_light"))
    sub.set(7, 5, ix("black"))
    sub.set(9, 5, ix("black"))

    sub.hline(3, 12, 3, ix("wheat_dark"))
    sub.rect(5, 1, 6, 2, ix("wheat_mid"))
    sub.set(4, 2, ix("wheat_light"))
    sub.set(11, 2, ix("wheat_light"))
    return sub


def prop_cow(rng):
    """A chubby spotted cow."""
    sub = Canvas(TILE, TILE)
    sub.ellipse(8, 9, 6.3, 4.1, ix("cream"))
    sub.ellipse(6.0, 8.5, 2.9, 2.3, ix("brn_deep"))
    sub.ellipse(11.0, 9.5, 2.3, 1.9, ix("brn_deep"))
    sub.circle(13.0, 6.5, 2.6, ix("cream"))
    sub.circle(14.0, 6.8, 1.5, ix("pink_mid"))
    sub.set(12, 6, ix("black"))
    sub.set(12, 4, ix("wheat_light"))
    sub.set(13, 4, ix("wheat_light"))
    for lx in (4, 6, 10, 12):
        sub.rect(lx, 12, 2, 3, ix("brn_deep"))
    return sub


def prop_cane(foliage, rng):
    """Sugarcane stalks with joint rings."""
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
    """Cattail reeds: arcs of leaf around two seed-head spikes."""
    sub = Canvas(TILE, TILE)
    dark, mid, light = foliage
    for i, x in enumerate((3, 7, 12)):
        h = 10 + (i % 2) * 3
        # Stalk bends with the wind: lean grows toward the tip.
        for k, y in enumerate(range(TILE - h, TILE)):
            px = x + (1 if i != 1 else -1) * k // 6
            sub.set(px, y, ix(mid))
            if k < 2:
                sub.set(px, y, ix(light))
        top_x = x + (1 if i != 1 else -1) * h // 6
        # Seed head: the brown sausage that says "cattail" at a glance.
        sub.rect(top_x - 1, TILE - h + 1, 3, 4, ix("brn_deep"))
        sub.vline(top_x, TILE - h + 1, TILE - h + 4, ix("brn_mid2"))
        sub.set(top_x, TILE - h, ix(light))
    # An arc leaf at the base, the reeds' own ground plane.
    for k in range(6):
        sub.set(4 + k, 14 - (k if k < 3 else 5 - k), ix(dark))
    return sub


def prop_steam_vent(rng):
    """A hot spring vent with swirling steam."""
    sub = Canvas(TILE, TILE)
    sub.ellipse(8, 11.5, 6.3, 3.9, ix("rock_dark"))
    sub.ellipse(8, 11.5, 5.1, 2.9, ix("rock_mid"))
    sub.ellipse(8, 11.5, 3.6, 1.9, ix("cyan_mid"))
    sub.ellipse(8, 11.5, 2.2, 1.1, ix("white_spark"))
    for (dx, dy, r) in ((-2, -3, 2.0), (2, -6, 2.6), (-1, -9, 1.8)):
        sub.circle(8 + dx, 11 + dy, r, ix("white_spark"))
        sub.circle(8 + dx, 11 + dy, r - 0.7, ix("grey_light"))
    return sub


def prop_cake_oven(rng):
    """The Kingdom's cake oven.

    Three reads stacked in one tile: a domed stone hearth, a mouth glowing with
    fire, and a frosted cake set on the dome with a candle in it. The dome is
    the shape that says "oven", so it gets the most contrast; the cake is what
    says this one is *the cake* oven.
    """
    sub = Canvas(TILE, TILE)

    # Hearth: a dome with a stone footing.
    sub.round_rect(0, 5, TILE, 11, 4, ix("stone_shadow"))
    sub.round_rect(1, 5, 14, 10, 4, ix("stone_dark"))
    sub.ellipse(8, 10.0, 6.6, 4.6, ix("stone_mid"))
    sub.ellipse(6.4, 8.4, 3.6, 2.4, ix("stone_light"))
    sub.hline(2, 13, 15, ix("stone_shadow"))

    # Mouth, with the fire banked inside it.
    sub.round_rect(4, 9, 8, 6, 2, ix("outline"))
    sub.round_rect(5, 10, 6, 5, 2, ix("black"))
    sub.ellipse(8, 13.4, 2.8, 2.0, ix("lava_mid"))
    sub.ellipse(8, 13.8, 2.0, 1.3, ix("ember_orange"))
    sub.circle(8, 14, 1.0, ix("ember_yellow"))
    sub.set(6, 12, ix("lava_light"))
    sub.set(10, 12, ix("lava_light"))

    # Cake on top, with a candle.
    sub.round_rect(4, 4, 8, 4, 1, ix("sponge_dark"))
    sub.round_rect(4, 4, 8, 3, 1, ix("sponge_mid"))
    sub.hline(4, 11, 4, ix("sponge_light"))
    sub.hline(4, 11, 5, ix("cream_mid"))
    sub.rect(7, 0, 2, 4, ix("cherry_mid"))
    sub.set(7, 0, ix("ember_yellow"))
    sub.set(8, 0, ix("ember_orange"))
    return sub


# ---------------------------------------------------------------------------
# Region Specs
# ---------------------------------------------------------------------------

REGIONS = {
    # The Kingdom is a wetland park rather than a town square: bright wet
    # meadow, muddy banks, reed beds and timber boardwalks instead of paved
    # roads, with the ponds themselves added in build_hub_map() below.
    "hub": dict(
        ground=("grass_mid", "grass_light", "grass_dark"), tuft="grass_light",
        # Pond banks are wet sand, not the dark peat the marsh region uses:
        # a dark ring around every pond reads as a hole in the lawn.
        ground2=("sand_dark", "clay_mid", "sand_light"),
        path=("plank_mid", "plank_light", "plank_shadow"),
        ground3=("grass_dark", "grass_mid", "grn_dark"),
        foliage=("leaf_dark", "leaf_mid", "leaf_light"),
        rock=("rock_dark", "rock_mid", "rock_light"),
        wood=("plank_shadow", "plank_mid", "plank_light"),
        water=("sea_deep", "sea_mid", "sea_light"),
        wall=("clay_dark", "cream", "white"),
        roof=("tile_dark", "tile_mid", "tile_light"),
        roof2=("slate_dark", "slate_mid", "slate_light"),
        bush=("leaf_dark", "leaf_mid", "leaf_light"),
        signature="oven",
        fruit="orange",
        deco=("reed", "flower", "reed"),
    ),
    "rain": dict(
        # The jungle floor is a *mid* green, not the darkest one: this is the
        # surface the player looks at for a whole run, and the dark tone is
        # reserved for the canopy props so they read against it.
        ground=("grn_mid", "grn_light", "grn_dark"), tuft="leaf_light",
        ground2=("brn_mid2", "brn_hi", "brn_deep"),
        path=("brn_mid2", "brn_hi", "brn_deep"),
        ground3=("leaf_dark", "leaf_mid", "pine_dark"),
        foliage=("leaf_dark", "leaf_mid", "leaf_light"),
        rock=("rock_dark", "rock_mid", "rock_light"),
        wood=("trunk_dark", "trunk_mid", "trunk_light"),
        water=("sea_deep", "sea_mid", "sea_light"),
        wall=("trunk_dark", "trunk_mid", "trunk_light"),
        roof=("leaf_dark", "leaf_mid", "leaf_light"),
        roof2=("slate_dark", "slate_mid", "slate_light"),
        bush=("grn_dark", "leaf_mid", "grn_light"),
        signature="orange_tree",
        fruit="orange",
        palms=True,
        deco=("flower", "grass", "flower"),
    ),
    "farm": dict(
        ground=("wheat_mid", "wheat_light", "wheat_dark"), tuft="wheat_light",
        ground2=("clay_mid", "clay_dark", "sand_mid"),
        path=("clay_mid", "sand_mid", "clay_dark"),
        ground3=("wheat_dark", "wheat_mid", "clay_mid"),
        foliage=("grn_dark", "grn_mid", "grn_light"),
        rock=("rock_dark", "rock_mid", "rock_light"),
        wood=("plank_shadow", "plank_mid", "plank_light"),
        water=("sea_deep", "sea_mid", "sea_light"),
        wall=("red_dark", "red_mid", "red_light"),
        roof=("tile_dark", "tile_mid", "tile_light"),
        roof2=("plank_shadow", "plank_mid", "plank_light"),
        bush=("grn_dark", "grn_mid", "grn_light"),
        signature="scarecrow",
        fruit=None,
        deco=("flower", "grass", "flower"),
    ),
    "past": dict(
        ground=("grass_mid", "grass_light", "grass_dark"), tuft="grass_light",
        ground2=("sand_mid", "sand_light", "sand_dark"),
        path=("stone_dark", "stone_mid", "stone_shadow"),
        ground3=("grass_dark", "grass_mid", "pine_mid"),
        foliage=("pine_dark", "pine_mid", "pine_light"),
        rock=("rock_dark", "rock_mid", "rock_light"),
        wood=("plank_shadow", "plank_mid", "plank_light"),
        water=("ice_dark", "ice_mid", "ice_light"),
        wall=("cream_dark", "cream_mid", "cream_light"),
        roof=("tile_dark", "tile_mid", "tile_light"),
        roof2=("slate_dark", "slate_mid", "slate_light"),
        bush=("pine_dark", "pine_mid", "pine_light"),
        signature="cow",
        fruit=None,
        deco=("flower", "grass", "flower"),
    ),
    "marsh": dict(
        # Wet meadow rather than a dark bog: the marsh is a place you fight
        # in for minutes at a time, so the base tone is the light one and the
        # dark olive is kept for the water and the shadows under the reeds.
        # The secondary ground is mid mud, not the near-black peat: large
        # patches of a very dark tone swallow everything drawn on top of them.
        ground=("murk_light", "cane_mid", "murk_mid"), tuft="cane_light",
        ground2=("brn_mid2", "brn_hi", "brn_deep"),
        path=("plank_shadow", "plank_light", "plank_dark"),
        ground3=("murk_dark", "murk_mid", "pine_dark"),
        foliage=("cane_dark", "cane_mid", "cane_light"),
        rock=("rock_dark", "rock_mid", "rock_light"),
        wood=("plank_shadow", "plank_mid", "plank_light"),
        water=("murk_dark", "murk_mid", "murk_light"),
        wall=("cane_dark", "cane_mid", "cane_light"),
        roof=("plank_shadow", "plank_mid", "plank_light"),
        roof2=("slate_dark", "slate_mid", "slate_light"),
        bush=("cane_dark", "cane_mid", "cane_light"),
        signature="cane",
        fruit=None,
        deco=("reed", "grass", "reed"),
    ),
    "cacao": dict(
        # Warm ash, brightened: the volcano's identity is carried by the lava
        # and the chocolate cottages, not by a dark ground plane. The soil
        # patches are mid brown for the same reason the marsh's are: a huge
        # patch of near-black ground reads as a hole in the world.
        ground=("ash_warm_light", "clay_mid", "ash_warm_mid"),
        tuft="sand_light",
        ground2=("brn_mid2", "brn_hi", "brn_shadow"),
        path=("ash_warm_light", "ash_warm_mid", "ash_warm_dark"),
        ground3=("ash_warm_dark", "ash_warm_mid", "rock_dark"),
        foliage=("leaf_dark", "leaf_mid", "leaf_light"),
        rock=("rock_dark", "rock_mid", "rock_light"),
        wood=("plank_shadow", "plank_mid", "plank_light"),
        water=("lava_dark", "lava_mid", "lava_light"),
        wall=("choco_dark", "choco_mid", "choco_light"),
        roof=("ash_warm_dark", "ash_warm_mid", "ash_warm_light"),
        roof2=("tile_dark", "tile_mid", "tile_light"),
        bush=("leaf_dark", "leaf_mid", "leaf_light"),
        signature="vent",
        fruit="cacao",
        palms=True,
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
        add(water_tile((wa, wb, wc), r, variant=i, shallow=(i == 2)))

    for i in range(4):
        r = random.Random(stable_seed("tree", i))
        c = ground_tile(base, sa, sb, r, tuft=tuft, tuft_style=i)
        if spec.get("palms") and i >= 2:
            paste_prop(c, prop_palm(spec["wood"], r))
        else:
            paste_prop(c, prop_tree(spec["foliage"], spec["wood"], r,
                                    fruit=spec.get("fruit")))
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
        paste_prop(c, prop_tallgrass(spec["foliage"], r), rim=None,
                   shadow=False)
        add(c)

    for i in range(2):
        r = random.Random(stable_seed("flower", i))
        c = ground_tile(base, sa, sb, r, tuft=tuft, tuft_style=i + 3)
        cols = ("pink_mid", "yellow_mid", "cherry_mid", "purple_mid")
        paste_prop(c, prop_flower(cols[i % len(cols)], r), rim=None,
                   shadow=False)
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

    # 37..39 -- the wall row of the building kit: plain / window / door.
    for i in range(3):
        r = random.Random(stable_seed("wall", i))
        c = ground_tile(base, sa, sb, r, tuft=tuft, tuft_style=i + 7)
        paste_prop(c, prop_wall(spec["wall"], r, kind=i))
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
            "orange_tree": lambda: prop_tree(spec["foliage"], spec["wood"], r,
                                             fruit="orange"),
            "vent": lambda: prop_steam_vent(r),
        }[sig]()
        paste_prop(c, sub)
        add(c)

    for i in range(3):
        r = random.Random(stable_seed("deco", i))
        c = ground_tile(base, sa, sb, r, tuft=tuft, tuft_style=i + 4)
        kind = spec["deco"][i]
        if kind == "flower":
            paste_prop(c, prop_flower("pink_mid" if i == 0 else "yellow_mid", r),
                       rim=None, shadow=False)
        elif kind == "reed":
            paste_prop(c, prop_reeds(spec["foliage"], r), rim=None,
                       shadow=False)
        elif kind == "vent":
            paste_prop(c, prop_steam_vent(r))
        else:
            paste_prop(c, prop_tallgrass(spec["foliage"], r), rim=None,
                       shadow=False)
        add(c)

    b3, s3a, s3b = spec["ground3"]
    for i in range(2):
        r = random.Random(stable_seed("groundC", i))
        add(ground_tile(b3, s3a, s3b, r, tuft=tuft, tuft_style=i + 12))

    # --- 48..63: the building kit and the kingdom's civic props -------------
    for i in range(3):
        r = random.Random(stable_seed("roofT", i))
        c = ground_tile(base, sa, sb, r, tuft=tuft, tuft_style=i + 3)
        paste_prop(c, prop_roof(spec["roof"], r, part=i))
        add(c)

    for i in range(3):
        r = random.Random(stable_seed("roofS", i))
        c = ground_tile(base, sa, sb, r, tuft=tuft, tuft_style=i + 5)
        paste_prop(c, prop_roof(spec["roof2"], r, part=i))
        add(c)

    civic = [
        ("tower", lambda r: prop_tower(r)),
        ("gate_l", lambda r: prop_gate_left(r)),
        ("gate_r", lambda r: prop_gate_right(r)),
        ("market", lambda r: prop_market(r)),
        ("well", lambda r: prop_well(r)),
        ("lamp", lambda r: prop_lamp(r)),
        ("fountain", lambda r: prop_fountain(r)),
        ("bridge", lambda r: prop_bridge(r)),
        ("hedge", lambda r: prop_hedge(r)),
        ("sign", lambda r: prop_sign(r)),
    ]
    for n, (tag, fn) in enumerate(civic):
        r = random.Random(stable_seed("civic", spec_name(spec), tag))
        c = ground_tile(base, sa, sb, r, tuft=tuft, tuft_style=n + 2)
        if tag == "bridge":
            # A bridge has to sit on water, not on grass, or the deck floats.
            c = water_tile((wa, wb, wc), r, variant=3)
        paste_prop(c, fn(r), shadow=(tag != "bridge"))
        add(c)

    assert len(tiles) == ATLAS_TILES, f"atlas has {len(tiles)} tiles"
    return tiles


def spec_name(spec):
    for k, v in REGIONS.items():
        if v is spec:
            return k
    return "?"


def scatter_blob(grid, rng, cx, cy, r, tile_id, jitter=0.45,
                 cols=WORLD_COLS, rows=WORLD_ROWS):
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            if not (0 <= x < cols and 0 <= y < rows):
                continue
            d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
            if d <= r * (1.0 - jitter + rng.random() * jitter * 2.0):
                grid[y][x] = tile_id


def scatter_on_ground(grid, rng, cx, cy, r, tile_id,
                      cols=WORLD_COLS, rows=WORLD_ROWS, jitter=0.9):
    """A cluster of walkable decoration, stamped only onto bare ground.

    Decoration must never overwrite a prop, a road or a pond, so this is a
    scatter_blob that refuses every tile that is not one of the three ground
    tones. The clusters it leaves behind read as a patch of tall grass or a
    tuft of flowers, where singles scattered uniformly read as noise.
    """
    walk = (T_GROUND_A, T_GROUND_B, T_GROUND_C)
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            if not (0 <= x < cols and 0 <= y < rows):
                continue
            if grid[y][x] not in walk:
                continue
            d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
            if d <= r * (1.0 - jitter + rng.random() * jitter * 2.0):
                grid[y][x] = tile_id


def stamp_house(grid, x, y, w, roof_id, wall_id, door_at=1,
                cols=HUB_COLS, rows=HUB_ROWS):
    """Stamp one building: a roof row over a wall row.

    `w` is the width in tiles. The leftmost and rightmost roof tiles are the
    gable pieces and the middle ones continue the ridge, which is what makes a
    three-wide house read as one roof rather than three huts.
    """
    for i in range(w):
        gx = x + i
        if not (0 <= gx < cols):
            continue
        if 0 <= y < rows:
            part = 0 if i == 0 else (2 if i == w - 1 else 1)
            grid[y][gx] = roof_id + part
        if 0 <= y + 1 < rows:
            if i == door_at:
                kind = 2
            elif i % 2 == 1:
                kind = 1
            else:
                kind = 0
            grid[y + 1][gx] = wall_id + kind


def plank_walk(grid, x0, y0, x1, y1, cols, rows, tile=T_PATH):
    """Lay a road from (x0, y0) to (x1, y1) along a Bresenham line.

    Solid tiles are stepped over rather than paved, so a path that runs into a
    house or a pond simply passes it by instead of overwriting it -- which
    is also what keeps the reachability check meaningful. `tile` picks the
    surface: the kingdom paves in timber, the wild regions in flagstone.
    """
    dx = abs(x1 - x0)
    dy = -abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx + dy
    x, y = x0, y0
    for _ in range(cols + rows):
        if 0 <= x < cols and 0 <= y < rows and grid[y][x] not in SOLID_IDS:
            grid[y][x] = tile
        if x == x1 and y == y1:
            break
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x += sx
        if e2 <= dx:
            err += dx
            y += sy


def build_hub_map(spec, rng):
    """The Kingdom: a compact wetland park, dense with buildings.

    The four original landmarks keep their function but move onto a smaller
    map, so the anchors in HUB_LANDMARKS -- not the old hardcoded pixel pairs --
    are what game.c reads (via hub_layout.h).

    Composition, from the outside in:
      1. a two-tile wild ring (trees outside, bushes inside) that walls the map
         and hides the fact that the world simply stops,
      2. a band of lawn with ponds, reeds and flower beds,
      3. a packed ring of houses facing the plaza,
      4. the plaza itself, with the oven at its centre and the boardwalk
         running through to the gate.

    Everything walkable is connected: the reachability check in main() enforces
    it, and the check exists because a pond placed one tile too close to a
    building can cut a landmark off from the plaza without looking wrong.
    """
    cols, rows = HUB_COLS, HUB_ROWS
    cx, cy = cols // 2, rows // 2          # (24, 20)

    grid = [[T_GROUND_A] * cols for _ in range(rows)]

    # --- 1. the wild ring -------------------------------------------------
    for y in range(rows):
        for x in range(cols):
            edge = min(x, y, cols - 1 - x, rows - 1 - y)
            if edge == 0:
                grid[y][x] = T_TREE
            elif edge == 1:
                grid[y][x] = T_BUSH if ((x * 7 + y * 13) % 5) else T_TREE

    # --- 2. lawn mottling -------------------------------------------------
    # The darker tone is placed by a hash rather than by a periodic rule: a
    # periodic rule lays the variation out on a diagonal lattice, which is the
    # one thing a park must not look like. The hash samples 2x2 blocks rather
    # than single tiles, so the shade falls in broad damp patches instead of
    # as per-tile dithering -- salt-and-pepper at tile scale is the other
    # thing a lawn must not look like.
    for y in range(2, rows - 2):
        for x in range(2, cols - 2):
            h = ((x >> 1) * 73856093) ^ ((y >> 1) * 19349663) \
                ^ (((x >> 1) + (y >> 1)) * 83492791)
            v = ((h >> 7) ^ (h >> 15)) % 100
            grid[y][x] = T_GROUND_C if v < 12 else T_GROUND_A

    # --- 3. ponds ---------------------------------------------------------
    # One pond in each corner of the garden ring, clear of the plaza, the
    # boardwalk and the houses. Each is ringed by walkable wet sand, which is
    # what makes the water read as a bank you could stand on and keeps the pond
    # from severing the lawn.
    ponds = [(9, 9, 3), (38, 9, 3), (9, 31, 3), (38, 31, 3)]
    for (px, py, pr) in ponds:
        for y in range(py - pr - 2, py + pr + 3):
            for x in range(px - pr - 2, px + pr + 3):
                if not (2 <= x < cols - 2 and 2 <= y < rows - 2):
                    continue
                d = ((x - px) ** 2 + ((y - py) * 1.15) ** 2) ** 0.5
                # Lily variant by hash, not by (x + y): a sum modulo lays the
                # pads out on diagonals, which across a whole pond reads as a
                # chequerboard rather than as a few plants.
                hsh = ((x * 2654435761) ^ (y * 40503)) >> 5
                if d <= pr - 1.0:
                    grid[y][x] = T_WATER + (1 if hsh % 7 == 0 else 0)
                elif d <= pr:
                    grid[y][x] = T_WATER + 2
                elif d <= pr + 1.3:
                    grid[y][x] = T_GROUND_B

    # --- 4. the boardwalk -------------------------------------------------
    # A spine from the gate straight down to the oven, a round plaza where it
    # meets the middle of the park, and four garden paths leaving the plaza on
    # the diagonals. Deliberately *not* a street grid spanning the map: a deck
    # that reaches every edge makes the park read as a road network, and the
    # whole point of the place is that it is a park.
    for y in range(6, cy + 8):
        grid[y][cx] = T_PATH
        grid[y][cx - 1] = T_PATH

    for y in range(cy - 4, cy + 5):
        for x in range(cx - 5, cx + 6):
            d = ((x - cx) ** 2 + ((y - cy) * 1.15) ** 2) ** 0.5
            if d <= 4.6:
                grid[y][x] = T_PATH

    # Garden paths. A Bresenham line, not an L: two long straight runs meeting
    # at a right angle read as a road junction, whereas a diagonal reads as a
    # path someone laid out. The four runs leave the plaza on the four
    # diagonals, which frames the plaza as a centre rather than a crossroads.
    for (tx, ty) in ((14, 11), (34, 11), (14, 29), (34, 29)):
        plank_walk(grid, cx, cy, tx, ty, cols, rows)

    # --- 5. the town ring of houses --------------------------------------
    # Two roof families alternate around the ring, and no two neighbours are
    # the same width, which is what stops a row of houses reading as wallpaper.
    R, r2 = T_ROOF_T, T_ROOF_S
    W = T_BUILDING
    row_a = [(4, 5, 3, R), (8, 5, 4, r2), (14, 4, 5, R), (30, 4, 5, r2),
             (36, 5, 4, R), (41, 5, 3, r2)]
    for (hx, hy, hw, roof) in row_a:
        stamp_house(grid, hx, hy, hw, roof, W, door_at=hw // 2)

    row_b = [(4, 12, 3, r2), (4, 21, 4, R), (4, 31, 3, r2),
             (41, 12, 3, R), (41, 21, 4, r2), (41, 31, 3, R)]
    for (hx, hy, hw, roof) in row_b:
        stamp_house(grid, hx, hy, hw, roof, W, door_at=hw // 2)

    row_c = [(7, 36, 4, R), (13, 36, 3, r2), (18, 37, 3, R),
             (28, 37, 3, r2), (33, 36, 4, R), (39, 36, 3, r2)]
    for (hx, hy, hw, roof) in row_c:
        stamp_house(grid, hx, hy, hw, roof, W, door_at=hw // 2)

    # Side streets so the houses are not just a painted border. Only bare
    # ground is eligible: paving over a pond would leave the water shaped like
    # a street, which is exactly the kind of detail that makes a map look
    # machine-made.
    for x in (5, 12, 20, 28, 35, 42):
        for y in range(9, 20):
            if grid[y][x] in (T_GROUND_A, T_GROUND_B, T_GROUND_C):
                grid[y][x] = T_PATH

    # --- 6. landmarks -----------------------------------------------------
    # The gate is two piers with the boardwalk running BETWEEN them, so the
    # player walks through the arch rather than up to a wall with a roof on it.
    gx, gy = HUB_LANDMARKS[0][1], HUB_LANDMARKS[0][2]
    grid[gy][gx - 1] = T_GATE_L
    grid[gy][gx + 2] = T_GATE_R
    grid[gy + 1][gx - 2] = T_LAMP
    grid[gy + 1][gx + 3] = T_LAMP
    grid[gy - 1][gx - 3] = T_SIGN

    # The pigeon tower, at the head of the north-west garden path.
    px, py = HUB_LANDMARKS[1][1], HUB_LANDMARKS[1][2]
    grid[py][px] = T_TOWER
    grid[py - 1][px] = T_HEDGE
    grid[py][px - 1] = T_HEDGE
    grid[py + 1][px + 1] = T_LAMP

    # The sacred tree, at the head of the north-east path: a clump, not a
    # single tile, so it towers over the houses around it.
    tx, ty = HUB_LANDMARKS[2][1], HUB_LANDMARKS[2][2]
    for (dx, dy) in ((0, 0), (-1, 1), (1, 1), (0, -1), (-1, -1), (1, -1),
                     (-2, 0), (2, 0)):
        grid[ty + dy][tx + dx] = T_TREE
    grid[ty + 2][tx - 1] = T_FLOWER
    grid[ty + 2][tx + 1] = T_FLOWER

    # The cake oven caps the south end of the boardwalk spine, so the walk
    # from the gate to the oven is one straight line and the player arrives at
    # it facing the thing they came for.
    ox, oy = HUB_LANDMARKS[3][1], HUB_LANDMARKS[3][2]
    grid[oy][ox] = T_SIGNATURE
    grid[oy][ox + 1] = T_SIGNATURE
    grid[oy][ox - 1] = T_LAMP
    grid[oy][ox + 2] = T_LAMP
    grid[oy + 1][ox - 2] = T_FLOWER
    grid[oy + 1][ox + 3] = T_FLOWER

    # Market stall and the hot spring, at the heads of the two south paths.
    mx, my = HUB_LANDMARKS[4][1], HUB_LANDMARKS[4][2]
    grid[my][mx] = T_MARKET
    grid[my][mx - 1] = T_SIGN
    grid[my - 1][mx + 1] = T_WELL

    sx2, sy2 = HUB_LANDMARKS[5][1], HUB_LANDMARKS[5][2]
    grid[sy2][sx2] = T_FOUNTAIN
    grid[sy2][sx2 + 1] = T_HEDGE
    grid[sy2][sx2 - 1] = T_HEDGE

    # A bridge over the two north ponds, so the water is crossable and the
    # deck has something to span rather than only running over grass.
    for (bx, by) in ((9, 9), (38, 9)):
        grid[by][bx + 2] = T_BRIDGE
        grid[by + 1][bx + 2] = T_BRIDGE

    # --- 7. greenery ------------------------------------------------------
    # Flower beds through the lawn: walkable, so they cost nothing against the
    # solid-tile budget, and they are what make the park read as tended rather
    # than as an empty field. Hedged into short beds of three so they look
    # planted rather than sprinkled.
    beds = [(cx - 6, cy - 5), (cx + 6, cy - 5), (cx - 4, cy + 7),
            (cx + 3, cy + 7), (5, 16), (42, 16), (5, 24), (42, 24),
            (20, 33), (27, 33), (18, 8), (29, 8), (13, 20), (34, 20)]
    for (bx, by) in beds:
        for i in range(3):
            gx, gy = bx + i, by
            if 2 <= gx < cols - 2 and 2 <= gy < rows - 2:
                if grid[gy][gx] in (T_GROUND_A, T_GROUND_C):
                    grid[gy][gx] = T_FLOWER if i != 1 else T_TALLGRASS

    # Orchard rows and loose clumps in the open lawn. The town needs trees
    # inside it, not only around it: a village of houses on bare grass looks
    # like a level editor, and the reference art is dense with foliage.
    clumps = [(7, 9), (11, 7), (17, 20), (7, 26), (19, 29), (29, 30),
              (39, 20), (30, 8), (41, 25), (13, 34), (24, 34), (36, 34),
              (21, 12), (27, 27), (6, 15), (42, 15)]
    for n, (bx, by) in enumerate(clumps):
        for (dx, dy) in ((0, 0), (1, 0), (0, 1), (-1, 0)):
            gx, gy = bx + dx, by + dy
            if 2 <= gx < cols - 2 and 2 <= gy < rows - 2:
                if grid[gy][gx] in (T_GROUND_A, T_GROUND_C, T_GROUND_B):
                    grid[gy][gx] = T_TREE if (n + dx + dy) % 3 else T_BUSH
    for by in range(9, 35, 5):
        for bx in (7, 40):
            if grid[by][bx] in (T_GROUND_A, T_GROUND_C):
                grid[by][bx] = T_TREE

    # Lamps along the spine, so the boardwalk is lit at both ends.
    for y in range(8, cy + 4, 4):
        for gx in (cx - 2, cx + 1):
            if grid[y][gx] in (T_GROUND_A, T_GROUND_C):
                grid[y][gx] = T_LAMP

    return grid


def build_map(spec, rng, region):
    if region == "hub":
        return build_hub_map(spec, rng)

    grid = [[T_GROUND_A] * WORLD_COLS for _ in range(WORLD_ROWS)]

    # Ground variation: a few LARGE, ragged patches of the secondary and
    # tertiary tones. Patches this size read as dry ground and shade on a
    # real field; the old spray of ninety four-to-thirteen-tile blobs read
    # as a checkerboard of confetti, which is the single worst thing a floor
    # you stare at for ten minutes can do.
    for _ in range(rng.randrange(10, 14)):
        scatter_blob(grid, rng,
                     rng.randrange(0, WORLD_COLS), rng.randrange(0, WORLD_ROWS),
                     rng.randrange(6, 14), T_GROUND_B, jitter=0.55)
    for _ in range(rng.randrange(5, 8)):
        scatter_blob(grid, rng,
                     rng.randrange(0, WORLD_COLS), rng.randrange(0, WORLD_ROWS),
                     rng.randrange(4, 9), T_GROUND_C, jitter=0.55)

    centre = (WORLD_COLS // 2, WORLD_ROWS // 2)
    for _ in range(rng.randrange(5, 9)):
        bx = rng.randrange(6, WORLD_COLS - 6)
        by = rng.randrange(6, WORLD_ROWS - 6)
        if abs(bx - centre[0]) < 8 and abs(by - centre[1]) < 8:
            continue
        scatter_blob(grid, rng, bx, by, rng.randrange(3, 7), T_WATER,
                     jitter=0.7)

    for _ in range(rng.randrange(55, 75)):
        bx = rng.randrange(4, WORLD_COLS - 4)
        by = rng.randrange(4, WORLD_ROWS - 4)
        if abs(bx - centre[0]) < 6 and abs(by - centre[1]) < 6:
            continue
        r = rng.choice([1, 1, 2, 2, 3])
        tile = rng.choice([T_TREE] * 5 + [T_ROCK] * 2 + [T_BUSH] * 3)
        scatter_blob(grid, rng, bx, by, r, tile, jitter=0.85)

    # Decoration grows in clusters -- a stand of tall grass, a patch of
    # flowers -- rather than as uniformly scattered singles. Clusters are
    # what a real meadow does; even singles are what a grid does.
    for _ in range(rng.randrange(55, 75)):
        bx = rng.randrange(4, WORLD_COLS - 4)
        by = rng.randrange(4, WORLD_ROWS - 4)
        if abs(bx - centre[0]) < 6 and abs(by - centre[1]) < 6:
            continue
        tile = rng.choice([T_TALLGRASS, T_FLOWER, T_DECOR])
        scatter_on_ground(grid, rng, bx, by, rng.choice([1, 1, 2, 2, 3]), tile)

    # The road surface: the marsh keeps timber boardwalks (that is what a
    # marsh path *is*), everywhere else paves in flagstone -- stone roads suit
    # jungle, field, pasture and ash ground better than deck boards do.
    road = T_PATH if region == "marsh" else T_PATH + 1

    mid_x, mid_y = WORLD_COLS // 2, WORLD_ROWS // 2

    # Villages: a cluster of houses is much more readable than isolated ones,
    # and it gives the region somewhere that looks lived in. Their centres are
    # kept, because the roads below have to actually lead to them.
    village_xy = []
    for _ in range(rng.randrange(3, 5)):
        vx = rng.randrange(8, WORLD_COLS - 16)
        vy = rng.randrange(8, WORLD_ROWS - 12)
        roof = rng.choice([T_ROOF_T, T_ROOF_S])
        for k in range(rng.randrange(2, 4)):
            stamp_house(grid, vx + k * 5, vy + (k % 2) * 3,
                        rng.choice([3, 4]), roof, T_BUILDING,
                        door_at=1, cols=WORLD_COLS, rows=WORLD_ROWS)
        for lx in range(2):
            grid[vy + 3][vx + lx * 6] = T_LAMP
        village_xy.append((vx + 5, vy + 4))

    # Roads from the spawn plaza to each village. A road has to start and end
    # somewhere to be worth drawing: the old generator laid a full-width cross
    # through the centre of every region, and a straight plank road to nowhere
    # reads exactly as the random noise it was.
    for vx, vy in village_xy:
        plank_walk(grid, mid_x, mid_y, vx, vy, WORLD_COLS, WORLD_ROWS, tile=road)

    # Fences belong to the villages -- a paddock beside the houses -- not to
    # the empty wilderness, where a free-standing run of fence is just noise.
    for vx, vy in village_xy:
        for _ in range(rng.randrange(1, 3)):
            fy = vy + rng.choice((5, 6))
            fx = vx + rng.randrange(-3, 4)
            run = rng.randrange(4, 8)
            for i in range(run):
                if 0 <= fx + i < WORLD_COLS and 0 <= fy < WORLD_ROWS - 2 \
                        and grid[fy][fx + i] in (T_GROUND_A, T_GROUND_B,
                                                 T_GROUND_C, T_TALLGRASS):
                    grid[fy][fx + i] = T_FENCE

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

    # The spawn clearing, kept walkable for game_start_region()...
    for y in range(mid_y - 5, mid_y + 6):
        for x in range(mid_x - 5, mid_x + 6):
            if 0 <= x < WORLD_COLS and 0 <= y < WORLD_ROWS:
                if grid[y][x] in SOLID_IDS:
                    grid[y][x] = T_GROUND_A

    # ...and then paved, because the run STARTS here: a flagstone circle under
    # the player's feet reads as the expedition's staging ground, and on the
    # minimap it is the "you are here" landmark. It is deliberately small -- a
    # plaza the size of a screen would read as a car park, not a camp -- and
    # solids stay: a boulder or two on the edge is scenery, not an obstacle.
    for y in range(mid_y - 6, mid_y + 7):
        for x in range(mid_x - 6, mid_x + 7):
            if not (0 <= x < WORLD_COLS and 0 <= y < WORLD_ROWS):
                continue
            if grid[y][x] in SOLID_IDS:
                continue
            d = ((x - mid_x) ** 2 + (y - mid_y) ** 2) ** 0.5
            if d <= 4.0:
                grid[y][x] = road

    return grid


def reachable_area(grid, start):
    rows = len(grid)
    cols = len(grid[0])
    seen = set()
    stack = [start]
    while stack:
        x, y = stack.pop()
        if (x, y) in seen:
            continue
        if not (0 <= x < cols and 0 <= y < rows):
            continue
        if grid[y][x] in SOLID_IDS:
            continue
        seen.add((x, y))
        stack.extend(((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))
    return seen


def emit_hub_layout_header(path):
    """Write the hub landmark anchors as C constants.

    Hand-copying these pixel pairs into game.c is exactly how a landmark ends
    up pointing at a tile that moved, so the generator owns them the same way
    it owns the palette indices.
    """
    lines = [
        "// Autogenerated by tools/gen_world.py -- do not edit.",
        "// Landmark anchors in the hub, in WORLD pixels (tile centre).",
        "",
        "#pragma once",
        "",
        f"#define HUB_COLS {HUB_COLS}",
        f"#define HUB_ROWS {HUB_ROWS}",
        f"#define HUB_W (HUB_COLS * 16)",
        f"#define HUB_H (HUB_ROWS * 16)",
        "",
    ]
    for i, (name, tx, ty) in enumerate(HUB_LANDMARKS):
        lines.append(f"#define HUB_LM_{name}_ID  {i + 1}")
        lines.append(f"#define HUB_LM_{name}_X   {tx * TILE + TILE // 2}")
        lines.append(f"#define HUB_LM_{name}_Y   {ty * TILE + TILE // 2}")
    lines.append("")
    lines.append(f"#define HUB_LM_COUNT {len(HUB_LANDMARKS)}")
    lines.append("")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("hub layout header ->", path)


def render_view(tiles, grid, cols, rows, cx, cy, view_w, view_h):
    x0 = max(0, min(cols - view_w, cx - view_w // 2))
    y0 = max(0, min(rows - view_h, cy - view_h // 2))
    screen = Canvas(view_w * TILE, view_h * TILE)
    for vy in range(view_h):
        for vx in range(view_w):
            tile_id = grid[y0 + vy][x0 + vx]
            t_canvas = tiles[tile_id]
            for py in range(TILE):
                for px in range(TILE):
                    screen.set(vx * TILE + px, vy * TILE + py, t_canvas.get(px, py))
    return screen


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
        cols = len(grid[0])
        rows = len(grid)

        blob = bytearray()
        for c in tiles:
            blob += c.to_bmp8()
        gc.write_bin(os.path.join(out, f"area_{name}_atlas.bin"), bytes(blob))

        mapbytes = bytes(grid[y][x] for y in range(rows) for x in range(cols))
        gc.write_bin(os.path.join(out, f"area_{name}_map.bin"), mapbytes)

        propbytes = bytes(1 if i in SOLID_IDS else 0
                          for i in range(ATLAS_TILES))
        gc.write_bin(os.path.join(out, f"area_{name}_prop.bin"), propbytes)

        walkable = sum(1 for y in range(rows) for x in range(cols)
                       if grid[y][x] not in SOLID_IDS)
        seen = reachable_area(grid, (cols // 2, rows // 2))
        frac = len(seen) / max(walkable, 1)
        assert frac > 0.75, (
            f"area_{name}: only {frac:.0%} of walkable tiles are reachable "
            f"from the spawn point; the map is fragmented")
        assert grid[rows // 2][cols // 2] not in SOLID_IDS, \
            f"area_{name}: spawn tile is solid"

        print(f"area_{name}: atlas {len(tiles)} tiles ({len(blob)} B), "
              f"map {cols}x{rows} ({len(mapbytes)} B), "
              f"{len(SOLID_IDS)} solid ids, "
              f"walkable {frac:.0%} reachable from spawn")

        # Landmarks must be reachable, or the hub has a service nobody can use.
        if name == "hub":
            for (lm, tx, ty) in HUB_LANDMARKS:
                ok = False
                for oy in range(-2, 3):
                    for ox in range(-2, 3):
                        if (tx + ox, ty + oy) in seen:
                            ok = True
                assert ok, f"hub landmark {lm} at {(tx, ty)} is walled off"

        # Half the map, so a preview shows the plaza, the roads out of it and
        # at least one village -- the whole composition, not just its centre.
        view_w, view_h = 64, 48
        cx, cy = cols // 2, rows // 2
        if name == "hub":
            cx, cy = HUB_LANDMARKS[0][1], HUB_LANDMARKS[0][2] + 6
            view_w, view_h = 32, 24
        previews.append(render_view(tiles, grid, cols, rows, cx, cy,
                                    view_w, view_h))
        labels.append(name)

    hub_rng = random.Random(stable_seed("area", "hub"))
    hub_tiles = build_atlas(REGIONS["hub"], hub_rng)
    preview_scale(hub_tiles, os.path.join(out, "preview_atlas.png"), scale=3,
                  cols=8, labels=[str(i) for i in range(len(hub_tiles))],
                  title="hub atlas (64 tiles)")

    # Full hub overview, so the whole town can be judged as a composition
    # rather than one screen at a time.
    hub_grid = build_hub_map(REGIONS["hub"], random.Random(stable_seed("area", "hub")))
    hub_img = Canvas(HUB_COLS * TILE, HUB_ROWS * TILE)
    for ty in range(HUB_ROWS):
        for tx in range(HUB_COLS):
            t_canvas = hub_tiles[hub_grid[ty][tx]]
            for py in range(TILE):
                for px in range(TILE):
                    hub_img.set(tx * TILE + px, ty * TILE + py,
                                t_canvas.get(px, py))
    preview_scale([hub_img], os.path.join(out, "preview_hub.png"), scale=2,
                  cols=1, title="hub: the kingdom park (48x40 tiles)")

    preview_scale(previews, os.path.join(out, "preview_world.png"), scale=1,
                  cols=3, labels=labels,
                  title="Capybara Kingdom: 6 Regions In-Game Battle Scenery")

    emit_hub_layout_header(os.path.join(root, "source", "hub_layout.h"))


if __name__ == "__main__":
    main()
