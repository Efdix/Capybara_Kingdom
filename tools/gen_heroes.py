"""
gen_heroes.py -- the two playable capybaras of Capybara Kingdom.

  公「卡」 hero_m  warm chestnut brown
  母「皮」 hero_f  soft golden tan: the SAME painter run through
                   gen_common's VARIANT_FEMALE remap (brown -> tan), here
                   extended with the matching fur-ramp names. 皮 also wears
                   a small flower by her left ear (leaf -> pink via the same
                   remap).

Art direction: GBA Pokemon partner proportions -- the head is the widest
shape on the sprite and clearly wider than the body behind it, ears are
separate bumps on top of the outline (not merged into it), the eyes are big
glossy features with a double glint, the muzzle is a light plane with a
small dark nose, and everything is lit in two hard planes (bottom shadow +
top-left rim) under a crisp near-black outline. The 卡 / 皮 forehead brand
stays: a hand-thinned 7x7 dot matrix drawn as ink INSIDE the silhouette.

Per-frame pipeline:
    fur base shapes + belly patch + muzzle plane
    -> pk_shade_bottom(brn_deep)    underside plane (thin parts skipped)
    -> pk_rim(brn_hl)               lit plane (far limbs painted deep and
                                    protected from the rim)
    -> ink details (pk_eye, nose, mouth, brand, claws)
    -> FX (sweat drops, bite arc + sparkle)
    -> auto_outline()               LAST, so nothing eats the outline

Sheet contract (mirrors source/assets.h and sprites.c::hero_frame()):
    32x32, 8 directions x 16 frames; per direction idle 4 / walk 6 / bite 4
    / hurt 2. Direction order DIR_DOWN, DIR_UP, DIR_LEFT (stored as a mirror
    of right), DIR_RIGHT, then the diagonals with down-left / up-left stored
    mirrored from their right-hand twins. Every frame keeps a 1px empty
    margin so the outline always has somewhere to grow; main() asserts it.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_common as gc
from gen_common import Canvas, ix

W, H = 32, 32
DIRS = 8
FRAMES_PER_DIR = 16

ANIM_ORDER = [
    ("idle", 4),
    ("walk", 6),
    ("bite", 4),
    ("hurt", 2),
]

# The female is the male painter with the palette's variant remap applied
# (brown -> tan, leaf -> flower).
VARIANT_F = dict(gc.VARIANT_FEMALE)
VARIANT_F.update({
    "brn_shadow": "tan_shadow",
    "brn_deep": "tan_deep",
    "brn_hi": "tan_hi",
    "brn_hl": "tan_hl",
})

HEROES = [
    ("hero_m", "卡", None),
    ("hero_f", "皮", VARIANT_F),
]

# Forehead brand: 7x7 dot matrices, hand-thinned from a bold system-font
# rendering so every surviving stroke reads at 1px. Bottom strokes are kept
# clear of the eye columns; see the eye positions in paint_front.
KANJI = {
    "卡": [
        "...X...",
        "...XXX.",
        "...X...",
        ".XXXXX.",
        "...X.X.",
        "...X.X.",
        "...X...",
    ],
    "皮": [
        ".XXXXX.",
        "X....XX",
        "X...X.X",
        ".X.XX..",
        ".X..X.X",
        "..X.XX.",
        "..X..X.",
    ],
}


# ---------------------------------------------------------------------------
# Pose: the animation driver
# ---------------------------------------------------------------------------

def pose_for(anim, f):
    """Turn (anim, frame) into the pose parameters every painter consumes.

    Frame TIMING lives only here; the painters decide where things are
    drawn. That keeps the six walk frames of every direction in antiphase
    by construction instead of by eight copies of the same table.
    """
    p = {
        "bob": 0,           # body+head vertical offset; feet stay planted
        "lunge": 0,         # bite: head thrust forward
        "recoil": 0,        # hurt: head thrown backward
        "ear": (0, 0),      # ear micro-motion (dx, dy)
        "waddle": 0,        # front/back body sway
        "tilt": 0,          # whole-sprite shiver (hurt recovery)
        "eyes": "open",     # open | blink | hurt
        "mouth": "closed",  # closed | open | chomp | scream | wavy
        "arc": False,       # white bite-arc fx
        "sweat": False,     # two sweat drops overhead
        "paw": 0,           # 0..2 front paws raised (bite)
        "legf": 0,          # side view: near-front leg stride
        "legb": 0,          # side view: near-back leg (antiphase)
        "footl": 0,         # front/back views: 1 = foot lifted
        "footr": 0,
    }
    if anim == "idle":
        # Breathe: base, inhale and hold, exhale overshoot. The ears flick
        # on every second frame and the cycle blinks once, on the hold.
        p["bob"] = (0, -1, -1, 1)[f]
        p["ear"] = ((-1, 0, 1, 0)[f], 0)
        if f == 2:
            p["eyes"] = "blink"
    elif anim == "walk":
        # Three half-strides: contact (legs spread, body pressed low) ->
        # passing (legs gathered, body lifted) x3. Near-front and near-back
        # legs move in antiphase; the ears and the sway follow the stride.
        p["bob"] = (1, 0, -1, 1, 0, -1)[f]
        p["legf"] = (2, 1, 0, -2, -1, 0)[f]
        p["legb"] = -p["legf"]
        p["waddle"] = (-1, -1, 0, 1, 1, 0)[f]
        p["footr"] = 1 if f in (0, 1) else 0
        p["footl"] = 1 if f in (3, 4) else 0
        p["ear"] = (-p["waddle"], 0)
    elif anim == "bite":
        if f == 0:      # wind-up: crouch, jaws part, paws come up
            p["bob"], p["lunge"], p["mouth"], p["paw"] = 1, -1, "open", 1
        elif f == 1:    # lunge: head snaps forward, the incisors show
            p["lunge"], p["mouth"], p["paw"] = 2, "open", 2
        elif f == 2:    # chomp: the jaws close on the target
            p["bob"], p["lunge"], p["mouth"], p["paw"] = 1, 1, "chomp", 1
        else:           # settle: a small white mark where the snap landed
            p["arc"] = True
    else:               # hurt
        if f == 0:      # thrown back: >_< eyes, a scream, two sweat drops
            p["recoil"], p["bob"] = 2, 1
            p["eyes"], p["mouth"], p["sweat"] = "hurt", "scream", True
        else:           # shivering recovery
            p["tilt"], p["eyes"] = -1, "hurt"
            p["mouth"], p["sweat"] = "wavy", True
    return p


# ---------------------------------------------------------------------------
# Shared craft helpers
# ---------------------------------------------------------------------------

def shade(c, V):
    """The two-plane lighting pass every view shares."""
    gc.pk_shade_bottom(c, "brn_deep", V, rows=2)
    gc.pk_rim(c, "brn_hl", V, protected={ix("brn_deep", V),
                                         ix("brn_shadow", V)})


def eye(c, cx, cy, rx, ry, mode, V):
    """Every eye goes through gc.pk_eye (white sclera, big pupil, double
    glint) -- the GBA partner look."""
    gc.pk_eye(c, cx, cy, rx, ry, mode=mode, variant=V)


def draw_kanji(c, ch, ox, oy, V=None):
    """The forehead brand: pixels inside the fur, drawn after the light
    passes so shading can never mute it and before the outline, which only
    ever fills empty pixels and therefore cannot eat it. Drawn in the deep
    fur tone rather than ink -- a 7x7 pure-black block was a hole punched
    through the pale female head, where the dark brown reads as a marking
    that is still unmistakably a glyph."""
    ink = ix("brn_deep", V)
    for r, row in enumerate(KANJI[ch]):
        for k, v in enumerate(row):
            if v == "X":
                c.set(ox + k, oy + r, ink)


def ear_pair(c, hx, hy, eax, eay, V, spread=8.4, drop=4.7, r=2.35):
    """Round ears sitting ON TOP of the head outline, with a shadow dot at
    the root where they meet the fur."""
    fur = ix("brn_mid", V)
    shadow = ix("brn_shadow", V)
    for sgn in (-1, 1):
        c.blob(hx + sgn * spread + eax, hy - drop + eay, r, r, fur, n=3.5)
        c.set(int(hx + sgn * spread + eax - sgn * 0.8),
              int(hy - drop + eay + 1.6), shadow)


def feet_front(c, cx, V, footl, footr, spread=4.5):
    """Two stubby planted feet, 5x4 with rounded corners."""
    fur = ix("brn_mid", V)
    deep = ix("brn_deep", V)
    for sgn, lift in ((-1, footl), (1, footr)):
        fx = int(cx + sgn * spread) - 2 + (sgn if lift else 0)
        c.round_rect(fx, 24 - lift, 5, 4, 2, fur)
        c.hline(fx + 1, fx + 3, 27 - lift, deep)


def mouth_front(c, mx, my, kind, V):
    """Front-facing mouth at the muzzle centre; `open` hangs the two
    incisors from the top lip -- the soul of a capybara bite."""
    ink = ix("fur_black")
    if kind == "closed":
        c.set(mx - 2, my, ink)
        c.set(mx - 1, my + 1, ink)
        c.set(mx + 1, my + 1, ink)
        c.set(mx + 2, my, ink)
    elif kind == "open":
        c.round_rect(mx - 2, my, 5, 3, 1, ink)
        c.vline(mx - 1, my, my + 1, ix("white"))
        c.vline(mx + 1, my, my + 1, ix("white"))
    elif kind == "chomp":
        c.hline(mx - 2, mx + 2, my, ink)
        c.set(mx - 1, my + 1, ix("white"))
        c.set(mx + 1, my + 1, ix("white"))
    elif kind == "scream":
        c.round_rect(mx - 2, my - 1, 5, 4, 1, ink)
        c.vline(mx - 1, my - 1, my, ix("white"))
        c.vline(mx + 1, my - 1, my, ix("white"))
        c.set(mx, my + 2, ix("pink_dark"))
    else:  # wavy shiver
        for k, dx in enumerate((-2, -1, 0, 1, 2)):
            c.set(mx + dx, my + (k & 1), ink)


def mouth_side(c, mx, my, kind, V):
    """Side mouth along the bottom edge of the blunt muzzle. Closed stays a
    soft seam (deep tone, not ink) so the profile does not grow a hole."""
    ink = ix("fur_black")
    if kind == "closed":
        c.hline(mx, mx + 2, my, ix("brn_deep", V))
    elif kind == "open":
        c.round_rect(mx, my - 1, 4, 3, 1, ink)
        c.vline(mx + 1, my - 1, my, ix("white"))
        c.vline(mx + 2, my - 1, my, ix("white"))
    elif kind == "chomp":
        c.hline(mx, mx + 2, my, ink)
        c.set(mx + 1, my + 1, ix("white"))
    elif kind == "scream":
        c.round_rect(mx, my - 2, 4, 4, 1, ink)
        c.vline(mx + 1, my - 2, my - 1, ix("white"))
        c.set(mx + 2, my + 1, ix("pink_dark"))
    else:  # wavy shiver
        for k, dx in enumerate((0, 1, 2, 3)):
            c.set(mx + dx, my + (k & 1), ink)


def sweat_drops(c, spots):
    """Two flying sweat drops overhead -- one blue, one pink."""
    for (x, y), mid, light in spots:
        c.set(x, y, light)
        c.rect(x, y + 1, 2, 2, mid)


def bite_arc(c, ax, ay):
    """The little white snap-mark: a sparkle with two arm pixels."""
    w = ix("white_spark")
    c.set(ax - 2, ay, w)
    c.set(ax + 2, ay, w)
    c.sparkle(ax, ay, 1, "white_spark", core="yellow_light")


def flower(c, fx, fy, V):
    """皮's ear flower: three petal pixels around a light centre. Drawn in
    the leaf_* ramp so the female remap turns it pink for free."""
    c.set(fx - 1, fy, ix("leaf_dark", V))
    c.set(fx + 1, fy, ix("leaf_dark", V))
    c.set(fx, fy - 1, ix("leaf_mid", V))
    c.set(fx, fy + 1, ix("leaf_mid", V))
    c.set(fx, fy, ix("leaf_light", V))


# ---------------------------------------------------------------------------
# Painters, one per stored view (left-facing rows are mirrors of these)
# ---------------------------------------------------------------------------

def paint_front(c, p, V, ch):
    """DIR_DOWN: square-on face. The forehead brand lives here."""
    fur = ix("brn_mid", V)
    deep = ix("brn_deep", V)
    belly = ix("brn_belly", V)
    snout = ix("brn_light", V)
    ink = ix("fur_black")

    bob, lunge, recoil = p["bob"], p["lunge"], p["recoil"]
    hdy = lunge - recoil          # from the front, "forward" is down-screen
    cx = 16 + p["waddle"] + p["tilt"]
    by = 20.9 + bob
    hx = cx
    hy = 9.8 + bob + hdy
    eax, eay = p["ear"]

    # -- fur base planes ---------------------------------------------------
    c.blob(cx, by, 7.2, 4.9, fur, n=4.0)                     # compact body
    c.ellipse(cx, by + 1.6, 4.6, 2.8, belly)                 # belly patch
    c.blob(hx, hy, 8.6, 6.9, fur, n=4.0)                     # big round head
    ear_pair(c, hx, hy, eax, eay, V)
    c.blob(hx, hy + 5.0, 4.9, 2.8, snout, n=3.0)             # blunt muzzle
    if V:                                                    # 皮's ear flower
        flower(c, int(hx - 8.0 + eax), int(hy - 6.2 + eay), V)
    feet_front(c, cx, V, p["footl"], p["footr"])

    # -- the two light planes ----------------------------------------------
    shade(c, V)

    # -- ink details ---------------------------------------------------------
    c.hline(int(hx) - 2, int(hx) + 2, int(hy) + 8, deep)      # chin shadow
    ey = hy + 1.7
    eye(c, hx - 4.7, ey, 1.9, 2.4, p["eyes"], V)
    eye(c, hx + 4.7, ey, 1.9, 2.4, p["eyes"], V)
    c.set(int(hx - 1.6), int(hy + 4.3), ink)                  # nostrils
    c.set(int(hx + 1.6), int(hy + 4.3), ink)
    mouth_front(c, int(hx), int(hy + 5.6), p["mouth"], V)
    for sgn in (-1, 1):                                       # blush
        bx0 = int(hx) + sgn * 6
        c.set(bx0, int(hy + 3.6), ix("blush"))
        c.set(bx0, int(hy + 4.4), ix("blush"))
    if ch:
        draw_kanji(c, ch, int(hx) - 3, int(hy) - 6, V)
    if p["paw"]:                                              # raised paws
        py = int(by) + 1 - p["paw"]
        for sgn in (-1, 1):
            px0 = int(cx) + sgn * 5 - 2 + sgn
            c.round_rect(px0, py, 4, 3, 1, fur)
            c.hline(px0 + 1, px0 + 2, py + 2, deep)
    if p["sweat"]:
        sweat_drops(c, [
            ((int(hx) - 11, int(hy) - 4), ix("cyan_mid"), ix("cyan_light")),
            ((int(hx) + 10, int(hy) - 3), ix("pink_mid"), ix("pink_light")),
        ])
    if p["arc"]:
        bite_arc(c, int(hx), int(hy) + 10)


def paint_back(c, p, V):
    """DIR_UP: the back of the loaf -- round rump, head-back, ears, hind
    feet and the tiny tail nub between them."""
    fur = ix("brn_mid", V)
    deep = ix("brn_deep", V)

    bob, lunge, recoil = p["bob"], p["lunge"], p["recoil"]
    hdy = lunge - recoil
    cx = 16 + p["waddle"] + p["tilt"]
    by = 21.0 + bob
    hy = 9.8 + bob + hdy
    eax, eay = p["ear"]

    c.blob(cx, by, 7.8, 5.8, fur, n=4.0)                      # rump
    c.blob(cx, hy, 8.0, 7.0, fur, n=4.0, squash_top=0.18)     # back of head
    ear_pair(c, cx, hy, eax, eay, V, spread=7.8, drop=4.6)
    # tail nub, dead centre -- the one thing a rear view has to say
    c.circle(cx, by + 4.6, 1.6, deep)
    c.set(int(cx) - 1, int(by) + 4, ix("brn_mid", V))
    feet_front(c, cx, V, p["footl"], p["footr"], spread=4.0)

    shade(c, V)

    if p["sweat"]:
        sweat_drops(c, [
            ((int(cx) - 10, int(hy) - 6), ix("cyan_mid"), ix("cyan_light")),
            ((int(cx) + 9, int(hy) - 5), ix("pink_mid"), ix("pink_light")),
        ])
    if p["arc"]:
        bite_arc(c, int(cx), int(hy) - 7)


def paint_side(c, p, V):
    """DIR_RIGHT: the profile. Big round head over a barrel body, one glossy
    eye, blunt muzzle thrust forward."""
    fur = ix("brn_mid", V)
    deep = ix("brn_deep", V)
    belly = ix("brn_belly", V)
    snout = ix("brn_light", V)

    bob, lunge, recoil = p["bob"], p["lunge"], p["recoil"]
    ldx = lunge - recoil
    ldy = (lunge - recoil) // 2
    bx = 13.5 + p["tilt"]
    by = 20.4 + bob
    hx = 20.8 + ldx + p["tilt"]
    hy = 10.6 + bob + ldy
    eax, eay = p["ear"]

    # far legs first: deep tone, antiphase to the near pair
    c.round_rect(int(14.5 + p["legb"]), 24, 3, 4, 1, deep)
    c.round_rect(int(7.5 + p["legf"]), 24, 3, 4, 1, deep)
    c.blob(bx, by, 8.8, 6.2, fur, n=4.2)                      # barrel
    c.ellipse(bx - 0.5, by + 2.6, 6.4, 2.8, belly)            # belly strip
    c.blob(hx, hy, 6.6, 6.4, fur, n=4.0)                      # round head
    c.blob(hx - 4.4, hy - 4.6, 1.3, 1.4, deep, n=3.5)         # far ear
    c.blob(hx - 2.4 + eax, hy - 5.4 + eay, 2.0, 2.1, fur, n=3.5)
    c.blob(hx + 5.4, hy + 2.6, 3.3, 2.8, snout, n=3.0)        # blunt muzzle
    if V:
        flower(c, int(hx - 2.4 + eax), int(hy - 5.6 + eay), V)
    # near legs; the front one lifts as the bite reaches
    paw = 1 if p["paw"] else 0
    c.round_rect(int(17.5 + p["legf"] + paw), 23 - paw, 4, 5, 2, fur)
    c.round_rect(int(4.5 + p["legb"]), 23, 4, 5, 2, fur)
    c.set(int(bx) - 10, int(by) + 0, fur)                     # tail nub
    c.set(int(bx) - 10, int(by) + 1, fur)

    shade(c, V)

    eye(c, hx + 1.4, hy + 0.6, 1.9, 2.3, p["eyes"], V)
    c.set(int(hx + 7.4), int(hy + 1.6), ix("fur_black"))      # nostril
    mouth_side(c, int(hx + 4.4), int(hy + 4.6), p["mouth"], V)
    c.set(int(hx - 2.4), int(hy + 3.6), ix("blush"))          # cheek blush
    if p["sweat"]:
        sweat_drops(c, [
            ((int(hx) - 7, int(hy) - 8), ix("cyan_mid"), ix("cyan_light")),
            ((int(hx) + 4, int(hy) - 9), ix("pink_mid"), ix("pink_light")),
        ])
    if p["arc"]:
        bite_arc(c, int(hx) + 7, int(hy) + 6)


def paint_diag_front(c, p, V, ch):
    """DIR_DOWN_RIGHT: three-quarter face -- the near (right) side wins the
    muzzle and the bigger eye, the brand follows the forehead around."""
    fur = ix("brn_mid", V)
    deep = ix("brn_deep", V)
    belly = ix("brn_belly", V)
    snout = ix("brn_light", V)
    ink = ix("fur_black")

    bob, lunge, recoil = p["bob"], p["lunge"], p["recoil"]
    hdy = lunge - recoil
    hdx = max(0, lunge) // 2 - max(0, recoil) // 2
    cx = 15.5 + p["waddle"] + p["tilt"]
    by = 20.9 + bob
    hx = 17.4 + hdx + p["tilt"]
    hy = 9.8 + bob + hdy
    eax, eay = p["ear"]

    c.blob(cx, by, 7.6, 5.6, fur, n=4.0)
    c.ellipse(cx + 0.5, by + 1.8, 4.6, 2.8, belly)
    c.blob(hx, hy, 8.4, 6.9, fur, n=4.0)
    ear_pair(c, hx, hy, eax, eay, V, spread=6.9, drop=4.7)
    c.blob(hx + 2.2, hy + 5.0, 4.7, 2.8, snout, n=3.0)         # blunt muzzle
    if V:
        flower(c, int(hx - 6.7 + eax), int(hy - 6.0 + eay), V)
    for sgn, lift, w in ((-1, p["footl"], 4), (1, p["footr"], 5)):
        fx = int(cx) + sgn * 4 - 2 + (sgn if lift else 0)
        c.round_rect(fx, 24 - lift, w, 4, 2, fur)
        c.hline(fx + 1, fx + w - 2, 27 - lift, deep)

    shade(c, V)

    c.hline(int(hx) + 1, int(hx) + 4, int(hy) + 8, deep)       # chin shadow
    eye(c, hx - 4.6, hy + 1.7, 1.7, 2.1, p["eyes"], V)         # far eye
    eye(c, hx + 4.8, hy + 1.7, 1.9, 2.4, p["eyes"], V)         # near eye
    c.set(int(hx + 0.6), int(hy + 4.3), ink)                   # nostrils
    c.set(int(hx + 3.6), int(hy + 4.3), ink)
    mouth_front(c, int(hx + 2.2), int(hy + 5.6), p["mouth"], V)
    c.set(int(hx - 5.6), int(hy + 4.0), ix("blush"))
    c.set(int(hx + 7.4), int(hy + 3.6), ix("blush"))
    if ch:
        draw_kanji(c, ch, int(hx) - 3, int(hy) - 6, V)
    if p["paw"]:
        py = int(by) + 1 - p["paw"]
        for sgn in (-1, 1):
            px0 = int(cx) + sgn * 5 - 2 + sgn
            c.round_rect(px0, py, 4, 3, 1, fur)
            c.hline(px0 + 1, px0 + 2, py + 2, deep)
    if p["sweat"]:
        sweat_drops(c, [
            ((int(hx) - 10, int(hy) - 4), ix("cyan_mid"), ix("cyan_light")),
            ((int(hx) + 10, int(hy) - 3), ix("pink_mid"), ix("pink_light")),
        ])
    if p["arc"]:
        bite_arc(c, int(hx) + 4, int(hy) + 10)


def paint_diag_back(c, p, V):
    """DIR_UP_RIGHT: three-quarter back -- rump, both ears, and a sliver of
    cheek to say which way the head is turned."""
    fur = ix("brn_mid", V)
    deep = ix("brn_deep", V)
    snout = ix("brn_light", V)

    bob, lunge, recoil = p["bob"], p["lunge"], p["recoil"]
    hdy = lunge - recoil
    cx = 16 + p["waddle"] + p["tilt"]
    by = 21.0 + bob
    hx = 17.8 + p["tilt"]
    hy = 9.8 + bob + hdy
    eax, eay = p["ear"]

    c.blob(cx, by, 7.8, 5.8, fur, n=4.0)
    c.blob(hx, hy, 7.8, 7.0, fur, n=4.0, squash_top=0.18)
    ear_pair(c, hx, hy, eax, eay, V, spread=6.6, drop=4.7)
    c.ellipse(hx + 5.2, hy + 2.8, 1.8, 2.0, snout)            # cheek sliver
    c.circle(cx + 1, by + 4.6, 1.6, deep)                     # tail nub
    for sgn, lift in ((-1, p["footl"]), (1, p["footr"])):
        fx = int(cx) + sgn * 4 - 2 + (sgn if lift else 0)
        c.round_rect(fx, 24 - lift, 5, 4, 2, fur)
        c.hline(fx + 1, fx + 3, 26 - lift, deep)

    shade(c, V)

    if p["sweat"]:
        sweat_drops(c, [
            ((int(hx) - 9, int(hy) - 6), ix("cyan_mid"), ix("cyan_light")),
            ((int(hx) + 9, int(hy) - 5), ix("pink_mid"), ix("pink_light")),
        ])
    if p["arc"]:
        bite_arc(c, int(hx), int(hy) - 7)


# ---------------------------------------------------------------------------
# Sheet assembly
# ---------------------------------------------------------------------------

PAINTERS = {
    "down": paint_front,
    "up": paint_back,
    "right": paint_side,
    "down_right": paint_diag_front,
    "up_right": paint_diag_back,
}

# Stored direction order == source/game.h DIR_*. The left-facing rows are
# mirrors of their right-hand twins, which sprites.c relies on.
DIR_VIEWS = [
    ("down", False),        # 0 DIR_DOWN
    ("up", False),          # 1 DIR_UP
    ("right", True),        # 2 DIR_LEFT   = mirror
    ("right", False),       # 3 DIR_RIGHT
    ("down_right", True),   # 4 DIR_DOWN_LEFT = mirror
    ("down_right", False),  # 5 DIR_DOWN_RIGHT
    ("up_right", True),     # 6 DIR_UP_LEFT = mirror
    ("up_right", False),    # 7 DIR_UP_RIGHT
]


def render_frame(view, p, V, ch):
    c = Canvas(W, H)
    if view == "down" or view == "down_right":
        PAINTERS[view](c, p, V, ch)
    else:
        PAINTERS[view](c, p, V)
    c.auto_outline()
    return c


def build_sheet(ch, V):
    """128 canvases in engine order: dir 0..7, each idle4 / walk6 / bite4 /
    hurt2 (HF_IDLE=0, HF_WALK_A=4, HF_ATTACK=10, HF_HURT=14)."""
    frames = []
    labels = []
    for d, (view, flip) in enumerate(DIR_VIEWS):
        for anim, count in ANIM_ORDER:
            for f in range(count):
                c = render_frame(view, pose_for(anim, f), V, ch)
                if flip:
                    c = c.mirrored()
                frames.append(c)
                labels.append(f"d{d}.{anim[:2]}{f}")
    return frames, labels


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------

def to_bmp8(c):
    """8bpp row-major frame bytes."""
    return bytes(c.px)


def check_margins(c, label):
    """The contract: 1px of empty margin around the ink so auto_outline
    always has somewhere to grow. The outer ring must hold nothing but
    empty or outline pixels."""
    edge = ix("outline")
    bad = []
    for x in range(W):
        for y in (0, H - 1):
            if c.get(x, y) not in (0, edge):
                bad.append((x, y))
    for y in range(H):
        for x in (0, W - 1):
            if c.get(x, y) not in (0, edge):
                bad.append((x, y))
    if bad:
        raise AssertionError(
            f"{label}: ink on the outer ring at {bad[:4]} -- leave a 1px "
            f"margin for the outline")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "graphics")
    if not os.path.isabs(out):
        out = os.path.join(root, out)
    os.makedirs(out, exist_ok=True)

    gc.emit_palette(os.path.join(out, "palette.bin"))
    gc.emit_palette_header(os.path.join(root, "source", "palette_ids.h"))

    total = DIRS * FRAMES_PER_DIR
    sheets = {}
    for tag, ch, V in HEROES:
        frames, labels = build_sheet(ch, V)
        assert len(frames) == total, f"{tag} has {len(frames)} frames"

        for i, c in enumerate(frames):
            check_margins(c, f"{tag}#{i}")
            assert c.count_filled() > 60, f"{tag} frame {i} is nearly empty"

        blob = b"".join(to_bmp8(c) for c in frames)
        assert len(blob) == total * W * H, "frame size mismatch"
        target = os.path.join(out, f"{tag}_tiles.bin")
        gc.write_bin(target, blob)
        print(f"{tag} ({ch}): {total} frames, {len(blob)} bytes -> {target}")
        sheets[tag] = (frames, labels)

        # right-facing strip, one row per anim
        base = FRAMES_PER_DIR * 3
        gc.preview_scale(frames[base:base + FRAMES_PER_DIR],
                         os.path.join(out, f"preview_{tag}.png"), scale=4,
                         cols=FRAMES_PER_DIR, labels=labels[base:base + 16],
                         title=f"{tag} ({ch}) right: idle/walk/bite/hurt")
        # every direction x every frame
        gc.preview_scale(frames, os.path.join(out, f"preview_{tag}_all.png"),
                         scale=3, cols=FRAMES_PER_DIR, labels=labels,
                         title=f"{tag} ({ch}) all 8 directions x 16 frames")

    # Overview contact sheet: both heroes, every direction, the four frames
    # a reviewer judges first (breath, contact step, lunge, recoil).
    overview, over_labels = [], []
    for tag, ch, _V in HEROES:
        frames, _ = sheets[tag]
        for d, dname in enumerate(("down", "up", "left", "right",
                                   "dn_left", "dn_right", "up_left",
                                   "up_right")):
            overview.append(frames[d * FRAMES_PER_DIR + 1])
            overview.append(frames[d * FRAMES_PER_DIR + 4])
            overview.append(frames[d * FRAMES_PER_DIR + 11])
            overview.append(frames[d * FRAMES_PER_DIR + 14])
            over_labels.extend([f"{ch} {dname} breathe", f"{ch} {dname} step",
                                f"{ch} {dname} lunge", f"{ch} {dname} hurt"])
    gc.preview_scale(overview, os.path.join(out, "preview_heroes.png"),
                     scale=4, cols=8, labels=over_labels,
                     title="Capybara Kingdom heroes: 卡 (P1) & 皮 (P2)")

    # Brand + incisors close-up at 8x for glyph-legibility checks.
    closeup, close_labels = [], []
    for tag, ch, _V in HEROES:
        frames, _ = sheets[tag]
        closeup.append(frames[0])
        close_labels.append(f"{ch} idle")
        closeup.append(frames[11])
        close_labels.append(f"{ch} bite")
    gc.preview_scale(closeup, os.path.join(out, "preview_hero_faces.png"),
                     scale=8, cols=2, labels=close_labels,
                     title="forehead brand + incisors, 8x")

    print("hero asset generation complete")


if __name__ == "__main__":
    main()
