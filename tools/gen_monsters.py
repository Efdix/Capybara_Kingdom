"""Monsters (32x32) and bosses (64x64) for Capybara Kingdom.

The catalog rework, drawn on one shared lighting pipeline:
  5 Regions x 3 Themed Mobs = 15 Mobs   (32x32, 4 poses)
  5 Regions x 2 Bosses      = 10 Bosses (64x64, 8 poses)

Art direction: GBA Pokemon creature work. Every animal is a chunky, convex
silhouette -- the head is the widest feature, the eyes are big glossy
double-glint features (gc.pk_eye), the fill is one flat base colour plus a
hard-edged bottom shadow and a top-left rim light, and a crisp near-black
outline closes the shape. No dithered gradients on creatures: at 32px they
read as noise, and the GBA originals never used them.

Poses are body language, not recolours:
  mobs    idle / bob (breath) / attack (commit to a hit) / hurt (flinch)
  bosses  + windup (coil) / special_a (signature) / special_b (enraged) /
          stagger (broken, the punish window)

Output:
  graphics/preview_monsters.png -- full overview showcase
  graphics/mob_tiles.bin        -- 15 x 4 x 32x32
  graphics/boss_tiles.bin       -- 10 x 8 x 64x64
"""

import math
import os
import sys

import gen_common as gc
from gen_common import Canvas, ix, preview_scale

VARIANTS = ["idle", "bob", "attack", "hurt"]
BOSS_VARIANTS = ["idle", "bob", "attack", "hurt",
                 "windup", "special_a", "special_b", "stagger"]
BOSS_SPRITE = 64


# ---------------------------------------------------------------------------
# Shared finishing pass
# ---------------------------------------------------------------------------

def _finish(c, shade, rim=None):
    """The one lighting pipeline every creature in the game runs through.

    Shade first (it darkens body pixels and skips thin parts), then the
    outline grows outward around the shaded form, then the rim replaces
    top-left interior pixels -- outline pixels are skipped, so the dark
    edge stays crisp.
    """
    gc.pk_shade_bottom(c, shade, rows=2)
    c.auto_outline()
    if rim:
        gc.pk_rim(c, rim, protected={ix(shade)})
    return c


def _dy(variant, bob=1, hit=1):
    return {"idle": 0, "bob": -bob, "attack": -2, "hurt": hit}[variant]


def _wince(c, lines):
    """Closed '>_<-style' eyes for the hurt pose: short dark slashes."""
    for (x, y, w) in lines:
        c.hline(x, x + w, y, ix("outline"))
        c.set(x + w + 1, y + 1, ix("outline"))


def _sweat(c, drops, color="cyan_light"):
    for (x, y) in drops:
        c.set(x, y, ix(color))
        c.set(x, y + 1, ix(color))


def _limb(c, x0, y0, x1, y1, w, color):
    """A thick limb: circles stamped along a line, the pixel-art brush."""
    steps = max(abs(x1 - x0), abs(y1 - y0)) or 1
    for s in range(steps + 1):
        x = x0 + (x1 - x0) * s // steps
        y = y0 + (y1 - y0) * s // steps
        c.circle(x, y, w / 2, ix(color))


def _teeth(c, x0, x1, y, n=4, down=True):
    for k in range(n):
        x = x0 + (x1 - x0) * k // max(1, n - 1)
        c.set(x, y, ix("white"))
        c.set(x, y + (1 if down else -1), ix("white"))


def _stars(c, cx, cy, r):
    """Circling stars over a staggered boss: the unmissable punish marker."""
    for k in range(4):
        a = k * math.pi / 2
        x = int(cx + r * math.cos(a))
        y = int(cy + r * 0.5 * math.sin(a))
        c.sparkle(x, y, 2, "yellow_light", "white")


def _pupil_bead(c, x, y, r=1.4):
    """A solid round black bead eye with one glint -- for tiny faces where a
    full sclera eye would not fit."""
    c.circle(x, y, r, ix("fur_black"))
    c.set(int(x - 0.6), int(y - 0.7), ix("white"))


# ===========================================================================
# 15 MOBS (32x32, 4 poses)
# ===========================================================================

# --- REGION 1: 雨林 (Rainforest) -------------------------------------------

def m_rain_frog(pal, variant="idle"):
    """Poison treefrog: emerald body, orange eyes on top, creamy belly."""
    c = Canvas(32, 32)
    dy = _dy(variant)
    lunge = 2 if variant == "attack" else 0
    y = 11 + dy + (1 if variant == "hurt" else 0)

    # folded hind legs
    c.blob(7, y + 8, 5, 4, ix("grn_dark"), n=3.0)
    c.blob(25, y + 8, 5, 4, ix("grn_dark"), n=3.0)
    c.ellipse(5, y + 12, 3, 1.6, ix("grn_mid"))
    c.ellipse(27, y + 12, 3, 1.6, ix("grn_mid"))
    # body + belly
    c.blob(16 + lunge, y + 7, 10, 7.5, ix("grn_mid"), n=3.2)
    c.ellipse(16 + lunge, y + 10, 6.5, 3.5, ix("cream"))
    # front feet
    c.ellipse(10 + lunge, y + 13, 3, 1.8, ix("grn_light"))
    c.ellipse(22 + lunge, y + 13, 3, 1.8, ix("grn_light"))
    # treefrog eyes sit ON TOP of the head, on raised orbs
    for ex in (10 + lunge, 22 + lunge):
        c.circle(ex, y - 1.0, 3.8, ix("grn_dark"))
        c.circle(ex - 0.8, y - 1.8, 1.6, ix("grn_mid"))
        gc.pk_eye(c, ex, y - 0.6, 2.2, 2.2, mode="open", iris="orange_mid",
                  variant=None, sclera=None)
    # mouth line
    c.hline(12 + lunge, 20 + lunge, y + 5, ix("grn_dark"))
    if variant == "attack":
        # tongue snap: a dark root with a bright pad
        c.hline(20 + lunge, 26 + lunge, y + 6, ix("red_dark"))
        c.circle(27 + lunge, y + 6, 2.0, ix("cyan_light"))
        c.set(27 + lunge, y + 6, ix("white"))
    if variant == "hurt":
        _wince(c, [(9 + lunge, y - 1, 2), (20 + lunge, y - 1, 2)])
        _sweat(c, [(6, y - 3), (26, y - 4)])
    return _finish(c, "grn_dark", rim="grn_light")


def m_rain_wasp(pal, variant="idle"):
    """Jungle wasp: gold livery, charcoal stripes, glassy wings."""
    c = Canvas(32, 32)
    dy = _dy(variant)
    flap = {"idle": 1, "bob": 3, "attack": 0, "hurt": 2}[variant]
    tilt = -1 if variant == "attack" else 0

    # wings BEHIND the body: two panes, the far one darker
    for wx, wr in ((6, 9), (17, 8)):
        c.ellipse(wx + tilt, 9 + dy - flap, wr * 0.7, 3.2, ix("cyan_mid"))
        c.ellipse(wx + tilt + 1, 8 + dy - flap, wr * 0.55, 2.2,
                  ix("cyan_light"))
    # abdomen: gold with charcoal bands
    c.blob(16, 20 + dy, 9, 7.5, ix("yellow_dark"), n=3.2)
    c.blob(16, 19 + dy, 8, 6.5, ix("yellow_mid"), n=3.2)
    for sy in (18, 21, 24):
        c.hline(10, 21, sy + dy, ix("yellow_dark"))
        c.hline(11, 20, sy + dy, ix("outline"))
    # thorax
    c.blob(16, 13 + dy, 6, 5, ix("fur_black"), n=3.0)
    # head: dark cap over the face so the eyes read on gold
    c.blob(16, 8 + dy, 5.5, 4.5, ix("yellow_dark"), n=3.0)
    for ex in (13, 19):
        gc.pk_eye(c, ex, 8 + dy, 1.7, 1.7, mode="open", variant=None)
    # antennae
    c.vline(13, 2 + dy, 4 + dy, ix("outline"))
    c.vline(19, 2 + dy, 4 + dy, ix("outline"))
    # stinger
    c.blob(16, 28 + dy, 1.6, 3.0, ix("steel_dark"), n=2.5)
    if variant == "attack":
        c.sparkle(16, 30 + dy, 2, "yellow_light")
    if variant == "hurt":
        _wince(c, [(12, 7 + dy, 2), (18, 7 + dy, 2)])
        _sweat(c, [(8, 5 + dy), (24, 4 + dy)])
        c.set(6, 14 + dy, ix("cream"))   # a loose scale drifting off
    return _finish(c, "yellow_dark")


def m_rain_slime(pal, variant="idle"):
    """Rainforest slime: translucent green with a berry core showing."""
    c = Canvas(32, 32)
    squash = {"idle": 0, "bob": -1, "attack": -3, "hurt": 2}[variant]
    top = 12 + squash
    h = 26 - top

    c.blob(16, top + h // 2, 10, h // 2 + 1, ix("grn_dark"), n=2.2)
    c.blob(16, top + h // 2, 9, h // 2, ix("grn_mid"), n=2.2)
    # the berry it swallowed
    c.circle(16, top + h - 6, 3.0, ix("cherry_dark"))
    c.circle(16, top + h - 6, 2.2, ix("cherry_mid"))
    c.set(15, top + h - 7, ix("cherry_light"))
    # gloss: a hard-edged light crescent on the upper-left (no dither)
    c.blob(11.5, top + 4.5, 3.4, 1.8, ix("grn_light"), n=3.0)
    c.set(10, top + 3, ix("white_spark"))
    # drip feet
    c.set(7, 27, ix("grn_mid"))
    c.set(24, 27, ix("grn_mid"))
    if variant == "attack":
        gc.pk_eye(c, 12, top + 5, 2.1, 2.4, mode="open", variant=None)
        gc.pk_eye(c, 20, top + 5, 2.1, 2.4, mode="open", variant=None)
        c.hline(11, 21, top + 9, ix("grn_dark"))
    else:
        gc.pk_eye(c, 12, top + 5, 1.9, 2.5, mode="open", variant=None)
        gc.pk_eye(c, 20, top + 5, 1.9, 2.5, mode="open", variant=None)
    if variant == "hurt":
        _wince(c, [(10, top + 4, 3), (17, top + 4, 3)])
        _sweat(c, [(8, top + 1), (23, top)])
    return _finish(c, "grn_dark", rim="grn_light")


# --- REGION 2: 农场 (Farm) --------------------------------------------------

def m_farm_rat(pal, variant="idle"):
    """The thief: warm brown, trophy ears, one stolen seed held up."""
    c = Canvas(32, 32)
    dy = _dy(variant)
    rear = 3 if variant == "attack" else 0
    y = 13 + dy - rear

    # tail: a long curve behind
    for s in range(8):
        c.set(24 + s, y + 8 + (s * s) // 10, ix("pink_dark"))
    # body
    c.blob(14, y + 5, 9, 6.5, ix("brn_mid2"), n=3.2)
    c.ellipse(13, y + 7, 6, 3.5, ix("tan_light"))     # belly
    # head with a pointed snout
    c.blob(10, y - 2, 6, 5, ix("brn_mid2"), n=3.0)
    c.blob(6, y - 1, 3, 2.2, ix("brn_mid2"), n=2.5)   # snout tip
    c.set(4, y - 1, ix("pink_dark"))                  # nose
    # whiskers
    for wy in (y - 2, y):
        c.set(2, wy, ix("cream"))
        c.set(3, wy + 1, ix("cream"))
    # ears: big discs, pink inner
    for ex in (8, 15):
        c.circle(ex, y - 6, 3.4, ix("brn_shadow"))
        c.circle(ex, y - 6, 2.2, ix("pink_light"))
    gc.pk_eye(c, 9, y - 1, 1.4, 1.5, mode="open", variant=None)
    gc.pk_eye(c, 13, y - 1, 1.4, 1.5, mode="open", variant=None)
    # buck teeth: the rat's signature
    c.vline(5, y + 1, y + 2, ix("white"))
    c.vline(6, y + 1, y + 2, ix("white"))
    # feet
    c.ellipse(10, y + 11, 2.4, 1.4, ix("pink_mid"))
    c.ellipse(18, y + 11, 2.4, 1.4, ix("pink_mid"))
    if variant == "attack":
        # the stolen seed held overhead
        c.ellipse(15, y - 10, 2.0, 2.6, ix("ui_gold"))
        c.set(15, y - 12, ix("green_spark"))
        c.sparkle(19, y - 9, 2, "white_spark")
    if variant == "hurt":
        _wince(c, [(7, y - 2, 2), (12, y - 2, 2)])
        _sweat(c, [(5, y - 5), (17, y - 6)])
    return _finish(c, "brn_shadow", rim="brn_hl")


def m_farm_crow(pal, variant="idle"):
    """Harvest crow: blue-black plumage, steel beak, one wing mid-flap."""
    c = Canvas(32, 32)
    dy = _dy(variant)
    spread = {"idle": 0, "bob": 1, "attack": 3, "hurt": -1}[variant]

    # far wing
    c.blob(21, 14 + dy, 6, 3.2 - spread * 0.3, ix("blue_dark"), n=2.5)
    # body
    c.blob(15, 17 + dy, 8, 6, ix("blue_dark"), n=3.0)
    c.blob(14, 17 + dy, 7, 5, ix("fur_black"), n=3.0)
    # near wing, feathered with steel lines
    c.blob(11 - spread, 14 + dy, 7, 4 + spread, ix("fur_black"), n=2.5)
    for f in range(3):
        c.vline(7 - spread + f * 3, 12 + dy, 16 + dy + spread,
                ix("steel_dark"))
    # tail fan
    for t in range(3):
        c.vline(21 + t, 21 + dy, 26 + dy, ix("fur_black"))
    # head + beak
    c.blob(11, 10 + dy, 5, 4.4, ix("fur_black"), n=3.0)
    c.blob(6, 11 + dy, 3.4, 1.8, ix("steel_mid"), n=2.2)     # beak
    c.set(4, 11 + dy, ix("steel_dark"))
    gc.pk_eye(c, 11, 9 + dy, 1.7, 1.7, mode="open", iris="yellow_mid",
              variant=None)
    # legs
    c.vline(13, 23 + dy, 27 + dy, ix("orange_dark"))
    c.vline(17, 23 + dy, 27 + dy, ix("orange_dark"))
    c.hline(11, 15, 28 + dy, ix("orange_dark"))
    if variant == "attack":
        c.hline(2, 7, 11 + dy, ix("white_spark"))   # dive streak
        c.sparkle(3, 9 + dy, 1, "white_spark")
    if variant == "hurt":
        _wince(c, [(9, 8 + dy, 3)])
        c.set(6, 5 + dy, ix("grey_light"))
        c.set(5, 4 + dy, ix("cream"))
        c.set(20, 8 + dy, ix("grey_light"))          # loose feathers
    return _finish(c, "blue_dark")


def m_farm_mudlet(pal, variant="idle"):
    """Living mud: a clay dome with a wheat sprout and pebble grit."""
    c = Canvas(32, 32)
    squash = {"idle": 0, "bob": -1, "attack": -3, "hurt": 2}[variant]
    top = 12 + squash
    h = 27 - top

    c.blob(16, top + h // 2, 10, h // 2, ix("clay_dark"), n=2.2)
    c.blob(16, top + h // 2, 9, h // 2 - 1, ix("clay_mid"), n=2.2)
    # wet sheen: a hard-edged crescent
    c.blob(11.5, top + 4.5, 3.4, 1.8, ix("sand_mid"), n=3.0)
    # grit stones embedded in the body
    for (gx, gy) in ((20, top + 6), (9, top + 9), (22, top + 11)):
        c.circle(gx, gy, 1.4, ix("rock_mid"))
    # sprout on the head: two leaves and a stem
    c.vline(16, top - 3, top, ix("grn_dark"))
    c.set(15, top - 4, ix("grn_light"))
    c.set(17, top - 4, ix("grn_mid"))
    c.set(14, top - 5, ix("leaf_light"))
    c.set(18, top - 5, ix("leaf_light"))
    if variant == "attack":
        gc.pk_eye(c, 12, top + 6, 2.0, 2.2, mode="open", variant=None)
        gc.pk_eye(c, 20, top + 6, 2.0, 2.2, mode="open", variant=None)
        c.hline(11, 21, top + 10, ix("clay_dark"))
        c.set(6, top + 3, ix("clay_dark"))
        c.set(5, top + 4, ix("clay_dark"))
    else:
        gc.pk_eye(c, 12, top + 6, 1.8, 2.2, mode="open", iris="rock_light",
                  variant=None)
        gc.pk_eye(c, 20, top + 6, 1.8, 2.2, mode="open", iris="rock_light",
                  variant=None)
    if variant == "hurt":
        _wince(c, [(10, top + 5, 3), (18, top + 5, 3)])
        c.vline(20, top + 2, top + 7, ix("clay_dark"))   # a crack
    return _finish(c, "clay_dark")


# --- REGION 3: 牧场 (Pasture) ----------------------------------------------

def m_past_dog(pal, variant="idle"):
    """Herding dog: cream coat with a grey saddle, one ear up one down."""
    c = Canvas(32, 32)
    dy = _dy(variant)
    lunge = 2 if variant == "attack" else 0
    y = 12 + dy

    # body + saddle patch
    c.blob(17, y + 6, 9, 6, ix("cream"), n=3.2)
    c.blob(19, y + 4, 6, 4, ix("grey_mid"), n=3.0)
    c.ellipse(15, y + 9, 6, 2.6, ix("white"))
    # head + muzzle
    c.blob(9 + lunge, y + 1, 6, 5, ix("cream"), n=3.0)
    c.blob(4 + lunge, y + 3, 3, 2.2, ix("cream"), n=2.5)
    c.set(2 + lunge, y + 3, ix("fur_black"))    # nose
    # ears: up + flop
    c.blob(7 + lunge, y - 5, 2.2, 3.4, ix("grey_dark"), n=2.5)
    c.blob(13 + lunge, y - 2, 3.0, 2.4, ix("grey_dark"), n=2.5)
    gc.pk_eye(c, 7 + lunge, y, 1.4, 1.5, mode="open", iris="brn_deep",
              variant=None)
    gc.pk_eye(c, 12 + lunge, y, 1.4, 1.5, mode="open", iris="brn_deep",
              variant=None)
    # legs
    for lx in (11, 16, 21):
        c.rect(lx, y + 10, 2, 5, ix("cream"))
        c.rect(lx, y + 14, 2, 2, ix("grey_light"))
    # tail
    _limb(c, 26, y + 4, 30, y, 2, "cream")
    if variant == "attack":
        # teeth bared + ears flat
        _teeth(c, 3 + lunge, 6 + lunge, y + 4, 3)
        c.blob(13 + lunge, y - 4, 2.6, 2.0, ix("grey_dark"), n=2.5)
        c.hline(1 + lunge, 4 + lunge, y + 2, ix("red_light"))  # gum line
    if variant == "hurt":
        _wince(c, [(5 + lunge, y - 1, 2), (10 + lunge, y - 1, 2)])
        _sweat(c, [(4, y - 4), (15, y - 5)])
    return _finish(c, "grey_dark", rim="white")


def m_past_ram(pal, variant="idle"):
    """Black ram: charcoal wool, spiral horns, a head built for charging."""
    c = Canvas(32, 32)
    dy = _dy(variant)
    charge = 2 if variant == "attack" else 0
    y = 12 + dy

    # wool body: three bumps so the outline is lumpy, not smooth
    c.blob(16, y + 6, 10, 6.5, ix("fur_grey"), n=2.4)
    c.blob(10, y + 4, 5, 4.5, ix("fur_grey"), n=2.4)
    c.blob(22, y + 5, 5, 4.5, ix("fur_grey"), n=2.4)
    # head, lowered in attack
    c.blob(8 + charge, y + 2, 5.5, 4.5, ix("fur_black"), n=3.0)
    # spiral horns: two arcs sweeping down-forward
    for s in range(7):
        hx = 5 + charge + s
        hy = y - 3 + (s * s) // 4
        c.set(hx, hy, ix("cream"))
        c.set(hx, hy + 1, ix("sand_dark"))
    for s in range(5):
        c.set(4 + charge - s, y - 2 + s, ix("cream"))
    gc.pk_eye(c, 8 + charge, y + 2, 1.5, 1.5, mode="open",
              iris="ember_yellow", variant=None)
    # legs + hooves
    for lx in (12, 20):
        c.rect(lx, y + 11, 3, 5, ix("fur_black"))
        c.rect(lx, y + 15, 3, 2, ix("fur_grey"))
    # stub tail
    c.circle(27, y + 4, 1.8, ix("fur_black"))
    if variant == "attack":
        c.set(2 + charge, y + 9, ix("sand_mid"))   # kicked dust
        c.set(1 + charge, y + 11, ix("sand_light"))
    if variant == "hurt":
        _wince(c, [(6 + charge, y + 1, 2), (11 + charge, y + 1, 2)])
        _sweat(c, [(5, y - 5), (14, y - 6)])
    return _finish(c, "fur_black", rim="fur_grey")


def m_past_hornet(pal, variant="idle"):
    """Drummer hornet: slimmer than its jungle cousin, wings like batons."""
    c = Canvas(32, 32)
    dy = _dy(variant)
    flap = {"idle": 1, "bob": 3, "attack": 0, "hurt": 2}[variant]

    # wings held like raised batons
    for wx in (7, 17):
        c.ellipse(wx, 7 + dy - flap, 6 * 0.7, 3.0, ix("purple_light"))
        c.ellipse(wx + 1, 6 + dy - flap, 4 * 0.6, 2.0, ix("white"))
    # body: purple with gold rings
    c.blob(16, 19 + dy, 8, 7, ix("purple_dark"), n=3.2)
    c.blob(16, 18 + dy, 7, 6, ix("purple_mid"), n=3.2)
    for sy in (17, 20, 23):
        c.hline(11, 20, sy + dy, ix("ui_gold"))
    # head
    c.blob(16, 11 + dy, 5.5, 4.5, ix("purple_mid"), n=3.0)
    for ex in (13, 19):
        gc.pk_eye(c, ex, 10 + dy, 1.6, 1.6, mode="open", variant=None)
    # the war drum: a little belt drum under the thorax
    c.round_rect(12, 14 + dy, 8, 5, 2, ix("wood_dark"))
    c.hline(13, 18, 15 + dy, ix("sand_light"))
    if variant == "attack":
        # the haste pulse: three notes flying out
        c.sparkle(5, 12 + dy, 2, "volt_inner")
        c.sparkle(27, 12 + dy, 2, "volt_inner")
        c.sparkle(16, 2 + dy, 2, "volt_inner")
    if variant == "hurt":
        _wince(c, [(12, 9 + dy, 2), (18, 9 + dy, 2)])
        _sweat(c, [(9, 6 + dy), (23, 6 + dy)])
    return _finish(c, "purple_dark")


# --- REGION 4: 沼泽 (Marsh) ------------------------------------------------

def m_marsh_slime(pal, variant="idle"):
    """Marsh slime: plum body, a reed sprouting from its crown."""
    c = Canvas(32, 32)
    squash = {"idle": 0, "bob": -1, "attack": -3, "hurt": 2}[variant]
    top = 12 + squash
    h = 26 - top

    c.blob(16, top + h // 2, 10, h // 2 + 1, ix("purple_dark"), n=2.2)
    c.blob(16, top + h // 2, 9, h // 2, ix("purple_mid"), n=2.2)
    c.blob(11.5, top + 4.5, 3.4, 1.8, ix("purple_light"), n=3.0)
    c.set(10, top + 3, ix("white_spark"))
    # the reed it swallowed grows back out
    c.vline(19, top - 5, top, ix("cane_mid"))
    c.set(18, top - 6, ix("cane_light"))
    c.set(20, top - 6, ix("cane_light"))
    c.rect(18, top - 4, 3, 2, ix("brn_deep"))
    if variant == "attack":
        gc.pk_eye(c, 12, top + 5, 2.1, 2.4, mode="open", variant=None)
        gc.pk_eye(c, 20, top + 5, 2.1, 2.4, mode="open", variant=None)
        c.hline(11, 21, top + 9, ix("purple_dark"))
    else:
        gc.pk_eye(c, 12, top + 5, 1.9, 2.5, mode="open", variant=None)
        gc.pk_eye(c, 20, top + 5, 1.9, 2.5, mode="open", variant=None)
    if variant == "hurt":
        _wince(c, [(10, top + 4, 3), (17, top + 4, 3)])
        _sweat(c, [(8, top + 1), (23, top)], color="heal_inner")
    return _finish(c, "purple_dark")


def m_marsh_viper(pal, variant="idle"):
    """Viper: an S-curve body, black bands, a forked tongue."""
    c = Canvas(32, 32)
    dy = _dy(variant)
    rear = 3 if variant == "attack" else 0

    # body: a sinuous ridge drawn as three sweeps
    _limb(c, 4, 26, 12, 20, 5, "pine_dark")
    _limb(c, 12, 20, 22, 22, 5, "pine_dark")
    _limb(c, 22, 22, 27, 15 - rear, 5, "pine_dark")
    # banding
    for (bx, by) in ((8, 24), (16, 20), (24, 20)):
        c.set(bx, by, ix("fur_black"))
        c.set(bx + 1, by, ix("fur_black"))
    # belly line
    c.hline(6, 20, 25 + dy, ix("grn_light"))
    # head, lifted
    hy = 12 - rear + dy
    c.blob(27, hy, 4.6, 3.6, ix("pine_dark"), n=2.8)
    gc.pk_eye(c, 28, hy - 1, 1.4, 1.5, mode="open", iris="yellow_mid",
              variant=None)
    # forked tongue
    c.set(31, hy + 1, ix("red_mid"))
    c.set(31, hy + 2, ix("red_mid"))
    if variant == "attack":
        c.set(30, hy + 2, ix("red_mid"))
        c.set(31, hy + 3, ix("red_mid"))
        _teeth(c, 29, 31, hy + 2, 2)
    if variant == "hurt":
        _wince(c, [(26, hy - 2, 3)])
        _sweat(c, [(25, hy - 5)])
    return _finish(c, "pine_dark")


def m_marsh_toad(pal, variant="idle"):
    """Bog toad: olive skin, warts, an inflated throat sac."""
    c = Canvas(32, 32)
    dy = _dy(variant)
    sac = 2 if variant == "attack" else 0
    y = 11 + dy

    # squat body
    c.blob(16, y + 8, 10, 7, ix("cane_dark"), n=2.6)
    c.blob(16, y + 7, 9, 6, ix("grn_mid"), n=2.6)
    c.ellipse(16, y + 11, 7, 3, ix("cane_light"))    # belly
    # warts: darker dots on the back
    for (wx, wy) in ((9, y + 4), (22, y + 5), (13, y + 2), (20, y + 3)):
        c.set(wx, wy, ix("pine_dark"))
        c.set(wx + 1, wy, ix("pine_dark"))
    # eyes on top
    for ex in (10, 22):
        c.circle(ex, y, 3.2, ix("cane_dark"))
        gc.pk_eye(c, ex, y, 2.0, 2.0, mode="open", iris="ember_orange",
                  variant=None, sclera=None)
    # wide mouth
    c.hline(10, 22, y + 5, ix("pine_dark"))
    # throat sac
    c.ellipse(16, y + 7 + sac, 4.0, 2.4 + sac, ix("pink_light"))
    if variant == "attack":
        c.hline(12, 20, y + 5 + sac, ix("pine_dark"))
    if variant == "hurt":
        _wince(c, [(8, y - 1, 2), (20, y - 1, 2)])
        _sweat(c, [(6, y - 3), (25, y - 4)])
    return _finish(c, "cane_dark")


# --- REGION 5: 火山 (Volcano) ----------------------------------------------

def m_volc_imp(pal, variant="idle"):
    """Cinder imp: ember-red hide, charcoal feet, a candle-flame tail."""
    c = Canvas(32, 32)
    dy = _dy(variant)
    y = 12 + dy
    lunge = 2 if variant == "attack" else 0

    # body
    c.blob(15 + lunge, y + 5, 8, 6.5, ix("red_dark"), n=3.0)
    c.blob(15 + lunge, y + 4, 7, 5.5, ix("red_mid"), n=3.0)
    c.ellipse(15 + lunge, y + 7, 5, 2.6, ix("ember_orange"))   # belly glow
    # head + horns
    c.blob(15 + lunge, y - 3, 6, 5, ix("red_mid"), n=3.0)
    for hx in (10 + lunge, 20 + lunge):
        c.vline(hx, y - 9, y - 6, ix("sand_dark"))
        c.set(hx, y - 10, ix("sand_light"))
    for ex in (12 + lunge, 18 + lunge):
        gc.pk_eye(c, ex, y - 4, 1.6, 1.6, mode="open",
                  iris="ember_yellow", variant=None)
    # grin
    c.hline(12 + lunge, 18 + lunge, y - 1, ix("red_dark"))
    _teeth(c, 13 + lunge, 17 + lunge, y, 3)
    # legs
    for lx in (11 + lunge, 18 + lunge):
        c.rect(lx, y + 9, 3, 4, ix("fur_black"))
    # tail with a flame tip
    _limb(c, 23 + lunge, y + 5, 28, y + 1, 2, "red_dark")
    c.grad_ellipse(29, y, 2.6, 3.4, ["ember_yellow", "ember_orange",
                                     "fire_outer"])
    if variant == "attack":
        c.sparkle(15 + lunge, y - 12, 3, "ember_yellow", "white")
    if variant == "hurt":
        _wince(c, [(10 + lunge, y - 5, 2), (16 + lunge, y - 5, 2)])
        c.set(8, y - 8, ix("grey_mid"))
        c.set(7, y - 9, ix("grey_light"))
    return _finish(c, "red_dark")


def m_volc_bat(pal, variant="idle"):
    """Dusk bat: rib-membrane wings, a puff of fuzz, tiny fangs."""
    c = Canvas(32, 32)
    dy = _dy(variant)
    flap = {"idle": 1, "bob": 3, "attack": 0, "hurt": 2}[variant]

    # wings: two membranes with rib lines
    for wx in (7, 25):
        c.blob(wx, 14 + dy - flap, 7, 5, ix("purple_dark"), n=2.4)
        for r in range(3):
            c.vline(wx - 3 + r * 3, 11 + dy - flap, 17 + dy - flap,
                    ix("purple_mid"))
    # body + head
    c.blob(16, 17 + dy, 5.5, 6, ix("purple_mid"), n=3.0)
    c.blob(16, 12 + dy, 5, 4.5, ix("purple_dark"), n=3.0)
    # ears
    for ex in (12, 20):
        c.blob(ex, 7 + dy, 1.8, 3.0, ix("purple_dark"), n=2.5)
        c.set(ex, 6 + dy, ix("purple_light"))
    for ex in (13, 19):
        gc.pk_eye(c, ex, 12 + dy, 1.6, 1.6, mode="open",
                  iris="ember_yellow", variant=None)
    _teeth(c, 14, 18, 15 + dy, 2)
    if variant == "attack":
        c.hline(2, 8, 10 + dy, ix("white_spark"))
        c.hline(24, 30, 10 + dy, ix("white_spark"))
    if variant == "hurt":
        _wince(c, [(11, 11 + dy, 2), (17, 11 + dy, 2)])
        _sweat(c, [(9, 8 + dy), (22, 8 + dy)])
    return _finish(c, "purple_dark")


def m_volc_orb(pal, variant="idle"):
    """Molten heart: a cracked crust around a white-hot core. No face --
    the one elemental in the crowd, which is what makes it unsettling."""
    c = Canvas(32, 32)
    flare = {"idle": 0, "bob": 1, "attack": 2, "hurt": -1}[variant]
    r = 8 + flare

    # outer heat halo, then crust, then core
    c.grad_ellipse(16, 16, r + 4, r + 4, ["fire_outer", "fire_smoke"])
    c.blob(16, 16, r, r, ix("lava_dark"), n=2.6)
    c.blob(16, 16, r - 2, r - 2, ix("fire_outer"), n=2.6)
    c.blob(15, 15, r - 4, r - 4, ix("fire_inner"), n=2.6)
    c.circle(14, 14, 2.0, ix("fire_core"))
    c.set(13, 13, ix("white_spark"))
    # crust cracks radiating out
    for (x0, y0, x1, y1) in ((16, 8, 19, 4), (8, 18, 4, 21),
                             (24, 20, 27, 23), (10, 9, 6, 7)):
        c.hline(x0, x1, y0, ix("lava_dark"))
        c.vline(x1, min(y0, y1), max(y0, y1), ix("lava_dark"))
    if variant == "attack":
        c.sparkle(16, 5, 3, "ember_yellow", "white")
        c.sparkle(6, 22, 2, "ember_orange")
        c.sparkle(26, 22, 2, "ember_orange")
    if variant == "hurt":
        c.dither_ellipse(16, 16, r + 2, r + 2, "fire_smoke", parity=1)
    return _finish(c, "lava_dark")


# ===========================================================================
# 10 BOSSES (64x64, 8 poses, drawn at full size)
# ===========================================================================

def _b(variant):
    """Shared boss pose parameters.

    dy: whole-body lift; squash: -3..+3 folded into each painter's body
    height; lean: forward tilt in pixels. The four duel poses get dramatic
    values -- windup coils low, special_a commits high, stagger collapses.
    """
    return {
        "idle":      dict(dy=0, squash=0, lean=0),
        "bob":       dict(dy=-1, squash=0, lean=0),
        "attack":    dict(dy=-2, squash=1, lean=3),
        "hurt":      dict(dy=1, squash=-1, lean=-2),
        "windup":    dict(dy=2, squash=-3, lean=-3),
        "special_a": dict(dy=-3, squash=2, lean=4),
        "special_b": dict(dy=-1, squash=1, lean=1),
        "stagger":   dict(dy=4, squash=-4, lean=2),
    }[variant]


def b_rain_ape(pal, variant="idle"):
    """雨林巨猿: knuckle-walking bulk, a silver chest, arms like pilons."""
    c = Canvas(BOSS_SPRITE, BOSS_SPRITE)
    p = _b(variant)
    y = 8 + p["dy"]

    # far arm
    _limb(c, 44, y + 18, 50, y + 44, 9, "brn_shadow")
    c.circle(50, y + 46, 6, ix("brn_dark"))
    # legs
    c.round_rect(22, y + 38, 9, 12, 4, ix("brn_dark"))
    c.round_rect(34, y + 38, 9, 12, 4, ix("brn_dark"))
    # torso
    c.blob(32, y + 24, 19, 16 - p["squash"], ix("brn_shadow"), n=3.0)
    c.blob(32, y + 23, 17, 14 - p["squash"], ix("brn_mid2"), n=3.0)
    # silver chest plate
    c.round_rect(24, y + 18, 16, 14, 6, ix("grey_dark"))
    c.round_rect(26, y + 20, 12, 10, 5, ix("grey_mid"))
    c.round_rect(27, y + 21, 8, 6, 4, ix("grey_light"))
    # near arm
    _limb(c, 14, y + 16, 10, y + 44, 9, "brn_dark")
    c.circle(10, y + 46, 7, ix("brn_shadow"))
    c.set(7, y + 44, ix("grey_mid"))
    # head with a heavy brow
    c.blob(32, y + 6, 12, 9, ix("brn_shadow"), n=3.0)
    c.round_rect(24, y, 16, 5, 2, ix("brn_mid2"))       # crest
    c.hline(24, 39, y + 6, ix("brn_dark"))              # brow
    for ex in (27, 36):
        gc.pk_eye(c, ex, y + 9, 2.2, 2.2, mode="open",
                  iris="red_mid" if variant == "special_b" else None,
                  variant=None, sclera=None)
    c.hline(27, 36, y + 13, ix("brn_dark"))             # muzzle line
    _teeth(c, 28, 35, y + 14, 4)
    c.hline(30, 33, y + 15, ix("fur_black"))            # nose

    if variant == "attack":
        _limb(c, 14, y + 12, 6, y + 2, 9, "brn_dark")   # arm swings up
        c.circle(6, y, 7, ix("brn_shadow"))
    elif variant == "special_a":
        c.circle(10, y + 48, 9, ix("sand_dark"))        # impact bloom
        c.dither_ellipse(10, y + 48, 13, 5, "sand_light", parity=1)
        c.sparkle(4, y + 42, 3, "white_spark")
    elif variant == "special_b":
        # bristling: silhouette spikes over shoulders and crest
        for (sx, sy) in ((18, y + 12), (22, y + 8), (42, y + 12),
                         (38, y + 6), (28, y - 2), (34, y - 2)):
            c.set(sx, sy, ix("brn_dark"))
            c.set(sx, sy - 1, ix("brn_dark"))
    elif variant == "stagger":
        _stars(c, 32, y - 4, 20)
        _sweat(c, [(18, y + 4), (46, y + 2)])
        c.hline(24, 39, y + 12, ix("brn_dark"))         # eyes squeezed shut
    return _finish(c, "brn_shadow", rim="brn_hl")


def b_rain_treant(pal, variant="idle"):
    """树精长老: a trunk that walks, root feet, branch arms, glowing hollows."""
    c = Canvas(BOSS_SPRITE, BOSS_SPRITE)
    p = _b(variant)
    y = 10 + p["dy"]
    leaf = ("leaf_dark", "leaf_mid", "leaf_light")
    if variant == "special_b":
        leaf = ("cane_dark", "ember_orange", "yellow_light")   # autumn rage

    # root feet
    for rx, flip in ((16, -1), (46, 1)):
        _limb(c, 32, y + 40, rx + flip * 6, y + 52, 6, "trunk_dark")
        c.circle(rx, y + 53, 5, ix("trunk_dark"))
    # trunk body
    c.blob(31, y + 24, 14, 18 - p["squash"] // 2, ix("trunk_dark"), n=2.6)
    c.blob(31, y + 23, 12, 17 - p["squash"] // 2, ix("trunk_mid"), n=2.6)
    c.vline(22, y + 12, y + 40, ix("trunk_light"))      # lit grain
    # bark ridges
    for (bx, by0, by1) in ((38, y + 14, y + 26), (26, y + 28, y + 38)):
        c.vline(bx, by0, by1, ix("trunk_dark"))
    # canopy: three lobes
    for (cx, cy, rx) in ((18, y + 2, 10), (44, y + 4, 10), (31, y - 4, 11)):
        c.blob(cx, cy, rx, rx * 0.8, ix(leaf[0]), n=3.0)
        c.blob(cx, cy - 1, rx - 2, rx * 0.8 - 2, ix(leaf[1]), n=3.0)
        c.blob(cx - 2, cy - 3, (rx - 2) * 0.6, (rx * 0.8 - 2) * 0.6,
               ix(leaf[2]), n=3.0)
    # branch arms
    _limb(c, 18, y + 18, 4, y + 8, 4, "trunk_dark")
    _limb(c, 44, y + 18, 58, y + 10, 4, "trunk_dark")
    # hollow eyes, lit from inside
    for ex in (25, 38):
        c.ellipse(ex, y + 8, 3.4, 3.4, ix("black"))
        c.ellipse(ex, y + 8, 2.4, 2.4,
                  ix("ember_orange") if variant == "special_b"
                  else ix("lamp_glow"))
        c.set(ex - 1, y + 7, ix("lamp_core"))
    # mouth hollow
    c.ellipse(31, y + 15, 4.5, 2.2, ix("black"))
    if variant == "attack":
        _limb(c, 44, y + 16, 58, y + 2, 4, "trunk_dark")    # branch raised
        c.sparkle(58, y, 3, "leaf_light")
    elif variant == "special_a":
        # seed pods orbiting
        for (sx, sy) in ((10, y + 2), (54, y + 6), (31, y - 10)):
            c.circle(sx, sy, 2.6, ix("orange_dark"))
            c.circle(sx - 1, sy - 1, 1.4, ix("orange_mid"))
    elif variant == "stagger":
        c.vline(31, y + 10, y + 30, ix("black"))            # trunk cracks
        c.vline(33, y + 18, y + 36, ix("black"))
        _stars(c, 31, y - 8, 18)
    return _finish(c, "trunk_dark")


def b_farm_boar(pal, variant="idle"):
    """狂暴野猪: tusks, a bristled ridge, and a body built to trample."""
    c = Canvas(BOSS_SPRITE, BOSS_SPRITE)
    p = _b(variant)
    y = 14 + p["dy"]
    charge = p["lean"]

    # legs
    for lx in (18, 30, 42):
        c.round_rect(lx, y + 26, 7, 14, 3, ix("brn_dark"))
        c.rect(lx, y + 36, 7, 4, ix("fur_black"))
    # body: heavy front, bristled ridge
    c.blob(30 + charge, y + 14, 20, 13 - p["squash"], ix("brn_shadow"), n=3.0)
    c.blob(30 + charge, y + 13, 18, 11 - p["squash"], ix("brn_mid2"), n=3.0)
    for b in range(5):
        c.set(22 + b * 4, y + 2 + (b % 2), ix("brn_dark"))
        c.set(23 + b * 4, y + 1 + (b % 2), ix("brn_dark"))
    # head: a dark face mask so snout, tusks and eyes all read
    c.blob(12 + charge * 2, y + 10, 10, 9, ix("brn_shadow"), n=2.8)
    c.blob(10 + charge * 2, y + 12, 7, 6, ix("brn_dark"), n=2.8)
    c.ellipse(6 + charge * 2, y + 14, 5, 4, ix("pink_mid"))     # snout
    c.ellipse(6 + charge * 2, y + 14, 3.4, 2.6, ix("pink_light"))
    c.set(4 + charge * 2, y + 13, ix("pink_dark"))
    c.set(4 + charge * 2, y + 15, ix("pink_dark"))
    for t in (10, 16):
        c.vline(t + charge, y + 12, y + 18, ix("cream"))
        c.set(t + charge, y + 11, ix("cream"))
        c.vline(t + charge - 1, y + 13, y + 17, ix("sand_dark"))
    for ex in (10 + charge * 2, 18 + charge * 2):
        gc.pk_eye(c, ex, y + 7, 2.2, 2.2, mode="open",
                  iris="red_mid" if variant != "stagger" else "brn_dark",
                  variant=None, sclera=None)
    # ears
    c.blob(20 + charge, y, 4, 3.4, ix("brn_dark"), n=2.5)
    c.blob(28 + charge, y + 1, 4, 3.4, ix("brn_dark"), n=2.5)
    # curly tail
    _limb(c, 48, y + 8, 54, y + 4, 2, "pink_dark")

    if variant == "windup":
        c.dither_ellipse(12, y + 40, 14, 4, "sand_mid", parity=0)
    elif variant == "special_a":
        c.dither_ellipse(48, y + 40, 16, 5, "sand_dark", parity=0)
        c.sparkle(52, y + 36, 3, "sand_light")
    elif variant == "special_b":
        for (sx, sy) in ((18, y + 1), (26, y - 2), (34, y), (42, y + 2)):
            c.set(sx, sy, ix("red_dark"))
            c.set(sx, sy - 1, ix("red_dark"))
    elif variant == "stagger":
        _stars(c, 30, y - 6, 20)
        _sweat(c, [(12, y + 2), (46, y)], color="cream")
    return _finish(c, "brn_shadow", rim="brn_hl")


def b_farm_ratking(pal, variant="idle"):
    """硕鼠大王: a crown, an honest pair of incisors, a stolen-velvet cape."""
    c = Canvas(BOSS_SPRITE, BOSS_SPRITE)
    p = _b(variant)
    y = 14 + p["dy"]

    # cape behind
    c.blob(36, y + 20, 16, 18, ix("purple_dark"), n=2.6)
    # tail
    _limb(c, 44, y + 34, 58, y + 28, 3, "pink_dark")
    c.circle(58, y + 27, 3, ix("pink_dark"))
    # body
    c.blob(30, y + 22, 15, 13 - p["squash"] // 2, ix("brn_mid2"), n=3.0)
    c.ellipse(27, y + 27, 9, 6, ix("tan_light"))            # belly
    # head with a pointed snout
    c.blob(24, y + 8, 11, 9, ix("brn_mid2"), n=2.8)
    c.blob(13, y + 12, 6, 4.5, ix("brn_mid2"), n=2.5)
    c.set(8, y + 12, ix("pink_dark"))                       # nose
    # whiskers
    for wy in (y + 10, y + 14):
        c.hline(2, 6, wy, ix("cream"))
    # crown
    c.round_rect(18, y - 8, 14, 6, 1, ix("ui_gold"))
    for k in range(3):
        c.vline(19 + k * 5, y - 12, y - 8, ix("ui_gold"))
        c.set(19 + k * 5, y - 13, ix("ui_gold_light"))
        c.set(21 + k * 5, y - 5, ix("cherry_mid"))
    c.hline(18, 31, y - 3, ix("ui_gold_light"))
    # ears
    for ex in (16, 34):
        c.circle(ex, y - 2, 6, ix("brn_shadow"))
        c.circle(ex, y - 2, 4, ix("pink_light"))
    # eyes + teeth
    for ex in (18, 28):
        gc.pk_eye(c, ex, y + 7, 2.4, 2.4, mode="open",
                  iris="red_mid" if variant == "special_b" else None,
                  variant=None, sclera=None)
    _teeth(c, 8, 13, y + 16, 2)
    c.vline(9, y + 16, y + 20, ix("white"))
    c.vline(12, y + 16, y + 20, ix("white"))
    # feet
    c.ellipse(20, y + 36, 5, 3, ix("pink_mid"))
    c.ellipse(38, y + 36, 5, 3, ix("pink_mid"))

    if variant == "attack":
        _limb(c, 20, y + 10, 10, y + 2, 8, "brn_mid2")      # pounce reach
    elif variant == "special_a":
        c.ellipse(50, y + 10, 3.0, 4.0, ix("ui_gold"))      # sceptre seed
        c.set(50, y + 5, ix("green_spark"))
        c.sparkle(54, y + 6, 2, "white_spark")
    elif variant == "special_b":
        c.sparkle(25, y - 10, 2, "white_spark")             # crown glint
    elif variant == "stagger":
        c.hline(18, 20, y - 6, ix("brn_dark"))              # crown askew
        _stars(c, 30, y - 8, 20)
        _wince(c, [(16, y + 5, 3), (26, y + 5, 3)])
    return _finish(c, "brn_shadow", rim="brn_hl")


def b_past_wolf(pal, variant="idle"):
    """夜狼领袖: lean, mane like a sawblade, built for one long pounce."""
    c = Canvas(BOSS_SPRITE, BOSS_SPRITE)
    p = _b(variant)
    y = 16 + p["dy"]
    stretch = p["lean"]

    # tail
    _limb(c, 52, y + 12, 60, y + 4, 4, "grey_dark")
    # hind legs
    _limb(c, 44, y + 20, 48, y + 40, 5, "grey_dark")
    c.circle(48, y + 42, 4, ix("fur_black"))
    # body: a lean sweep up to the shoulders
    c.blob(34, y + 16, 16, 9 - p["squash"] // 2, ix("grey_dark"), n=2.8)
    c.blob(32, y + 15, 14, 8 - p["squash"] // 2, ix("grey_mid"), n=2.8)
    # chest fur, cream
    c.ellipse(24, y + 20, 6, 5, ix("cream"))
    # mane spikes along the back
    for k in range(6):
        c.set(24 + k * 5, y + 6 + (k % 2), ix("grey_dark"))
        c.set(24 + k * 5, y + 5 + (k % 2), ix("grey_dark"))
    # head: long muzzle, ears back
    c.blob(16, y + 6, 9, 7, ix("grey_mid"), n=2.8)
    c.blob(8, y + 9, 6, 3.5, ix("grey_mid"), n=2.5)        # muzzle
    c.set(2, y + 9, ix("fur_black"))                       # nose
    c.blob(14, y - 2, 3, 4, ix("grey_dark"), n=2.5)        # ear
    c.blob(22, y - 1, 3, 4, ix("grey_dark"), n=2.5)
    gc.pk_eye(c, 15, y + 5, 2.0, 1.8, mode="open",
              iris="red_mid" if variant == "special_b" else "ember_orange",
              variant=None, sclera=None)
    # forelegs, reaching
    _limb(c, 20, y + 18, 10 + stretch * 2, y + 40, 5, "grey_mid")
    c.circle(10 + stretch * 2, y + 42, 4, ix("fur_black"))

    if variant == "attack":
        _limb(c, 20, y + 18, 6, y + 36, 5, "grey_mid")      # full lunge
        c.hline(2, 10, y + 2, ix("white_spark"))
    elif variant == "special_a":
        # the howl: head up, sound arcs leaving the muzzle
        c.blob(16, y + 2, 9, 7, ix("grey_mid"), n=2.8)
        for a in range(3):
            ax = 8 - a * 4
            c.hline(ax, ax + 3, y - 4 - a * 3, ix("cyan_light"))
        _teeth(c, 4, 8, y + 12, 3)
    elif variant == "stagger":
        _stars(c, 24, y - 6, 18)
        _wince(c, [(12, y + 4, 3), (19, y + 4, 3)])
        _sweat(c, [(8, y - 2), (30, y - 4)])
    return _finish(c, "grey_dark", rim="grey_light")


def b_past_bull(pal, variant="idle"):
    """狂暴黑牛: horns, a nose ring, a hump of muscle and bad intent."""
    c = Canvas(BOSS_SPRITE, BOSS_SPRITE)
    p = _b(variant)
    y = 12 + p["dy"]
    charge = p["lean"]

    # legs
    for lx in (20, 34, 46):
        c.round_rect(lx, y + 30, 8, 16, 3, ix("fur_black"))
        c.rect(lx, y + 42, 8, 4, ix("grey_dark"))
    # body with a front hump
    c.blob(34 + charge, y + 18, 20, 14 - p["squash"], ix("fur_black"), n=2.8)
    c.blob(24 + charge, y + 10, 12, 10, ix("fur_black"), n=2.8)
    c.blob(36 + charge, y + 22, 16, 10, ix("grey_dark"), n=3.0)
    # head lowered between the horns
    c.blob(14 + charge * 2, y + 14, 10, 9, ix("fur_black"), n=2.8)
    # horns: cream sweeps
    for hx, hy in ((6 + charge * 2, y + 6), (24 + charge * 2, y + 4)):
        _limb(c, 15 + charge * 2, y + 8, hx, hy, 4, "cream")
        c.set(hx, hy - 1, ix("white"))
    # face: ring in the nose, hot eyes
    c.ellipse(8 + charge * 2, y + 18, 4.5, 3.5, ix("grey_dark"))
    c.circle(6 + charge * 2, y + 19, 2.0, ix("brass_mid"))
    for ex in (10 + charge * 2, 19 + charge * 2):
        gc.pk_eye(c, ex, y + 12, 2.0, 1.8, mode="open", iris="red_mid",
                  variant=None, sclera=None)
    _teeth(c, 6 + charge * 2, 12 + charge * 2, y + 22, 3)
    # tail with a tuft
    _limb(c, 52, y + 12, 58, y + 20, 3, "fur_black")
    c.circle(58, y + 22, 3, ix("grey_dark"))

    steam = "red_light" if variant == "special_b" else "white"
    if variant == "windup":
        c.set(2, y + 14, ix(steam))
        c.set(3, y + 12, ix(steam))
        c.set(1, y + 10, ix(steam))                          # snort steam
    elif variant == "special_a":
        c.dither_ellipse(24, y + 46, 18, 5, "sand_dark", parity=0)
        c.sparkle(20, y + 42, 3, "sand_light")
    elif variant == "special_b":
        c.set(2, y + 12, ix(steam))
        c.set(3, y + 10, ix(steam))
        c.sparkle(15, y + 4, 2, "red_light")
    elif variant == "stagger":
        _stars(c, 30, y - 4, 20)
        _wince(c, [(8 + charge * 2, y + 10, 3),
                   (17 + charge * 2, y + 10, 3)])
    return _finish(c, "fur_black", rim="fur_grey")


def b_marsh_toadlord(pal, variant="idle"):
    """泥沼巨蟾: warts, a crown, and a throat sac that swells like a bell."""
    c = Canvas(BOSS_SPRITE, BOSS_SPRITE)
    p = _b(variant)
    y = 16 + p["dy"]
    sac = 4 if variant in ("windup", "special_b") else 0

    # legs folded
    c.blob(12, y + 32, 9, 7, ix("cane_dark"), n=2.8)
    c.blob(50, y + 32, 9, 7, ix("cane_dark"), n=2.8)
    c.ellipse(8, y + 38, 5, 3, ix("cane_mid"))
    c.ellipse(54, y + 38, 5, 3, ix("cane_mid"))
    # body
    c.blob(31, y + 20, 20, 14 - p["squash"], ix("cane_dark"), n=2.6)
    c.blob(31, y + 19, 18, 12 - p["squash"], ix("grn_mid"), n=2.6)
    c.ellipse(31, y + 28, 12, 6, ix("cane_light"))          # belly
    # warts
    for (wx, wy) in ((16, y + 12), (46, y + 14), (24, y + 8), (40, y + 8)):
        c.set(wx, wy, ix("pine_dark"))
        c.set(wx + 1, wy, ix("pine_dark"))
    # eyes on stalks
    for ex in (18, 44):
        c.circle(ex, y, 4.4, ix("cane_dark"))
        gc.pk_eye(c, ex, y, 2.8, 2.8, mode="open", iris="ember_orange",
                  variant=None, sclera=None)
    # crown, tiny and ridiculous
    c.round_rect(26, y - 9, 11, 4, 1, ix("ui_gold"))
    for k in range(3):
        c.set(27 + k * 4, y - 11, ix("ui_gold"))
    # wide mouth
    c.hline(14, 48, y + 12, ix("pine_dark"))
    # throat sac
    c.ellipse(31, y + 16 + sac // 2, 9, 4 + sac, ix("pink_light"))
    c.dither_ellipse(31, y + 16 + sac // 2, 6, 2 + sac, "pink_mid", parity=1)

    if variant == "attack":
        # tongue lash across the arena
        c.hline(8, 31, y + 14, ix("red_mid"))
        c.circle(8, y + 14, 3.0, ix("red_dark"))
        c.set(7, y + 13, ix("pink_light"))
    elif variant == "special_a":
        # bubble barrage rising
        for (bx, by, r) in ((12, y - 6, 3), (28, y - 10, 4), (46, y - 5, 3)):
            c.circle(bx, by, r, ix("cyan_mid"))
            c.circle(bx, by, r - 1, ix("cyan_light"))
            c.set(bx - 1, by - 1, ix("white"))
    elif variant == "stagger":
        c.hline(14, 48, y + 11, ix("pine_dark"))            # mouth slack
        _stars(c, 31, y - 10, 22)
        _sweat(c, [(10, y + 4), (52, y + 2)], color="heal_inner")
    return _finish(c, "cane_dark")


def b_marsh_caiman(pal, variant="idle"):
    """凯门鳄王: armored scutes down the spine, jaws that end arguments."""
    c = Canvas(BOSS_SPRITE, BOSS_SPRITE)
    p = _b(variant)
    y = 14 + p["dy"]
    gape = 4 if variant in ("windup", "attack") else 0

    # tail, thick to thin
    _limb(c, 48, y + 24, 60, y + 16, 8, "pine_dark")
    _limb(c, 58, y + 17, 62, y + 10, 4, "pine_dark")
    # legs
    for lx in (16, 40):
        c.round_rect(lx, y + 30, 8, 14, 3, ix("pine_dark"))
    # body
    c.blob(32, y + 20, 18, 12 - p["squash"] // 2, ix("pine_dark"), n=2.8)
    c.blob(32, y + 19, 16, 10 - p["squash"] // 2, ix("grn_dark"), n=2.8)
    c.ellipse(32, y + 26, 12, 5, ix("cane_mid"))            # belly
    # scutes: a sawblade spine
    for k in range(6):
        sx = 20 + k * 6
        c.set(sx, y + 6 + (k % 2), ix("cane_light"))
        c.set(sx + 1, y + 5 + (k % 2), ix("cane_light"))
    # head + jaws
    c.blob(14, y + 12, 12, 8, ix("grn_dark"), n=2.8)
    c.blob(6, y + 14 + gape, 8, 3.5, ix("grn_dark"), n=2.5)   # upper jaw
    c.blob(8, y + 20, 7, 3.0, ix("grn_dark"), n=2.5)          # lower jaw
    c.hline(2, 12, y + 17 + gape // 2, ix("pine_dark"))
    _teeth(c, 2, 12, y + 16 + gape // 2, 5)
    _teeth(c, 3, 11, y + 19, 4, down=False)
    for ex in (12, 20):
        gc.pk_eye(c, ex, y + 8, 2.0, 1.8, mode="open",
                  iris="red_mid" if variant == "special_b" else "ember_orange",
                  variant=None, sclera=None)
    # nostril bumps
    c.set(2, y + 12, ix("cane_light"))
    c.set(4, y + 12, ix("cane_light"))

    if variant == "attack":
        c.sparkle(4, y + 8, 2, "cyan_light")                # snap spray
    elif variant == "special_a":
        c.dither_ellipse(52, y + 38, 14, 5, "murk_dark", parity=0)
        c.sparkle(56, y + 34, 3, "sea_light")               # tail-sweep spray
    elif variant == "stagger":
        _stars(c, 24, y - 6, 18)
        _wince(c, [(10, y + 6, 3), (18, y + 6, 3)])
    return _finish(c, "pine_dark")


def b_volc_golem(pal, variant="idle"):
    """焦糖熔岩巨兽: slab shoulders, boulder fists, a white-hot core."""
    c = Canvas(BOSS_SPRITE, BOSS_SPRITE)
    p = _b(variant)
    y = 8 + p["dy"]
    hot = ("ember_yellow", "ember_orange") if variant != "special_b" \
        else ("white", "ember_yellow")

    # legs: two rock slabs
    c.round_rect(18, y + 38, 12, 14, 3, ix("ash_dark"))
    c.round_rect(34, y + 38, 12, 14, 3, ix("ash_dark"))
    c.hline(18, 29, y + 50, ix("rock_dark"))
    c.hline(34, 45, y + 50, ix("rock_dark"))
    # torso
    c.blob(32, y + 22, 18, 15 - p["squash"], ix("ash_dark"), n=2.6)
    c.blob(32, y + 21, 16, 13 - p["squash"], ix("ash_mid"), n=2.6)
    # core cracks: the lava inside showing through
    for (x0, y0, x1, y1) in ((26, y + 14, 32, y + 24), (38, y + 16, 34, y + 28),
                             (30, y + 30, 26, y + 36)):
        c.vline(x0, min(y0, y1), max(y0, y1), ix(hot[1]))
        c.set(x1, y1, ix(hot[1]))
        c.set((x0 + x1) // 2, (y0 + y1) // 2, ix(hot[0]))
    # far fist
    c.blob(52, y + 26, 8, 8, ix("rock_dark"), n=2.6)
    _limb(c, 46, y + 18, 52, y + 24, 8, "ash_dark")
    # near fist, raised in attack
    c.blob(10, y + 24 - (6 if variant == "attack" else 0),
           9, 9, ix("rock_dark"), n=2.6)
    _limb(c, 18, y + 16, 11, y + 22, 8, "ash_mid")
    # head: a small slab under the shoulders
    c.round_rect(24, y, 16, 10, 3, ix("ash_dark"))
    c.hline(24, 39, y, ix("ash_mid"))
    for ex in (28, 36):
        c.ellipse(ex, y + 5, 2.6, 2.0, ix(hot[0]))
        c.set(ex, y + 5, ix("white_spark"))
    # shoulder slabs
    c.round_rect(8, y + 10, 12, 8, 3, ix("ash_dark"))
    c.round_rect(44, y + 10, 12, 8, 3, ix("ash_dark"))

    if variant == "attack":
        c.dither_ellipse(12, y + 34, 12, 4, "ash_dark", parity=0)
        c.sparkle(8, y + 30, 3, hot[0])
    elif variant == "special_a":
        c.grad_ellipse(32, y + 24, 22, 18, [hot[0], hot[1], "lava_dark"])
        c.blob(32, y + 21, 16, 13, ix("ash_mid"), n=2.6)
    elif variant == "stagger":
        c.vline(24, y + 4, y + 16, ix("black"))             # slab cracked
        c.vline(40, y + 20, y + 34, ix("black"))
        _stars(c, 32, y - 6, 20)
    return _finish(c, "ash_dark")


def b_volc_cakelord(pal, variant="idle"):
    """蛋糕魔王: three chocolate tiers, icing drips, candle horns."""
    c = Canvas(BOSS_SPRITE, BOSS_SPRITE)
    p = _b(variant)
    y = 12 + p["dy"]
    icing = "fire_outer" if variant == "special_b" else "cream"

    # feet
    c.ellipse(22, y + 44, 7, 4, ix("choco_dark"))
    c.ellipse(42, y + 44, 7, 4, ix("choco_dark"))
    # tier 3 (bottom)
    c.round_rect(12, y + 30, 40, 16, 4, ix("choco_dark"))
    c.round_rect(13, y + 30, 38, 14, 4, ix("choco_mid"))
    # tier 2
    c.round_rect(17, y + 16, 30, 16, 4, ix("choco_dark"))
    c.round_rect(18, y + 16, 28, 14, 4, ix("choco_light"))
    # tier 1 (head)
    c.round_rect(22, y + 2, 20, 16, 4, ix("choco_dark"))
    c.round_rect(23, y + 2, 18, 14, 4, ix("choco_mid"))
    # icing drips down every tier edge
    for dx in range(13, 51, 5):
        c.vline(dx, y + 30, y + 33 + (dx % 3), ix(icing))
    for dx in range(18, 47, 5):
        c.vline(dx, y + 16, y + 19 + (dx % 3), ix(icing))
    for dx in range(23, 41, 4):
        c.vline(dx, y + 2, y + 5 + (dx % 3), ix(icing))
    # face: cherry eyes, icing mouth
    for ex in (27, 37):
        gc.pk_eye(c, ex, y + 8, 2.4, 2.2, mode="open",
                  iris="red_mid" if variant == "special_b" else "cherry_dark",
                  variant=None, sclera="cream")
    c.hline(26, 38, y + 13, ix(icing))
    _teeth(c, 27, 37, y + 14, 5)
    # candle horns with real flames
    for hx in (24, 40):
        c.vline(hx, y - 6, y + 2, ix("cream"))
        c.grad_ellipse(hx, y - 9, 2.2, 3.2,
                       ["ember_yellow", "ember_orange", "fire_outer"])
        c.set(hx, y - 10, ix("white_spark"))
    # arms: little stubs with mitts
    _limb(c, 12, y + 22, 4, y + 32, 6, "choco_mid")
    c.circle(4, y + 34, 5, ix("choco_dark"))
    _limb(c, 52, y + 22, 60, y + 32, 6, "choco_mid")
    c.circle(60, y + 34, 5, ix("choco_dark"))

    if variant == "attack":
        c.dither_ellipse(32, y + 47, 20, 4, "choco_dark", parity=0)
    elif variant == "special_a":
        # sprinkles raining
        for (sx, sy, col) in ((14, y + 4, "yellow_light"),
                              (50, y + 2, "pink_light"),
                              (22, y - 6, "cyan_light"),
                              (44, y - 8, "white")):
            c.set(sx, sy, ix(col))
            c.set(sx + 1, sy + 1, ix(col))
    elif variant == "stagger":
        # tiers slip sideways and the candles go out
        c.round_rect(20, y + 2, 20, 16, 4, ix("choco_mid"))
        c.hline(24, 40, y - 9, ix("grey_dark"))             # smoke wisp
        _stars(c, 32, y - 12, 20)
        _wince(c, [(25, y + 6, 3), (35, y + 6, 3)])
    return _finish(c, "choco_dark")


MOBS = {
    # 雨林
    "rain_frog": m_rain_frog,
    "rain_wasp": m_rain_wasp,
    "rain_slime": m_rain_slime,
    # 农场
    "farm_rat": m_farm_rat,
    "farm_crow": m_farm_crow,
    "farm_mudlet": m_farm_mudlet,
    # 牧场
    "past_dog": m_past_dog,
    "past_ram": m_past_ram,
    "past_hornet": m_past_hornet,
    # 沼泽
    "marsh_slime": m_marsh_slime,
    "marsh_viper": m_marsh_viper,
    "marsh_toad": m_marsh_toad,
    # 火山
    "volc_imp": m_volc_imp,
    "volc_bat": m_volc_bat,
    "volc_orb": m_volc_orb,
}

BOSSES = {
    # 雨林
    "rain_ape": (b_rain_ape, ("grn_dark", "grn_mid")),
    "rain_treant": (b_rain_treant, ("leaf_dark", "leaf_mid")),
    # 农场
    "farm_boar": (b_farm_boar, ("sand_dark", "wheat_mid")),
    "farm_ratking": (b_farm_ratking, ("wheat_dark", "wheat_light")),
    # 牧场
    "past_wolf": (b_past_wolf, ("grey_dark", "grey_mid")),
    "past_bull": (b_past_bull, ("fur_black", "steel_dark")),
    # 沼泽
    "marsh_toadlord": (b_marsh_toadlord, ("purple_dark", "cact_dark")),
    "marsh_caiman": (b_marsh_caiman, ("pine_dark", "cane_dark")),
    # 火山
    "volc_golem": (b_volc_golem, ("ash_dark", "lava_mid")),
    "volc_cakelord": (b_volc_cakelord, ("choco_dark", "cherry_mid")),
}


def to_boss_canvas(art, aura=None):
    """Frame a 64x64 boss painting: contact shadow, then an aura ring.

    The aura is a dithered radial gradient in the region's own ramp --
    light it emits rather than a plate it stands on.
    """
    out = Canvas(BOSS_SPRITE, BOSS_SPRITE)

    if aura:
        cx, cy = BOSS_SPRITE / 2 - 0.5, BOSS_SPRITE / 2 - 0.5
        out.grad_ellipse(cx, cy, 30, 27, [aura[1], aura[0]])
        out.dither_ellipse(cx, cy, 30, 27, aura[0], parity=1)

    # ground shadow
    for i, r in enumerate((22, 17, 12)):
        cx = BOSS_SPRITE / 2 - 0.5
        col = ix("shadow") if i else ix("outline_soft")
        out.ellipse(cx, 58, r, 3.0, col)

    out.paste(art, 0, 0)
    return out


def pack(canvases):
    blob = bytearray()
    for c in canvases:
        blob += c.to_bmp8()
    return bytes(blob)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    graphics_dir = os.path.join(root, "graphics")
    os.makedirs(graphics_dir, exist_ok=True)

    previews = []
    labels = []

    all_mob_frames = []
    print("--- Generating 15 Mobs (32x32) ---")
    for name, fn in MOBS.items():
        frames = [fn(None, v) for v in VARIANTS]
        all_mob_frames.extend(frames)
        previews.append(frames[0])
        labels.append(name)

    mob_bin = pack(all_mob_frames)
    mob_path = os.path.join(graphics_dir, "mob_tiles.bin")
    with open(mob_path, "wb") as f:
        f.write(mob_bin)
    print(f"Generated mob tiles: {mob_path} "
          f"({len(all_mob_frames)} frames, {len(mob_bin)} bytes)")

    all_boss_frames = []
    print("--- Generating 10 Dual-Phase Bosses (64x64, 8 poses) ---")
    boss_previews = []
    boss_labels = []
    for name, (fn, aura) in BOSSES.items():
        frames = [to_boss_canvas(fn(None, v), aura) for v in BOSS_VARIANTS]
        all_boss_frames.extend(frames)
        boss_previews.append(frames[0])
        boss_labels.append(name)

    boss_bin = pack(all_boss_frames)
    boss_path = os.path.join(graphics_dir, "boss_tiles.bin")
    with open(boss_path, "wb") as f:
        f.write(boss_bin)
    print(f"Generated boss tiles: {boss_path} "
          f"({len(all_boss_frames)} frames, {len(boss_bin)} bytes)")

    out_png = os.path.join(graphics_dir, "preview_monsters.png")
    preview_scale(previews + boss_previews, out_png, scale=3, cols=5,
                  labels=labels + boss_labels, title="15 Mobs and 10 Bosses")
    print(f"Generated monster catalog: {out_png}")


if __name__ == "__main__":
    main()
