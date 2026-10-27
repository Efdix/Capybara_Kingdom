"""
Full-screen UI art: the title screen and the world map.

Both are 256x192 8bpp images blitted straight into the main screen's buffer
(render_ui_image in source/render.c -- row-major, opaque, unscaled), so they
share the game's palette and need no separate asset pipeline. They are
*backgrounds*: everything the player reads or presses is drawn by the C side
on top. The overlays below are read out of source/main.c, not guessed, and
every one of them gets calm ground in the art:

  Title screen (top screen, draw_title / title_screen):
    * ui_plate(UI_MAIN, 28, 10, 200, 50) -- the name plate, covers y10..59
    * a yellow "materials collected" line at (74, 66) once the cake is ready
    * the two capybaras are RUNTIME sprites: 32x32 blits at (96, 138) and
      (128, 138) with a +-2px celebrate hop. The art deliberately does not
      draw capybaras -- it keeps their footing clean instead.
    * a 2x2 candle flicker at (127, 62), sitting just above the flame, so the
      flame tip lives at ~(128, 62)
    * 14 drifting 1px sparkles across y0..54

  World map (top screen, draw_world_map):
    * ui_plate(UI_MAIN, 28, 2, 200, 44) -- the banner, covers y2..45
    * two circle outlines r20/r21 around the home castle at (136, 96)
    * dotted straight routes from the castle to each node (2x2 dots on every
      even step of a 24-step lerp -- the art's own trail uses the same line)
    * the five node pins: r9 circles + a rotating spark ring when selected +
      threat pips at y+12. Node anchors are emitted as worldmap_nodes.h from
      the same MAP_NODES table that paints the biomes, so art and C side
      cannot drift apart; the art keeps a quiet r20 clearing around each.

Style: Hollow Knight depth (layered silhouettes, dithered haze, one restrained
high-contrast accent light) meets Zelda cartography (relief coasts, ink icons).

Outputs:
  ui_title.bin          256x192, the title screen scene
  ui_worldmap.bin       256x192, the expedition chart
  worldmap_nodes.h      node anchors, read by source/main.c
  preview_title.png
  preview_worldmap.png
"""

import math
import os
import random
import sys
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_common as gc
from gen_common import Canvas, ix, preview_scale

W, H = 256, 192

# ---------------------------------------------------------------------------
# C-side overlay geometry -- read out of source/main.c, do not guess.
# ---------------------------------------------------------------------------

TITLE_PLATE_RECT = (28, 10, 200, 50)     # name plate, covers y10..59
TITLE_STAR_TEXT = (74, 66)               # yellow "materials" line, cake ready
TITLE_HEROES = ((96, 138), (128, 138))   # 32x32 runtime sprites, +-2 hop
TITLE_FLAME_XY = (128, 62)               # flame tip; C flickers at (127, 62)

MAP_BANNER_RECT = (28, 2, 200, 44)       # chart banner, covers y2..45
MAP_HOME_RING_R = 21                     # C draws circle outlines r20/r21 home
NODE_QUIET_R = 20                        # pin r9 + selection ring + threat pips

# Sunset composition anchors.
HORIZON = 124                            # where the meadow starts
SUN = (192, 88, 24)                      # cx, cy, r -- clear of the plate
CAKE = (128, 148)                        # cx, plate line
TREE_BASE = (88, 134)                    # trunk foot, left of the cake


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

_BAYER4 = (0, 8, 2, 10,
           12, 4, 14, 6,
           3, 11, 1, 9,
           15, 7, 13, 5)


def bayer4(x, y):
    """4x4 ordered-dither threshold in 0..1 -- the honest 8bpp gradient."""
    return _BAYER4[(y & 3) * 4 + (x & 3)] / 15.0


def lerp(a, b, t):
    return a + (b - a) * t


def _wave(x, freq, phase):
    return math.sin(x * freq + phase)


def random_module(seed=None):
    return random.Random(seed if seed is not None else 20260917)


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


def paint_banded_gradient(c, y0, y1, stops, gamma=1.0):
    """Full-width vertical gradient in `stops`, top colour first.

    Solid tone bands whose seams are crossed by a 4x4 ordered dither: at 8bpp
    with no alpha this is the only honest gradient, and the checker seam is
    what stops two adjacent tones reading as a hard posterised edge.
    """
    nseg = len(stops) - 1
    span = max(1, y1 - y0)
    for y in range(y0, y1 + 1):
        t = ((y - y0) / span) ** gamma
        level = t * nseg
        base = min(int(level), nseg - 1)
        frac = level - base
        row = y * c.w
        for x in range(c.w):
            idx = base + 1 if frac > bayer4(x, y) else base
            c.px[row + x] = ix(stops[idx])


def glow(c, cx, cy, rx, ry, tones=("lamp_glow", "sunset_gold", "cream")):
    """A dithered radial glow: sparse outer ring, dense middle, hot core.

    Three stacked checker layers at alternating parity -- the outer layer
    seeds one checker, the middle fills it in (so the middle reads solid),
    and the core overwrites one parity again. No alpha, no hard rim, which
    is exactly what a candle flame or a window wants at 8bpp.
    """
    p = (int(cx) + int(cy)) & 1
    c.dither_ellipse(cx, cy, rx, ry, ix(tones[0]), parity=p)
    c.dither_ellipse(cx, cy, rx * 0.7, ry * 0.7, ix(tones[1]), parity=1 - p)
    c.dither_ellipse(cx, cy, rx * 0.42, ry * 0.42, ix(tones[2]), parity=p)


def circle_outline(c, cx, cy, r, color):
    """Midpoint circle outline, 1px."""
    col = ix(color)
    x, y, err = r, 0, 1 - r
    while x >= y:
        for (px, py) in ((x, y), (y, x), (-x, y), (-y, x),
                         (-x, -y), (-y, -x), (x, -y), (y, -x)):
            c.set(cx + px, cy + py, col)
        y += 1
        if err < 0:
            err += 2 * y + 1
        else:
            x -= 1
            err += 2 * (y - x) + 1


# ---------------------------------------------------------------------------
# Title screen
# ---------------------------------------------------------------------------

SKY_STOPS = ["dusk_deep", "dusk_indigo", "dusk_violet", "dusk_plum",
             "sunset_rose", "sunset_ember", "sunset_gold"]
MEADOW_STOPS = ["murk_mid", "dusk_grass", "dusk_grass_deep"]


def build_title():
    """A dusk birthday: layered silhouettes, one low sun, the cake aglow.

    Depth reads from five planes -- sky, sun haze, three ridges getting
    darker as they near, the meadow, and the foreground grass band. The cake
    is the one saturated warm mass in the middle distance; the two capybaras
    watching it are runtime sprites, so the art only reserves their footing.
    """
    c = Canvas(W, H)

    # --- sky: banded dusk gradient with dithered seams --------------------
    paint_banded_gradient(c, 0, HORIZON - 1, SKY_STOPS, gamma=1.3)

    # Stars, thinning out toward the warm horizon. Most of the upper band is
    # covered by the C name plate; the visible corners get deliberate stars.
    rng = random_module(seed=7)
    for i in range(60):
        sx = rng.randrange(W)
        sy = rng.randrange(0, 56)
        if rng.random() < sy / 66.0:
            continue
        c.set(sx, sy, ix("white_spark" if i % 4 else "yellow_light"))
    for (sx, sy) in ((10, 16), (240, 24), (26, 52), (234, 44), (6, 40)):
        c.sparkle(sx, sy, 1, "white_spark", core="white_spark")

    # --- the sun and its haze --------------------------------------------
    # Four checker rings, each a 50%->100% step of the same two warm tones,
    # so the falloff never shows a hard disc edge: the classic "glow without
    # alpha" build-up. The disc itself is painted last, on top.
    sun_x, sun_y, sun_r = SUN
    p0 = (sun_x + sun_y) & 1
    c.dither_ellipse(sun_x, sun_y, 54, 46, ix("lamp_glow"), parity=p0)
    c.dither_ellipse(sun_x, sun_y, 46, 39, ix("lamp_glow"), parity=1 - p0)
    c.dither_ellipse(sun_x, sun_y, 38, 32, ix("sunset_gold"), parity=p0)
    c.dither_ellipse(sun_x, sun_y, 30, 25, ix("sunset_gold"), parity=1 - p0)
    for r in range(sun_r, 0, -1):
        t = 1.0 - r / sun_r
        c.circle(sun_x, sun_y, r,
                 ramp_color(["sunset_ember", "sunset_gold", "yellow_light",
                             "cream"], t))

    # Cloud bars crossing the disc: the classic sunset read, and a dark
    # backstop that keeps the yellow star line legible when it appears.
    c.rect(150, 78, 86, 3, ix("cloud_dusk"))
    c.rect(140, 92, 96, 3, ix("dusk_plum"))
    c.hline(150, 236, 91, ix("sunset_gold"))
    c.rect(168, 103, 48, 2, ix("cloud_dusk"))
    # Far layer: long thin bars higher up (partly behind the name plate).
    c.rect(96, 64, 84, 2, ix("dusk_plum"))
    c.rect(30, 70, 60, 2, ix("cloud_dusk"))
    c.rect(150, 56, 80, 2, ix("cloud_dusk"))
    # Near layer: puffy clouds with lit top edges, left of the sun.
    for lobes in (((44, 98, 11, 4), (54, 96, 13, 5), (64, 99, 9, 3)),
                  ((96, 91, 12, 4), (108, 89, 14, 5), (118, 92, 10, 3))):
        for (lx, ly, lrx, lry) in lobes:
            c.ellipse(lx, ly, lrx, lry, ix("dusk_plum"))
        for (lx, ly, lrx, lry) in lobes:
            for i in range(-lrx + 1, lrx, 2):
                dx = i / lrx
                yt = ly - lry * math.sqrt(max(0.0, 1 - dx * dx))
                c.set(lx + i, int(yt) - 1, ix("sunset_gold"))

    # --- three ridge layers, lighter with distance ------------------------
    _ridge(c, base=102, a1=5, f1=0.045, p1=0.8, a2=2.5, f2=0.13, p2=2.0,
           color="haze_mauve", bottom=126, lit=True)
    ridge_b = _ridge(c, base=111, a1=6, f1=0.032, p1=2.6, a2=3, f2=0.11,
                     p2=0.5, color="dusk_plum", bottom=127)
    # The hill castle on the mid ridge's left shoulder, against the quiet
    # sunset bands: dark silhouette, warm windows. Painted between the mid
    # and near ridges so the near ridge tucks its base away.
    _hill_castle(c, 30, ridge_b(30) + 2)
    _ridge(c, base=118, a1=4, f1=0.05, p1=4.4, a2=2, f2=0.17, p2=1.2,
           color="dusk_violet", bottom=127, teeth=True)

    # --- the meadow -------------------------------------------------------
    paint_banded_gradient(c, HORIZON, H - 1, MEADOW_STOPS, gamma=1.6)

    # Meadow dressing: tufts and a few muted flowers, kept off the cake and
    # the runtime sprites' footing.
    for i in range(30):
        fx = rng.randrange(6, W - 6)
        fy = HORIZON + rng.randrange(0, 20)
        if 88 <= fx <= 168 or (TITLE_HEROES[0][0] - 4 <= fx <=
                               TITLE_HEROES[1][0] + 36 and fy > 132):
            continue
        if i % 3:
            c.vline(fx, fy, fy + 1, ix("murk_mid"))
        else:
            col = rng.choice(["cherry_mid", "pink_mid", "yellow_mid"])
            c.set(fx, fy, ix(col))
            c.set(fx, fy + 1, ix("dusk_grass_deep"))

    # Candlelight pooled over the meadow around the cake, and cast shadows.
    p0 = (CAKE[0] + 114) & 1
    c.dither_ellipse(CAKE[0], 114, 62, 26, ix("lamp_glow"), parity=p0)
    c.dither_ellipse(CAKE[0], 114, 52, 21, ix("sunset_gold"), parity=1 - p0)
    c.dither_ellipse(CAKE[0], 114, 32, 14, ix("cream"), parity=p0)
    c.ellipse(112, 169, 12, 3, ix("dusk_grass_deep"))
    c.ellipse(144, 169, 12, 3, ix("dusk_grass_deep"))

    # --- the silver-leaved tree over the cake ------------------------------
    _tree(c, rng)

    # --- the cake ----------------------------------------------------------
    _draw_cake(c, CAKE[0], CAKE[1])

    # --- foreground grass band --------------------------------------------
    _foreground_grass(c, rng)

    # --- fireflies ----------------------------------------------------------
    # Placed clear of the sun disc and the cake so each point stays visible.
    for (fx, fy, fr, fcol) in ((12, 94, 1, "yellow_light"),
                               (44, 118, 2, "lamp_glow"),
                               (58, 92, 1, "white_spark"),
                               (78, 110, 1, "yellow_light"),
                               (90, 140, 1, "lamp_glow"),
                               (160, 98, 1, "yellow_light"),
                               (188, 120, 2, "lamp_glow"),
                               (222, 104, 1, "yellow_light"),
                               (232, 80, 1, "white_spark"),
                               (176, 134, 2, "yellow_light")):
        c.sparkle(fx, fy, fr, fcol, core="white_spark")
    return c


def _ridge(c, base, a1, f1, p1, a2, f2, p2, color, bottom, lit=False,
           teeth=False):
    """One silhouette ridge; returns a function giving its top row per x."""
    rng = random_module(seed=int(base * 31 + a1))

    def top(x):
        return int(base + a1 * _wave(x, f1, p1) + a2 * _wave(x, f2, p2))

    for x in range(W):
        yt = top(x)
        c.vline(x, yt, bottom, ix(color))
        if lit and x % 2 == 0:
            c.set(x, yt, ix("sunset_ember"))
        if teeth and x % 7 == 0:
            c.vline(x, yt - rng.randrange(2, 4), yt - 1, ix(color))
    return top


def _hill_castle(c, cx, base_y):
    """The kingdom's keep on the mid ridge, backlit: dark silhouette, two
    warm windows, one banner. A treeline of bumps anchors it to the ridge."""
    body = "cloud_dusk"
    # Treeline the castle rises out of.
    for (ox, tr) in ((-19, 3), (-13, 2), (12, 3), (17, 2)):
        c.ellipse(cx + ox, base_y - tr // 2, tr, tr - 1, ix(body))
    # Keep and towers.
    c.rect(cx - 8, base_y - 12, 17, 12, ix(body))
    c.rect(cx - 13, base_y - 16, 6, 16, ix(body))
    c.rect(cx + 7, base_y - 14, 6, 14, ix(body))
    for k in range(4):                      # tower roofs
        c.rect(cx - 13 + k, base_y - 20 + k, 6 - k * 2, 1, ix(body))
        c.rect(cx + 7 + k, base_y - 18 + k, 6 - k * 2, 1, ix(body))
    for tx in (cx - 8, cx - 4, cx, cx + 4):  # keep battlements
        c.rect(tx, base_y - 14, 2, 2, ix(body))
    # Banner: the one colour on the silhouette.
    c.vline(cx, base_y - 23, base_y - 14, ix("map_ink"))
    c.rect(cx + 1, base_y - 23, 5, 3, ix("cloth_red"))
    c.set(cx + 5, base_y - 22, ix("cloth_red_hi"))
    # Two windows with a pooled glow: inhabited, not abandoned.
    for wx in (cx - 4, cx + 2):
        glow(c, wx, base_y - 7, 2, 2,
             ("dusk_plum", "sunset_gold", "lamp_core"))
        c.set(wx, base_y - 7, ix("lamp_core"))


def _tree(c, rng):
    """A big dusk tree leaning over the cake: silhouette, warm rim on the
    sun-facing edges, trunk foot flaring into the meadow."""
    bx, by = TREE_BASE
    for y in range(90, by + 1):
        t = (by - y) / (by - 90)
        x0 = bx + int(4 * t)
        wdt = 10 - int(4 * t)
        c.rect(x0 - wdt // 2, y, wdt, 1, ix("cloud_dusk"))
        if y % 4 == 0:
            c.set(x0 + wdt // 2, y, ix("sunset_ember"))
    c.rect(bx - 7, by - 2, 6, 3, ix("cloud_dusk"))
    c.rect(bx + 3, by - 2, 6, 3, ix("cloud_dusk"))
    c.rect(bx + 2, 92, 9, 2, ix("cloud_dusk"))
    c.rect(bx - 9, 96, 10, 2, ix("cloud_dusk"))

    blobs = ((76, 72, 18, 12), (98, 62, 20, 13), (108, 82, 14, 9),
             (62, 84, 13, 9), (90, 80, 16, 10))
    for (lx, ly, lrx, lry) in blobs:
        c.blob(lx, ly, lrx, lry, ix("cloud_dusk"), n=3.2)
    # Rim: dotted warm edge along the sun-facing (right/top) half of each lobe.
    for (lx, ly, lrx, lry) in blobs:
        for i in range(-lrx + 1, lrx):
            dx = i / lrx
            yt = ly - lry * math.sqrt(max(0.0, 1 - dx * dx))
            if i >= 0 and i % 2 == 0:
                c.set(lx + i, int(yt), ix("sunset_ember"))
            if i > 3 and i % 3 == 0:
                c.set(lx + i, int(yt) + 1, ix("sunset_gold"))


def _drips(c, x0, x1, y, lens):
    """Cream running off a tier edge: hanging columns with rounded tips."""
    for k, x in enumerate(range(x0, x1 - 2, 5)):
        ln = lens[k % len(lens)]
        c.rect(x, y, 3, ln, ix("cream_mid"))
        c.ellipse(x + 1, y + ln, 1.5, 1.2, ix("cream_mid"))
        c.vline(x, y, y + ln - 1, ix("cream_light"))
        c.vline(x + 2, y, y + ln - 2, ix("cream_dark"))


def _tier(c, cx, y_top, w, h, drip_lens):
    """One sponge tier with a white cream cap and dripping icing."""
    x0, x1 = cx - w // 2, cx + w // 2
    c.rect(x0, y_top, w, h, ix("sponge_mid"))
    c.rect(x0, y_top, 4, h, ix("sponge_light"))
    c.rect(x1 - 3, y_top, 3, h, ix("sponge_dark"))
    c.hline(x0, x1 - 1, y_top + h - 1, ix("sponge_dark"))
    c.hline(x0 + 3, x1 - 4, y_top + h - 3, ix("choco_dark"))
    # Cream cap, slightly overhanging, with drips down the body.
    c.rect(x0 - 1, y_top - 3, w + 2, 4, ix("cream_mid"))
    c.hline(x0 - 1, x1, y_top - 3, ix("cream_light"))
    c.hline(x0 - 1, x1, y_top, ix("cream_dark"))
    _drips(c, x0 + 1, x1 - 2, y_top + 1, drip_lens)


def _draw_cake(c, cx, plate_y):
    """The game's one piece of iconography: three sponge tiers, white drip
    icing, strawberries, and a single lit candle.

    The flame tip is pinned near TITLE_FLAME_XY because the C side flickers a
    2x2 spark at (127, 62), "just above the flame" -- move the cake and the
    flicker lands in the wrong sky.
    """
    # Cast shadow + ceramic stand.
    c.ellipse(cx, plate_y + 4, 40, 4, ix("dusk_grass_deep"))
    c.ellipse(cx, plate_y + 1, 36, 4, ix("stone_dark"))
    c.ellipse(cx, plate_y, 36, 3, ix("stone_mid"))
    c.hline(cx - 30, cx + 30, plate_y - 1, ix("stone_light"))
    c.rect(cx - 6, plate_y + 3, 13, 2, ix("stone_dark"))

    # Tiers, bottom up: each cap overhangs and drips onto the tier below.
    _tier(c, cx, 128, 68, 21, (6, 4, 7, 5, 3, 6))
    _tier(c, cx, 110, 48, 18, (4, 6, 3, 5, 4))
    _tier(c, cx, 94, 32, 16, (3, 5, 2, 4))

    # Strawberries on the top: one on a cream dollop, two against the rim.
    for (sx, sy, big) in ((cx - 10, 89, False), (cx, 85, True),
                          (cx + 11, 90, False)):
        if big:
            c.ellipse(sx, sy + 3, 3, 2, ix("cream_mid"))
        c.ellipse(sx, sy, 2, 2, ix("cherry_mid"))
        c.set(sx - 1, sy - 1, ix("cherry_light"))
        c.set(sx + 1, sy + 1, ix("cherry_dark"))
        c.set(sx, sy - 2, ix("leaf_dark"))
        c.set(sx + 1, sy - 2, ix("leaf_mid"))

    # Candle + flame. The glow's dithered rim goes down first, then the
    # grad_ellipse halo (solid warm core out to a dithered edge), so the wax
    # and the flame body stay crisp when drawn over it.
    p = (cx + 66) & 1
    c.dither_ellipse(cx, 66, 19, 20, ix("lamp_glow"), parity=p)
    c.grad_ellipse(cx, 66, 13, 14, ["lamp_glow", "sunset_gold", "cream"])
    c.rect(cx - 1, 72, 2, 19, ix("white"))
    c.vline(cx, 72, 90, ix("grey_light"))
    c.vline(cx, 70, 71, ix("outline"))
    c.ellipse(cx, 66, 2, 5, ix("ember_orange"))
    c.ellipse(cx, 67, 1, 3, ix("yellow_mid"))
    c.set(cx, 68, ix("cream"))
    c.sparkle(cx + 10, 54, 2, "yellow_light", core="white_spark")
    c.sparkle(cx - 12, 57, 1, "white_spark")


def _foreground_grass(c, rng):
    """The dark grass band that frames the bottom of the picture."""
    tops = []
    for x in range(W):
        h = 16 + int(4 * _wave(x, 0.09, 1.0) + 3 * _wave(x, 0.23, 2.2))
        tops.append(H - h)
    for x in range(W):
        c.vline(x, tops[x], H - 1, ix("dusk_grass_deep"))
        if x % 2 == 0:
            c.set(x, tops[x],
                  ix("ember_orange" if x > 150 else "murk_mid"))
    # Blades and seed heads, kept clear of the runtime sprites' footing.
    for x in range(0, W, 3):
        if 90 <= x <= 166:
            continue
        bl = rng.randrange(2, 6)
        c.vline(x, tops[x] - bl, tops[x] - 1, ix("dusk_grass_deep"))
    for (sx, sy) in ((178, 172), (192, 175), (206, 171), (222, 176),
                     (36, 174), (54, 177)):
        c.vline(sx, sy, sy + 4, ix("dusk_grass_deep"))
        c.set(sx, sy - 1, ix("ember_orange"))


# ---------------------------------------------------------------------------
# World map
# ---------------------------------------------------------------------------

# Where each region's node sits on the chart, plus the castle. This table is
# the single source of truth: it paints the biome washes AND is emitted as
# source/worldmap_nodes.h for the C side's pins, routes and pips.
MAP_NODES = [
    # (key, cx, cy, (base wash, highlight wash, rim tone))
    ("rain", 58, 132, ("map_forest", "map_forest_ho", "grn_dark")),
    ("farm", 74, 62, ("map_desert", "map_desert_ho", "clay_mid")),
    ("past", 152, 48, ("map_grass", "map_grass_ho", "grass_dark")),
    # The marsh has to be unmistakably *not* the jungle: the jungle is one
    # green and the marsh is a sick olive water meadow, so the two never get
    # confused at a glance even though both are green.
    ("marsh", 116, 142, ("murk_mid", "murk_light", "murk_dark")),
    ("cacao", 200, 122, ("map_ash", "map_ash_ho", "ash_dark")),
]
MAP_CASTLE = (136, 96)

MAP_REGION_NAMES = ["雨林", "农场", "牧场", "沼泽", "可可火山"]

# Each biome is a cluster of overlapping lobes (offsets from its node) rather
# than one ellipse: a single noisy ellipse still reads as a circle with a
# ragged edge, but two or three lobes at different sizes read as a landscape.
LOBES = {
    "rain": [(-14, 4, 20, 15), (12, -6, 17, 13), (0, 16, 14, 10),
             (-2, -16, 13, 9)],
    "farm": [(-12, -2, 18, 13), (12, 8, 15, 11), (2, -12, 12, 8)],
    "past": [(-10, 4, 18, 13), (12, -4, 15, 11), (0, -12, 11, 8)],
    "marsh": [(-12, 2, 17, 11), (12, -4, 14, 9), (0, 10, 13, 8)],
    "cacao": [(-8, 2, 18, 14), (12, -6, 14, 11), (-2, -14, 12, 9)],
}

# Island blobs: the main mass, per-region lobes, a centre filler and islets.
# Corners stay sea for the chart furniture: whirlpool NW, compass SW, boat NE.
LAND_BLOBS = [
    (128, 104, 88, 58),
    (76, 64, 38, 26),
    (150, 54, 38, 22),
    (58, 128, 30, 22),
    (114, 142, 32, 20),
    (198, 122, 36, 26),
    (170, 152, 34, 20),
    (100, 100, 40, 30),
    (32, 42, 9, 6),      # NW islet
    (240, 160, 11, 8),   # SE islet
    (26, 104, 7, 5),     # the sea monster's rock
]


def build_worldmap():
    """A parchment expedition chart: Zelda relief coasts, ink icons, burn.

    Layer order is the chart's own logic: paper, graticule and speckle; sea
    wash and wave lines; the island with its sand->grass->shade relief;
    biome washes; terrain and icons; then the burnt edges that frame it all.
    """
    c = Canvas(W, H)
    rng = random_module(seed=424242)

    # --- paper, speckle noise, graticule ----------------------------------
    c.rect(0, 0, W, H, ix("map_paper"))
    for _ in range(int(W * H * 0.02)):
        sx, sy = rng.randrange(W), rng.randrange(H)
        r = rng.random()
        c.set(sx, sy, ix("map_paper_shade" if r < 0.7 else
                         ("map_burn" if r < 0.9 else "map_ink")))
    for gx in range(32, W, 32):
        for y in range(H):
            if (gx + y) & 1:
                c.set(gx, y, ix("map_paper_shade"))
    for gy in range(24, H, 24):
        for x in range(W):
            if (x + gy) & 1:
                c.set(x, gy, ix("map_paper_shade"))

    # --- land mask + coast distances ---------------------------------------
    land = [[False] * W for _ in range(H)]
    for (bx, by, brx, bry) in LAND_BLOBS:
        for y in range(max(0, by - bry - 3), min(H, by + bry + 4)):
            for x in range(max(0, bx - brx - 3), min(W, bx + brx + 4)):
                if land[y][x]:
                    continue
                dx = (x - bx) / brx
                dy = (y - by) / bry
                n = 1.0 + 0.11 * _wave(x * 0.9 + y * 1.7, 0.09, bx)
                if dx * dx + dy * dy <= n:
                    land[y][x] = True
    dist = _coast_distances(land)

    # --- sea: tinted wash + foam ring + wave-line mesh ---------------------
    for y in range(H):
        for x in range(W):
            if land[y][x]:
                continue
            if _near_land(land, x, y):
                c.set(x, y, ix("foam"))
            elif bayer4(x, y) < 0.40:
                c.set(x, y, ix("map_sea_ho"))
    for _ in range(150):
        wx, wy = rng.randrange(2, W - 8), rng.randrange(2, H - 4)
        if not _all_sea(land, wx, wy, 6, 3):
            continue
        up = rng.random() < 0.5
        for i in range(5):
            dy = (1 if up else -1) if i in (0, 4) else 0
            c.set(wx + i, wy + dy, ix("map_sea"))
        if rng.random() < 0.35:
            c.set(wx + 2, wy, ix("foam"))

    # --- island relief: sand edge -> paper -> shaded inland ----------------
    for y in range(H):
        for x in range(W):
            if not land[y][x]:
                continue
            d = dist[y][x]
            if d == 1:
                c.set(x, y, ix("sand_light"))
            elif d in (2, 3):
                c.set(x, y, ix("map_paper"))
            elif d in (4, 5):
                c.set(x, y, ix("map_paper_shade"))
            elif bayer4(x, y) < 0.30:
                c.set(x, y, ix("map_paper_shade"))

    # --- biome washes: rim lobes, base lobes, highlight cores --------------
    for (key, nx, ny, ramp) in MAP_NODES:
        base, hi, lo = ramp
        for (ox, oy, rx, ry) in LOBES[key]:
            _paint_blob(c, land, dist, nx + ox, ny + oy, rx * 1.12, ry * 1.12,
                        lo, 0.30)
        for (ox, oy, rx, ry) in LOBES[key]:
            _paint_blob(c, land, dist, nx + ox, ny + oy, rx, ry, base, 0.30)
        for (ox, oy, rx, ry) in LOBES[key][:2]:
            _paint_blob(c, land, dist, nx + ox, ny + oy, rx * 0.6, ry * 0.6,
                        hi, 0.34)
        # A soft bright ring under where the C pin lands.
        c.dither_ellipse(nx, ny, 13, 12, ix(hi), parity=0)

    # Texture marks so a wash never reads as flat colour.
    for (key, nx, ny, ramp) in MAP_NODES:
        base, hi, lo = ramp
        for _ in range(36):
            tx = nx + rng.randrange(-32, 33)
            ty = ny + rng.randrange(-24, 25)
            if (0 <= tx < W and 0 <= ty < H and land[ty][tx]
                    and dist[ty][tx] > 3
                    and c.get(tx, ty) in (ix(base), ix(hi))):
                c.set(tx, ty, ix(lo))

    # Age stains on the open paper between regions.
    for (sx, sy, srx, sry) in ((96, 86, 13, 9), (176, 88, 12, 8),
                               (66, 104, 10, 7)):
        c.dither_ellipse(sx, sy, srx, sry, ix("map_paper_shade"), parity=0)

    # --- terrain: river, mountains, forests --------------------------------
    _map_river(c, land, dist)
    _map_mountains(c, land, dist)
    _map_forests(c, land, dist, rng)

    # --- per-region icons ----------------------------------------------------
    # Anchors hand-placed to clear the C pins' quiet radius and the coast;
    # each icon family sits in (or at the edge of) its own biome wash.
    for (x, y) in ((40, 116), (78, 148), (40, 144)):
        _icon_palm(c, x, y)
    for (x, y) in ((52, 50), (94, 80), (58, 78)):
        _icon_wheat(c, x, y)
    for (x, y) in ((148, 72), (170, 62)):
        _icon_fence(c, x, y, rng)
    for (x, y) in ((98, 128), (134, 156), (102, 156)):
        _icon_cane(c, x, y)
    for (x, y) in ((184, 138), (220, 136), (196, 98)):
        _icon_vent(c, x, y)

    # --- chart furniture ---------------------------------------------------
    _whirlpool(c, 14, 62)
    _sea_monster(c, 26, 106)
    _boat(c, 231, 78)
    _routes(c)
    _map_castle(c, MAP_CASTLE[0], MAP_CASTLE[1])
    _compass(c, 26, 168)

    # --- burnt edges, over everything at the borders ------------------------
    _burn_edges(c)
    return c


def _coast_distances(land):
    """BFS distance from open sea, over land. 0 = sea, 1 = beach ..."""
    dist = [[99] * W for _ in range(H)]
    q = deque()
    for y in range(H):
        for x in range(W):
            if not land[y][x]:
                dist[y][x] = 0
                q.append((x, y))
    while q:
        x, y = q.popleft()
        d = dist[y][x] + 1
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if 0 <= nx < W and 0 <= ny < H and land[ny][nx] \
                    and dist[ny][nx] == 99:
                dist[ny][nx] = d
                q.append((nx, ny))
    return dist


def _near_land(land, x, y):
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        nx, ny = x + dx, y + dy
        if 0 <= nx < W and 0 <= ny < H and land[ny][nx]:
            return True
    return False


def _all_sea(land, x, y, w, h):
    for j in range(y, y + h):
        for i in range(x, x + w):
            if not (0 <= i < W and 0 <= j < H) or land[j][i]:
                return False
    return True


def _far_from_pins(x, y, node_r=NODE_QUIET_R, castle_r=23):
    """True where chart furniture may sit: out of the C pins' quiet zones."""
    for (_k, nx, ny, _r) in MAP_NODES:
        if (x - nx) ** 2 + (y - ny) ** 2 < node_r * node_r:
            return False
    hx, hy = MAP_CASTLE
    if (x - hx) ** 2 + (y - hy) ** 2 < castle_r * castle_r:
        return False
    return True


def _paint_blob(c, land, dist, cx, cy, rx, ry, color, noise, min_dist=2):
    """Fill one organic lobe, clipped to land and off the beach.

    The radius is modulated by two sines at different frequencies: one slow
    enough to make the outline wander, one faster to break up the regularity.
    """
    idx = ix(color)
    for y in range(max(0, int(cy - ry) - 4), min(H, int(cy + ry) + 5)):
        for x in range(max(0, int(cx - rx) - 4), min(W, int(cx + rx) + 5)):
            if not land[y][x] or dist[y][x] < min_dist:
                continue
            dx = (x - cx) / rx
            dy = (y - cy) / ry
            d = dx * dx + dy * dy
            n = (1.0
                 + noise * _wave(x * 0.7 + y * 1.1, 0.11, cx * 0.3)
                 + noise * 0.55 * _wave(x * 1.9 - y * 0.8, 0.27, cy * 0.7))
            if d <= n:
                c.set(x, y, idx)


def _map_river(c, land, dist):
    """A river from the volcano's slopes down to the south coast.

    Steered as an absolute sine meander around a WSW heading (never a
    cumulative turn -- accumulating the wander lets the heading swing through
    full circles and the river ends up flowing uphill). The one line on a
    chart that must read as water may not run straight.
    """
    fx, fy = 206.0, 98.0
    for step in range(260):
        ang = 2.55 + 0.55 * math.sin(step * 0.055)
        fx += math.cos(ang)
        fy += math.sin(ang)
        px, py = int(fx), int(fy)
        if not (0 <= px < W and 0 <= py < H) or not land[py][px] \
                or dist[py][px] < 2:
            break
        # A thinner guard than the pins' full quiet radius: a 2px water line
        # passing 13px out still reads as calm ground around the marker.
        if not _far_from_pins(px, py, node_r=13):
            continue
        c.set(px, py, ix("map_sea_ho"))
        c.set(px, py + 1, ix("map_sea"))


def _cone(c, land, ax, ay, half_w, snow=True):
    """One shaded triangle peak with an optional snow cap, clipped to land."""
    for i in range(half_w + 1):
        y = ay + i
        if not (0 <= y < H):
            continue
        for x in range(ax - i, ax + i + 1):
            if 0 <= x < W and land[y][x]:
                c.set(x, y, ix("stone_mid" if x <= ax else "stone_dark"))
    if snow:
        for i in range(min(2, half_w)):
            y = ay + i
            for x in range(ax - i, ax + i + 1):
                if 0 <= x < W and 0 <= y < H and land[y][x]:
                    c.set(x, y, ix("snow_light"))
        if 0 <= ay < H and land[ay][ax]:
            c.set(ax, ay, ix("stone_light"))


def _map_mountains(c, land, dist):
    """Small triangle groups with snow caps, and the volcano with a crater."""
    _cone(c, land, 206, 86, 9, snow=False)          # the volcano
    _cone(c, land, 220, 104, 5)
    _cone(c, land, 190, 100, 4)
    _cone(c, land, 118, 54, 4)
    _cone(c, land, 131, 60, 5)
    # Crater + lava + a wisp of smoke.
    c.hline(203, 209, 87, ix("lava_dark"))
    c.set(205, 86, ix("lava_mid"))
    c.set(207, 86, ix("lava_mid"))
    c.set(206, 85, ix("lava_light"))
    c.set(209, 82, ix("ash_light"))
    c.set(211, 79, ix("ash_light"))
    c.sparkle(206, 82, 1, "ember_yellow")


def _tree_mark(c, x, y):
    """A two-pixel tree crown: the universal map convention."""
    c.set(x, y, ix("leaf_mid"))
    c.set(x + 1, y, ix("leaf_dark"))
    c.set(x, y + 1, ix("leaf_dark"))
    c.set(x + 1, y + 1, ix("grn_dark"))


def _map_forests(c, land, dist, rng):
    """Tree clusters over the jungle and the south-east woods."""
    for _ in range(90):
        tx, ty = rng.randrange(6, W - 6), rng.randrange(6, H - 6)
        if not land[ty][tx] or dist[ty][tx] < 3 or not _far_from_pins(tx, ty):
            continue
        if c.get(tx, ty) not in (ix("map_forest"), ix("map_forest_ho")):
            continue
        _tree_mark(c, tx, ty)
    for _ in range(24):
        tx, ty = 170 + rng.randrange(-24, 25), 152 + rng.randrange(-12, 13)
        if not (0 <= tx < W and 0 <= ty < H):
            continue
        if not land[ty][tx] or dist[ty][tx] < 3 or not _far_from_pins(tx, ty):
            continue
        if c.get(tx, ty) in (ix("map_grass"), ix("map_grass_ho"),
                             ix("map_paper")):
            _tree_mark(c, tx, ty)


# --- region icons -----------------------------------------------------------

def _icon_palm(c, x, y):
    top_x, top_y = x + 2, y - 4
    for i in range(5):
        c.set(x + i // 2, y - i, ix("trunk_mid"))
    for (dx, dy) in ((2, 0), (-2, 0), (1, -1), (-1, -1), (2, -1), (-2, -1)):
        c.set(top_x + dx, top_y + dy, ix("leaf_dark"))
    c.set(top_x, top_y - 1, ix("leaf_mid"))
    c.set(top_x - 1, top_y, ix("leaf_light"))


def _icon_wheat(c, x, y):
    for k in (-1, 0, 1):
        c.vline(x + k, y - 3, y, ix("wheat_mid" if k else "wheat_light"))
        c.set(x + k, y - 4, ix("wheat_light"))
    c.hline(x - 1, x + 1, y - 1, ix("wheat_dark"))
    c.set(x, y + 1, ix("wheat_dark"))


def _icon_fence(c, x, y, rng):
    for k in range(4):
        c.vline(x + k * 3, y - 3, y + 1, ix("plank_dark"))
    c.hline(x - 1, x + 10, y - 2, ix("plank_mid"))
    c.hline(x - 1, x + 10, y, ix("plank_mid"))
    c.set(x - 2, y + 1, ix("grass_dark"))
    c.set(x + 12, y + 1, ix("grass_dark"))


def _icon_cane(c, x, y):
    for (dx, lean) in ((0, -1), (1, 0), (2, 1)):
        for i in range(5):
            c.set(x + dx + (lean if i > 3 else 0), y - i,
                  ix("cane_light" if i % 2 else "cane_mid"))
        c.set(x + dx, y - 2, ix("cane_dark"))
    c.hline(x - 2, x + 3, y + 1, ix("murk_light"))


def _icon_vent(c, x, y):
    c.ellipse(x, y, 3, 2, ix("ash_dark"))
    c.hline(x - 1, x + 1, y - 1, ix("lava_dark"))
    c.set(x, y - 1, ix("lava_mid"))
    c.set(x, y - 2, ix("lava_light"))
    c.set(x + 2, y - 4, ix("ash_light"))
    c.set(x + 3, y - 6, ix("ash_light"))


# --- chart furniture ---------------------------------------------------------

def _whirlpool(c, cx, cy):
    for t in range(46):
        r = 0.6 + t * 0.17
        a = t * 0.5
        x = int(cx + r * math.cos(a))
        y = int(cy + r * 0.8 * math.sin(a))
        c.set(x, y, ix("map_sea"))
        if t % 3 == 0:
            c.set(x, y - 1, ix("foam"))
    for a in range(0, 360, 24):
        x = int(cx + 9 * math.cos(math.radians(a)))
        y = int(cy + 7.5 * math.sin(math.radians(a)))
        c.set(x, y, ix("map_sea"))


def _sea_monster(c, cx, cy):
    """A rock islet with two curling tentacles: the chart's one joke."""
    c.ellipse(cx, cy, 7, 4, ix("stone_dark"))
    c.ellipse(cx - 1, cy - 1, 5, 2, ix("stone_mid"))
    for (bx, lean, h) in ((cx - 4, -1, 9), (cx + 3, 1, 7)):
        for i in range(h):
            t = i / max(1, h - 1)
            x = bx + int(3.5 * lean * t * t)
            y = cy - 2 - i
            c.set(x, y, ix("map_ink"))
            c.set(x + 1, y, ix("map_ink"))
            if i % 2 == 0 and i > 1:
                c.set(x + (0 if lean > 0 else 1), y, ix("map_sea_ho"))
        tx, ty = bx + int(3.5 * lean), cy - 1 - h
        for k in range(4):
            c.set(tx + lean * k, ty - (3 - k), ix("map_ink"))
    for a in range(0, 360, 30):
        x = int(cx + 10 * math.cos(math.radians(a)))
        y = int(cy + 3.5 * math.sin(math.radians(a)))
        c.set(x, y, ix("map_sea"))


def _boat(c, x, y):
    c.rect(x + 2, y + 2, 12, 2, ix("map_ink"))
    c.set(x + 1, y + 3, ix("map_ink"))
    c.set(x + 14, y + 3, ix("map_ink"))
    c.hline(x + 3, x + 12, y + 2, ix("plank_mid"))
    c.vline(x + 7, y - 8, y + 2, ix("wood_dark"))
    for k in range(7):
        c.hline(x + 8, x + 8 + k, y - 7 + k, ix("cream"))
    c.rect(x + 4, y - 9, 3, 2, ix("cloth_red"))
    for i in range(0, 17, 3):
        c.set(x + i, y + 5, ix("map_sea"))


def _routes(c):
    """Dotted trails from the castle to each node.

    The C side draws 2x2 dots on every even step of a 24-step lerp between
    home and the node; the art uses the same line so the two coincide, with
    small single-pixel steps between the C dots.
    """
    hx, hy = MAP_CASTLE
    for (_key, nx, ny, _ramp) in MAP_NODES:
        for k in range(1, 24):
            x = hx + (nx - hx) * k // 24
            y = hy + (ny - hy) * k // 24
            if not (0 <= x < W - 1 and 0 <= y < H - 1):
                continue
            if k % 2 == 0:
                c.rect(x, y, 2, 2, ix("map_route"))
            else:
                c.set(x, y, ix("map_route"))


def _map_castle(c, x, y):
    """The kingdom's own marker: keep, towers, gate, banner.

    Sized so the C highlight ring (r20/r21 circles) rings the castle rather
    than slicing through it: the whole body sits inside r19 and the flagpole
    is the only thing the ring touches.
    """
    hx, hy = x, y
    # Plaza: quiet ground for the runtime ring to land on.
    c.ellipse(hx, hy, 23, 20, ix("map_paper_shade"))
    c.ellipse(hx, hy, 20, 17, ix("map_paper"))
    # Curtain wall.
    c.rect(hx - 14, hy - 2, 29, 10, ix("stone_mid"))
    c.hline(hx - 14, hx + 14, hy - 2, ix("stone_light"))
    c.hline(hx - 14, hx + 14, hy + 7, ix("stone_dark"))
    for i in range(5):
        bx = hx - 14 + i * 6
        c.rect(bx, hy - 5, 3, 3, ix("stone_mid"))
        c.hline(bx, bx + 2, hy - 5, ix("stone_light"))
    # Gate.
    c.rect(hx - 4, hy + 1, 9, 7, ix("wood_dark"))
    c.rect(hx - 3, hy, 7, 1, ix("wood_dark"))
    c.vline(hx - 1, hy + 1, hy + 7, ix("wood_mid"))
    c.vline(hx + 2, hy + 1, hy + 7, ix("wood_mid"))
    # Towers with conical roofs.
    for sx in (hx - 15, hx + 9):
        c.rect(sx, hy - 9, 7, 17, ix("stone_mid"))
        c.hline(sx, sx + 6, hy - 9, ix("stone_light"))
        for k in range(4):
            c.rect(sx + 3 - k, hy - 13 + k, k * 2 + 1, 1, ix("slate_dark"))
        c.set(sx + 3, hy - 13, ix("slate_light"))
        c.set(sx + 3, hy - 5, ix("lamp_core"))
    # Central keep.
    c.rect(hx - 6, hy - 16, 13, 10, ix("stone_mid"))
    c.hline(hx - 6, hx + 6, hy - 16, ix("stone_light"))
    for k in (-6, -2, 2):
        c.rect(hx + k, hy - 18, 2, 2, ix("stone_mid"))
        c.hline(hx + k, hx + k + 1, hy - 18, ix("stone_light"))
    c.rect(hx - 2, hy - 13, 2, 3, ix("lamp_core"))
    c.rect(hx + 2, hy - 13, 2, 3, ix("lamp_core"))
    # Banner.
    c.vline(hx, hy - 26, hy - 17, ix("map_ink"))
    c.rect(hx + 1, hy - 26, 7, 4, ix("cloth_red"))
    c.hline(hx + 1, hx + 7, hy - 26, ix("cloth_red_hi"))
    c.set(hx + 7, hy - 24, ix("map_paper"))
    # Path out of the gate.
    c.vline(hx, hy + 8, hy + 11, ix("map_route"))
    c.vline(hx + 1, hy + 8, hy + 10, ix("map_route"))


def _compass(c, cx, cy, R=14):
    """A compass rose in the open sea corner: ink points, gold centre."""
    ink = ix("map_ink")
    burn = ix("map_burn")
    cream = ix("cream")
    circle_outline(c, cx, cy, R, "map_ink")
    circle_outline(c, cx, cy, 4, "map_ink")
    for a in range(0, 360, 45):
        x = int(cx + (R - 2) * math.cos(math.radians(a)))
        y = int(cy + (R - 2) * math.sin(math.radians(a)))
        c.set(x, y, ink)
    # Cardinal points: long diamonds, lit centreline.
    for (dx, dy) in ((0, -1), (1, 0), (0, 1), (-1, 0)):
        for i in range(1, R - 1):
            half = max(0, int(round(2.4 * (1 - i / R))))
            px, py = cx + dx * i, cy + dy * i
            if dx == 0:
                c.hline(px - half, px + half, py, ink)
                c.set(px, py, cream)
            else:
                c.vline(px, py - half, py + half, ink)
                c.set(px, py, cream)
    # Intercardinal points: shorter, burnt tone.
    for (dx, dy) in ((1, -1), (1, 1), (-1, 1), (-1, -1)):
        for i in range(1, int(R * 0.62)):
            half = max(0, int(round(1.6 * (1 - i / (R * 0.62)))))
            px, py = cx + dx * i, cy + dy * i
            c.hline(px - half, px + half, py, burn)
            c.vline(px, py - half, py + half, burn)
    c.set(cx, cy, ix("ui_gold"))


def _burn_edges(c):
    """Scorched parchment frame: dark at the rim, dithered fade inward."""
    for y in range(H):
        for x in range(W):
            e = min(x, y, W - 1 - x, H - 1 - y)
            v = e + 1.3 * math.sin(x * 0.12 + 1.7) + 1.3 * math.sin(y * 0.09 + 0.4)
            if v < 1.4:
                c.set(x, y, ix("map_burn_deep"))
            elif v < 3.2:
                c.set(x, y, ix("map_burn_deep" if bayer4(x, y) < 0.55
                               else "map_burn"))
            elif v < 5.0:
                if bayer4(x, y) < 0.45:
                    c.set(x, y, ix("map_burn"))


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------

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
