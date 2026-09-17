"""
Skill card art: the twelve cards of the level-up picker, 72x96 each.

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

Outputs (both into graphics/ unless an output dir is given on the command line):
  card_atlas.bin     12 cards x 72x96, 8bpp row-major, no tiling, one byte per
                     pixel, in card-index order 0..11 -> 82944 bytes exactly
  preview_cards.png  contact sheet at 3x, labelled with the card names
"""

import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_common as gc
from gen_common import Canvas, ix


CARD_W, CARD_H = 72, 96
CARD_COUNT = 12                 # game.h CARD_COUNT; the atlas is indexed by id
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
]


def build_card(kind, emblem, name=""):
    """One card: the shared physical frame plus one emblem."""
    c = Canvas(CARD_W, CARD_H)
    acc, acc_deep = KIND_ACCENT[kind]

    # --- edge: 2px dark, then 2px lighter bevel -----------------------------
    c.round_rect(0, 0, CARD_W, CARD_H, CORNER_R, ix("outline"))
    c.round_rect(2, 2, CARD_W - 4, CARD_H - 4, 3, ix("ui_frame"))
    c.hline(3, CARD_W - 4, 2, ix("ui_frame_light"))
    c.vline(2, 3, CARD_H - 4, ix("ui_frame_light"))
    c.hline(3, CARD_W - 4, CARD_H - 3, ix("wood_dark"))
    c.vline(CARD_W - 3, 5, CARD_H - 4, ix("wood_dark"))

    # Golden card corner brackets
    for (cx0, cy0) in ((3, 3), (CARD_W - 6, 3), (3, CARD_H - 6), (CARD_W - 6, CARD_H - 6)):
        c.rect(cx0, cy0, 3, 3, ix("ui_gold_light"))
        c.set(cx0 + 1, cy0 + 1, ix("white_spark"))

    # --- face: warm parchment cardstock (ZERO PURPLE!) ---
    c.round_rect(FACE_X, FACE_X, FACE_W, FACE_H, 2, ix("tan_belly"))

    # --- name band: deep wood banner with elemental accent ---
    c.rect(FACE_X, HEADER_Y0, FACE_W, HEADER_Y1 - HEADER_Y0 + 1, ix("wood_dark"))
    c.rect(FACE_X, HEADER_Y1 - ACCENT_H + 1, FACE_W, ACCENT_H - 1, ix(acc))
    c.rect(FACE_X, HEADER_Y1, FACE_W, 1, ix(acc_deep))

    # Decorative header gem in center of name band
    hx = CARD_W // 2
    c.round_rect(hx - 8, HEADER_Y0 + 2, 16, 9, 2, ix("outline"))
    c.round_rect(hx - 7, HEADER_Y0 + 3, 14, 7, 1, ix(acc))
    c.round_rect(hx - 5, HEADER_Y0 + 4, 10, 4, 1, ix("white_spark"))

    # --- art window: clean warm ivory arena ---
    c.rect(ART_X0, ART_Y0, ART_W, ART_H, ix("cream"))
    c.hline(ART_X0, ART_X0 + ART_W - 1, ART_Y0, ix("wood_dark"))
    c.vline(ART_X0, ART_Y0 + 1, ART_Y0 + ART_H - 1, ix("wood_dark"))
    c.hline(ART_X0 + 1, ART_X0 + ART_W - 1, ART_Y0 + ART_H - 1, ix("tan_light"))
    c.vline(ART_X0 + ART_W - 1, ART_Y0 + 1, ART_Y0 + ART_H - 2, ix("tan_light"))

    # Inner window corner accents
    for wx, wy in ((ART_X0 + 2, ART_Y0 + 2), (ART_X0 + ART_W - 4, ART_Y0 + 2),
                   (ART_X0 + 2, ART_Y0 + ART_H - 4), (ART_X0 + ART_W - 4, ART_Y0 + ART_H - 4)):
        c.rect(wx, wy, 2, 2, ix("ui_gold"))

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
    c.hline(FACE_X, FACE_X + FACE_W - 1, FOOTER_Y0, ix(acc))
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

    cards = [build_card(kind, emblem, name) for name, kind, emblem in CARDS]
    assert len(cards) == CARD_COUNT, \
        f"expected {CARD_COUNT} cards, got {len(cards)}"

    blob = bytearray()
    for c in cards:
        blob += c.to_bmp8()
    expect = CARD_W * CARD_H * CARD_COUNT
    assert expect == 82944, "card geometry changed; update the C side too"
    assert len(blob) == expect, \
        f"atlas is {len(blob)} bytes, expected {expect}"

    zeros = [check_card(c, i) for i, c in enumerate(cards)]
    assert all(z > 0 for z in zeros), "rounded corners produced no cut pixels"
    assert len(set(zeros)) == 1, \
        f"cards have different corner cuts: {sorted(set(zeros))}"

    path = os.path.join(out, "card_atlas.bin")
    gc.write_bin(path, bytes(blob))
    print(f"card_atlas: {len(cards)} cards, {CARD_W}x{CARD_H}, "
          f"{len(blob)} bytes ({len(blob) // len(cards)} B/card), "
          f"{zeros[0]} transparent px/card")
    print("card_atlas ->", path)

    labels = [f"{i} {name}" for i, (name, _k, _e) in enumerate(CARDS)]
    if not install_label_font():
        labels = [f"{i}" for i in range(len(CARDS))]
    gc.preview_scale(cards, os.path.join(out, "preview_cards.png"),
                     scale=3, cols=6, labels=labels,
                     title="Capybara Skill Cards 0..11")


if __name__ == "__main__":
    main()
