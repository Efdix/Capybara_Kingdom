"""
Skill card art: the twenty cards of the level-up picker, 72x96 each.

Modern indie aesthetic with distinct Capybara identity:
  0 尖牙: Massive rodent chisel incisors chomping a green reed with crunch sparks!
  1 铁尾: Capybara round rump with armored steel tail slam and ground shockwaves!
  2 冲撞: Chubby capybara loaf sprinting forward with speed lines and dust puffs!
  3 水柱: Erupting onsen water geyser with a floating yuzu orange and crystal spray!
  4 泥沼: Soothing warm mud spa tub with bubbles, steam, and relaxed capybara snout!
  5 粪球: Highly polished herbal fiber pellet rolling with green herbal aura sparkles!
  6 安抚: Golden zen halo aura with serene capybara head and floating lotus petals!
  7 感化: Chubby capybara with a cute little yellow duckling on its head! Friendship!
  8 厚皮: Sturdy golden-iron buckler shield with capybara paw emblem and rivets!
  9 快腿: Golden webbed agility paw print with swirling wind dash ribbons!
 10 磁吸: Ruby-cobalt horseshoe magnet drawing in golden coins and yuzu orange!
 11 利爪: Capybara furry paw raking with 3 huge glowing razor claw slashes!

The fused eight (ids 12..19, game.h CARD_FUSE_*). They are bought with two
cards rather than one, so they wear the *same* frame geometry in a gold plate:
brass rim, gold name band, a twin-ring badge in the band, and inside the art
window the shared vignette in gold (`fuse_halo`) instead of the base cards'
tan one. A player who has never read a description can see that a fused card is
a promotion of the twelve rather than a thirteenth kind of thing:
 12 掠食突袭: Diving capybara biting clean through the enemy in its path!
 13 荆棘铁壁: Thorned steel ring, tail braced, incoming hit flashing off it!
 14 潮汐双炮: Three piercing water bolts volleyed past orbiting crystal orbs!
 15 恶臭香氛: Purple reek cloud over a poison pool, enemy's will draining away!
 16 百兽盟主: Crowned capybara flanked by two buffed little retainers!
 17 疾风吸取: Wind vortex dragging coins into a gold sprinting paw print!
 18 沸泉地雷: Buried mine erupting into a scalding geyser burst!
 19 回旋冲角: Whirling horned capybara sweeping the ring clear!

Outputs (both into graphics/ unless an output dir is given on the command line):
  card_atlas.bin     20 cards x 72x96, 8bpp row-major, no tiling, one byte per
                     pixel, in card-index order 0..19 -> 138240 bytes exactly
  preview_cards.png  contact sheet at 3x, labelled with the card names
"""

import math
import os
import random
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_common as gc
from gen_common import Canvas, ix


CARD_W, CARD_H = 72, 96
CARD_COUNT = 20                 # game.h CARD_COUNT; the atlas is indexed by id
CARD_BASE_COUNT = 12            # game.h CARD_BASE_COUNT; ids 12+ are the fusions
CORNER_R = 4                    # rounded corners: these pixels stay index 0

FACE_X = 4                      # 2px dark edge + 2px lighter bevel, then face
FACE_W = CARD_W - FACE_X * 2
FACE_H = CARD_H - FACE_X * 2

HEADER_Y0, HEADER_Y1 = 4, 19    # the name band
ACCENT_H = 3                    # kind-coloured strip along the band's bottom

ART_X0, ART_Y0 = 6, 23          # recessed art window: 6px in from each side
ART_W, ART_H = 60, 55

FOOTER_Y0, FOOTER_H = 80, 12    # the pip band the engine draws levels on
EM_CX, EM_CY = 36.0, 50.5


# ---------------------------------------------------------------------------
# Drawing helpers
# ---------------------------------------------------------------------------

def fill_poly(c, pts, col):
    """Scanline-fill a polygon given in pixel-centre coordinates."""
    if len(pts) < 3:
        return
    ys = [p[1] for p in pts]
    y_lo, y_hi = int(math.floor(min(ys))), int(math.ceil(max(ys)))
    n = len(pts)
    for j in range(y_lo, y_hi + 1):
        yc = j + 0.5
        xs = []
        for k in range(n):
            ax, ay = pts[k]
            bx, by = pts[(k + 1) % n]
            if ay == by:
                continue
            if (ay <= yc < by) or (by <= yc < ay):
                t = (yc - ay) / (by - ay)
                xs.append(ax + t * (bx - ax))
        xs.sort()
        for k in range(0, len(xs) - 1, 2):
            xa, xb = xs[k], xs[k + 1]
            for i in range(int(math.ceil(xa - 0.5)), int(math.floor(xb - 0.5)) + 1):
                c.set(i, j, col)


def spark(c, x, y, r, col):
    """A four-point sparkle: two crossed bars plus diagonal tips."""
    xi, yi = int(round(x)), int(round(y))
    c.vline(xi, int(round(y - r)), int(round(y + r)), col)
    c.hline(int(round(x - r)), int(round(x + r)), yi, col)
    for dx, dy in ((-1, -1), (1, -1), (-1, 1), (1, 1)):
        c.set(xi + dx * max(1, int(r * 0.7)), yi + dy * max(1, int(r * 0.7)), col)


def stamp_arc(c, cx, cy, rx, ry, a0, a1, stamp_r, col, taper=None):
    """A thick curved stroke: filled discs stamped along an elliptical path."""
    span = abs(a1 - a0)
    r_path = max(rx, ry, 1.0)
    t_min = min(taper(k / 8.0) for k in range(9)) if taper else 1.0
    step_deg = max(0.8, math.degrees(max(stamp_r * t_min * 0.7, 0.5) / r_path))
    n = max(2, int(span / step_deg))
    for k in range(n + 1):
        u = k / float(n)
        r = stamp_r * (taper(u) if taper else 1.0)
        if r < 0.35:
            continue
        a = math.radians(a0 + (a1 - a0) * u)
        c.circle(cx + rx * math.cos(a), cy + ry * math.sin(a), r, col)


def spike(c, x, y, deg, r_base, r_tip, half, col):
    """One outward triangle: a thorn on a ring, a shard of an explosion.

    Angles are screen angles (0 = right, 90 = down) so a caller can walk a
    circle with range(0, 360, 45) the way `stamp_arc` does.
    """
    rad = math.radians(deg)
    nx, ny = math.cos(rad), math.sin(rad)
    tx, ty = -ny, nx
    fill_poly(c, [(x + nx * r_base + tx * half, y + ny * r_base + ty * half),
                  (x + nx * r_base - tx * half, y + ny * r_base - ty * half),
                  (x + nx * r_tip, y + ny * r_tip)], ix(col))


def water_lance(c, x0, x1, y, half=4.2, tip=0.6):
    """A horizontal water bolt: fat tail, point at the tip -- the piercing shot.

    Same four-band ramp as `em_water`'s column (cyan_dark edge, cyan_mid body,
    cyan_light core, white_spark filament) so water reads the same on every card
    it appears on; only the axis changes. The white filament is deliberately
    thin: a wide one turns the bolt into a milky bar instead of water.
    """
    span = max(1.0, float(x1 - x0))
    for i in range(int(x0), int(x1) + 1):
        u = (i - x0) / span
        h = tip + (half - tip) * (1.0 - u) ** 1.3
        c.vline(i, int(round(y - h)) - 1, int(round(y + h)) + 1, ix("cyan_dark"))
        c.vline(i, int(round(y - h)), int(round(y + h)), ix("cyan_mid"))
        c.vline(i, int(round(y - h * 0.72)), int(round(y + h * 0.55)),
                ix("cyan_light"))
        if h > 3.2:
            c.vline(i, int(round(y - h * 0.22)), int(round(y + h * 0.12)),
                    ix("white_spark"))


def fuse_halo(c, cx, cy):
    """The fused set's shared backdrop: the base cards' vignette, in gold.

    The twelve base emblems that use a vignette open with a quiet tan/cream one
    -- it is a paper tint under the picture, not a frame around it, and it has
    to stay that way or the emblem's own silhouette stops reading. So the fused
    eight keep the identical structure and size (same five pixels of tint, same
    cream arena to draw on) and only swap the tint: tan becomes wheat. The rest
    of the gold lives on the plate, where the reward is announced.
    """
    c.ellipse(cx, cy, 22.0, 18.0, ix("wheat_mid"))
    c.ellipse(cx, cy, 20.5, 16.5, ix("wheat_light"))
    c.ellipse(cx, cy, 17.0, 13.0, ix("cream_light"))


def draw_fuse_badge(c):
    """The mark of a fused card, centred in the gold name band.

    Twin rings (two parents, one card) with a pip on each side for the pair that
    went into it. Read at 48x64 as on the DS, it is the one element that
    separates a fused card from a base one before any text is read.
    """
    hx, hy = CARD_W // 2, HEADER_Y0 + 7
    for px in (hx - 16, hx + 16):
        c.circle(px, hy, 2.6, ix("outline"))
        c.circle(px, hy, 1.5, ix("ui_gold_light"))
    c.circle(hx, hy, 6.0, ix("outline"))
    c.circle(hx, hy, 5.0, ix("brass_hi"))
    c.circle(hx, hy, 3.6, ix("brass_shadow"))
    c.circle(hx, hy, 2.4, ix("ui_gold_light"))
    c.circle(hx, hy, 1.1, ix("brass_shadow"))
    c.set(hx - 2, hy - 3, ix("white_spark"))


# ---------------------------------------------------------------------------
# 12 Emblems
# ---------------------------------------------------------------------------

def em_fangs(c, cx, cy, acc):
    """0 尖牙 -- Massive rodent chisel incisors chomping a green reed with crunch sparks!"""
    c.ellipse(cx, cy, 22, 18, ix("tan_light"))
    c.ellipse(cx, cy, 17, 13, ix("cream_light"))

    # Upper snout / gum arch
    c.round_rect(int(cx - 16), int(cy - 16), 32, 8, 3, ix("brn_deep"))
    c.round_rect(int(cx - 15), int(cy - 15), 30, 5, 2, ix("brn_mid2"))
    c.hline(int(cx - 13), int(cx + 13), int(cy - 16), ix("brn_hi"))
    c.hline(int(cx - 12), int(cx + 12), int(cy - 11), ix("pink_mid"))

    # Bamboo reed being bitten across the teeth
    reed_y = int(cy + 2)
    c.rect(int(cx - 24), reed_y - 2, 48, 5, ix("grn_dark"))
    c.rect(int(cx - 24), reed_y - 1, 48, 3, ix("grn_mid"))
    c.hline(int(cx - 23), int(cx + 23), reed_y - 1, ix("grn_light"))
    for jx in (int(cx - 16), int(cx + 16)):
        c.vline(jx, reed_y - 2, reed_y + 2, ix("leaf_dark"))
        c.vline(jx + 1, reed_y - 2, reed_y + 2, ix("leaf_light"))

    # Two big chisel incisors
    for side, tx in ((-1, cx - 6.5), (1, cx + 6.5)):
        c.round_rect(int(tx - 4.5), int(cy - 10), 9, 17, 2, ix("outline"))
        c.round_rect(int(tx - 3.5), int(cy - 9), 7, 15, 1, ix("cream"))
        c.vline(int(tx - 2.5), int(cy - 8), int(cy + 2), ix("white"))
        c.vline(int(tx - 1.5), int(cy - 8), int(cy - 3), ix("white"))
        c.hline(int(tx - 3.5), int(tx + 2.5), int(cy + 5), ix("white"))
        c.hline(int(tx - 2.5), int(tx + 2.5), int(cy + 6), ix("tan_belly"))

    # Splinter crunch in center
    c.rect(int(cx - 3), reed_y - 2, 6, 5, ix("cream"))
    for sx, sy in ((-10, -3), (10, -4), (-8, 8), (9, 7), (0, 9)):
        c.circle(cx + sx, cy + sy, 1.2, ix("wheat_light"))

    spark(c, cx - 14, cy - 2, 4.0, ix("ui_gold_light"))
    spark(c, cx + 14, cy - 2, 4.0, ix("ui_gold_light"))
    spark(c, cx, cy - 9, 2.5, ix("white_spark"))


def em_iron_tail(c, cx, cy, acc):
    """1 铁尾 -- Capybara rump with armored steel tail slam and ground shockwaves!"""
    c.ellipse(cx, cy, 22, 18, ix("tan_light"))
    c.ellipse(cx, cy, 16, 12, ix("cream_light"))

    # Capybara round hindquarters on the left
    rx, ry = cx - 9.0, cy - 2.0
    c.blob(rx, ry, 11.0, 9.0, ix("outline"), n=4.0)
    c.blob(rx, ry, 10.0, 8.0, ix("brn_deep"), n=4.0)
    c.blob(rx - 1.0, ry - 1.0, 8.5, 6.5, ix("brn_mid2"), n=4.0)
    c.blob(rx - 2.0, ry - 2.0, 6.0, 4.5, ix("brn_hi"), n=4.0)

    # Steel armored tail swinging in a powerful crescent
    ax, ay = cx + 2.0, cy - 3.0
    for r_out, r_cut, col in ((16.0, 12.0, ix("steel_shadow")),
                              (14.5, 12.0, ix("steel_dark")),
                              (12.5, 12.0, ix("steel_mid")),
                              (10.5, 12.0, ix("steel_hi")),
                              (8.5, 12.0, ix("steel_glint"))):
        c.circle(ax, ay, r_out, col)
        c.circle(ax + 9.0, ay - 7.0, r_cut, ix("cream"))

    # Heavy iron mace/tail ball at the tip of the tail
    tx, ty = cx + 11.0, cy + 5.0
    c.circle(tx, ty, 5.0, ix("outline"))
    c.circle(tx, ty, 4.2, ix("steel_dark"))
    c.circle(tx - 0.8, ty - 0.8, 3.2, ix("steel_mid"))
    c.circle(tx - 1.5, ty - 1.5, 1.8, ix("steel_hi"))
    c.set(int(tx - 2), int(ty - 2), ix("steel_glint"))

    for sx, sy in ((-4, 0), (4, 0), (0, -4), (0, 4)):
        c.set(int(tx + sx), int(ty + sy), ix("steel_hi"))

    c.circle(rx + 2.0, ry, 2.0, ix("brass_dark"))
    c.circle(rx + 2.0, ry, 1.2, ix("brass_hi"))

    iy0 = int(cy + 14)
    c.hline(int(cx - 6), int(cx + 18), iy0, ix("outline"))
    c.hline(int(cx - 2), int(cx + 16), iy0 - 1, ix("steel_dark"))

    spark(c, tx + 2, ty + 2, 4.5, ix("ui_gold_light"))
    spark(c, tx - 5, ty - 4, 2.5, ix("white_spark"))


def em_charge(c, cx, cy, acc):
    """2 冲撞 -- Chubby capybara loaf sprinting forward with speed lines and dust puffs!"""
    for dy, xa, xb in ((-12, -22, 18), (0, -24, 20), (12, -22, 18)):
        y = int(cy + dy)
        c.hline(int(cx + xa), int(cx + xb), y, ix("tan_light"))

    bx = cx - 3.0
    by = cy + 2.0
    c.blob(bx, by, 14.5, 11.0, ix("outline"), n=4.0)
    c.blob(bx, by, 13.5, 10.0, ix("brn_deep"), n=4.0)
    c.blob(bx, by - 1.0, 12.5, 8.5, ix("brn_mid2"), n=4.0)
    c.blob(bx + 1.0, by - 3.0, 9.5, 5.5, ix("brn_hi"), n=4.0)

    # Snout
    c.round_rect(int(bx + 8), int(by - 5), 7, 9, 2, ix("outline"))
    c.round_rect(int(bx + 7), int(by - 4), 7, 7, 2, ix("brn_belly"))
    c.set(int(bx + 13), int(by - 2), ix("outline"))
    c.set(int(bx + 13), int(by - 1), ix("outline"))
    # Chill slit eye `—` with sparkle
    c.hline(int(bx + 4), int(bx + 7), int(by - 3), ix("outline"))
    c.set(int(bx + 6), int(by - 4), ix("white_spark"))
    # Ear pinned back
    c.ellipse(bx - 3, by - 7, 3.0, 1.8, ix("outline"))
    c.ellipse(bx - 3, by - 7, 2.0, 1.0, ix("pink_mid"))

    # Galloping little feet underneath
    c.round_rect(int(bx - 8), int(by + 8), 4, 4, 1, ix("brn_deep"))
    c.round_rect(int(bx + 3), int(by + 8), 4, 4, 1, ix("brn_deep"))

    # Golden charge barrier / shockwave arc
    head_x = bx + 15.0
    fill_poly(c, [(head_x + 1, cy - 12.0), (head_x + 7.0, cy), (head_x + 1, cy + 12.0)],
              ix("orange_dark"))
    fill_poly(c, [(head_x + 2, cy - 9.0), (head_x + 6.0, cy), (head_x + 2, cy + 9.0)],
              ix("ember_yellow"))
    fill_poly(c, [(head_x + 3, cy - 5.0), (head_x + 5.5, cy), (head_x + 3, cy + 5.0)],
              ix("white_spark"))

    # Dust clouds
    for dx, dy, r in ((-15, 7, 3.8), (-20, 4, 3.0), (-17, 1, 2.4)):
        c.circle(bx + dx, by + dy, r, ix("outline"))
        c.circle(bx + dx, by + dy, r - 0.8, ix("grey_light"))
        c.circle(bx + dx - 0.6, by + dy - 0.6, r - 1.8, ix("white_spark"))


def em_water(c, cx, cy, acc):
    """3 水柱 -- Erupting onsen water spout with floating yuzu and crystal spray!"""
    top, bot = cy - 18.0, cy + 14.0
    for j in range(int(top), int(bot) + 1):
        t = (j - top) / (bot - top)
        mid = cx + 5.0 * math.sin(t * 3.5)
        half = 4.0 + 8.0 * (1.0 - t) * (1.0 - t * 0.5)
        c.hline(int(mid - half - 1), int(mid + half + 1), j, ix("cyan_dark"))
        c.hline(int(mid - half), int(mid + half), j, ix("cyan_mid"))
        c.hline(int(mid - half * 0.6), int(mid + half * 0.4), j, ix("cyan_light"))
        if half > 4.0:
            c.hline(int(mid - half * 0.2), int(mid + half * 0.1), j, ix("white_spark"))

    # Floating cute yuzu orange surfing the water crest
    yx, yy = cx + 2.0, cy - 8.0
    c.circle(yx, yy, 5.5, ix("outline"))
    c.circle(yx, yy, 4.5, ix("orange_dark"))
    c.circle(yx - 0.8, yy - 0.8, 3.8, ix("orange_mid"))
    c.circle(yx - 1.5, yy - 1.5, 2.2, ix("yellow_mid"))
    c.set(int(yx), int(yy - 5), ix("grn_dark"))
    c.set(int(yx + 1), int(yy - 6), ix("grn_light"))
    c.set(int(yx + 2), int(yy - 6), ix("leaf_light"))

    # Splashing droplets
    for sx, sy, sr in ((-12, -14, 2.5), (14, -12, 2.2), (-6, -19, 2.5),
                       (10, -19, 2.0), (16, -4, 1.8), (-14, 6, 2.0)):
        if ART_X0 + 2 <= cx + sx <= ART_X0 + ART_W - 3 and ART_Y0 + 2 <= cy + sy <= ART_Y0 + ART_H - 3:
            c.circle(cx + sx, cy + sy, sr, ix("cyan_dark"))
            c.circle(cx + sx, cy + sy, sr - 0.7, ix("cyan_light"))
            c.set(int(round(cx + sx - 0.5)), int(round(cy + sy - 0.5)), ix("white_spark"))


def em_water_orbs(c, cx, cy, acc):
    """4 灵水 -- Orbiting glowing crystal water orbs circling zen capybara!"""
    # Orbit path ring
    for deg in range(0, 360, 10):
        rad = math.radians(deg)
        px = int(round(cx + 19.0 * math.cos(rad)))
        py = int(round(cy + 15.0 * math.sin(rad)))
        if ART_X0 + 2 <= px <= ART_X0 + ART_W - 3 and ART_Y0 + 2 <= py <= ART_Y0 + ART_H - 3:
            c.set(px, py, ix("cyan_light"))

    # Zen Capybara resting in center
    c.blob(cx, cy + 3, 10.5, 7.5, ix("outline"), n=4.0)
    c.blob(cx, cy + 3, 9.5, 6.5, ix("brn_deep"), n=4.0)
    c.blob(cx, cy + 2, 8.0, 5.0, ix("brn_mid2"), n=4.0)
    c.ellipse(cx - 8, cy - 1, 2.2, 1.5, ix("outline"))
    c.ellipse(cx - 8, cy - 1, 1.4, 0.8, ix("pink_mid"))
    c.ellipse(cx + 8, cy - 1, 2.2, 1.5, ix("outline"))
    c.ellipse(cx + 8, cy - 1, 1.4, 0.8, ix("pink_mid"))
    # Zen slit eyes
    c.hline(int(cx - 5), int(cx - 2), int(cy + 2), ix("outline"))
    c.hline(int(cx + 2), int(cx + 5), int(cy + 2), ix("outline"))
    # Yuzu on head
    c.circle(cx, cy - 5, 2.5, ix("orange_mid"))
    c.set(int(cx), int(cy - 7), ix("grn_mid"))

    # 3 Glowing crystal water orbs
    for deg in (30, 150, 270):
        rad = math.radians(deg)
        ox = cx + 19.0 * math.cos(rad)
        oy = cy + 15.0 * math.sin(rad)
        c.circle(ox, oy, 4.6, ix("outline"))
        c.circle(ox, oy, 3.8, ix("cyan_dark"))
        c.circle(ox, oy, 2.8, ix("cyan_mid"))
        c.circle(ox - 0.8, oy - 0.8, 1.5, ix("cyan_light"))
        c.set(int(ox - 1), int(oy - 1), ix("white_spark"))

    # Water splash sparkles
    for sx, sy in ((-15, -12), (15, -10), (-14, 12), (14, 11)):
        c.set(int(cx + sx), int(cy + sy), ix("cyan_light"))
        c.set(int(cx + sx), int(cy + sy - 1), ix("white_spark"))


def em_dung(c, cx, cy, acc):
    """5 粪球 -- Highly polished herbal fiber pellet rolling with green herbal sparkles!"""
    c.ellipse(cx, cy + 2, 22, 17, ix("tan_light"))
    c.ellipse(cx, cy + 2, 16, 12, ix("cream_light"))

    by = cy + 3.0
    c.circle(cx + 0.5, by + 1.0, 14.0, ix("outline"))
    c.circle(cx, by, 13.0, ix("choco_dark"))
    c.circle(cx - 2.5, by - 2.5, 9.5, ix("choco_mid"))
    c.circle(cx - 4.5, by - 4.5, 5.5, ix("choco_light"))
    c.circle(cx - 5.5, by - 5.5, 2.5, ix("cream"))
    c.set(int(cx - 6), int(by - 6), ix("white_spark"))

    c.circle(cx + 4.0, by + 4.0, 2.0, ix("brn_shadow"))
    c.circle(cx + 6.0, by - 1.0, 1.5, ix("brn_deep"))
    c.circle(cx - 1.0, by + 7.0, 1.8, ix("brn_shadow"))
    c.set(int(cx + 1), int(by - 3), ix("leaf_mid"))
    c.set(int(cx - 3), int(by + 2), ix("leaf_light"))

    for sx, sy in ((-15, -10), (14, -8), (-12, 11), (13, 10), (0, -15)):
        c.circle(cx + sx, cy + sy, 1.6, ix("green_spark"))
        c.set(int(cx + sx), int(cy + sy), ix("white_spark"))

    c.hline(int(cx - 22), int(cx - 14), int(by + 11), ix("ui_gold_light"))
    c.hline(int(cx - 20), int(cx - 12), int(by + 13), ix("ui_text_dim"))


def em_pacify(c, cx, cy, acc):
    """6 安抚 -- Golden zen halo aura with serene capybara head and floating lotus petals!"""
    for r_out, r_in, col in ((21.0, 19.5, ix("heal_outer")),
                             (17.0, 15.5, ix("heal_inner")),
                             (13.0, 11.5, ix("ui_gold_light")),
                             (9.0, 7.5, ix("pink_mid"))):
        for deg in range(0, 360, 6):
            rad = math.radians(deg)
            px = int(round(cx + ((r_out + r_in) * 0.5) * math.cos(rad)))
            py = int(round(cy + ((r_out + r_in) * 0.5 * 0.85) * math.sin(rad)))
            if ART_X0 + 1 <= px <= ART_X0 + ART_W - 2 and ART_Y0 + 1 <= py <= ART_Y0 + ART_H - 2:
                c.set(px, py, col)

    # Serene golden capybara head in center
    c.blob(cx, cy, 8.5, 6.5, ix("outline"), n=4.0)
    c.blob(cx, cy, 7.5, 5.5, ix("ui_gold"), n=4.0)
    c.blob(cx, cy - 0.5, 6.0, 4.0, ix("ui_gold_light"), n=4.0)

    # Little capybara ears
    c.ellipse(cx - 7, cy - 4, 2.0, 1.5, ix("outline"))
    c.ellipse(cx - 7, cy - 4, 1.2, 0.8, ix("pink_mid"))
    c.ellipse(cx + 7, cy - 4, 2.0, 1.5, ix("outline"))
    c.ellipse(cx + 7, cy - 4, 1.2, 0.8, ix("pink_mid"))

    # Zen horizontal slit eyes `— —`
    c.hline(int(cx - 5), int(cx - 2), int(cy), ix("outline"))
    c.hline(int(cx + 2), int(cx + 5), int(cy), ix("outline"))
    c.set(int(cx), int(cy + 2), ix("outline"))

    # Floating lotus petals
    for px, py in ((-12, -12), (12, -12), (-13, 8), (13, 8), (0, -16)):
        c.circle(cx + px, cy + py, 1.8, ix("pink_mid"))
        c.set(int(cx + px), int(cy + py - 1), ix("pink_light"))
        c.set(int(cx + px), int(cy + py), ix("white_spark"))


def em_recruit(c, cx, cy, acc):
    """7 感化 -- Capybara with a cute little yellow duckling on its head! Friendship!"""
    c.ellipse(cx, cy + 2, 22, 18, ix("tan_light"))
    c.ellipse(cx, cy + 2, 16, 12, ix("cream_light"))

    # Calm brown capybara resting at bottom
    c.blob(cx, cy + 6, 14.5, 9.5, ix("outline"), n=4.0)
    c.blob(cx, cy + 6, 13.5, 8.5, ix("brn_deep"), n=4.0)
    c.blob(cx, cy + 5, 12.0, 7.0, ix("brn_mid2"), n=4.0)

    # Ears
    c.ellipse(cx - 12, cy + 2, 2.5, 1.8, ix("outline"))
    c.ellipse(cx - 12, cy + 2, 1.6, 1.0, ix("pink_mid"))
    c.ellipse(cx + 12, cy + 2, 2.5, 1.8, ix("outline"))
    c.ellipse(cx + 12, cy + 2, 1.6, 1.0, ix("pink_mid"))

    # Slit eyes `— —`
    c.hline(int(cx - 7), int(cx - 3), int(cy + 5), ix("outline"))
    c.hline(int(cx + 3), int(cx + 7), int(cy + 5), ix("outline"))

    # Cute duckling perched on top
    dx, dy = cx, cy - 6.0
    c.ellipse(dx, dy + 1, 5.2, 4.0, ix("outline"))
    c.ellipse(dx, dy + 1, 4.2, 3.0, ix("yellow_mid"))
    c.ellipse(dx - 0.5, dy, 2.8, 1.8, ix("yellow_light"))
    c.circle(dx + 2, dy - 3, 3.2, ix("outline"))
    c.circle(dx + 2, dy - 3, 2.4, ix("yellow_mid"))
    c.circle(dx + 1.5, dy - 3.5, 1.4, ix("yellow_light"))
    c.set(int(dx + 2), int(dy - 4), ix("outline"))
    c.rect(int(dx + 5), int(dy - 3), 3, 2, ix("orange_mid"))
    c.ellipse(dx - 1, dy + 1, 2.2, 1.4, ix("yellow_dark"))

    # Friendship pink heart
    hx, hy = cx - 13, cy - 8
    c.circle(hx - 1.5, hy - 1, 1.8, ix("pink_mid"))
    c.circle(hx + 1.5, hy - 1, 1.8, ix("pink_mid"))
    fill_poly(c, [(hx - 3.0, hy), (hx + 3.0, hy), (hx, hy + 3.5)], ix("pink_mid"))
    c.set(int(hx - 1), int(hy - 1), ix("pink_light"))

    # Musical note
    nx, ny = int(cx + 12), int(cy - 9)
    c.circle(nx, ny + 3, 2.0, ix("ui_gold_light"))
    c.vline(nx + 1, ny - 4, ny + 3, ix("ui_gold_light"))
    c.hline(nx + 1, nx + 4, ny - 4, ix("ui_gold_light"))

    spark(c, cx - 14, cy + 4, 3.0, ix("ui_gold_light"))
    spark(c, cx + 14, cy + 4, 3.0, ix("ui_gold_light"))


def em_hide(c, cx, cy, acc):
    """8 厚皮 -- Sturdy golden-iron buckler shield with capybara paw emblem!"""
    c.ellipse(cx, cy, 22, 18, ix("tan_light"))
    c.ellipse(cx, cy, 16, 13, ix("cream_light"))

    def shield_pts(half, top, waist, tip):
        pts = [(cx - half, top), (cx + half, top), (cx + half, waist)]
        steps = 6
        for k in range(1, steps + 1):
            u = k / float(steps)
            pts.append((cx + half * (1.0 - u) ** 0.55, waist + (tip - waist) * u))
        for k in range(steps - 1, 0, -1):
            u = k / float(steps)
            pts.append((cx - half * (1.0 - u) ** 0.55, waist + (tip - waist) * u))
        return pts

    fill_poly(c, shield_pts(17.0, cy - 16.0, cy + 2.0, cy + 18.0), ix("outline"))
    fill_poly(c, shield_pts(16.0, cy - 15.0, cy + 1.5, cy + 17.0), ix("brass_dark"))
    fill_poly(c, shield_pts(14.0, cy - 13.5, cy + 1.0, cy + 15.0), ix("ui_gold"))
    fill_poly(c, shield_pts(12.0, cy - 11.5, cy + 0.5, cy + 13.0), ix("ui_gold_light"))

    fill_poly(c, shield_pts(10.0, cy - 9.5, cy - 0.5, cy + 11.0), ix("brn_deep"))
    fill_poly(c, shield_pts(8.5, cy - 8.0, cy - 1.0, cy + 9.5), ix("brn_mid2"))

    px, py = cx, cy + 1.0
    c.blob(px, py + 1.5, 4.5, 3.2, ix("outline"), n=4.0)
    c.blob(px, py + 1.5, 3.5, 2.2, ix("ui_gold_light"), n=4.0)
    for tx, ty in ((-4.0, -3.0), (0.0, -4.5), (4.0, -3.0)):
        c.circle(px + tx, py + ty, 1.8, ix("outline"))
        c.circle(px + tx, py + ty, 1.2, ix("ui_gold_light"))

    for rx, ry in ((-12, -12), (12, -12), (-11, 2), (11, 2), (0, 14)):
        c.circle(cx + rx, cy + ry, 1.4, ix("steel_glint"))

    spark(c, cx - 11, cy - 11, 3.0, ix("white_spark"))


def em_swift(c, cx, cy, acc):
    """9 快腿 -- Golden webbed agility paw print with swirling wind dash ribbons!"""
    c.ellipse(cx, cy, 22, 18, ix("tan_light"))
    c.ellipse(cx, cy, 16, 13, ix("cream_light"))

    for dy, xa, xb in ((-13, -18, 12), (0, -22, 16), (13, -16, 18)):
        y = int(cy + dy)
        c.hline(int(cx + xa), int(cx + xb), y, ix("cyan_mid"))
        c.hline(int(cx + xa + 4), int(cx + xb - 2), y, ix("cyan_light"))
        c.hline(int(cx + xa + 8), int(cx + xb - 6), y, ix("white_spark"))

    px, py = cx + 1.0, cy + 1.0
    c.blob(px, py + 3.0, 7.5, 5.2, ix("outline"), n=4.0)
    c.blob(px, py + 3.0, 6.5, 4.2, ix("ui_gold"), n=4.0)
    c.blob(px - 1.0, py + 2.0, 4.5, 2.5, ix("ui_gold_light"), n=4.0)

    for toe_x, toe_y, r in ((-7.0, -5.0, 3.2), (-2.5, -8.0, 3.5),
                            (2.5, -8.0, 3.5), (7.0, -5.0, 3.2)):
        c.circle(px + toe_x, py + toe_y, r, ix("outline"))
        c.circle(px + toe_x, py + toe_y, r - 0.8, ix("ui_gold"))
        c.circle(px + toe_x - 0.5, py + toe_y - 0.5, r - 1.6, ix("ui_gold_light"))
        c.set(int(px + toe_x), int(py + toe_y - r), ix("cream"))

    fill_poly(c, [(px - 6, py - 3), (px - 2, py - 6), (px, py), (px - 5, py)],
              ix("brass_dark"))
    fill_poly(c, [(px + 6, py - 3), (px + 2, py - 6), (px, py), (px + 5, py)],
              ix("brass_dark"))

    spark(c, cx + 15, cy - 10, 4.0, ix("ui_gold_light"))
    spark(c, cx - 14, cy + 11, 3.0, ix("white_spark"))


def em_magnet(c, cx, cy, acc):
    """10 磁吸 -- Ruby-cobalt horseshoe magnet drawing in golden coins and yuzu orange!"""
    c.ellipse(cx, cy - 2, 22, 18, ix("tan_light"))
    c.ellipse(cx, cy - 2, 16, 12, ix("cream_light"))

    arch_cy = cy - 6.0
    arch_r = 13.0
    leg_x0 = int(cx) - 16
    leg_x1 = int(cx) + 8
    leg_y0 = int(arch_cy)
    leg_y1 = int(cy + 10.0)
    leg_h = leg_y1 - leg_y0 + 1

    stamp_arc(c, cx, arch_cy, arch_r, arch_r, 180.0, 360.0, 5.0, ix("outline"))
    c.rect(leg_x0 - 1, leg_y0, 10, leg_h + 1, ix("outline"))
    c.rect(leg_x1 - 1, leg_y0, 10, leg_h + 1, ix("outline"))

    # Red left
    stamp_arc(c, cx, arch_cy, arch_r, arch_r, 180.0, 270.0, 3.5, ix("red_mid"))
    c.rect(leg_x0, leg_y0, 8, leg_h - 2, ix("red_mid"))
    c.rect(leg_x0 + 1, leg_y0, 3, leg_h - 2, ix("red_light"))
    c.rect(leg_x0, leg_y1 - 3, 8, 4, ix("grey_light"))
    c.rect(leg_x0 + 1, leg_y1 - 3, 6, 3, ix("white"))

    # Blue right
    stamp_arc(c, cx, arch_cy, arch_r, arch_r, 270.0, 360.0, 3.5, ix("blue_mid"))
    c.rect(leg_x1, leg_y0, 8, leg_h - 2, ix("blue_mid"))
    c.rect(leg_x1 + 1, leg_y0, 3, leg_h - 2, ix("blue_light"))
    c.rect(leg_x1, leg_y1 - 3, 8, 4, ix("grey_light"))
    c.rect(leg_x1 + 1, leg_y1 - 3, 6, 3, ix("white"))

    stamp_arc(c, cx, arch_cy, arch_r, arch_r, 210.0, 280.0, 1.4, ix("white_spark"))

    for y in (leg_y1 - 1, leg_y1 + 4):
        c.hline(leg_x0 + 4, leg_x1 + 4, y, ix("volt_inner"))
        c.set(int(cx - 2), y - 1, ix("volt_core"))
        c.set(int(cx + 2), y + 1, ix("volt_core"))

    coin_x, coin_y = cx, cy + 14.0
    c.circle(coin_x, coin_y, 4.0, ix("outline"))
    c.circle(coin_x, coin_y, 3.2, ix("ui_gold"))
    c.circle(coin_x - 0.5, coin_y - 0.5, 1.8, ix("ui_gold_light"))

    yx, yy = cx + 15, cy - 8
    c.circle(yx, yy, 3.5, ix("outline"))
    c.circle(yx, yy, 2.6, ix("orange_mid"))
    c.set(int(yx), int(yy - 3), ix("grn_mid"))

    spark(c, cx - 8, cy + 15, 2.5, ix("white_spark"))
    spark(c, cx + 8, cy + 15, 2.5, ix("ui_gold_light"))


def em_claws(c, cx, cy, acc):
    """11 利爪 -- Capybara furry paw raking with 3 huge glowing razor claw slashes!"""
    c.ellipse(cx, cy, 22, 18, ix("tan_light"))
    c.ellipse(cx, cy, 16, 13, ix("cream_light"))

    c.blob(cx + 10, cy - 10, 8.5, 6.5, ix("outline"), n=4.0)
    c.blob(cx + 10, cy - 10, 7.5, 5.5, ix("brn_deep"), n=4.0)
    c.blob(cx + 9, cy - 11, 5.5, 3.5, ix("brn_mid2"), n=4.0)

    y_top, y_bot = cy - 15.0, cy + 16.0
    for k in range(3):
        x_top = cx + 2.0 + k * 8.0
        for j in range(int(y_top), int(y_bot) + 1):
            u = min(1.0, max(0.0, (j - y_top) / (y_bot - y_top)))
            x = x_top - 20.0 * u
            w = 1.0 + 3.8 * math.sin(math.pi * u) ** 0.6
            c.hline(int(round(x - w / 2.0 - 0.8)), int(round(x + w / 2.0 + 0.8)), j,
                    ix("orange_dark"))
            c.hline(int(round(x - w / 2.0)), int(round(x + w / 2.0)), j,
                    ix("ui_gold"))
            if 0.15 < u < 0.8:
                c.hline(int(round(x - w / 3.5)), int(round(x + w / 3.5)), j,
                        ix("white"))

    spark(c, cx - 5, cy + 3, 5.0, ix("white_spark"))
    spark(c, cx - 11, cy + 11, 3.0, ix("ui_gold_light"))
    spark(c, cx + 5, cy - 5, 3.5, ix("ember_yellow"))


# ---------------------------------------------------------------------------
# 8 Fusion emblems (ids 12..19)
#
# Each one shows what the fusion *does*, in the same visual language as its two
# parents so the recipe is legible from the picture: a bite that reaches, a
# guard that answers, a volley, a cloud, a retinue, a vortex, a trap, a spin.
# ---------------------------------------------------------------------------

def em_predator_rush(c, cx, cy, acc):
    """12 掠食突袭 -- 尖牙 + 冲撞: one dive, and the bite lands on everything."""
    fuse_halo(c, cx, cy)

    # What the dive already went through: three of them, flat out, and the
    # trail it left. This is the "hits everything on the way" clause, and it is
    # why this card needs no speed lines -- the wreckage says how fast it was.
    for ex, ey in ((cx - 19.0, cy - 14.0), (cx - 23.0, cy - 2.0),
                   (cx - 18.0, cy + 10.0)):
        c.hline(int(ex + 4), int(ex + 9), int(ey), ix("cream"))
        c.circle(ex, ey, 4.0, ix("outline"))
        c.circle(ex, ey, 3.2, ix("grey_mid"))
        c.circle(ex - 0.6, ey - 0.6, 2.0, ix("grey_light"))
        c.set(int(ex - 1), int(ey - 1), ix("outline"))
        c.set(int(ex - 1), int(ey + 1), ix("outline"))
    for k in range(6):
        c.set(int(cx - 11 + k * 3), int(cy - 7 + math.sin(k * 1.3) * 4.0),
              ix("white_spark"))
    # Air parted under the pounce.
    for dy, x0, x1 in ((14, -16, 4), (19, -12, 2)):
        y = int(cy + dy)
        c.hline(int(cx + x0), int(cx + x1), y + 1, ix("tan_shadow"))
        c.hline(int(cx + x0), int(cx + x1), y, ix("cream"))
        c.hline(int(cx + x0 + 3), int(cx + x1 - 3), y, ix("white_spark"))

    # The capybara, nose down in profile: skull up, snout out low, jaws open.
    sx, sy = cx + 1.0, cy - 1.0
    c.blob(sx, sy, 11.5, 10.0, ix("outline"), n=4.0)
    c.blob(sx, sy, 10.5, 9.0, ix("brn_deep"), n=4.0)
    c.blob(sx - 0.5, sy - 1.5, 9.0, 7.0, ix("brn_mid2"), n=4.0)
    c.blob(sx - 2.0, sy - 3.5, 6.5, 4.0, ix("brn_hi"), n=4.0)
    c.ellipse(sx - 9.0, sy - 5.0, 3.0, 2.0, ix("outline"))
    c.ellipse(sx - 9.0, sy - 5.0, 1.8, 1.1, ix("pink_mid"))
    c.hline(int(sx + 1), int(sx + 5), int(sy - 2), ix("outline"))
    c.set(int(sx + 3), int(sy - 3), ix("white_spark"))
    # A rounded snout, not a second box: two boxes in a row read as a crate.
    c.blob(sx + 13.0, sy + 6.0, 8.0, 5.5, ix("outline"), n=4.0)
    c.blob(sx + 13.0, sy + 6.0, 7.0, 4.5, ix("brn_belly"), n=4.0)
    c.blob(sx + 12.0, sy + 4.5, 5.0, 3.0, ix("tan_belly"), n=4.0)
    c.set(int(sx + 20), int(sy + 5), ix("outline"))
    c.set(int(sx + 20), int(sy + 6), ix("outline"))
    c.set(int(sx + 19), int(sy + 8), ix("brn_shadow"))
    # The gape: dark, so the two cream incisors in it read as teeth.
    c.rect(int(sx + 8), int(sy + 10), 15, 5, ix("outline"))
    c.rect(int(sx + 9), int(sy + 11), 13, 3, ix("brn_shadow"))
    for tx in (sx + 11.0, sx + 16.0):
        fill_poly(c, [(tx - 2.2, sy + 9.0), (tx + 2.2, sy + 9.0), (tx, sy + 14.0)],
                  ix("cream"))
    c.round_rect(int(sx + 6), int(sy + 15), 14, 5, 2, ix("outline"))
    c.round_rect(int(sx + 7), int(sy + 16), 13, 3, 1, ix("brn_belly"))
    c.round_rect(int(sx - 13), int(sy + 7), 5, 4, 1, ix("brn_deep"))

    # The bite itself.
    fx, fy = cx + 19.0, cy + 17.0
    spark(c, fx, fy, 4.6, ix("orange_dark"))
    spark(c, fx, fy, 3.2, ix("ember_yellow"))
    spark(c, fx, fy, 1.6, ix("white_spark"))
    for dx, dy in ((4, -3), (5, 2), (1, 5)):
        c.set(int(fx + dx), int(fy + dy), ix("red_mid"))
        c.set(int(fx + dx), int(fy + dy + 1), ix("orange_mid"))


def em_thorn_bastion(c, cx, cy, acc):
    """13 荆棘铁壁 -- 铁尾 + 厚皮: a thorned ring, and the answer to a parry."""
    fuse_halo(c, cx, cy)

    # The wall: a riveted steel ring wearing eight thorns.
    for deg in range(0, 360, 45):
        rad = math.radians(deg)
        spike(c, cx, cy, deg, 13.0, 22.0, 5.0, "outline")
        spike(c, cx, cy, deg, 13.6, 20.6, 4.0, "leaf_dark")
        spike(c, cx, cy, deg, 13.8, 19.2, 2.5, "grn_mid")
        c.set(int(round(cx + math.cos(rad) * 17.0 - math.sin(rad) * 1.2)),
              int(round(cy + math.sin(rad) * 17.0 + math.cos(rad) * 1.2)),
              ix("leaf_light"))

    c.circle(cx, cy, 17.5, ix("outline"))
    c.circle(cx, cy, 16.5, ix("steel_shadow"))
    c.circle(cx, cy, 15.0, ix("steel_dark"))
    c.circle(cx, cy, 13.0, ix("steel_mid"))
    c.circle(cx, cy, 12.0, ix("steel_hi"))
    c.circle(cx, cy, 10.5, ix("cream"))
    for deg in range(22, 360, 45):
        rad = math.radians(deg)
        c.circle(cx + 14.2 * math.cos(rad), cy + 14.2 * math.sin(rad), 1.5,
                 ix("steel_glint"))

    # The 铁尾 parent, planted across the ring as the brace.
    c.round_rect(int(cx - 11), int(cy - 3), 13, 5, 2, ix("outline"))
    c.round_rect(int(cx - 10), int(cy - 2), 12, 3, 1, ix("steel_dark"))
    c.hline(int(cx - 9), int(cx + 1), int(cy - 2), ix("steel_hi"))
    for jx in (cx - 7.0, cx - 3.0):
        c.vline(int(jx), int(cy - 3), int(cy + 1), ix("brass_mid"))
        c.set(int(jx), int(cy - 2), ix("brass_hi"))
    c.circle(cx + 3.0, cy, 5.5, ix("outline"))
    c.circle(cx + 3.0, cy, 4.6, ix("steel_dark"))
    c.circle(cx + 2.2, cy - 0.8, 3.4, ix("steel_mid"))
    c.circle(cx + 1.4, cy - 1.6, 1.7, ix("steel_hi"))
    for sx, sy in ((-3, 0), (3, 0), (0, -3), (0, 3)):
        c.set(int(cx + 3 + sx), int(cy + sy), ix("steel_glint"))

    # What the parry does about it: the hit flashes off, the hitter is stunned.
    fx, fy = cx + 12.0, cy - 12.0
    spark(c, fx, fy, 5.0, ix("white_spark"))
    spark(c, fx + 2.0, fy - 2.0, 3.0, ix("ember_yellow"))
    for dx, dy in ((6, -5), (8, -1), (2, -7)):
        c.set(int(fx + dx), int(fy + dy), ix("red_mid"))
        c.set(int(fx + dx), int(fy + dy + 1), ix("orange_mid"))
    sx, sy = cx - 12.0, cy + 13.0
    spark(c, sx, sy, 5.0, ix("volt_inner"))
    spark(c, sx, sy, 3.0, ix("volt_core"))
    for dx, dy in ((-7, -3), (6, -4), (0, 6)):
        c.set(int(sx + dx), int(sy + dy), ix("volt_outer"))


def em_tidal_twin(c, cx, cy, acc):
    """14 潮汐双炮 -- 水柱 + 灵水: a three-bolt volley, orbs circling the muzzle."""
    fuse_halo(c, cx, cy)

    # The 灵水 parent: an orbit of beads around the muzzle, two of them solid.
    ox, oy = cx - 12.0, cy
    for deg in range(0, 360, 12):
        rad = math.radians(deg)
        bx = int(round(ox + 12.5 * math.cos(rad)))
        by = int(round(oy + 11.0 * math.sin(rad)))
        c.set(bx, by, ix("cyan_light"))
    for deg in (135, 225):
        rad = math.radians(deg)
        bx, by = ox + 12.5 * math.cos(rad), oy + 11.0 * math.sin(rad)
        c.circle(bx, by, 4.2, ix("outline"))
        c.circle(bx, by, 3.4, ix("cyan_dark"))
        c.circle(bx, by, 2.6, ix("cyan_mid"))
        c.circle(bx - 0.7, by - 0.7, 1.4, ix("cyan_light"))
        c.set(int(bx - 1), int(by - 1), ix("white_spark"))

    c.circle(ox, oy, 7.0, ix("outline"))
    c.circle(ox, oy, 6.0, ix("cyan_dark"))
    c.circle(ox, oy, 4.8, ix("cyan_mid"))
    c.circle(ox - 1.0, oy - 1.0, 3.0, ix("cyan_light"))
    c.set(int(ox - 1), int(oy - 1), ix("white_spark"))

    # The 水柱 parent: three bolts, the middle one thrown furthest.
    for k, dy in enumerate((-12.0, 0.0, 12.0)):
        x_tip = cx + 24.0 - abs(k - 1) * 3.0
        water_lance(c, cx - 5.0, x_tip, cy + dy, half=4.2)
        spark(c, x_tip - 4.0, cy + dy, 2.6, ix("cyan_light"))
    for dx, dy, r in ((-3, -15, 2.2), (-1, 15, 2.0), (-6, 13, 1.6)):
        c.circle(cx + dx, cy + dy, r, ix("cyan_dark"))
        c.circle(cx + dx, cy + dy, r - 0.7, ix("cyan_light"))
        c.set(int(cx + dx - 1), int(cy + dy - 1), ix("white_spark"))
    c.set(int(cx + 8), int(cy - 16), ix("cyan_mid"))
    c.set(int(cx + 10), int(cy + 17), ix("cyan_mid"))


def em_miasma(c, cx, cy, acc):
    """15 恶臭香氛 -- 粪球 + 安抚: a reek cloud over the pool, and lost will."""
    fuse_halo(c, cx, cy)

    # The cloud, in three passes: silhouette, body, then one dark arc along the
    # top of every lobe. Outlining each puff as it is drawn turns the cloud into
    # a bunch of grapes, and outlining none of them turns it into a mushroom --
    # a lobe needs a shadow line, not a ring.
    puffs = ((-15.5, -2.0, 6.5), (-8.0, -5.5, 7.5), (1.0, -6.5, 8.0),
             (9.0, -3.5, 7.0), (15.0, 0.5, 5.5), (-8.0, 1.5, 6.5),
             (2.0, 0.0, 7.0), (10.0, 2.5, 6.0))
    for px, py, r in puffs:
        c.circle(cx + px, cy + py + 1.0, r + 1.0, ix("outline"))
    for px, py, r in puffs:
        c.circle(cx + px, cy + py + 1.0, r, ix("shadow_outer"))
    for px, py, r in puffs:
        c.circle(cx + px - 0.8, cy + py, r - 1.6, ix("shadow_inner"))
    for px, py, r in puffs:
        stamp_arc(c, cx + px, cy + py + 1.0, r + 0.4, r + 0.4, 188.0, 352.0,
                  1.1, ix("outline"))
    for px, py, r in ((-11.0, -4.0, 4.2), (2.0, -6.0, 4.6), (12.0, -1.0, 3.4)):
        c.circle(cx + px, cy + py, r, ix("shadow_core"))
    for px, py, r in ((-13.5, -3.5, 1.9), (1.0, -7.0, 2.1), (10.0, -3.5, 1.7)):
        c.circle(cx + px, cy + py, r, ix("shadow_inner"))
        c.set(int(cx + px - 1), int(cy + py - 2), ix("white_spark"))
    # It keeps working on you: bubbles of it boiling off the top.
    for px, py in ((-16.0, -8.0), (-2.0, -12.0), (11.0, -8.5), (17.0, -3.0)):
        c.circle(cx + px, cy + py, 2.2, ix("outline"))
        c.circle(cx + px, cy + py, 1.5, ix("green_spark"))
        c.set(int(cx + px - 1), int(cy + py - 1), ix("white_spark"))
    for x0 in (-13.5, -1.0, 11.0):
        for k in range(7):
            t = k / 6.0
            ys = int(cy - 15.0 - k * 0.9)
            xs = int(round(cx + x0 + 2.6 * math.sin(t * 5.0)))
            c.set(xs, ys, ix("green_spark"))
            c.set(xs, ys + 1, ix("grn_mid"))

    # The pool it lays down, with the 便便 parent cooking in it.
    c.ellipse(cx - 2.0, cy + 16.0, 17.0, 6.0, ix("outline"))
    c.ellipse(cx - 2.0, cy + 16.0, 16.0, 5.1, ix("murk_dark"))
    c.ellipse(cx - 2.0, cy + 15.6, 14.5, 4.1, ix("murk_mid"))
    for dx, dy, r in ((-10, -1, 1.8), (-3, 2, 1.5), (7, 1, 2.0), (11, -2, 1.4)):
        c.circle(cx + dx, cy + dy + 16.0, r, ix("murk_light"))
        c.set(int(cx + dx), int(cy + dy + 15.0), ix("leaf_light"))
    px, py = cx + 5.0, cy + 12.0
    c.circle(px, py, 5.4, ix("outline"))
    c.circle(px, py, 4.6, ix("choco_dark"))
    c.circle(px - 1.0, py - 1.0, 3.2, ix("choco_mid"))
    c.circle(px - 1.8, py - 1.8, 1.6, ix("choco_light"))
    c.set(int(px - 2), int(py - 2), ix("cream"))
    c.set(int(px + 3), int(py + 2), ix("leaf_mid"))

    # The 安抚 parent's enemy: eyes shut, its will already leaving it. Painted
    # pale tan, because a brown lump next to a brown pellet is two lumps.
    hx, hy = cx - 14.0, cy + 11.0
    c.ellipse(hx, hy, 9.5, 6.5, ix("outline"))
    c.ellipse(hx, hy, 8.6, 5.7, ix("tan_dark"))
    c.ellipse(hx - 1.0, hy - 1.0, 7.2, 4.4, ix("tan_mid"))
    c.ellipse(hx - 2.0, hy - 2.4, 4.6, 2.4, ix("tan_light"))
    c.ellipse(hx + 7.0, hy - 4.0, 2.4, 1.7, ix("outline"))
    c.ellipse(hx + 7.0, hy - 4.0, 1.4, 0.9, ix("pink_mid"))
    c.ellipse(hx + 7.5, hy + 1.0, 2.2, 1.0, ix("outline"))
    c.hline(int(hx - 5), int(hx - 2), int(hy), ix("outline"))
    c.hline(int(hx + 1), int(hx + 4), int(hy + 1), ix("outline"))
    c.set(int(hx), int(hy + 3), ix("outline"))
    c.set(int(hx - 9.0), int(cy + 3.0), ix("brass_shadow"))
    c.set(int(hx - 10.0), int(cy + 1.0), ix("ui_gold"))
    c.set(int(hx - 11.0), int(cy - 0.5), ix("ui_gold_light"))


def em_beast_king(c, cx, cy, acc):
    """16 百兽盟主 -- 感化 + 利爪: a crowned capybara and its two empowered kin."""
    fuse_halo(c, cx, cy)

    # 利爪's slashes, heraldic now: a "V" flaring up behind the crown. Painted
    # in orange-under-gold, because plain gold has no contrast against the
    # wheat tint every fused card sits on.
    for side in (-1, 1):
        for k in range(2):
            x0 = cx + side * (8.0 + k * 5.0)
            x1 = cx + side * (21.0 + k * 2.0)
            for j in range(13):
                u = j / 12.0
                x = x0 + (x1 - x0) * u
                y = cy - 3.0 - 12.0 * u
                c.set(int(round(x)), int(round(y)) + 1, ix("orange_dark"))
                c.set(int(round(x)), int(round(y)), ix("ui_gold" if k else "ui_gold_light"))

    # The retinue: two small capybaras, each with a buff arrow over its head.
    for side in (-1, 1):
        ax, ay = cx + side * 20.0, cy + 15.0
        c.blob(ax, ay, 7.5, 5.5, ix("outline"), n=4.0)
        c.blob(ax, ay, 6.5, 4.6, ix("tan_dark"), n=4.0)
        c.blob(ax - side * 0.8, ay - 1.2, 4.8, 3.0, ix("tan_mid"), n=4.0)
        c.ellipse(ax - side * 6.0, ay - 3.5, 2.0, 1.4, ix("outline"))
        c.ellipse(ax - side * 6.0, ay - 3.5, 1.1, 0.7, ix("pink_mid"))
        c.set(int(ax - side * 2.5), int(ay - 1), ix("outline"))
        c.set(int(ax + side * 1.5), int(ay - 1), ix("outline"))
        c.set(int(ax + side * 1), int(ay + 2.5), ix("brn_shadow"))
        by = ay - 13.0
        fill_poly(c, [(ax - 4.6, by + 8.0), (ax, by), (ax + 4.6, by + 8.0)],
                  ix("outline"))
        fill_poly(c, [(ax - 2.8, by + 6.8), (ax, by + 2.2), (ax + 2.8, by + 6.8)],
                  ix("ember_yellow"))

    # The king.
    c.blob(cx, cy + 5.0, 11.5, 8.5, ix("outline"), n=4.0)
    c.blob(cx, cy + 5.0, 10.5, 7.5, ix("brn_mid2"), n=4.0)
    c.blob(cx, cy + 3.5, 8.8, 5.8, ix("brn_hi"), n=4.0)
    c.blob(cx - 1.0, cy + 1.5, 6.0, 3.2, ix("brn_hl"), n=4.0)
    c.blob(cx, cy + 9.0, 5.2, 2.8, ix("brn_belly"), n=4.0)
    c.hline(int(cx - 4), int(cx + 4), int(cy + 10), ix("tan_belly"))
    for side in (-1, 1):
        c.ellipse(cx + side * 10.0, cy + 1.0, 2.5, 1.7, ix("outline"))
        c.ellipse(cx + side * 10.0, cy + 1.0, 1.5, 0.9, ix("pink_mid"))
        c.hline(int(cx + side * 7), int(cx + side * 2), int(cy + 3), ix("outline"))
        c.set(int(cx + side * 4), int(cy + 2), ix("white_spark"))
    c.set(int(cx - 1), int(cy + 10), ix("outline"))
    c.set(int(cx + 1), int(cy + 10), ix("outline"))

    # The 感化 crown: narrow, and sitting on the skull's crown line rather than
    # ringing the whole head -- a wreath is not a crown.
    band = cy - 8.0
    for dx, hgt in ((-7, 7), (0, 10), (7, 7)):
        fill_poly(c, [(cx + dx - 3.4, band + 4.0), (cx + dx + 3.4, band + 4.0),
                      (cx + dx, band + 4.0 - hgt)], ix("outline"))
        fill_poly(c, [(cx + dx - 2.3, band + 3.6), (cx + dx + 2.3, band + 3.6),
                      (cx + dx, band + 4.4 - hgt)], ix("ui_gold"))
    c.round_rect(int(cx - 9), int(band), 18, 5, 1, ix("outline"))
    c.round_rect(int(cx - 8), int(band + 1), 16, 3, 1, ix("ui_gold"))
    c.hline(int(cx - 7), int(cx + 7), int(band + 1), ix("ui_gold_light"))
    c.hline(int(cx - 7), int(cx + 7), int(band + 4), ix("brass_dark"))
    for dx in (-6, 6):
        c.set(int(cx + dx), int(band + 2), ix("red_mid"))
    c.set(int(cx), int(band + 2), ix("cherry_mid"))
    spark(c, cx - 13, cy - 15, 2.6, ix("white_spark"))
    spark(c, cx + 13, cy - 15, 2.6, ix("ui_gold_light"))


def em_gale_siphon(c, cx, cy, acc):
    """17 疾风吸取 -- 快腿 + 磁吸: a vortex hauling loot into a sprinting paw."""
    fuse_halo(c, cx, cy)

    # One inward spiral, drawn as three arcs that hand over to each other: the
    # radius drops every segment, which is the only thing that makes a set of
    # arcs read as a vortex rather than as a ring.
    spiral = ((22.0, 18.5, 150.0, 250.0, 3.0, "cyan_mid"),
              (15.5, 13.0, 250.0, 350.0, 2.6, "cyan_light"),
              (9.5, 8.0, 350.0, 445.0, 2.0, "white_spark"))
    for rx, ry, a0, a1, sr, col in spiral:
        stamp_arc(c, cx, cy, rx, ry, a0, a1, sr, ix(col),
                  taper=lambda u: 0.85 - 0.25 * u)

    # The 快腿 paw, in gold: this is the card that owns both halves.
    px, py = cx, cy + 1.0
    c.blob(px, py + 3.5, 6.5, 4.6, ix("outline"), n=4.0)
    c.blob(px, py + 3.5, 5.6, 3.7, ix("ui_gold"), n=4.0)
    c.blob(px - 0.8, py + 2.6, 3.6, 2.0, ix("ui_gold_light"), n=4.0)
    for tx, ty, r in ((-5.5, -4.5, 2.8), (-1.8, -6.3, 3.0),
                      (1.8, -6.3, 3.0), (5.5, -4.5, 2.8)):
        c.circle(px + tx, py + ty, r, ix("outline"))
        c.circle(px + tx, py + ty, r - 0.7, ix("ui_gold"))
        c.circle(px + tx - 0.4, py + ty - 0.4, r - 1.3, ix("ui_gold_light"))

    # Loot riding the spiral in: 磁吸's coins, now with a vortex to ride.
    for deg, rr in ((214.0, 21.5), (288.0, 21.0), (352.0, 15.0), (415.0, 14.5)):
        rad = math.radians(deg)
        ccx, ccy = cx + rr * math.cos(rad), cy + rr * math.sin(rad)
        txx, tyy = math.sin(rad), -math.cos(rad)
        for k in (4.2, 5.8):
            c.set(int(round(ccx + txx * k)), int(round(ccy + tyy * k)),
                  ix("white_spark") if k < 5.0 else ix("cyan_light"))
        c.circle(ccx, ccy, 3.6, ix("outline"))
        c.circle(ccx, ccy, 2.9, ix("ui_gold"))
        c.circle(ccx - 0.6, ccy - 0.6, 1.5, ix("ui_gold_light"))
        c.set(int(ccx + 1.4), int(ccy + 1.4), ix("brass_dark"))


def em_geyser_mine(c, cx, cy, acc):
    """18 沸泉地雷 -- 水柱 + 便便: a buried trap, and what happens when it is stepped on."""
    fuse_halo(c, cx, cy)

    # Ground, banked so the plate sits in it rather than on a pancake.
    c.ellipse(cx, cy + 19.0, 25.0, 5.0, ix("outline"))
    c.ellipse(cx, cy + 19.0, 24.0, 4.2, ix("tan_deep"))
    c.ellipse(cx, cy + 18.6, 23.0, 3.4, ix("tan_mid"))
    for dx in (-18, -12, 9, 17):
        c.set(int(cx + dx), int(cy + 19), ix("tan_shadow"))

    # The mine: a studded steel plate, flat and low, with the 便便 payload
    # sitting proud of it -- a payload flush in the middle reads as an eye.
    mx, my = cx - 7.0, cy + 13.0
    c.ellipse(mx, my, 15.0, 5.2, ix("outline"))
    c.ellipse(mx, my, 14.0, 4.4, ix("steel_dark"))
    c.ellipse(mx, my - 0.6, 13.0, 3.6, ix("steel_mid"))
    c.ellipse(mx, my - 1.2, 11.5, 2.6, ix("steel_hi"))
    for dx in (-11, -6, 6, 11):
        c.set(int(mx + dx), int(my - 1), ix("steel_shadow"))
    px, py = mx - 8.0, my - 2.5
    c.circle(px, py, 4.8, ix("outline"))
    c.circle(px, py, 4.0, ix("choco_dark"))
    c.circle(px - 0.8, py - 0.8, 2.6, ix("choco_mid"))
    c.circle(px - 1.6, py - 1.6, 1.3, ix("choco_light"))
    c.set(int(px - 2), int(py - 2), ix("cream"))
    # The trigger, armed and visible.
    c.circle(mx + 9.0, my - 2.4, 2.4, ix("outline"))
    c.circle(mx + 9.0, my - 2.4, 1.5, ix("red_mid"))
    c.set(int(mx + 9), int(my - 4), ix("orange_mid"))

    # The eruption: 水柱's column, but as a jet -- pinched at the nozzle,
    # flaring into a plume where the charge goes off.
    top, bot = cy - 13.0, cy + 12.0
    for j in range(int(top), int(bot) + 1):
        t = min(1.0, max(0.0, (j - top) / (bot - top)))
        mid = cx - 7.0 + 2.6 * math.sin((1.0 - t) * 4.2)
        half = 2.4 + 6.0 * max(0.0, (1.0 - t) - 0.3) / 0.7
        c.hline(int(mid - half - 1), int(mid + half + 1), j, ix("cyan_dark"))
        c.hline(int(mid - half), int(mid + half), j, ix("cyan_mid"))
        c.hline(int(mid - half * 0.55), int(mid + half * 0.35), j, ix("cyan_light"))
        if half > 3.4:
            c.hline(int(mid - half * 0.22), int(mid + half * 0.08), j,
                    ix("white_spark"))
    for dx, dy, r in ((-5, -6, 2.2), (6, -2, 1.8), (1, 4, 2.0)):
        c.circle(cx - 7 + dx, cy + dy, r, ix("cyan_light"))
        c.set(int(cx - 7 + dx - 1), int(cy + dy - 1), ix("white_spark"))
    for side in (-1, 1):
        c.circle(cx - 7 + side * 12.0, cy + 10.0, 2.8, ix("cyan_dark"))
        c.circle(cx - 7 + side * 12.0, cy + 10.0, 2.0, ix("cyan_light"))
        c.set(int(cx - 7 + side * 13), int(cy + 6), ix("cyan_mid"))
        c.set(int(cx - 7 + side * 16), int(cy + 7), ix("cyan_light"))

    # The burst: eight long shards over eight short ones, so "double damage"
    # is legible without a number.
    bx, by = cx - 7.0, cy - 17.0
    for deg in range(0, 360, 45):
        spike(c, bx, by, deg, 5.0, 9.0, 2.6, "orange_dark")
    for deg in range(22, 360, 45):
        spike(c, bx, by, deg, 4.4, 7.0, 1.9, "ember_yellow")
    c.circle(bx, by, 4.2, ix("orange_mid"))
    c.circle(bx, by, 2.8, ix("ember_yellow"))
    c.circle(bx, by, 1.3, ix("white_spark"))
    for dx, dy in ((-9, -7), (9, -7), (11, 4)):
        c.set(int(bx + dx), int(by + dy), ix("red_mid"))

    # Footprints walking in on the right: the trap needs somebody to step on it.
    for fx, fy in ((cx + 14.0, cy + 20.0), (cx + 19.0, cy + 17.5)):
        c.ellipse(fx, fy, 2.6, 1.8, ix("brn_shadow"))
        for k in (-2, 0, 2):
            c.set(int(fx + k), int(fy - 2.4), ix("brn_shadow"))


def em_whirl_charge(c, cx, cy, acc):
    """19 回旋冲角 -- 铁尾 + 冲撞: a spinning charge that clears the ring."""
    fuse_halo(c, cx, cy)

    # Spin trails, each with an arrowhead so the sweep has a direction.
    for rx, ry, a0, a1, sr, col in ((20.5, 18.0, 150.0, 275.0, 2.8, "cyan_light"),
                                    (15.0, 13.5, 330.0, 452.0, 2.3, "white_spark")):
        stamp_arc(c, cx, cy, rx, ry, a0, a1, sr, ix(col),
                  taper=lambda u: 0.62 + 0.45 * math.sin(math.pi * u) ** 0.45)
        rad = math.radians(a1)
        ex = cx + rx * math.cos(rad)
        ey = cy + ry * math.sin(rad)
        tx, ty = -rx * math.sin(rad), ry * math.cos(rad)
        n = max(1e-3, math.hypot(tx, ty))
        tx, ty = tx / n, ty / n
        nx, ny = -ty, tx
        fill_poly(c, [(ex + tx * 5.0, ey + ty * 5.0),
                      (ex + nx * 3.4, ey + ny * 3.4),
                      (ex - nx * 3.4, ey - ny * 3.4)], ix(col))

    # The curled body, spinning: 冲撞's loaf rolled into a ball.
    bx, by = cx - 1.0, cy + 1.0
    c.blob(bx, by, 11.5, 10.0, ix("outline"), n=4.0)
    c.blob(bx, by, 10.5, 9.0, ix("brn_deep"), n=4.0)
    c.blob(bx - 0.5, by - 1.0, 9.0, 7.0, ix("brn_mid2"), n=4.0)
    c.blob(bx - 2.0, by - 3.0, 6.0, 4.0, ix("brn_hi"), n=4.0)
    for k in range(3):
        stamp_arc(c, bx - 1.0, by, 8.0 - k * 2.0, 6.5 - k * 1.6, 150.0, 250.0,
                  1.0, ix("brn_hl"))
    # Head tucked in at the front of the roll, snout clear of the ball.
    c.round_rect(int(cx + 5), int(cy + 4), 11, 7, 3, ix("outline"))
    c.round_rect(int(cx + 6), int(cy + 5), 10, 5, 2, ix("brn_belly"))
    c.hline(int(cx + 7), int(cx + 14), int(cy + 5), ix("tan_belly"))
    c.circle(cx + 15.0, cy + 7.0, 1.6, ix("outline"))
    c.hline(int(cx + 6), int(cx + 9), int(cy + 3), ix("outline"))
    c.set(int(cx + 8), int(cy + 2), ix("white_spark"))
    c.ellipse(cx + 4.0, cy - 6.5, 2.6, 1.8, ix("outline"))
    c.ellipse(cx + 4.0, cy - 6.5, 1.5, 1.0, ix("pink_mid"))

    # The horn: an ivory curl, thick where it leaves the skull and clear of the
    # body's silhouette -- 尖牙's teeth colour, because a horn has to look hard.
    # The four ridges are what stop it reading as a plain shell.
    for r, col in ((5.6, "outline"), (4.4, "tan_deep"), (3.0, "tan_light"),
                   (1.6, "cream")):
        stamp_arc(c, cx - 6.0, cy - 8.0, 9.0, 8.0, 352.0, 176.0, r, ix(col),
                  taper=lambda u: 1.0 - 0.68 * u)
    for deg in (22.0, 62.0, 102.0, 142.0):
        rad = math.radians(deg)
        c.circle(cx - 6.0 + 10.4 * math.cos(rad), cy - 8.0 + 9.4 * math.sin(rad),
                 0.9, ix("tan_deep"))

    # The 铁尾 parent, swung out on the end of the spin.
    c.round_rect(int(cx + 3), int(cy + 1), 11, 4, 2, ix("outline"))
    c.round_rect(int(cx + 4), int(cy + 2), 10, 2, 1, ix("steel_dark"))
    tx, ty = cx + 12.0, cy + 3.0
    c.circle(tx, ty, 5.5, ix("outline"))
    c.circle(tx, ty, 4.6, ix("steel_dark"))
    c.circle(tx - 0.8, ty - 0.8, 3.4, ix("steel_mid"))
    c.circle(tx - 1.6, ty - 1.6, 1.7, ix("steel_hi"))
    c.set(int(tx - 2), int(ty - 2), ix("steel_glint"))

    # What the sweep is doing to the crowd.
    for deg, rr in ((35.0, 19.5), (165.0, 20.5), (262.0, 19.5)):
        rad = math.radians(deg)
        ex = cx + rr * math.cos(rad)
        ey = cy + rr * math.sin(rad)
        c.circle(ex + math.cos(rad) * 4.0, ey + math.sin(rad) * 4.0, 2.4,
                 ix("white_spark"))
        c.circle(ex, ey, 3.6, ix("outline"))
        c.circle(ex, ey, 2.8, ix("grey_mid"))
        c.circle(ex - 0.6, ey - 0.6, 1.7, ix("grey_light"))
        c.set(int(ex - 1), int(ey), ix("outline"))
        c.set(int(ex + 1), int(ey - 1), ix("outline"))


# ---------------------------------------------------------------------------
# Card assembly
# ---------------------------------------------------------------------------

KIND_PHYS, KIND_MAGIC, KIND_SUPPORT, KIND_PASSIVE = 0, 1, 2, 3

KIND_ACCENT = {
    KIND_PHYS:    ("orange_mid", "orange_dark"),
    KIND_MAGIC:   ("cyan_light", "cyan_dark"),
    KIND_SUPPORT: ("green_spark", "heal_outer"),
    KIND_PASSIVE: ("ui_gold_light", "ui_gold"),
}

CARDS = [
    ("尖牙", KIND_PHYS,    em_fangs),
    ("铁尾", KIND_PHYS,    em_iron_tail),
    ("冲撞", KIND_PHYS,    em_charge),
    ("水柱", KIND_MAGIC,   em_water),
    ("灵水", KIND_MAGIC,   em_water_orbs),
    ("便便", KIND_MAGIC,   em_dung),
    ("安抚", KIND_SUPPORT, em_pacify),
    ("感化", KIND_SUPPORT, em_recruit),
    ("厚皮", KIND_PASSIVE, em_hide),
    ("快腿", KIND_PASSIVE, em_swift),
    ("磁吸", KIND_PASSIVE, em_magnet),
    ("利爪", KIND_PASSIVE, em_claws),

    # The fused eight. Order and ids are game.h CARD_FUSE_*; everything from
    # CARD_BASE_COUNT on is drawn as a gold plate (see build_card).
    ("掠食突袭", KIND_PHYS,    em_predator_rush),
    ("荆棘铁壁", KIND_PHYS,    em_thorn_bastion),
    ("潮汐双炮", KIND_MAGIC,   em_tidal_twin),
    ("恶臭香氛", KIND_MAGIC,   em_miasma),
    ("百兽盟主", KIND_SUPPORT, em_beast_king),
    ("疾风吸取", KIND_PASSIVE, em_gale_siphon),
    ("沸泉地雷", KIND_MAGIC,   em_geyser_mine),
    ("回旋冲角", KIND_PHYS,    em_whirl_charge),
]


def build_card(kind, emblem, name="", fuse=False):
    """One card: the shared physical frame plus one emblem.

    `fuse` swaps the frame's materials for the gold plate -- identical geometry,
    identical call order, brass rim, gold name band and the twin-ring badge --
    so the twelve base cards and the eight fused ones are the same object at two
    tiers. Nothing about the layout moves: the overlay the engine draws (level
    pips in the footer, the fusion mark over the top-right corner) lands exactly
    where it always did.
    """
    c = Canvas(CARD_W, CARD_H)
    acc, acc_deep = KIND_ACCENT[kind]
    if fuse:
        rim, rim_hi, rim_sh = "brass_mid", "brass_hi", "brass_shadow"
        band, band_strip, band_deep = "brass_dark", "ui_gold", "brass_shadow"
        win_dark, win_light = "brass_dark", "wheat_light"
        bracket, chip = "wheat_light", "ui_gold_light"
    else:
        rim, rim_hi, rim_sh = "ui_frame", "ui_frame_light", "wood_dark"
        band, band_strip, band_deep = "wood_dark", acc, acc_deep
        win_dark, win_light = "wood_dark", "tan_light"
        bracket, chip = "ui_gold_light", "ui_gold"

    # --- edge: 2px dark, then 2px lighter bevel -----------------------------
    c.round_rect(0, 0, CARD_W, CARD_H, CORNER_R, ix("outline"))
    c.round_rect(2, 2, CARD_W - 4, CARD_H - 4, 3, ix(rim))
    c.hline(3, CARD_W - 4, 2, ix(rim_hi))
    c.vline(2, 3, CARD_H - 4, ix(rim_hi))
    c.hline(3, CARD_W - 4, CARD_H - 3, ix(rim_sh))
    c.vline(CARD_W - 3, 5, CARD_H - 4, ix(rim_sh))
    if fuse:
        # The fourth rim pixel: a gold fillet inside the brass, which the base
        # plates do not carry.
        c.hline(4, CARD_W - 5, 3, ix("ui_gold"))
        c.vline(3, 4, CARD_H - 5, ix("ui_gold"))
        c.hline(4, CARD_W - 5, CARD_H - 4, ix("ui_gold"))
        c.vline(CARD_W - 4, 4, CARD_H - 5, ix("ui_gold"))

    # Golden card corner brackets
    for (cx0, cy0) in ((3, 3), (CARD_W - 6, 3), (3, CARD_H - 6), (CARD_W - 6, CARD_H - 6)):
        c.rect(cx0, cy0, 3, 3, ix(bracket))
        c.set(cx0 + 1, cy0 + 1, ix("white_spark"))

    # --- face: warm parchment cardstock (ZERO PURPLE!) ---
    c.round_rect(FACE_X, FACE_X, FACE_W, FACE_H, 2, ix("tan_belly"))

    # --- name band: deep wood banner with elemental accent ---
    c.rect(FACE_X, HEADER_Y0, FACE_W, HEADER_Y1 - HEADER_Y0 + 1, ix(band))
    c.rect(FACE_X, HEADER_Y1 - ACCENT_H + 1, FACE_W, ACCENT_H - 1, ix(band_strip))
    c.rect(FACE_X, HEADER_Y1, FACE_W, 1, ix(band_deep))

    # Decorative header gem in center of name band
    hx = CARD_W // 2
    if fuse:
        draw_fuse_badge(c)
    else:
        c.round_rect(hx - 8, HEADER_Y0 + 2, 16, 9, 2, ix("outline"))
        c.round_rect(hx - 7, HEADER_Y0 + 3, 14, 7, 1, ix(acc))
        c.round_rect(hx - 5, HEADER_Y0 + 4, 10, 4, 1, ix("white_spark"))

    # --- art window: a per-kind atmosphere, not a flat fill -----------------
    #
    # The window used to be a plain cream rectangle, so all twenty emblems
    # floated on the same empty page. Now each kind gets its own stage: a
    # dithered radial glow behind the emblem plus a sparse texture pass, all
    # in light tones so the emblem still reads first.
    c.rect(ART_X0, ART_Y0, ART_W, ART_H, ix(win_dark))
    stage = {
        KIND_PHYS:    ("tan_light", "tan_belly", "yellow_light"),
        KIND_MAGIC:   ("sea_light", "cream", "cyan_light"),
        KIND_SUPPORT: ("grn_light", "cream", "heal_inner"),
        KIND_PASSIVE: ("wheat_light", "cream", "ui_gold_light"),
    }[kind]
    cx_w = ART_X0 + ART_W / 2
    cy_w = ART_Y0 + ART_H / 2
    c.rect(ART_X0 + 1, ART_Y0 + 1, ART_W - 2, ART_H - 2, ix(stage[0]))
    c.dither_rect(ART_X0 + 1, ART_Y0 + 1, ART_W - 2, ART_H - 2,
                  stage[1], parity=1)
    # radial glow: bright core behind where the emblem sits
    for (rr, col, par) in ((26, stage[2], 0), (19, stage[1], 1),
                           (12, stage[2], 1)):
        c.dither_ellipse(cx_w, cy_w, rr, rr * 0.9, col, parity=par)
    # texture pass per kind, sparse enough to stay behind the emblem
    rng = random.Random(hash(name) & 0xFFFF)
    if kind == KIND_PHYS:
        for k in range(6):   # speed rays
            a = math.radians(k * 60 + 15)
            x0 = cx_w + 14 * math.cos(a)
            y0 = cy_w + 13 * math.sin(a)
            x1 = cx_w + 26 * math.cos(a)
            y1 = cy_w + 24 * math.sin(a)
            c.hline(int(x0), int(x1), int(y0), ix(stage[2]))
    elif kind == KIND_MAGIC:
        for k in range(7):   # rising bubbles
            bx = ART_X0 + 4 + rng.randrange(ART_W - 8)
            by = ART_Y0 + 6 + rng.randrange(ART_H - 12)
            r = 1 + rng.randrange(2)
            c.circle(bx, by, r, ix(stage[2]))
            c.set(bx - 1, by - 1, ix("white"))
    elif kind == KIND_SUPPORT:
        for k in range(6):   # drifting petals
            bx = ART_X0 + 4 + rng.randrange(ART_W - 8)
            by = ART_Y0 + 6 + rng.randrange(ART_H - 12)
            c.set(bx, by, ix(stage[2]))
            c.set(bx + 1, by, ix(stage[2]))
            c.set(bx, by + 1, ix("white"))
    else:
        for k in range(8):   # golden motes
            bx = ART_X0 + 4 + rng.randrange(ART_W - 8)
            by = ART_Y0 + 6 + rng.randrange(ART_H - 12)
            c.set(bx, by, ix("ui_gold"))
    c.hline(ART_X0, ART_X0 + ART_W - 1, ART_Y0, ix(win_dark))
    c.vline(ART_X0, ART_Y0 + 1, ART_Y0 + ART_H - 1, ix(win_dark))
    c.hline(ART_X0 + 1, ART_X0 + ART_W - 1, ART_Y0 + ART_H - 1, ix(win_light))
    c.vline(ART_X0 + ART_W - 1, ART_Y0 + 1, ART_Y0 + ART_H - 2, ix(win_light))

    # Inner window corner accents
    for wx, wy in ((ART_X0 + 2, ART_Y0 + 2), (ART_X0 + ART_W - 4, ART_Y0 + 2),
                   (ART_X0 + 2, ART_Y0 + ART_H - 4), (ART_X0 + ART_W - 4, ART_Y0 + ART_H - 4)):
        c.rect(wx, wy, 2, 2, ix(chip))

    before = list(c.px)
    emblem(c, EM_CX, EM_CY, acc)

    # Invariant: emblem must never escape the art window
    for i, v in enumerate(c.px):
        if v != before[i]:
            x, y = i % CARD_W, i // CARD_W
            assert (ART_X0 + 1 <= x <= ART_X0 + ART_W - 2
                    and ART_Y0 + 1 <= y <= ART_Y0 + ART_H - 2), \
                f"emblem {name} pixel at ({x},{y}) escapes the art window"

    # --- pip band: footer ---
    c.rect(FACE_X, FOOTER_Y0, FACE_W, FOOTER_H, ix("wood_dark"))
    c.hline(FACE_X, FACE_X + FACE_W - 1, FOOTER_Y0,
            ix("ui_gold") if fuse else ix(acc))
    return c


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

def check_card(c, index):
    """Assert invariants for the C engine."""
    zeros = 0
    for i, v in enumerate(c.px):
        assert 0 <= v <= gc.PAL[gc.PALETTE_NAMES[-1]], \
            f"card {index}: pixel {i} is palette index {v} (out of range)"
        if v:
            continue
        zeros += 1
        x, y = i % CARD_W, i // CARD_W
        in_corner = ((x < CORNER_R or x >= CARD_W - CORNER_R)
                     and (y < CORNER_R or y >= CARD_H - CORNER_R))
        assert in_corner, \
            f"card {index}: transparent pixel at ({x},{y}) is not a corner"

    probes = ((CARD_W // 2, CARD_H // 2),
              (CARD_W // 2, 0), (CARD_W // 2, CARD_H - 1),
              (0, CARD_H // 2), (CARD_W - 1, CARD_H // 2))
    for x, y in probes:
        assert c.get(x, y) != 0, \
            f"card {index}: transparent pixel at probe ({x},{y})"
    return zeros


def card_names_from_c(root):
    path = os.path.join(root, "source", "globals.c")
    if not os.path.exists(path):
        return None
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    parts = text.split("g_card_defs[CARD_COUNT] = {", 1)
    if len(parts) != 2:
        return None
    body = parts[1].split("};", 1)[0]
    return re.findall(r'\{\s*"([^"]+)"', body)


def install_label_font():
    from PIL import ImageDraw, ImageFont
    for path in (r"C:\Windows\Fonts\simhei.ttf",
                 r"C:\Windows\Fonts\msyh.ttc",
                 r"C:\Windows\Fonts\simsun.ttc"):
        if not os.path.exists(path):
            continue
        try:
            ImageDraw.ImageDraw.font = ImageFont.truetype(path, 10)
            return True
        except OSError:
            continue
    return False


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "graphics")
    if not os.path.isabs(out):
        out = os.path.join(root, out)
    os.makedirs(out, exist_ok=True)

    names = card_names_from_c(root)
    if names is not None:
        assert names == [n for n, _k, _e in CARDS], \
            f"card order drifted from source/globals.c: {names}"

    cards = [build_card(kind, emblem, name, fuse=(i >= CARD_BASE_COUNT))
             for i, (name, kind, emblem) in enumerate(CARDS)]
    assert len(cards) == CARD_COUNT, \
        f"expected {CARD_COUNT} cards, got {len(cards)}"

    blob = bytearray()
    for c in cards:
        blob += c.to_bmp8()
    expect = CARD_W * CARD_H * CARD_COUNT
    assert expect == 138240, "card geometry changed; update the C side too"
    assert len(blob) == expect, \
        f"atlas is {len(blob)} bytes, expected {expect}"

    zeros = [check_card(c, i) for i, c in enumerate(cards)]
    assert all(z > 0 for z in zeros), "rounded corners produced no cut pixels"
    assert len(set(zeros)) == 1, \
        f"cards have different corner cuts: {sorted(set(zeros))}"

    path = os.path.join(out, "card_atlas.bin")
    gc.write_bin(path, bytes(blob))
    print(f"card_atlas: {len(cards)} cards ({CARD_BASE_COUNT} base + "
          f"{len(cards) - CARD_BASE_COUNT} fused), {CARD_W}x{CARD_H}, "
          f"{len(blob)} bytes ({len(blob) // len(cards)} B/card), "
          f"{zeros[0]} transparent px/card")
    print("card_atlas ->", path)

    labels = [f"{i} {name}" + ("*" if i >= CARD_BASE_COUNT else "")
              for i, (name, _k, _e) in enumerate(CARDS)]
    if not install_label_font():
        labels = [f"{i}" for i in range(len(CARDS))]
    gc.preview_scale(cards, os.path.join(out, "preview_cards.png"),
                     scale=3, cols=5, labels=labels,
                     title="Capybara Skill Cards 0..19 (12 base + 8 fused *)")


if __name__ == "__main__":
    main()
