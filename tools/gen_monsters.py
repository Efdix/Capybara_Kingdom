"""
Monsters (32x32) and bosses (64x64) for Capybara Kingdom.

Ecological rework:
  5 Regions x 3 Themed Mobs (Chaser, Dasher, Shooter) = 15 Mobs
  5 Regions x 2 Bosses (Mid-Boss & Final Boss) = 10 Bosses

Output:
  graphics/preview_monsters.png -- full overview showcase
  graphics/mob_<name>.bin       -- 32x32 4-frame sheets
  graphics/boss_<name>.bin      -- 64x64 4-frame sheets
"""

import os
import sys
import math

import gen_common as gc
from gen_common import Canvas, ix, preview_scale

VARIANTS = ["idle", "bob", "attack", "hurt"]
BOSS_VARIANTS = ["idle", "bob", "attack", "hurt",
                 "windup", "special_a", "special_b", "stagger"]
BOSS_SPRITE = 64


# --- boss pose vocabulary --------------------------------------------------
#
# A duel is read off body language, so a boss needs more than a walk cycle. The
# three extra poses below are the ones a fight actually leans on:
#
#   windup     -- coiled back and low: "this is about to hurt"
#   special_a  -- the signature move's own posture (a dig, a rise, a glow)
#   special_b  -- the phase-2 stance: bigger, hotter, unmistakably angrier
#   stagger    -- on its knees: the reward window, drawn so it is unmissable
#
# Each boss maps the four to its own anatomy, because the poses have to read as
# that creature doing them, not as a generic puppet being scaled.
BOSS_POSE = {
    "idle":      (0, 0, 0),
    "bob":       (0, 1, 0),
    "attack":    (0, -2, 0),
    "hurt":      (0, 2, 0),
    "windup":    (-2, 4, 0),
    "special_a": (0, -3, 0),
    "special_b": (0, -1, 0),
    "stagger":   (3, 6, 0),
}


# ===========================================================================
# 15 ECOLOGICAL MOOKS (32x32)
# ===========================================================================

# --- REGION 1: 雨林 (Rainforest) -------------------------------------------

def m_rain_frog(pal, variant="idle"):
    """Poison treefrog: bright green with orange eyes and creamy belly (Shooter)."""
    c = Canvas(32, 32)
    squash = {"idle": 0, "bob": 1, "attack": -2, "hurt": 2}[variant]
    y = 11 + squash
    # back legs / feet
    c.round_rect(4, y + 8, 7, 7, 3, ix("grn_dark"))
    c.round_rect(21, y + 8, 7, 7, 3, ix("grn_dark"))
    # body
    c.blob(16, y + 8, 10, 8, ix("grn_dark"), n=3.5)
    c.blob(16, y + 8, 9, 7, ix("grn_mid"), n=3.5)
    c.round_rect(11, y + 9, 10, 6, 3, ix("cream"))
    # front feet
    c.round_rect(8, y + 13, 4, 3, 1, ix("grn_light"))
    c.round_rect(20, y + 13, 4, 3, 1, ix("grn_light"))
    # prominent treefrog eyes on top
    for ex in (9, 21):
        c.circle(ex, y + 2, 4, ix("grn_dark"))
        c.circle(ex, y + 2, 3, ix("orange_mid"))
        c.circle(ex, y + 2, 1, ix("black"))
        c.set(ex - 1, y + 1, ix("white"))
    # attack tongue / poison shot
    if variant == "attack":
        c.rect(14, y + 7, 4, 2, ix("red_dark"))
        c.circle(16, y + 7, 3, ix("cyan_light"))
        c.set(16, y + 7, ix("white"))
    c.auto_outline()
    return c


def m_rain_wasp(pal, variant="idle"):
    """Jungle wasp: striped abdomen, cyan wings, dasher."""
    c = Canvas(32, 32)
    flap = {"idle": 0, "bob": 2, "attack": 3, "hurt": 0}[variant]
    dy = -2 if variant != "hurt" else 0
    # wings
    c.round_rect(6, 6 + dy - flap, 9, 5, 2, ix("cyan_light"))
    c.round_rect(17, 6 + dy - flap, 9, 5, 2, ix("cyan_light"))
    c.round_rect(8, 8 + dy - flap, 6, 2, 1, ix("cyan_mid"))
    c.round_rect(18, 8 + dy - flap, 6, 2, 1, ix("cyan_mid"))
    # body
    c.round_rect(9, 14 + dy, 14, 13, 5, ix("yellow_dark"))
    c.round_rect(10, 15 + dy, 12, 11, 4, ix("yellow_mid"))
    # stripes
    for sy in (17, 20, 23):
        c.rect(11, sy + dy, 10, 2, ix("black"))
    # head
    c.round_rect(11, 9 + dy, 10, 7, 3, ix("yellow_mid"))
    c.rect(12, 9 + dy, 8, 2, ix("yellow_dark"))
    # eyes
    for ex in (12, 17):
        c.rect(ex, 11 + dy, 3, 3, ix("outline"))
        c.set(ex, 11 + dy, ix("white"))
    # stinger
    c.round_rect(15, 27 + dy, 2, 4, 1, ix("outline"))
    c.auto_outline()
    return c


def m_rain_slime(pal, variant="idle"):
    """Tropical rainforest green slime with fruit berry core (Chaser)."""
    c = Canvas(32, 32)
    squash = {"idle": 0, "bob": 1, "attack": -2, "hurt": 2}[variant]
    top = 9 + squash
    c.round_rect(5, top, 22, 32 - top - 1, 8, ix("leaf_dark"))
    c.round_rect(6, top + 1, 20, 32 - top - 3, 7, ix("leaf_mid"))
    c.round_rect(8, top + 2, 8, 5, 3, ix("leaf_light"))
    # berry inside
    c.circle(16, top + 12, 4, ix("cherry_mid"))
    c.circle(15, top + 11, 2, ix("cherry_light"))
    # eyes
    for ex in (10, 20):
        c.rect(ex, top + 6, 3, 4, ix("outline"))
        c.set(ex, top + 6, ix("white"))
        c.set(ex, top + 7, ix("black"))
    if variant == "attack":
        c.rect(13, top + 12, 6, 2, ix("outline"))
    c.auto_outline()
    return c


# --- REGION 2: 农场 (Farm) -------------------------------------------------

def m_farm_rat(pal, variant="idle"):
    """Field rat / mouse: round brown body, pink ears, whiskers, buckteeth (Chaser)."""
    c = Canvas(32, 32)
    squash = {"idle": 0, "bob": 1, "attack": -1, "hurt": 2}[variant]
    y = 10 + squash
    # long tail curling behind
    tail_pts = [(5, y + 12), (3, y + 10), (3, y + 7), (5, y + 5), (8, y + 4)]
    for tx, ty in tail_pts:
        c.set(tx, ty, ix("pink_mid"))
    # round body
    c.blob(17, y + 9, 10, 8, ix("brn_dark"), n=3.8)
    c.blob(17, y + 9, 9, 7, ix("brn_mid"), n=3.8)
    c.round_rect(13, y + 11, 7, 5, 2, ix("tan_belly"))
    # ears
    for ex, dx in ((11, -1), (21, 1)):
        c.circle(ex, y + 2, 4, ix("brn_dark"))
        c.circle(ex, y + 2, 2, ix("pink_light"))
    # eyes
    for ex in (13, 20):
        c.rect(ex, y + 6, 2, 3, ix("black"))
        c.set(ex, y + 6, ix("white"))
    # nose & buckteeth
    c.set(17, y + 9, ix("pink_dark"))
    c.rect(16, y + 11, 3, 2, ix("white"))
    c.rect(15, y + 13, 5, 1, ix("outline"))
    # tiny feet
    c.rect(11, y + 16, 3, 2, ix("pink_mid"))
    c.rect(20, y + 16, 3, 2, ix("pink_mid"))
    c.auto_outline()
    return c


def m_farm_crow(pal, variant="idle"):
    """Farm crow: blue-black feathers, sharp yellow beak, dasher."""
    c = Canvas(32, 32)
    flap = {"idle": 0, "bob": 2, "attack": 3, "hurt": -1}[variant]
    y = 8
    # wings out
    c.round_rect(3, y + 4 - flap, 8, 7, 2, ix("fur_black"))
    c.round_rect(4, y + 5 - flap, 6, 5, 2, ix("steel_dark"))
    c.round_rect(21, y + 4 - flap, 8, 7, 2, ix("fur_black"))
    c.round_rect(22, y + 5 - flap, 6, 5, 2, ix("steel_dark"))
    # body
    c.round_rect(10, y + 6, 12, 14, 5, ix("fur_black"))
    c.round_rect(11, y + 7, 10, 12, 4, ix("steel_dark"))
    # head
    c.circle(16, y + 4, 5, ix("fur_black"))
    c.circle(16, y + 4, 4, ix("steel_dark"))
    # eyes
    c.set(14, y + 3, ix("red_light"))
    c.set(18, y + 3, ix("red_light"))
    # yellow sharp beak
    c.round_rect(14, y + 6, 4, 5, 1, ix("yellow_mid"))
    c.set(15, y + 10, ix("yellow_dark"))
    # claws
    c.rect(12, y + 20, 2, 3, ix("yellow_dark"))
    c.rect(18, y + 20, 2, 3, ix("yellow_dark"))
    c.auto_outline()
    return c


def m_farm_mudlet(pal, variant="idle"):
    """Farm ditch mudlet: earthy mud clump with wheat sprout, throws dirt (Shooter)."""
    c = Canvas(32, 32)
    squash = {"idle": 0, "bob": 1, "attack": -2, "hurt": 2}[variant]
    y = 11 + squash
    # wheat stalk sprout on head
    c.set(16, y - 4, ix("wheat_light"))
    c.set(15, y - 3, ix("wheat_mid"))
    c.set(17, y - 3, ix("wheat_mid"))
    c.rect(16, y - 2, 1, 3, ix("wheat_dark"))
    # muddy lump body
    c.blob(16, y + 8, 11, 8, ix("clay_dark"), n=3.0)
    c.blob(16, y + 8, 9, 6, ix("clay_mid"), n=3.0)
    # mud ripples
    c.round_rect(7, y + 13, 18, 5, 2, ix("clay_dark"))
    # beady eyes
    for ex in (12, 19):
        c.rect(ex, y + 6, 3, 3, ix("outline"))
        c.set(ex, y + 6, ix("yellow_light"))
    if variant == "attack":
        # spitting mud clod
        c.circle(16, y + 11, 3, ix("brn_dark"))
    c.auto_outline()
    return c


# --- REGION 3: 牧场 (Pasture) ----------------------------------------------

def m_past_dog(pal, variant="idle"):
    """Pasture stray dog / pup: grey coat, floppy ears, chasing (Chaser)."""
    c = Canvas(32, 32)
    squash = {"idle": 0, "bob": 1, "attack": -1, "hurt": 2}[variant]
    y = 9 + squash
    # tail wagging
    c.round_rect(4, y + 6, 4, 4, 1, ix("grey_mid"))
    c.set(3, y + 5, ix("grey_light"))
    # body
    c.round_rect(8, y + 9, 16, 11, 4, ix("grey_dark"))
    c.round_rect(9, y + 10, 14, 9, 3, ix("grey_mid"))
    c.round_rect(12, y + 11, 8, 7, 2, ix("cream"))
    # head
    c.circle(18, y + 5, 6, ix("grey_dark"))
    c.circle(18, y + 5, 5, ix("grey_mid"))
    # floppy ears
    c.round_rect(13, y + 2, 3, 5, 1, ix("grey_dark"))
    c.round_rect(22, y + 2, 3, 5, 1, ix("grey_dark"))
    # eyes & snout
    c.set(16, y + 4, ix("black"))
    c.set(20, y + 4, ix("black"))
    c.round_rect(17, y + 7, 4, 3, 1, ix("cream"))
    c.set(18, y + 7, ix("black"))
    # legs
    c.rect(10, y + 20, 3, 3, ix("grey_dark"))
    c.rect(19, y + 20, 3, 3, ix("grey_dark"))
    c.auto_outline()
    return c


def m_past_ram(pal, variant="idle"):
    """Aggressive black-faced horned sheep / ram (Dasher)."""
    c = Canvas(32, 32)
    sway = {"idle": 0, "bob": 1, "attack": -2, "hurt": 1}[variant]
    y = 8 + sway
    # big curved ram horns
    c.circle(10, y + 4, 5, ix("brn_dark"))
    c.circle(10, y + 4, 3, ix("brn_mid"))
    c.circle(22, y + 4, 5, ix("brn_dark"))
    c.circle(22, y + 4, 3, ix("brn_mid"))
    # fluffy wool body
    c.blob(16, y + 11, 11, 8, ix("cream_mid"), n=4.0)
    c.blob(16, y + 11, 9, 6, ix("white"), n=4.0)
    # black face in center
    c.round_rect(13, y + 6, 7, 8, 3, ix("fur_black"))
    c.round_rect(14, y + 7, 5, 6, 2, ix("grey_dark"))
    # angry eyes
    c.set(14, y + 8, ix("yellow_light"))
    c.set(17, y + 8, ix("yellow_light"))
    # hooves
    c.rect(11, y + 19, 3, 3, ix("fur_black"))
    c.rect(18, y + 19, 3, 3, ix("fur_black"))
    c.auto_outline()
    return c


def m_past_hornet(pal, variant="idle"):
    """Pasture horsefly / hornet: blue-dark body, buzzing wings, shooter."""
    c = Canvas(32, 32)
    flap = {"idle": 0, "bob": 2, "attack": 3, "hurt": -1}[variant]
    y = 8
    # gauzy wings
    c.round_rect(5, y + 2 - flap, 8, 4, 2, ix("cyan_light"))
    c.round_rect(19, y + 2 - flap, 8, 4, 2, ix("cyan_light"))
    # body
    c.round_rect(10, y + 7, 12, 13, 5, ix("steel_dark"))
    c.round_rect(11, y + 8, 10, 11, 4, ix("steel_mid"))
    c.rect(12, y + 11, 8, 2, ix("steel_hi"))
    # big compound eyes
    c.circle(12, y + 6, 3, ix("red_dark"))
    c.set(12, y + 5, ix("red_light"))
    c.circle(20, y + 6, 3, ix("red_dark"))
    c.set(20, y + 5, ix("red_light"))
    # stinger
    c.rect(15, y + 20, 2, 4, ix("outline"))
    if variant == "attack":
        c.circle(16, y + 23, 2, ix("yellow_light"))
    c.auto_outline()
    return c


# --- REGION 4: 沼泽 (Marsh / Swamp) ----------------------------------------

def m_marsh_slime(pal, variant="idle"):
    """Dark peat-bog muck / mud slime with oily sheen (Chaser)."""
    c = Canvas(32, 32)
    squash = {"idle": 0, "bob": 1, "attack": -2, "hurt": 2}[variant]
    top = 9 + squash
    c.round_rect(5, top, 22, 32 - top - 1, 8, ix("clay_dark"))
    c.round_rect(6, top + 1, 20, 32 - top - 3, 7, ix("trunk_dark"))
    # oily sheen highlights
    c.round_rect(8, top + 2, 7, 4, 2, ix("purple_mid"))
    c.set(17, top + 3, ix("cyan_mid"))
    # sinister yellow swamp eyes
    for ex in (10, 19):
        c.rect(ex, top + 7, 3, 3, ix("outline"))
        c.set(ex, top + 7, ix("yellow_mid"))
    if variant == "attack":
        c.rect(13, top + 13, 6, 2, ix("purple_dark"))
    c.auto_outline()
    return c


def m_marsh_viper(pal, variant="idle"):
    """Marsh viper / swamp snake: coiled green-black body, forked tongue (Dasher)."""
    c = Canvas(32, 32)
    squash = {"idle": 0, "bob": 1, "attack": -2, "hurt": 1}[variant]
    y = 8 + squash
    # coiled body loops
    c.round_rect(6, y + 10, 20, 12, 6, ix("pine_dark"))
    c.round_rect(8, y + 11, 16, 10, 5, ix("pine_mid"))
    c.circle(16, y + 16, 4, ix("pine_dark"))
    # head reared up
    c.round_rect(12, y + 4, 9, 8, 4, ix("pine_mid"))
    c.set(13, y + 3, ix("pine_light"))
    # slit eyes
    c.set(14, y + 6, ix("yellow_light"))
    c.set(18, y + 6, ix("yellow_light"))
    # forked red tongue
    if variant == "attack":
        c.set(16, y + 11, ix("red_light"))
        c.set(15, y + 12, ix("red_mid"))
        c.set(17, y + 12, ix("red_mid"))
    else:
        c.set(16, y + 10, ix("red_mid"))
    c.auto_outline()
    return c


def m_marsh_toad(pal, variant="idle"):
    """Toxic swamp bullfrog: warty dark-olive toad, spits venom bubbles (Shooter)."""
    c = Canvas(32, 32)
    squash = {"idle": 0, "bob": 1, "attack": -2, "hurt": 2}[variant]
    y = 10 + squash
    # squat wide body
    c.blob(16, y + 8, 11, 8, ix("cact_dark"), n=3.5)
    c.blob(16, y + 8, 9, 6, ix("cact_mid"), n=3.5)
    # yellow-green warts
    for wx, wy in ((10, y + 5), (21, y + 6), (14, y + 12), (18, y + 11)):
        c.set(wx, wy, ix("yellow_mid"))
    # bulging eyes
    for ex in (10, 21):
        c.circle(ex, y + 2, 4, ix("cact_dark"))
        c.circle(ex, y + 2, 2, ix("yellow_light"))
        c.set(ex, y + 2, ix("black"))
    # bloated throat sac
    c.round_rect(13, y + 9, 7, 5, 2, ix("tan_belly"))
    if variant == "attack":
        # poison bubble
        c.circle(16, y + 10, 4, ix("purple_light"))
        c.circle(16, y + 10, 2, ix("purple_mid"))
    c.auto_outline()
    return c


# --- REGION 5: 可可火山 (Cacao Volcano) ------------------------------------

def m_volc_imp(pal, variant="idle"):
    """Caramel ember imp: fiery orange imp with twin flame horns (Chaser)."""
    c = Canvas(32, 32)
    flick = {"idle": 0, "bob": 1, "attack": 2, "hurt": 0}[variant]
    # flame horns
    for fx in (11, 21):
        c.round_rect(fx - 2, 5 - flick, 4, 7, 2, ix("lava_mid"))
        c.set(fx, 4 - flick, ix("ember_yellow"))
    # body
    c.round_rect(8, 12, 16, 17, 6, ix("lava_dark"))
    c.round_rect(9, 13, 14, 15, 5, ix("lava_mid"))
    c.round_rect(11, 21, 10, 6, 3, ix("ember_orange"))
    # glowing eyes
    for ex in (11, 18):
        c.rect(ex, 16, 3, 3, ix("outline"))
        c.set(ex, 16, ix("ember_yellow"))
    if variant == "attack":
        c.circle(16, 22, 3, ix("white_spark"))
    c.auto_outline()
    return c


def m_volc_bat(pal, variant="idle"):
    """Dark chocolate volcanic cave bat with glowing eyes (Dasher)."""
    c = Canvas(32, 32)
    flap = {"idle": 0, "bob": 2, "attack": 4, "hurt": -2}[variant]
    y = 8
    # wide scalloped bat wings
    c.round_rect(3, y + 3 - flap, 10, 8, 3, ix("choco_dark"))
    c.round_rect(19, y + 3 - flap, 10, 8, 3, ix("choco_dark"))
    c.round_rect(5, y + 5 - flap, 7, 5, 2, ix("choco_mid"))
    c.round_rect(20, y + 5 - flap, 7, 5, 2, ix("choco_mid"))
    # bat body
    c.round_rect(11, y + 7, 10, 12, 4, ix("choco_dark"))
    c.round_rect(12, y + 8, 8, 10, 3, ix("choco_mid"))
    # pointed ears
    c.set(13, y + 5, ix("choco_light"))
    c.set(18, y + 5, ix("choco_light"))
    # glowing red eyes & fangs
    c.set(14, y + 9, ix("red_light"))
    c.set(17, y + 9, ix("red_light"))
    c.set(15, y + 12, ix("white"))
    c.set(16, y + 12, ix("white"))
    c.auto_outline()
    return c


def m_volc_orb(pal, variant="idle"):
    """Floating magma lava orb spitting fireballs (Shooter)."""
    c = Canvas(32, 32)
    bob = {"idle": 0, "bob": 2, "attack": 3, "hurt": 0}[variant]
    cy = 15 + bob
    c.circle(16, cy, 12, ix("lava_dark"))
    c.circle(16, cy, 10, ix("lava_mid"))
    c.circle(16, cy, 6, ix("lava_light"))
    c.circle(14, cy - 2, 3, ix("ember_yellow"))
    # flame wisps
    for a in range(0, 360, 45):
        rad = math.radians(a)
        wx = int(16 + math.cos(rad) * 12)
        wy = int(cy + math.sin(rad) * 12)
        c.set(wx, wy, ix("ember_orange"))
    # evil eyes
    for ex in (11, 18):
        c.rect(ex, cy - 2, 3, 3, ix("outline"))
        c.set(ex, cy - 2, ix("white_spark"))
    c.auto_outline()
    return c


# ===========================================================================
# 10 DUAL-PHASE BOSSES (64x64 Canvas)
# ===========================================================================

def to_boss_canvas(art, aura=None):
    """Embed creature inside 64x64 canvas with shadow and aura."""
    out = Canvas(BOSS_SPRITE, BOSS_SPRITE)
    ox = (BOSS_SPRITE - art.w) // 2
    oy = (BOSS_SPRITE - art.h) // 2

    if aura:
        cx, cy = BOSS_SPRITE / 2 - 0.5, BOSS_SPRITE / 2 - 0.5
        out.ellipse(cx, cy, 30, 26, ix(aura[0]))
        out.ellipse(cx, cy, 27, 23, ix(aura[1]))

    # ground shadow
    bottom = oy + art.h
    for i, r in enumerate((22, 17, 12)):
        cx = BOSS_SPRITE / 2 - 0.5
        col = ix("shadow") if i else ix("outline_soft")
        out.ellipse(cx, bottom - 2, r, 3.0, col)

    out.paste(art, ox, oy)
    return out


# --- REGION 1 BOSSES (雨林) ------------------------------------------------

def boss_off(mapping, variant):
    """Boss pose offset, with the duel poses derived from the original four.

    Every boss function below was written with four poses and its own anatomy
    driving them. Rather than duplicate that anatomy eight times, the four new
    duel poses fall back onto the originals and add their own tilt -- so the
    creature's proportions stay its own while the new postures still read as
    wind-up / signature / enraged / broken.

      windup     coil lower and further back than a swing
      special_a  the signature move lifts the body
      special_b  the phase-2 stance: taller and prouder
      stagger    dropped and slumped forward
    """
    base = mapping.get(variant)
    if base is not None:
        return base
    if variant == "windup":
        return mapping["attack"] + 2
    if variant == "special_a":
        return mapping["bob"] - 3
    if variant == "special_b":
        return mapping["bob"] - 4
    if variant == "stagger":
        return mapping["hurt"] + 4
    return 0


def b_rain_ape(pal, variant="idle"):
    """Mid-Boss: 雨林巨猿 (Jungle Silverback Ape). Powerful broad-chested gorilla."""
    c = Canvas(48, 48)
    heave = boss_off({"idle": 0, "bob": 1, "attack": -2, "hurt": 2}, variant)
    y = 6 + heave
    # massive arms & fists
    c.round_rect(4, y + 14, 10, 22, 4, ix("brn_dark"))
    c.circle(9, y + 34, 6, ix("brn_mid"))
    c.round_rect(34, y + 14, 10, 22, 4, ix("brn_dark"))
    c.circle(39, y + 34, 6, ix("brn_mid"))
    # torso
    c.blob(24, y + 20, 16, 14, ix("brn_dark"), n=3.5)
    c.blob(24, y + 20, 14, 12, ix("brn_mid"), n=3.5)
    # silver/grey chest plate
    c.round_rect(17, y + 14, 14, 13, 4, ix("grey_mid"))
    c.round_rect(19, y + 16, 10, 9, 3, ix("grey_light"))
    # massive head & brow ridge
    c.blob(24, y + 9, 10, 8, ix("brn_dark"), n=3.0)
    c.round_rect(18, y + 6, 12, 4, 2, ix("brn_light"))
    # glowing angry eyes
    c.set(20, y + 9, ix("red_light"))
    c.set(27, y + 9, ix("red_light"))
    # fangs
    c.set(21, y + 14, ix("white"))
    c.set(26, y + 14, ix("white"))
    # legs
    c.round_rect(16, y + 32, 6, 8, 2, ix("brn_dark"))
    c.round_rect(26, y + 32, 6, 8, 2, ix("brn_dark"))
    c.auto_outline()
    return c


def b_rain_treant(pal, variant="idle"):
    """Final Boss: 树精长老 (Treant Elder). Towering oak spirit with leafy crown."""
    c = Canvas(48, 48)
    sway = boss_off({"idle": 0, "bob": 1, "attack": 2, "hurt": -1}, variant)
    # sprawling leafy crown
    c.circle(24 + sway, 12, 17, ix("leaf_dark"))
    c.circle(24 + sway, 12, 15, ix("leaf_mid"))
    c.circle(18 + sway, 9, 7, ix("leaf_light"))
    c.circle(30 + sway, 10, 6, ix("leaf_light"))
    # gnarled trunk body
    c.round_rect(16 + sway, 20, 16, 20, 5, ix("trunk_dark"))
    c.round_rect(18 + sway, 21, 12, 18, 4, ix("trunk_mid"))
    # bark ridges
    for by in (25, 30, 35):
        c.hline(19 + sway, 28 + sway, by, ix("trunk_dark"))
    # glowing amber hollow eyes
    for ex in (19, 27):
        c.circle(ex + sway, 26, 3, ix("outline"))
        c.set(ex + sway, 26, ix("yellow_light"))
    # root legs
    for rx in (14, 22, 30):
        c.round_rect(rx + sway, 38, 5, 6, 2, ix("trunk_dark"))
    c.auto_outline()
    return c


# --- REGION 2 BOSSES (农场) ------------------------------------------------

def b_farm_boar(pal, variant="idle"):
    """Mid-Boss: 狂暴野猪 (Raging Boar). Bristled boar with giant ivory tusks."""
    c = Canvas(48, 48)
    charge = boss_off({"idle": 0, "bob": 1, "attack": -3, "hurt": 2}, variant)
    y = 8 + charge
    # hefty bristled body
    c.blob(24, y + 18, 18, 14, ix("brn_shadow"), n=3.5)
    c.blob(24, y + 18, 16, 12, ix("brn_deep"), n=3.5)
    # spiky dorsal ridge
    for sx in range(12, 36, 4):
        c.rect(sx, y + 5, 2, 4, ix("brn_shadow"))
    # big snout
    c.round_rect(10, y + 16, 10, 8, 3, ix("pink_dark"))
    c.set(13, y + 18, ix("outline"))
    c.set(16, y + 18, ix("outline"))
    # GIANT IVORY TUSKS
    c.round_rect(7, y + 12, 4, 9, 2, ix("white"))
    c.set(8, y + 11, ix("white_spark"))
    c.round_rect(17, y + 12, 4, 9, 2, ix("white"))
    c.set(18, y + 11, ix("white_spark"))
    # raging bloodshot eyes
    c.set(18, y + 13, ix("red_mid"))
    c.set(27, y + 13, ix("red_mid"))
    # heavy hooves
    for hx in (14, 20, 28, 34):
        c.rect(hx, y + 30, 4, 6, ix("fur_black"))
    c.auto_outline()
    return c


def b_farm_ratking(pal, variant="idle"):
    """Final Boss: 硕鼠大王 (Rat King). Colossal fat rat king with golden crown."""
    c = Canvas(48, 48)
    squash = boss_off({"idle": 0, "bob": 1, "attack": -2, "hurt": 2}, variant)
    y = 6 + squash
    # curled thick tail
    for tx, ty in ((6, y + 32), (4, y + 26), (4, y + 20), (6, y + 16), (9, y + 14)):
        c.circle(tx, ty, 3, ix("pink_mid"))
    # huge round pear-shaped body
    c.blob(25, y + 24, 18, 16, ix("brn_mid"), n=3.8)
    c.blob(25, y + 24, 16, 14, ix("tan_mid"), n=3.8)
    # fat belly
    c.blob(25, y + 26, 12, 10, ix("tan_belly"), n=3.5)
    # rat ears
    c.circle(17, y + 7, 5, ix("brn_mid"))
    c.circle(17, y + 7, 3, ix("pink_light"))
    c.circle(33, y + 7, 5, ix("brn_mid"))
    c.circle(33, y + 7, 3, ix("pink_light"))
    # GOLDEN WHEAT CROWN
    c.rect(21, y + 2, 9, 3, ix("ui_gold"))
    c.set(22, y, ix("ui_gold_light"))
    c.set(25, y - 1, ix("ui_gold_light"))
    c.set(28, y, ix("ui_gold_light"))
    # devious face
    c.set(21, y + 12, ix("black"))
    c.set(28, y + 12, ix("black"))
    c.set(25, y + 15, ix("pink_dark"))
    # big sharp buckteeth
    c.rect(23, y + 17, 2, 4, ix("white"))
    c.rect(26, y + 17, 2, 4, ix("white"))
    # stubby claw paws holding wheat stalk
    c.round_rect(12, y + 22, 6, 5, 2, ix("pink_mid"))
    c.round_rect(32, y + 22, 6, 5, 2, ix("pink_mid"))
    c.rect(34, y + 14, 2, 16, ix("wheat_dark"))
    c.circle(35, y + 13, 3, ix("wheat_light"))
    c.auto_outline()
    return c


# --- REGION 3 BOSSES (牧场) ------------------------------------------------

def b_past_wolf(pal, variant="idle"):
    """Mid-Boss: 夜狼领袖 (Night Wolf Alpha). Sleek silver-grey wolf with cyan eyes."""
    c = Canvas(48, 48)
    stalk = boss_off({"idle": 0, "bob": 1, "attack": -2, "hurt": 2}, variant)
    y = 8 + stalk
    # bushy tail
    for tx, ty in ((36, y + 16), (40, y + 12), (42, y + 7)):
        c.circle(tx, ty, 4, ix("grey_mid"))
    # lean muscular body
    c.round_rect(12, y + 14, 26, 14, 5, ix("grey_dark"))
    c.round_rect(14, y + 15, 22, 12, 4, ix("grey_mid"))
    c.round_rect(16, y + 16, 12, 8, 3, ix("cream"))
    # strong legs
    for lx in (15, 22, 28, 34):
        c.round_rect(lx, y + 26, 4, 10, 1, ix("grey_dark"))
        c.rect(lx, y + 34, 4, 2, ix("grey_light"))
    # wolf head
    c.round_rect(8, y + 7, 14, 11, 4, ix("grey_mid"))
    c.round_rect(6, y + 11, 7, 5, 2, ix("grey_light"))
    c.set(6, y + 12, ix("black"))
    # pointed alert ears
    c.round_rect(10, y + 3, 4, 6, 1, ix("grey_dark"))
    c.round_rect(17, y + 3, 4, 6, 1, ix("grey_dark"))
    # glowing cyan feral eyes
    c.set(11, y + 9, ix("cyan_light"))
    c.set(15, y + 9, ix("cyan_light"))
    c.auto_outline()
    return c


def b_past_bull(pal, variant="idle"):
    """Final Boss: 狂暴黑牛 (Raging Bull). Massive muscular black bull with golden nose ring."""
    c = Canvas(48, 48)
    pant = boss_off({"idle": 0, "bob": 1, "attack": -3, "hurt": 2}, variant)
    y = 6 + pant
    # SHARP WIDE CURVED HORNS
    c.round_rect(8, y + 2, 7, 5, 2, ix("white"))
    c.set(7, y + 1, ix("white_spark"))
    c.round_rect(33, y + 2, 7, 5, 2, ix("white"))
    c.set(40, y + 1, ix("white_spark"))
    # massive hulking black body
    c.blob(24, y + 21, 18, 16, ix("fur_black"), n=3.5)
    c.blob(24, y + 21, 16, 14, ix("steel_dark"), n=3.5)
    # broad bull head
    c.round_rect(16, y + 6, 16, 14, 4, ix("fur_black"))
    c.round_rect(17, y + 7, 14, 12, 3, ix("steel_dark"))
    # red furious eyes
    c.set(19, y + 10, ix("red_light"))
    c.set(28, y + 10, ix("red_light"))
    # snout & GOLDEN NOSE RING
    c.round_rect(18, y + 14, 12, 8, 3, ix("grey_mid"))
    c.circle(24, y + 20, 3, ix("ui_gold"))
    c.circle(24, y + 20, 1, ix("grey_mid"))
    # sturdy hooves
    for hx in (14, 21, 27, 33):
        c.rect(hx, y + 33, 4, 6, ix("fur_black"))
    c.auto_outline()
    return c


# --- REGION 4 BOSSES (沼泽) ------------------------------------------------

def b_marsh_toadlord(pal, variant="idle"):
    """Mid-Boss: 泥沼巨蟾 (Swamp Toad Lord). Bloated purple-warted toxic toad."""
    c = Canvas(48, 48)
    croak = boss_off({"idle": 0, "bob": 1, "attack": -2, "hurt": 2}, variant)
    y = 7 + croak
    # gargantuan squat body
    c.blob(24, y + 21, 19, 14, ix("cact_dark"), n=3.5)
    c.blob(24, y + 21, 17, 12, ix("cact_mid"), n=3.5)
    # toxic purple warts
    for wx, wy in ((14, y + 14), (32, y + 15), (20, y + 18), (28, y + 19)):
        c.circle(wx, wy, 3, ix("purple_dark"))
        c.set(wx, wy, ix("purple_light"))
    # giant bloated throat sac
    c.round_rect(16, y + 23, 16, 10, 4, ix("tan_belly"))
    # giant bulbous eyes
    for ex in (15, 32):
        c.circle(ex, y + 8, 6, ix("cact_dark"))
        c.circle(ex, y + 8, 4, ix("yellow_mid"))
        c.set(ex, y + 8, ix("black"))
    # lotus leaf crown
    c.round_rect(20, y + 2, 8, 3, 1, ix("leaf_light"))
    # webbed splayed feet
    c.round_rect(6, y + 28, 8, 5, 2, ix("cact_dark"))
    c.round_rect(34, y + 28, 8, 5, 2, ix("cact_dark"))
    c.auto_outline()
    return c


def b_marsh_caiman(pal, variant="idle"):
    """Final Boss: 凯门鳄王 (Caiman King). Giant armored caiman, capybara's real-world nemesis."""
    c = Canvas(48, 48)
    lunge = boss_off({"idle": 0, "bob": 1, "attack": -3, "hurt": 2}, variant)
    y = 8 + lunge
    # long serrated crocodile tail
    for tx, ty in ((38, y + 14), (42, y + 18), (44, y + 24)):
        c.circle(tx, ty, 4, ix("pine_dark"))
    # armored scutes along the spine
    for sx in range(16, 36, 4):
        c.set(sx, y + 9, ix("leaf_light"))
        c.set(sx + 1, y + 10, ix("pine_dark"))
    # heavy armored body
    c.blob(26, y + 19, 16, 12, ix("pine_dark"), n=3.2)
    c.blob(26, y + 19, 14, 10, ix("pine_mid"), n=3.2)
    # LONG JAGGED CROCODILE SNOUT
    c.round_rect(4, y + 15, 18, 10, 3, ix("pine_dark"))
    c.round_rect(6, y + 16, 14, 7, 2, ix("pine_mid"))
    # sharp white teeth in jaw
    for tooth_x in (7, 10, 13, 16):
        c.set(tooth_x, y + 19, ix("white"))
        c.set(tooth_x + 1, y + 22, ix("white"))
    # predatory yellow reptile slit eyes
    c.circle(20, y + 13, 3, ix("yellow_mid"))
    c.set(20, y + 13, ix("black"))
    # splayed webbed claw legs
    for lx in (12, 18, 28, 34):
        c.round_rect(lx, y + 27, 5, 6, 2, ix("pine_dark"))
    c.auto_outline()
    return c


# --- REGION 5 BOSSES (可可火山) --------------------------------------------

def b_volc_golem(pal, variant="idle"):
    """Mid-Boss: 焦糖熔岩巨兽 (Caramel Lava Golem). Basalt colossus with molten veins."""
    c = Canvas(48, 48)
    heave = boss_off({"idle": 0, "bob": 1, "attack": -2, "hurt": 2}, variant)
    y = 6 + heave
    # massive obsidian fists
    c.circle(8, y + 28, 7, ix("ash_dark"))
    c.circle(8, y + 28, 5, ix("lava_mid"))
    c.circle(40, y + 28, 7, ix("ash_dark"))
    c.circle(40, y + 28, 5, ix("lava_mid"))
    # hulking torso
    c.blob(24, y + 19, 16, 14, ix("ash_dark"), n=3.0)
    c.blob(24, y + 19, 14, 12, ix("ash_mid"), n=3.0)
    # glowing lava fissures
    c.hline(16, 26, y + 16, ix("ember_orange"))
    c.hline(20, 30, y + 22, ix("ember_yellow"))
    # head
    c.round_rect(19, y + 6, 10, 8, 3, ix("ash_dark"))
    # blazing eyes
    c.set(21, y + 9, ix("white_spark"))
    c.set(26, y + 9, ix("white_spark"))
    # pillars for legs
    c.round_rect(15, y + 30, 7, 10, 2, ix("ash_dark"))
    c.round_rect(26, y + 30, 7, 10, 2, ix("ash_dark"))
    c.auto_outline()
    return c


def b_volc_cakelord(pal, variant="idle"):
    """Final Boss: 蛋糕魔王 (Cake Lord). 3-tier dark chocolate crown demon king."""
    c = Canvas(48, 48)
    bob = boss_off({"idle": 0, "bob": 2, "attack": -2, "hurt": 3}, variant)
    y = 6 + bob
    # bottom tier: dark chocolate
    c.round_rect(6, y + 26, 36, 16, 4, ix("choco_dark"))
    c.round_rect(7, y + 27, 34, 13, 3, ix("choco_mid"))
    c.rect(9, y + 28, 30, 2, ix("choco_light"))
    # frosting drips
    for dx in range(8, 42, 5):
        c.round_rect(dx, y + 24, 4, 4, 2, ix("cream_mid"))
    # middle tier: sponge
    c.round_rect(11, y + 15, 26, 12, 4, ix("sponge_dark"))
    c.round_rect(12, y + 16, 24, 9, 3, ix("sponge_mid"))
    for dx in range(13, 37, 5):
        c.round_rect(dx, y + 13, 4, 4, 2, ix("cream_mid"))
    # top tier: cherry-topped crown
    c.round_rect(16, y + 5, 16, 10, 3, ix("cherry_dark"))
    c.round_rect(17, y + 6, 14, 7, 2, ix("cherry_mid"))
    # cherry bomb on top
    c.circle(24, y + 2, 4, ix("cherry_dark"))
    c.circle(24, y + 2, 3, ix("cherry_light"))
    c.set(24, y - 2, ix("trunk_mid"))
    # wicked evil grin on bottom tier
    c.round_rect(16, y + 32, 16, 5, 2, ix("outline"))
    for wx in range(18, 31, 3):
        c.set(wx, y + 33, ix("white"))
        c.set(wx + 1, y + 34, ix("white"))
    # sinister yellow eyes on middle tier
    for ex in (17, 28):
        c.rect(ex, y + 18, 3, 3, ix("outline"))
        c.set(ex, y + 18, ix("yellow_light"))
    c.auto_outline()
    return c


# ===========================================================================
# REGISTRY & BUILD
# ===========================================================================

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
        previews.append(frames[0])  # idle frame for showcase
        labels.append(f"{name}")

    mob_bin = pack(all_mob_frames)
    mob_path = os.path.join(graphics_dir, "mob_tiles.bin")
    with open(mob_path, "wb") as f:
        f.write(mob_bin)
    print(f"Generated mob tiles: {mob_path} ({len(all_mob_frames)} frames, {len(mob_bin)} bytes)")

    all_boss_frames = []
    print("--- Generating 10 Dual-Phase Bosses (64x64, 8 poses) ---")
    boss_previews = []
    boss_labels = []
    for name, (fn, aura) in BOSSES.items():
        # Eight poses per boss: the four original walk poses plus the four the
        # duel reads off body language (wind-up / signature / enraged / broken).
        frames = [to_boss_canvas(fn(None, v), aura) for v in BOSS_VARIANTS]
        all_boss_frames.extend(frames)
        boss_previews.append(frames[0])
        boss_labels.append(f"{name}")

    boss_bin = pack(all_boss_frames)
    boss_path = os.path.join(graphics_dir, "boss_tiles.bin")
    with open(boss_path, "wb") as f:
        f.write(boss_bin)
    print(f"Generated boss tiles: {boss_path} ({len(all_boss_frames)} frames, {len(boss_bin)} bytes)")

    # Generate an all-in-one preview of all mobs and bosses
    out_png = os.path.join(graphics_dir, "preview_monsters.png")
    
    # Scale and stitch previews into an aesthetic catalog
    preview_scale(previews + boss_previews, out_png, scale=3, cols=5,
                  labels=labels + boss_labels, title="15 Mobs and 10 Bosses")
    print(f"Generated monster catalog: {out_png}")


if __name__ == "__main__":
    main()

