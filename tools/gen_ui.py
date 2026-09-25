"""
Full-screen UI art: the title screen and the world map.

Both are 256x192 8bpp images blitted straight into the main screen's shadow
buffer, so they share the game's palette and need no separate asset pipeline.
They are *backgrounds*: the C side draws text, menus, node markers and the
selection cursor over them, which is why every one of them leaves the bands
where the C code draws calm.

Outputs:
  ui_title.bin          256x192, the title screen scene
  ui_worldmap.bin       256x192, the expedition chart
  worldmap_nodes.h      node anchors + names index, read by source/main.c
  preview_title.png
  preview_worldmap.png
"""

import os
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_common as gc
from gen_common import Canvas, ix, preview_scale

W, H = 256, 192

# Bands the C code draws over. Nothing visually load-bearing may go here.
TITLE_BANNER_Y = (8, 46)      # the game's name plate
TITLE_MENU_Y = (146, 190)     # the two menu entries
MAP_TEXT_Y = (0, 40)          # the map's own header strip


def stable_seed(*parts):
    return zlib.crc32("|".join(str(p) for p in parts).encode("utf-8")) & 0xFFFF


def lerp(a, b, t):
    return a + (b - a) * t


def ramp_color(stops, t):
    """Sample a multi-stop ramp at t in 0..1, returning a palette index."""
    t = max(0.0, min(1.0, t))
    n = len(stops) - 1
    i = min(int(t * n), n - 1)
    local = t * n - i
    a = gc._RGB[stops[i]]
    b = gc._RGB[stops[i + 1]]
    rgb = (int(lerp(a[0], b[0], local)),
           int(lerp(a[1], b[1], local)),
           int(lerp(a[2], b[2], local)))
    return nearest_index(rgb)


_PAL_CACHE = None


def nearest_index(rgb):
    """Snap an arbitrary RGB to the closest palette entry.

    The UI art is the one place where a smooth gradient is worth having, and
    the DS palette is fixed, so the gradient has to be quantised. Doing it by
    nearest-neighbour over the *whole* palette keeps the banding honest: it
    picks whatever the palette actually has rather than assuming a ramp exists.
    """
    global _PAL_CACHE
    if _PAL_CACHE is None:
        _PAL_CACHE = []
        for name in gc.PALETTE_NAMES:
            # Skip the magenta transparent marker: it is never a real colour.
            if name == "transparent":
                continue
            _PAL_CACHE.append((gc._RGB[name], gc.PAL[name]))
    best = None
    best_d = 1 << 30
    for (prgb, idx) in _PAL_CACHE:
        d = ((prgb[0] - rgb[0]) ** 2 * 3 +
             (prgb[1] - rgb[1]) ** 2 * 4 +
             (prgb[2] - rgb[2]) ** 2 * 2)
        if d < best_d:
            best_d = d
            best = idx
    return best


# ---------------------------------------------------------------------------
# Title screen
# ---------------------------------------------------------------------------

def build_title():
    """A sunset over the kingdom, with the pair and the cake in the foreground.

    Composition: three depth layers (sky, hills, meadow) so the eye has
    somewhere to go, the village skyline on the horizon line at roughly a
    third up, and the two subjects -- the capybaras and the cake -- centred in
    the calm band between the two text panels.
    """
    c = Canvas(W, H)

    # --- sky ------------------------------------------------------------
    sky_stops = ["purple_dark", "castle_mid", "purple_mid",
                 "orange_mid", "yellow_mid", "yellow_light"]
    horizon = 104
    for y in range(horizon):
        t = y / horizon
        # Bias the ramp so the warm band stays low, close to the horizon,
        # which is what makes it read as a sunset rather than as a rainbow.
        c.hline(0, W - 1, y, ramp_color(sky_stops, t ** 1.7))

    # Stars, thinning out toward the warm end.
    rng = random_module()
    for i in range(70):
        sx = rng.randrange(W)
        sy = rng.randrange(0, 62)
        if rng.random() < (1.0 - sy / 70.0):
            c.set(sx, sy, ix("white_spark" if i % 3 else "yellow_light"))

    # Sun, off-centre and clear of the hills. A centred sun makes a symmetric
    # picture, and one sitting *on* the ridge line disappears into it.
    sun_x, sun_y, sun_r = 190, 70, 19
    for r in range(sun_r, 0, -1):
        t = 1.0 - r / sun_r
        c.circle(sun_x, sun_y, r, ramp_color(["orange_mid", "ember_orange",
                                              "yellow_mid", "lamp_core"], t))
    # A few horizontal cloud bars across the sun: the classic sunset read.
    for (cy, cw, col) in ((58, 44, "orange_mid"), (66, 60, "lamp_glow"),
                          (74, 38, "yellow_mid")):
        c.rect(sun_x - cw // 2, cy, cw, 3, ix(col))
        c.rect(sun_x - cw // 2 + 4, cy + 3, cw - 8, 1, ix(col))

    # --- far hills -------------------------------------------------------
    for x in range(W):
        h = 26 + int(9 * _wave(x, 0.031, 1.4) + 5 * _wave(x, 0.07, 0.3))
        c.rect(x, horizon - h, 1, h + 8, ix("slate_dark"))
    for x in range(W):
        h = 16 + int(8 * _wave(x, 0.021, 3.1) + 4 * _wave(x, 0.055, 1.7))
        c.rect(x, horizon - h + 12, 1, h + 10, ix("pine_dark"))

    # --- the village on the horizon -------------------------------------
    ground_y = horizon + 4
    c.rect(0, ground_y, W, H - ground_y, ix("grass_mid"))
    for y in range(ground_y, H):
        t = (y - ground_y) / (H - ground_y)
        c.hline(0, W - 1, y, ramp_color(["grass_dark", "grass_mid",
                                         "grass_light"], t * 0.85))

    rng = random_module(seed=7)
    # Houses across the skyline, tallest near the centre.
    for i in range(14):
        hx = 18 + i * 17 + rng.randrange(-4, 5)
        hw = rng.choice([14, 18, 22])
        hh = 14 + int(10 * _wave(hx, 0.02, 0.0)) + rng.randrange(0, 6)
        hy = ground_y - hh
        roof = "tile_mid" if i % 2 else "slate_mid"
        c.rect(hx, hy + 6, hw, hh - 6, ix("plank_mid"))
        c.rect(hx, hy + 6, hw, 2, ix("plank_light"))
        # Roof: a triangle, drawn as shrinking rows.
        for k in range(7):
            c.rect(hx - 2 + k, hy + k, hw + 4 - k * 2, 1, ix(roof))
            c.set(hx - 1 + k, hy + k, ix("white_spark"))
        # A lit window.
        c.rect(hx + hw // 2 - 2, hy + hh - 8, 4, 4, ix("lamp_core"))
        c.rect(hx + hw // 2 - 2, hy + hh - 8, 4, 1, ix("lamp_glow"))

    # Trees between the houses.
    for i in range(20):
        tx = rng.randrange(W)
        ty = ground_y + rng.randrange(-4, 12)
        tr = rng.choice([5, 6, 7])
        c.circle(tx, ty - tr, tr, ix("leaf_dark"))
        c.circle(tx - 1, ty - tr - 1, tr - 2, ix("leaf_mid"))
        c.circle(tx - 2, ty - tr - 2, 1, ix("leaf_light"))
        c.rect(tx - 1, ty, 2, 4, ix("trunk_mid"))

    # --- foreground meadow ----------------------------------------------
    # A darker lip at the very bottom gives the picture a frame and stops the
    # menu text from sitting on the brightest green on the screen.
    for y in range(150, H):
        c.hline(0, W - 1, y, ix("grass_dark"))
    for y in range(160, H):
        c.hline(0, W - 1, y, ix("grn_dark"))

    # A path curving up to the village.
    for y in range(H - 1, 128, -1):
        t = (H - y) / (H - 128)
        cx = 128 + int(28 * _wave(y, 0.05, 0.9))
        half = int(lerp(26, 5, t))
        c.hline(cx - half, cx + half, y, ix("plank_mid"))
        c.hline(cx - half, cx - half + 1, y, ix("plank_light"))

    # Flowers and grass tufts on the meadow, kept out of the menu band.
    for i in range(46):
        fx = rng.randrange(W)
        fy = 128 + rng.randrange(0, 22)
        if abs(fx - 128) < 34 and fy > 132:
            continue
        col = rng.choice(["pink_mid", "yellow_mid", "cherry_mid",
                          "purple_mid", "white_spark"])
        c.set(fx, fy, ix(col))
        c.set(fx - 1, fy, ix(col))
        c.set(fx, fy - 1, ix("leaf_light"))
        c.set(fx, fy + 1, ix("grn_dark"))

    # --- the pair and the cake ------------------------------------------
    _draw_cake(c, 128, 128, scale=1)
    return c


def _draw_cake(c, cx, base_y, scale=1):
    """A three-tier cake with frosting, cherries and a lit candle.

    Drawn rather than blitted so the title art can vary the scale, and because
    the cake is the game's one piece of iconography -- it has to be the same
    cake here, on the ending screen and in the oven tile.
    """
    s = scale

    def box(x, y, w, h, col):
        c.rect(cx + x * s, base_y + y * s, w * s, h * s, ix(col))

    # Bottom tier.
    box(-30, 4, 60, 18, "choco_mid")
    box(-30, 4, 60, 3, "choco_light")
    box(-30, 20, 60, 2, "choco_dark")
    # Frosting drips over the tier below.
    for i in range(6):
        box(-30 + i * 10, 2, 7, 6, "cream_mid")
        box(-29 + i * 10, 2, 5, 3, "cream_light")
    # Middle tier.
    box(-22, -14, 44, 18, "sponge_mid")
    box(-22, -14, 44, 3, "sponge_light")
    box(-22, 2, 44, 2, "sponge_dark")
    for i in range(5):
        box(-22 + i * 9, -16, 6, 5, "pink_mid")
    # Top tier.
    box(-14, -30, 28, 16, "cherry_mid")
    box(-14, -30, 28, 3, "cherry_light")
    for i in range(4):
        box(-14 + i * 7, -32, 5, 4, "cream_mid")
    # Cherry and candle.
    box(-3, -38, 6, 6, "cherry_dark")
    box(-2, -38, 4, 3, "cherry_light")
    box(-1, -52, 2, 14, "white")
    box(-1, -52, 1, 14, "grey_light")
    box(-2, -58, 4, 6, "ember_orange")
    box(-1, -60, 2, 4, "ember_yellow")
    box(-1, -60, 2, 1, "white_spark")
    # A soft glow around the flame.
    for (dx, dy, col) in ((-4, -56, "lamp_glow"), (4, -56, "lamp_glow"),
                          (0, -62, "lamp_core")):
        box(dx, dy, 1, 1, col)


def _wave(x, freq, phase):
    import math
    return math.sin(x * freq + phase)


def random_module(seed=None):
    import random
    return random.Random(seed if seed is not None else 20260917)


# ---------------------------------------------------------------------------
# World map
# ---------------------------------------------------------------------------

# Where each region's node sits on the chart, plus the castle. Kept in the
# generator because the art has to place the biome under the node -- moving a
# node in C without moving the biome would leave the marker floating over the
# wrong landscape.
MAP_NODES = [
    # (key, cx, cy, biome palette ramp)
    ("rain", 58, 132, ("map_forest", "map_forest_ho", "grn_dark")),
    ("farm", 74, 62, ("map_desert", "map_desert_ho", "clay_mid")),
    ("past", 152, 48, ("map_grass", "map_grass_ho", "grass_dark")),
    # The marsh has to be unmistakably *not* the jungle: the jungle is one
    # green and the marsh is a sick olive-brown water meadow, so the two never
    # get confused at a glance even though both are green.
    ("marsh", 116, 142, ("murk_mid", "cane_dark", "murk_dark")),
    ("cacao", 200, 122, ("map_ash", "map_ash_ho", "ash_dark")),
]
MAP_CASTLE = (136, 96)

MAP_REGION_NAMES = ["雨林", "农场", "牧场", "沼泽", "可可火山"]


def build_worldmap():
    """A parchment chart of the kingdom's island.

    The chart is the level-select screen, so legibility beats realism: each
    destination is a labelled blob of its own biome colour with a hard outline,
    and the routes between them are drawn as dotted trails. A player should be
    able to tell in one glance which blob is the volcano.
    """
    c = Canvas(W, H)

    # --- sea -------------------------------------------------------------
    for y in range(H):
        t = y / H
        c.hline(0, W - 1, y, ramp_color(["map_sea", "map_sea_ho"], t * 0.7))
    rng = random_module(seed=99)
    for i in range(150):
        wx = rng.randrange(W)
        wy = rng.randrange(H)
        c.set(wx, wy, ix("foam"))
        c.set(wx + 1, wy, ix("map_sea_ho"))
        c.set(wx + 2, wy, ix("map_sea_ho"))

    # --- landmass --------------------------------------------------------
    # Built from overlapping ellipses, then a foam ring found by testing each
    # sea pixel for a land neighbour. Doing it as a mask (rather than painting
    # ellipses and hoping) is what lets the coastline be a clean 1px line.
    land = [[False] * W for _ in range(H)]
    blobs = [(128, 100, 96, 62), (74, 74, 46, 38), (74, 132, 42, 34),
             (150, 56, 44, 30), (196, 118, 40, 34), (140, 150, 52, 30),
             (196, 62, 32, 26), (52, 104, 30, 30)]
    for (bx, by, brx, bry) in blobs:
        for y in range(max(0, by - bry - 2), min(H, by + bry + 3)):
            for x in range(max(0, bx - brx - 2), min(W, bx + brx + 3)):
                dx = (x - bx) / brx
                dy = (y - by) / bry
                # A little noise on the radius keeps the coast from being a
                # stack of perfect ellipses.
                n = 1.0 + 0.11 * _wave(x * 0.9 + y * 1.7, 0.09, bx)
                if dx * dx + dy * dy <= n:
                    land[y][x] = True

    for y in range(H):
        for x in range(W):
            if not land[y][x]:
                continue
            near_sea = False
            for (dx, dy) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if not (0 <= nx < W and 0 <= ny < H) or not land[ny][nx]:
                    near_sea = True
                    break
            c.set(x, y, ix("foam") if near_sea else ix("map_paper"))

    # Inland shading: a band of the shaded parchment just inside the coast,
    # which is what gives the island some relief instead of a flat cut-out.
    for y in range(H):
        for x in range(W):
            if not land[y][x]:
                continue
            for (dx, dy) in ((3, 0), (-3, 0), (0, 3), (0, -3)):
                nx, ny = x + dx, y + dy
                if not (0 <= nx < W and 0 <= ny < H) or not land[ny][nx]:
                    if c.get(x, y) == ix("map_paper"):
                        c.set(x, y, ix("map_paper_shade"))

    # --- biome washes ----------------------------------------------------
    # Each biome is a cluster of overlapping lobes rather than one ellipse.
    # A single noisy ellipse still reads as a circle with a ragged edge; two
    # or three lobes at different sizes read as a landscape.
    LOBES = {
        "rain": [(-12, 6, 22, 18), (14, -4, 20, 16), (2, 16, 16, 12),
                 (-4, -14, 14, 11)],
        "farm": [(-10, -4, 20, 15), (10, 8, 16, 12), (4, -12, 12, 9)],
        "past": [(-8, 2, 20, 14), (12, -6, 16, 12), (-2, -10, 12, 9)],
        "marsh": [(-10, 2, 16, 10), (10, -3, 13, 9), (0, 9, 12, 7)],
        "cacao": [(-6, 4, 17, 14), (10, -6, 13, 11), (-10, -8, 11, 9)],
    }
    for (key, nx, ny, ramp) in MAP_NODES:
        base, hi, lo = ramp
        for (ox, oy, rx, ry) in LOBES[key]:
            _paint_blob(c, land, nx + ox, ny + oy, rx, ry, base, 0.30)

    # Light cores, painted after every base so a later biome's wash cannot
    # cover an earlier biome's highlight.
    for (key, nx, ny, ramp) in MAP_NODES:
        base, hi, lo = ramp
        for (ox, oy, rx, ry) in LOBES[key][:2]:
            _paint_blob(c, land, nx + ox, ny + oy, rx * 0.62, ry * 0.62, hi,
                        0.34)

    # Texture marks: darker specks so a wash never reads as flat colour, and
    # a scattering of the biome's own dark tone around its rim.
    for (key, nx, ny, ramp) in MAP_NODES:
        base, hi, lo = ramp
        for i in range(34):
            tx = nx + rng.randrange(-34, 35)
            ty = ny + rng.randrange(-26, 27)
            if 0 <= tx < W and 0 <= ty < H and land[ty][tx]:
                if c.get(tx, ty) in (ix(base), ix(hi)):
                    c.set(tx, ty, ix(lo))

    # --- terrain features -------------------------------------------------
    _map_river(c, land)
    _map_mountains(c, land)
    _map_forest_dots(c, land, rng)

    # --- routes -----------------------------------------------------------
    hx, hy = MAP_CASTLE
    for (key, nx, ny, ramp) in MAP_NODES:
        _dotted_route(c, land, hx, hy, nx, ny)
    _map_castle(c, hx, hy)
    return c


def _paint_blob(c, land, cx, cy, rx, ry, color, noise):
    """Fill one organic lobe, clipped to land.

    The radius is modulated by two sines at different frequencies: one slow
    enough to make the outline wander, one faster to break up the regularity.
    A single frequency produces a scalloped edge that still reads as a circle.
    """
    idx = ix(color)
    for y in range(max(0, int(cy - ry - 4)), min(H, int(cy + ry + 5))):
        for x in range(max(0, int(cx - rx - 4)), min(W, int(cx + rx + 5))):
            if not land[y][x]:
                continue
            dx = (x - cx) / rx
            dy = (y - cy) / ry
            d = dx * dx + dy * dy
            n = (1.0
                 + noise * _wave(x * 0.7 + y * 1.1, 0.11, cx * 0.3)
                 + noise * 0.55 * _wave(x * 1.9 - y * 0.8, 0.27, cy * 0.7))
            if d <= n:
                c.set(x, y, idx)


def _map_river(c, land):
    """A river wandering from the volcano side down to the south coast.

    Routed as a sine-modulated walk rather than a straight line: a chart's
    river is the one line on it that must obviously be water, and a straight
    diagonal reads as a fold in the paper.
    """
    import math
    fx, fy = 178.0, 82.0
    ang = 2.25          # heading roughly down-left, toward the south coast
    for step in range(300):
        # Steer by a slow sine so the course meanders: the one thing a river
        # must not do on a chart is run straight, because a straight line reads
        # as a fold or a border rather than as water.
        ang += 0.30 * math.sin(step * 0.075)
        fx += math.cos(ang)
        fy += math.sin(ang)
        px, py = int(fx), int(fy)
        if not (0 <= px < W and 0 <= py < H):
            break
        if not land[py][px]:
            break
        c.set(px, py, ix("map_sea_ho"))
        c.set(px, py + 1, ix("map_sea"))
        if step > 24:
            c.set(px + 1, py, ix("map_sea_ho"))


def _map_mountains(c, land):
    """Three chevrons of ash-grey peaks, marking the volcano side."""
    for i, (mx, my) in enumerate(((206, 96), (218, 108), (194, 108))):
        for k in range(7):
            y = my - 6 + k
            if not (0 <= y < H):
                continue
            for x in range(mx - k, mx + k + 1):
                if 0 <= x < W and land[y][x]:
                    c.set(x, y, ix("stone_mid") if k < 4 else ix("stone_dark"))
        c.set(mx, my - 6, ix("snow_light"))
        c.set(mx, my - 5, ix("stone_light"))


def _map_forest_dots(c, land, rng):
    """Little tree crowns over the jungle, the universal map convention."""
    for i in range(70):
        tx = rng.randrange(W)
        ty = rng.randrange(H)
        if not (0 <= tx < W and 0 <= ty < H) or not land[ty][tx]:
            continue
        if c.get(tx, ty) not in (ix("map_forest"), ix("map_forest_ho")):
            continue
        c.set(tx, ty, ix("grn_dark"))
        c.set(tx, ty - 1, ix("leaf_mid"))
        c.set(tx - 1, ty, ix("grn_dark"))


def _dotted_route(c, land, x0, y0, x1, y1):
    """A dotted trail between two nodes, drawn only over land."""
    steps = max(abs(x1 - x0), abs(y1 - y0))
    for i in range(0, steps, 4):
        t = i / steps
        x = int(lerp(x0, x1, t))
        y = int(lerp(y0, y1, t))
        if not (0 <= x < W - 1 and 0 <= y < H - 1):
            continue
        if not land[y][x]:
            continue
        c.set(x, y, ix("map_route"))
        c.set(x + 1, y, ix("map_route"))
        c.set(x, y + 1, ix("map_ink"))


def _map_castle(c, x, y):
    """The kingdom's own marker: a keep, so home is never confused with a
    destination."""
    c.rect(x - 11, y - 9, 22, 19, ix("map_ink"))
    c.rect(x - 10, y - 8, 20, 17, ix("stone_dark"))
    c.rect(x - 10, y - 8, 20, 2, ix("stone_mid"))
    # Battlements.
    for i in range(5):
        c.rect(x - 10 + i * 4, y - 12, 3, 4, ix("stone_dark"))
        c.rect(x - 10 + i * 4, y - 12, 3, 1, ix("stone_mid"))
    # Keep and a lit window.
    c.rect(x - 5, y - 18, 10, 10, ix("stone_dark"))
    c.rect(x - 5, y - 18, 10, 2, ix("stone_mid"))
    c.rect(x - 2, y - 15, 4, 4, ix("lamp_core"))
    # A banner, which is what makes it read as *our* castle.
    c.rect(x, y - 26, 8, 6, ix("cloth_red"))
    c.rect(x, y - 26, 8, 2, ix("cloth_red_hi"))
    c.rect(x + 7, y - 27, 1, 8, ix("map_ink"))
    c.rect(x - 6, y + 10, 12, 3, ix("map_ink"))
    c.rect(x - 4, y + 10, 8, 2, ix("plank_mid"))


def emit_map_header(path):
    lines = [
        "// Autogenerated by tools/gen_ui.py -- do not edit.",
        "// World-map node anchors in SCREEN pixels of ui_worldmap.bin.",
        "",
        "#pragma once",
        "",
        f"#define WORLD_MAP_W {W}",
        f"#define WORLD_MAP_H {H}",
        f"#define WORLD_MAP_HOME_X {MAP_CASTLE[0]}",
        f"#define WORLD_MAP_HOME_Y {MAP_CASTLE[1]}",
        "",
    ]
    for i, (key, nx, ny, _ramp) in enumerate(MAP_NODES):
        lines.append(f"#define WORLD_MAP_NODE_{i}_X {nx}")
        lines.append(f"#define WORLD_MAP_NODE_{i}_Y {ny}")
    lines.append("")
    # A flat array so the C side can walk it without a switch.
    lines.append("static const short g_world_map_node_xy"
                 f"[{len(MAP_NODES)}][2] = {{")
    for (key, nx, ny, _ramp) in MAP_NODES:
        lines.append(f"    {{ {nx}, {ny} }},   // {key}")
    lines.append("};")
    lines.append("")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("world map header ->", path)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = sys.argv[1] if len(sys.argv) > 1 else "graphics"
    if not os.path.isabs(out):
        out = os.path.join(root, out)
    os.makedirs(out, exist_ok=True)

    title = build_title()
    gc.write_bin(os.path.join(out, "ui_title.bin"), title.to_bmp8())
    print(f"ui_title: {W}x{H}, {len(title.to_bmp8())} B")

    world = build_worldmap()
    gc.write_bin(os.path.join(out, "ui_worldmap.bin"), world.to_bmp8())
    print(f"ui_worldmap: {W}x{H}, {len(world.to_bmp8())} B")

    preview_scale([title], os.path.join(out, "preview_title.png"), scale=2,
                  cols=1, title="title screen scene")
    preview_scale([world], os.path.join(out, "preview_worldmap.png"), scale=2,
                  cols=1, title="expedition chart")

    emit_map_header(os.path.join(root, "source", "worldmap_nodes.h"))


if __name__ == "__main__":
    main()
