"""
gen_heroes.py -- Procedural generation of 32x32 Zen Chubby Capybaras (佛系胖水豚).
Aesthetic: Classic GBC (Capybara Village) & Pokémon Gen 2/3 handheld pixel art style.

Updated Proportions:
  - "身体大些头小些" (Dominant chunky bread-loaf body, compact cute head)
  - "四肢可以不用如此显眼" (Unobtrusive stubby little paws tucked low under belly)
  - "佛系一字横线眼" (Zen calm slit eyes `— —`)

Characters:
  hero_m  P1 阿棕 (Warm Chestnut Brown + Mini Yuzu on head)
  hero_f  P2 阿奶 (Soft Golden Milk Tea Tan + Green Sprout on head)
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
    ("attack", 4),
    ("hurt", 2),
]

HEROES = [
    ("hero_m", "阿棕", {
        "outline": "outline",
        "fur_main": "brn_mid",
        "fur_dark": "brn_dark",
        "fur_light": "brn_light",
        "fur_belly": "brn_belly",
        "ear_inner": "brn_shadow",
        "snout_col": "brn_light",
        "nostril": "black",
        "eye_col": "black",
        "incisor": "white",
        "incisor_edge": "yellow_dark",
        "item": "yuzu",
    }),
    ("hero_f", "阿奶", {
        "outline": "outline",
        "fur_main": "tan_mid",
        "fur_dark": "tan_dark",
        "fur_light": "tan_light",
        "fur_belly": "cream",
        "ear_inner": "tan_shadow",
        "snout_col": "tan_light",
        "nostril": "black",
        "eye_col": "black",
        "incisor": "white",
        "incisor_edge": "yellow_dark",
        "item": "sprout",
    }),
]


def draw_accessory(c, th, hx, hy, direction):
    """Small, snug head accessory."""
    if th["item"] == "yuzu":
        yx = hx
        yy = hy - 3
        c.rect(yx - 2, yy, 4, 3, ix("orange_mid"))
        c.set(yx - 1, yy + 1, ix("orange_dark"))
        c.set(yx + 1, yy, ix("yellow_light"))
        c.set(yx, yy - 1, ix("grn_mid"))
        c.set(yx + 1, yy - 1, ix("grn_light"))
    elif th["item"] == "sprout":
        sx = hx
        sy = hy - 3
        c.set(sx, sy + 2, ix("grn_dark"))
        c.set(sx, sy + 1, ix("grn_mid"))
        c.set(sx - 1, sy, ix("leaf_light"))
        c.set(sx + 1, sy, ix("grn_light"))


def render_capybara(th, direction, anim, f):
    c = Canvas(W, H)
    out_col = ix(th["outline"])
    fur_m = ix(th["fur_main"])
    fur_d = ix(th["fur_dark"])
    fur_l = ix(th["fur_light"])
    fur_b = ix(th["fur_belly"])
    ear_in = ix(th["ear_inner"])
    eye_c = ix(th["eye_col"])
    nos_c = ix(th["nostril"])
    inc_c = ix(th["incisor"])

    # -------------------------------------------------------------------------
    # 1. RIGHT VIEW (SIDE PROFILE)
    # -------------------------------------------------------------------------
    if direction == "right":
        bob = 0
        step_f = 0
        step_b = 0
        bite_open = False
        bite_snap = False
        head_lunge = 0

        if anim == "idle":
            bob = -1 if f in (1, 2) else 0
        elif anim == "walk":
            bobs = [0, -1, 0, 0, -1, 0]
            bob = bobs[f % 6]
            steps = [(1, -1), (1, -1), (0, 0), (-1, 1), (-1, 1), (0, 0)]
            step_f, step_b = steps[f % 6]
        elif anim == "attack":
            if f == 0:
                bob = -1
                bite_open = True
                head_lunge = -1
            elif f in (1, 2):
                bob = 1
                bite_snap = True
                head_lunge = 2
            elif f == 3:
                bob = 0
        if anim == "hurt":
            if f == 0:
                bob = -2
                head_lunge = -2
            else:
                bob = 1
                head_lunge = 0

        by = 10 + bob
        hy = 10 + bob
        hx = 15 + head_lunge

        # Subtle far-side paw tips
        c.rect(8 + step_b, by + 15, 3, 2, fur_d)
        c.rect(19 + step_f, by + 15, 3, 2, fur_d)

        # Massive round loaf body
        c.round_rect(2, by + 2, 21, 14, 5, fur_m)
        c.rect(12, by + 1, 7, 6, fur_m)

        c.rect(4, by + 12, 17, 4, fur_d)
        c.round_rect(6, by + 10, 12, 4, 1, fur_b)
        c.hline(5, 16, by + 2, fur_l)
        c.hline(12, 17, by + 1, fur_l)

        # Tail stub
        c.set(1, by + 9, fur_d)
        c.set(1, by + 10, fur_d)

        # Head base + Boxy extended muzzle (jutting out to hx+12)
        c.round_rect(hx, hy, 10, 8, 2, fur_m)
        c.rect(hx + 5, hy + 1, 8, 7, fur_m)
        c.rect(hx + 8, hy + 2, 4, 6, fur_l)

        c.hline(hx + 1, hx + 10, hy, fur_l)
        c.hline(hx + 4, hx + 12, hy + 8, fur_d)

        ear_twitch = -1 if (anim == "idle" and f == 2) else 0
        c.rect(hx - 1, hy - 1 + ear_twitch, 2, 3, fur_m)
        c.set(hx - 1, hy + ear_twitch, ear_in)

        # High front nostril
        c.rect(hx + 11, hy + 2, 2, 2, nos_c)

        # Eyes: Zen slit eye, NO round white highlights!
        if anim == "hurt":
            if f == 0:
                # Wide panic shocked eye
                c.rect(hx + 3, hy + 2, 3, 3, ix("white"))
                c.set(hx + 4, hy + 3, eye_c)
            else:
                # Squeezed shut `>` eye
                c.set(hx + 3, hy + 2, eye_c)
                c.set(hx + 4, hy + 3, eye_c)
                c.set(hx + 5, hy + 3, eye_c)
                c.set(hx + 4, hy + 4, eye_c)
                c.set(hx + 3, hy + 5, eye_c)
        elif bite_open:
            c.hline(hx + 3, hx + 5, hy + 2, eye_c)
        else:
            c.hline(hx + 3, hx + 5, hy + 3, eye_c)
            c.hline(hx + 3, hx + 5, hy + 2, fur_d)

        if anim == "hurt":
            if f == 0:
                # Gaping screaming mouth with buckteeth!
                c.rect(hx + 8, hy + 5, 4, 4, ix("outline"))
                c.rect(hx + 9, hy + 6, 2, 2, ix("pink_dark"))
                c.set(hx + 8, hy + 5, inc_c)
                c.set(hx + 9, hy + 5, inc_c)
                c.set(hx + 8, hy + 6, ix("yellow_dark"))
                # Flying panic sweat drops!
                c.set(hx + 2, hy - 4, ix("cyan_light"))
                c.rect(hx + 1, hy - 3, 2, 2, ix("cyan_mid"))
                c.set(hx + 7, hy - 2, ix("cyan_light"))
            else:
                # Shivering mouth & tear
                c.set(hx + 8, hy + 6, eye_c)
                c.set(hx + 9, hy + 7, eye_c)
                c.set(hx + 10, hy + 6, eye_c)
                c.set(hx + 11, hy + 7, eye_c)
                c.set(hx + 5, hy + 5, ix("cyan_light"))
                c.set(hx + 5, hy + 6, ix("cyan_mid"))
        elif bite_open:
            c.rect(hx + 9, hy + 6, 3, 3, inc_c)
            c.set(hx + 11, hy + 6, nos_c)
        elif bite_snap:
            c.rect(hx + 9, hy + 5, 3, 4, inc_c)
            c.set(hx + 10, hy + 5, ix(th["incisor_edge"]))
            c.set(hx + 12, hy + 4, ix("white_spark"))
            c.set(hx + 13, hy + 4, ix("yellow_light"))
            c.set(hx + 12, hy + 7, ix("white_spark"))
            if f == 2:
                c.set(hx + 14, hy + 6, ix("white_spark"))
        else:
            c.hline(hx + 8, hx + 11, hy + 7, fur_d)

        # Unobtrusive stubby paws
        c.round_rect(16 - step_f, by + 14, 4, 3, 1, fur_m)
        c.hline(16 - step_f, 18 - step_f, by + 16, fur_l)
        c.round_rect(5 - step_b, by + 14, 4, 3, 1, fur_m)
        c.hline(5 - step_b, 7 - step_b, by + 16, fur_l)

        draw_accessory(c, th, hx + 3, hy, "right")

    # -------------------------------------------------------------------------
    # 2. DOWN VIEW (FRONT / SOUTH)
    # -------------------------------------------------------------------------
    elif direction == "down":
        bob = 0
        waddle = 0
        bite_open = False
        bite_snap = False

        if anim == "idle":
            bob = -1 if f in (1, 2) else 0
        elif anim == "walk":
            waddles = [-1, -1, 0, 1, 1, 0]
            bobs = [0, -1, 0, 0, -1, 0]
            waddle = waddles[f % 6]
            bob = bobs[f % 6]
        elif anim == "attack":
            if f == 0:
                bob = -1
                bite_open = True
            elif f in (1, 2):
                bob = 1
                bite_snap = True
            elif f == 3:
                bob = 0
        elif anim == "hurt":
            bob = -2 if f == 0 else 1

        cx = 16 + waddle
        by = 10 + bob
        hy = 8 + bob

        # Massive round loaf body
        c.round_rect(cx - 11, by + 2, 22, 16, 6, fur_m)
        c.round_rect(cx - 10, by + 12, 20, 6, 4, fur_d)
        c.round_rect(cx - 7, by + 7, 14, 10, 3, fur_b)

        # Compact head
        c.round_rect(cx - 7, hy, 14, 9, 2, fur_m)
        c.hline(cx - 5, cx + 4, hy, fur_l)

        ear_twitch = -1 if (anim == "idle" and f == 2) else 0
        c.rect(cx - 8, hy + ear_twitch, 2, 3, fur_m)
        c.set(cx - 8, hy + 1 + ear_twitch, ear_in)
        c.rect(cx + 6, hy + ear_twitch, 2, 3, fur_m)
        c.set(cx + 7, hy + 1 + ear_twitch, ear_in)

        # Eyes: Zen slit eye, NO round white highlights!
        if anim == "hurt":
            if f == 0:
                # Shocked wide eyes
                c.rect(cx - 6, hy + 2, 3, 3, ix("white"))
                c.set(cx - 5, hy + 3, eye_c)
                c.rect(cx + 3, hy + 2, 3, 3, ix("white"))
                c.set(cx + 4, hy + 3, eye_c)
                # Flying sweat drops
                c.rect(cx - 9, hy - 2, 2, 2, ix("cyan_mid"))
                c.set(cx - 9, hy - 3, ix("cyan_light"))
                c.rect(cx + 8, hy - 2, 2, 2, ix("cyan_mid"))
                c.set(cx + 9, hy - 3, ix("cyan_light"))
            else:
                # Squeezed `> <` eyes
                c.set(cx - 6, hy + 3, eye_c)
                c.set(cx - 5, hy + 4, eye_c)
                c.set(cx - 6, hy + 5, eye_c)
                c.set(cx + 5, hy + 3, eye_c)
                c.set(cx + 4, hy + 4, eye_c)
                c.set(cx + 5, hy + 5, eye_c)
                # Tear drops
                c.set(cx - 7, hy + 3, ix("cyan_light"))
                c.set(cx - 7, hy + 4, ix("cyan_mid"))
                c.set(cx + 6, hy + 3, ix("cyan_light"))
                c.set(cx + 6, hy + 4, ix("cyan_mid"))
        elif bite_open:
            c.hline(cx - 5, cx - 4, hy + 3, eye_c)
            c.hline(cx + 3, cx + 4, hy + 3, eye_c)
        else:
            c.hline(cx - 5, cx - 4, hy + 4, eye_c)
            c.hline(cx - 5, cx - 4, hy + 3, fur_d)
            c.hline(cx + 3, cx + 4, hy + 4, eye_c)
            c.hline(cx + 3, cx + 4, hy + 3, fur_d)

        c.round_rect(cx - 3, hy + 3, 6, 6, 2, fur_l)
        c.set(cx - 2, hy + 6, nos_c)
        c.set(cx + 1, hy + 6, nos_c)
        c.hline(cx - 2, cx + 1, hy + 8, fur_d)

        # Front paws on lower chest
        c.round_rect(cx - 6, by + 7, 3, 4, 1, fur_m)
        c.set(cx - 6, by + 10, fur_l)
        c.round_rect(cx + 3, by + 7, 3, 4, 1, fur_m)
        c.set(cx + 5, by + 10, fur_l)

        # Unobtrusive stubby foot pads
        paw_ly = by + 16 + (1 if waddle < 0 else 0)
        paw_ry = by + 16 + (1 if waddle > 0 else 0)
        c.rect(cx - 8, paw_ly, 4, 2, fur_m)
        c.set(cx - 7, paw_ly + 1, fur_l)
        c.rect(cx + 4, paw_ry, 4, 2, fur_m)
        c.set(cx + 5, paw_ry + 1, fur_l)

        if anim == "hurt":
            if f == 0:
                # Open screaming mouth
                c.rect(cx - 2, hy + 6, 4, 3, ix("outline"))
                c.rect(cx - 1, hy + 7, 2, 2, ix("pink_dark"))
                c.set(cx - 1, hy + 6, inc_c)
                c.set(cx, hy + 6, inc_c)
            else:
                # Shivering flat mouth
                c.hline(cx - 2, cx + 1, hy + 7, eye_c)
        elif bite_open:
            c.rect(cx - 2, hy + 8, 4, 3, inc_c)
            c.set(cx - 1, hy + 8, ix(th["incisor_edge"]))
            c.set(cx, hy + 8, ix(th["incisor_edge"]))
        elif bite_snap:
            c.rect(cx - 2, hy + 7, 4, 4, inc_c)
            c.set(cx - 1, hy + 7, ix(th["incisor_edge"]))
            c.set(cx, hy + 7, ix(th["incisor_edge"]))
            c.set(cx - 4, hy + 10, ix("white_spark"))
            c.set(cx + 4, hy + 10, ix("white_spark"))
            if f == 2:
                c.set(cx - 5, hy + 9, ix("yellow_light"))
                c.set(cx + 5, hy + 9, ix("yellow_light"))

        draw_accessory(c, th, cx, hy, "down")

    # -------------------------------------------------------------------------
    # 3. UP VIEW (BACK / NORTH)
    # -------------------------------------------------------------------------
    elif direction == "up":
        bob = 0
        waddle = 0
        if anim == "walk":
            waddles = [-1, -1, 0, 1, 1, 0]
            bobs = [0, -1, 0, 0, -1, 0]
            waddle = waddles[f % 6]
            bob = bobs[f % 6]
        elif anim == "hurt":
            bob = -2 if f == 0 else 1

        cx = 16 + waddle
        by = 10 + bob
        hy = 8 + bob

        if anim == "attack":
            if f == 0:
                bob = 1
                by = 10 + bob
                hy = 8 + bob
            elif f in (1, 2):
                bob = -2
                by = 10 + bob
                hy = 8 + bob
                c.rect(cx - 2, hy - 2, 4, 2, inc_c)
                c.set(cx - 1, hy - 2, ix(th["incisor_edge"]))
                c.set(cx, hy - 2, ix(th["incisor_edge"]))
                c.set(cx - 4, hy - 4, ix("white_spark"))
                c.set(cx + 4, hy - 4, ix("white_spark"))
                if f == 2:
                    c.set(cx - 5, hy - 5, ix("yellow_light"))
                    c.set(cx + 5, hy - 5, ix("yellow_light"))
            elif f == 3:
                bob = 0
                by = 10 + bob
                hy = 8 + bob

        # Massive round bread-loaf rump
        c.round_rect(cx - 11, by + 1, 22, 17, 6, fur_m)
        c.round_rect(cx - 10, by + 12, 20, 6, 4, fur_d)
        c.round_rect(cx - 7, by + 3, 14, 7, 3, fur_l)

        # Tail stub
        c.rect(cx - 1, by + 13, 2, 2, fur_d)

        # Compact head back
        c.round_rect(cx - 7, hy, 14, 7, 2, fur_m)
        c.hline(cx - 5, cx + 4, hy, fur_l)

        ear_twitch = -1 if (anim == "idle" and f == 2) else 0
        c.rect(cx - 8, hy + ear_twitch, 2, 3, fur_m)
        c.rect(cx + 6, hy + ear_twitch, 2, 3, fur_m)

        c.rect(cx - 8, by + 16, 4, 2, fur_d)
        c.rect(cx + 4, by + 16, 4, 2, fur_d)

        if anim == "hurt":
            if f == 0:
                c.set(cx - 6, hy - 3, ix("cyan_light"))
                c.rect(cx - 7, hy - 2, 2, 2, ix("cyan_mid"))
                c.set(cx + 6, hy - 3, ix("cyan_light"))
                c.rect(cx + 5, hy - 2, 2, 2, ix("cyan_mid"))
            else:
                c.set(cx - 8, hy + 1, ix("cyan_light"))
                c.set(cx + 7, hy + 1, ix("cyan_light"))

        draw_accessory(c, th, cx, hy, "up")


    # -------------------------------------------------------------------------
    # 4. DOWN-RIGHT (3/4 FRONT VIEW)
    # -------------------------------------------------------------------------
    elif direction == "down_right":
        bob = 0
        step_f = 0
        step_b = 0
        bite_open = False
        bite_snap = False
        head_lunge = 0

        if anim == "idle":
            bob = -1 if f in (1, 2) else 0
        elif anim == "walk":
            bobs = [0, -1, 0, 0, -1, 0]
            bob = bobs[f % 6]
            steps = [(1, -1), (1, -1), (0, 0), (-1, 1), (-1, 1), (0, 0)]
            step_f, step_b = steps[f % 6]
        elif anim == "attack":
            if f == 0:
                bob = -1
                bite_open = True
                head_lunge = -1
            elif f in (1, 2):
                bob = 1
                bite_snap = True
                head_lunge = 2
            elif f == 3:
                bob = 0
        elif anim == "hurt":
            bob = -2 if f == 0 else 1
            head_lunge = -2 if f == 0 else 0

        by = 10 + bob
        hy = 9 + bob
        hx = 14 + head_lunge

        # Body: 3/4 angled bread loaf
        c.round_rect(3, by + 2, 21, 15, 5, fur_m)
        c.rect(13, by + 1, 8, 7, fur_m)
        c.rect(5, by + 13, 17, 4, fur_d)
        c.round_rect(7, by + 9, 13, 6, 2, fur_b)
        c.hline(6, 17, by + 2, fur_l)

        # Tail stub
        c.set(2, by + 10, fur_d)

        # Head: 3/4 angle
        c.round_rect(hx - 2, hy, 12, 8, 2, fur_m)
        c.rect(hx + 4, hy + 2, 6, 6, fur_m)
        c.rect(hx + 6, hy + 3, 4, 5, fur_l)
        c.hline(hx, hx + 9, hy, fur_l)

        # Ears: two ears visible in 3/4 view
        c.rect(hx - 4, hy - 1, 2, 3, fur_m)
        c.set(hx - 4, hy, ear_in)
        c.rect(hx + 1, hy - 2, 2, 3, fur_m)
        c.set(hx + 1, hy - 1, ear_in)

        # High front-right nostril
        c.rect(hx + 9, hy + 3, 2, 2, nos_c)

        # Eyes: 3/4 perspective (prominent near eye facing camera, serene zen slit)
        if anim == "hurt":
            if f == 0:
                c.rect(hx + 3, hy + 2, 3, 3, ix("white"))
                c.set(hx + 4, hy + 3, eye_c)
                # Panic sweat
                c.set(hx + 1, hy - 4, ix("cyan_light"))
                c.rect(hx, hy - 3, 2, 2, ix("cyan_mid"))
            else:
                c.set(hx + 3, hy + 2, eye_c)
                c.set(hx + 4, hy + 3, eye_c)
                c.set(hx + 5, hy + 3, eye_c)
                c.set(hx + 4, hy + 4, eye_c)
                c.set(hx + 5, hy + 5, ix("cyan_light"))
        elif bite_open:
            c.hline(hx + 3, hx + 5, hy + 2, eye_c)
        else:
            c.hline(hx + 3, hx + 5, hy + 3, eye_c)
            c.hline(hx + 3, hx + 5, hy + 2, fur_d)

        # Mouth / teeth
        if anim == "hurt":
            if f == 0:
                c.rect(hx + 5, hy + 5, 4, 3, ix("outline"))
                c.rect(hx + 6, hy + 6, 2, 2, ix("pink_dark"))
                c.set(hx + 5, hy + 5, inc_c)
                c.set(hx + 6, hy + 5, inc_c)
            else:
                c.hline(hx + 5, hx + 8, hy + 6, eye_c)
        elif bite_open:
            c.rect(hx + 6, hy + 6, 3, 3, inc_c)
        elif bite_snap:
            c.rect(hx + 6, hy + 5, 3, 4, inc_c)
            c.set(hx + 7, hy + 5, ix(th["incisor_edge"]))
            c.set(hx + 9, hy + 4, ix("white_spark"))
        else:
            c.hline(hx + 5, hx + 8, hy + 7, fur_d)

        # Paws
        c.round_rect(16 + step_f, by + 14, 4, 3, 1, fur_m)
        c.hline(16 + step_f, 18 + step_f, by + 16, fur_l)
        c.round_rect(6 + step_b, by + 14, 4, 3, 1, fur_m)
        c.hline(6 + step_b, 8 + step_b, by + 16, fur_l)

        draw_accessory(c, th, hx + 1, hy, "right")

    # -------------------------------------------------------------------------
    # 5. UP-RIGHT (3/4 BACK VIEW)
    # -------------------------------------------------------------------------
    elif direction == "up_right":
        bob = 0
        waddle = 0
        step_f = 0
        step_b = 0
        if anim == "walk":
            waddles = [-1, -1, 0, 1, 1, 0]
            bobs = [0, -1, 0, 0, -1, 0]
            waddle = waddles[f % 6]
            bob = bobs[f % 6]
            steps = [(1, -1), (1, -1), (0, 0), (-1, 1), (-1, 1), (0, 0)]
            step_f, step_b = steps[f % 6]
        elif anim == "hurt":
            bob = -2 if f == 0 else 1

        by = 10 + bob
        hy = 8 + bob
        hx = 13

        # 3/4 Back View: chubby rump visible on left, back of head on right
        c.round_rect(3, by + 1, 21, 16, 5, fur_m)
        c.rect(13, by + 1, 7, 7, fur_m)
        c.rect(4, by + 12, 18, 5, fur_d)
        c.round_rect(5, by + 3, 13, 7, 3, fur_l)

        # Tail stub
        c.set(2, by + 10, fur_d)
        c.set(2, by + 11, fur_d)

        # Compact head back (tilted right)
        c.round_rect(hx, hy, 11, 7, 2, fur_m)
        c.hline(hx + 1, hx + 9, hy, fur_l)

        # Ears
        c.rect(hx - 2, hy + 1, 2, 3, fur_m)
        c.rect(hx + 5, hy - 1, 2, 3, fur_m)
        c.set(hx + 5, hy, ear_in)

        # Back foot pads
        c.rect(6 + step_b, by + 15, 4, 2, fur_d)
        c.rect(16 + step_f, by + 15, 4, 2, fur_d)

        if anim == "hurt":
            if f == 0:
                c.set(hx + 3, hy - 3, ix("cyan_light"))
                c.rect(hx + 2, hy - 2, 2, 2, ix("cyan_mid"))
            else:
                c.set(hx + 4, hy + 1, ix("cyan_light"))

        draw_accessory(c, th, hx + 3, hy, "up")

    # -------------------------------------------------------------------------
    # 6. OUTLINE PASS
    # -------------------------------------------------------------------------
    src = list(c.px)
    for y in range(H):
        for x in range(W):
            if src[y * W + x] != 0:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < W and 0 <= ny < H and src[ny * W + nx] != 0:
                    c.set(x, y, out_col)
                    break

    return c


def build_hero_sheet(th):
    frames = []
    labels = []

    # 0. Down (Dir 0)
    for anim, count in ANIM_ORDER:
        for i in range(count):
            frames.append(render_capybara(th, "down", anim, i))
            labels.append(f"do.{anim}{i}")

    # 1. Up (Dir 1)
    for anim, count in ANIM_ORDER:
        for i in range(count):
            frames.append(render_capybara(th, "up", anim, i))
            labels.append(f"up.{anim}{i}")

    # Temporary right frames
    right_frames = []
    right_labels = []
    for anim, count in ANIM_ORDER:
        for i in range(count):
            right_frames.append(render_capybara(th, "right", anim, i))
            right_labels.append(f"ri.{anim}{i}")

    # 2. Left (Dir 2) - Mirrored Right
    for i in range(FRAMES_PER_DIR):
        frames.append(right_frames[i].mirrored())
        labels.append(right_labels[i].replace("ri.", "le.", 1))

    # 3. Right (Dir 3)
    frames.extend(right_frames)
    labels.extend(right_labels)

    # Temporary down-right frames
    dr_frames = []
    dr_labels = []
    for anim, count in ANIM_ORDER:
        for i in range(count):
            dr_frames.append(render_capybara(th, "down_right", anim, i))
            dr_labels.append(f"dr.{anim}{i}")

    # 4. Down-Left (Dir 4) - Mirrored Down-Right
    for i in range(FRAMES_PER_DIR):
        frames.append(dr_frames[i].mirrored())
        labels.append(dr_labels[i].replace("dr.", "dl.", 1))

    # 5. Down-Right (Dir 5)
    frames.extend(dr_frames)
    labels.extend(dr_labels)

    # Temporary up-right frames
    ur_frames = []
    ur_labels = []
    for anim, count in ANIM_ORDER:
        for i in range(count):
            ur_frames.append(render_capybara(th, "up_right", anim, i))
            ur_labels.append(f"ur.{anim}{i}")

    # 6. Up-Left (Dir 6) - Mirrored Up-Right
    for i in range(FRAMES_PER_DIR):
        frames.append(ur_frames[i].mirrored())
        labels.append(ur_labels[i].replace("ur.", "ul.", 1))

    # 7. Up-Right (Dir 7)
    frames.extend(ur_frames)
    labels.extend(ur_labels)

    return frames, labels


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "graphics")
    if not os.path.isabs(out):
        out = os.path.join(root, out)
    os.makedirs(out, exist_ok=True)

    gc.emit_palette(os.path.join(out, "palette.bin"))
    gc.emit_palette_header(os.path.join(root, "source", "palette_ids.h"))

    total = DIRS * FRAMES_PER_DIR
    for tag, name, th in HEROES:
        frames, labels = build_hero_sheet(th)
        blob = bytearray()
        for c in frames:
            blob += c.to_bmp8()

        target_file = os.path.join(out, f"{tag}_tiles.bin")
        gc.write_bin(target_file, bytes(blob))
        print(f"{tag} ({name}): {len(frames)} frames, {len(blob)} bytes ({len(blob) // len(frames)} B/frame)")

        assert len(frames) == total, f"{tag} has {len(frames)} frames"
        assert len(blob) == total * W * H, "frame size mismatch"
        for i, c in enumerate(frames):
            assert c.count_filled() > 50, f"{tag} frame {i} is nearly empty"

        # Preview of right-facing block
        base = FRAMES_PER_DIR * 2
        gc.preview_scale(frames[base:base + FRAMES_PER_DIR],
                         os.path.join(out, f"preview_{tag}.png"),
                         scale=4, cols=FRAMES_PER_DIR,
                         labels=labels[base:base + FRAMES_PER_DIR],
                         title=f"{tag} ({name}) right: idle/walk/chomp/hurt")

    # Overview contact sheet of both heroes
    overview = []
    over_labels = []
    for tag, name, th in HEROES:
        frames, _ = build_hero_sheet(th)
        for d, dname in ((0, "down"), (1, "up"), (2, "left"), (3, "right"), (4, "dn_left"), (5, "dn_right"), (6, "up_left"), (7, "up_right")):
            overview.append(frames[d * FRAMES_PER_DIR + 0])  # idle
            overview.append(frames[d * FRAMES_PER_DIR + 1])  # walk step
            overview.append(frames[d * FRAMES_PER_DIR + 10]) # windup
            overview.append(frames[d * FRAMES_PER_DIR + 11]) # chomp/bite
            over_labels.extend([f"{name} {dname} idle", f"{name} {dname} walk",
                                f"{name} {dname} windup", f"{name} {dname} chomp"])

    gc.preview_scale(overview, os.path.join(out, "preview_heroes.png"),
                     scale=4, cols=8, labels=over_labels,
                     title="GBC & Pokémon Style Zen Capybaras: 阿棕 (P1) & 阿奶 (P2)")
    print("Hero asset generation complete!")


if __name__ == "__main__":
    main()
