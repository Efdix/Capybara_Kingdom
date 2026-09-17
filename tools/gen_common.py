"""
Shared palette + procedural pixel-art toolkit for Capybara Kingdom assets.

Art style: storybook pixel art (Zelda / Pokemon flavor) -- rounded chunky
silhouettes, dark outline, warm saturated colors on a single global palette
so every 8bpp sprite shares one 256-color VRAM palette.

Why procedural instead of hand-written ASCII art: at 16x16 a capybara is a
handful of ellipses/rects, and parametric shapes stay aligned and are trivial
to retune per variant (fur color, direction, animation offsets) -- hand-counted
ASCII rows drift and are painful to keep consistent across ~40 sprites.

NDS specifics:
  * 8bpp palette: 256 x u16 little endian, BGR555
  * 8bpp tiles: 8x8 blocks, 64 bytes each, tile row-major (NDS "1D" mapping)
  * pixel index 0 is transparent for sprites
"""

import os
import struct

# ---------------------------------------------------------------------------
# Palette
# ---------------------------------------------------------------------------

TRANSPARENT = "transparent"

PALETTE_NAMES = [
    # --- neutrals ---------------------------------------------------------
    "outline",
    "outline_soft",
    "shadow",
    "black",
    "white",
    "cream",
    "grey_light",
    "grey_mid",
    "grey_dark",

    # --- capybara fur ----------------------------------------------------
    "brn_dark", "brn_mid", "brn_light", "brn_belly",
    "tan_dark", "tan_mid", "tan_light", "tan_belly",

    # --- skin / pink -----------------------------------------------------
    "pink_dark", "pink_mid", "pink_light", "blush",

    # --- forest ----------------------------------------------------------
    "grn_dark", "grn_mid", "grn_light",
    "leaf_dark", "leaf_mid", "leaf_light",
    "grass_dark", "grass_mid", "grass_light",
    "trunk_dark", "trunk_mid", "trunk_light",

    # --- desert ----------------------------------------------------------
    "sand_dark", "sand_mid", "sand_light",
    "cact_dark", "cact_mid", "cact_light",
    "rock_dark", "rock_mid", "rock_light",
    "clay_dark", "clay_mid",

    # --- snow / ice ------------------------------------------------------
    "snow_dark", "snow_mid", "snow_light",
    "ice_dark", "ice_mid", "ice_light",
    "pine_dark", "pine_mid", "pine_light",

    # --- volcano ---------------------------------------------------------
    "ash_dark", "ash_mid", "ash_light",
    "lava_dark", "lava_mid", "lava_light",
    "ember_orange", "ember_yellow",

    # --- castle / cake ---------------------------------------------------
    "castle_dark", "castle_mid", "castle_light",
    "choco_dark", "choco_mid", "choco_light",
    "sponge_dark", "sponge_mid", "sponge_light",
    "cream_dark", "cream_mid", "cream_light",
    "cherry_dark", "cherry_mid", "cherry_light",

    # --- fx --------------------------------------------------------------
    "red_dark", "red_mid", "red_light",
    "orange_dark", "orange_mid",
    "yellow_dark", "yellow_mid", "yellow_light",
    "blue_dark", "blue_mid", "blue_light",
    "cyan_dark", "cyan_mid", "cyan_light",
    "purple_dark", "purple_mid", "purple_light",
    "green_spark", "white_spark",

    # --- fur ramps (5 steps each: the fix for flat-looking characters) ----
    "brn_shadow", "brn_deep", "brn_mid2", "brn_hi", "brn_hl",
    "tan_shadow", "tan_deep", "tan_mid2", "tan_hi", "tan_hl",
    "fur_black", "fur_white", "fur_grey",

    # --- metal / weapon (katana + revolver) ------------------------------
    "steel_shadow", "steel_dark", "steel_mid", "steel_hi", "steel_glint",
    "brass_shadow", "brass_dark", "brass_mid", "brass_hi",
    "wood_dark", "wood_mid", "wood_light",
    "gun_dark", "gun_mid", "gun_hi",
    "grip_dark", "grip_mid", "grip_hi",

    # --- cloth / outfit --------------------------------------------------
    "cloth_red_sh", "cloth_red", "cloth_red_hi",
    "cloth_blue_sh", "cloth_blue", "cloth_blue_hi",
    "belt_dark", "belt_mid",

    # --- skill FX: slash arcs and magic ----------------------------------
    "arc_core", "arc_inner", "arc_outer", "arc_fade",
    "fire_core", "fire_inner", "fire_outer", "fire_smoke",
    "ice_core", "ice_inner", "ice_outer", "ice_shard",
    "volt_core", "volt_inner", "volt_outer",
    "shadow_core", "shadow_inner", "shadow_outer",
    "heal_core", "heal_inner", "heal_outer",

    # --- ui --------------------------------------------------------------
    "ui_panel", "ui_panel_light", "ui_frame", "ui_frame_light",
    "ui_text", "ui_text_dim", "ui_gold", "ui_gold_light",
    "hud_heart", "hud_heart_dark", "hud_heart_empty",

    # --- region terrain (appended at the END so no existing palette index
    #     shifts; palette_ids.h is regenerated from this list) ------------
    # farm: golden wheat / straw
    "wheat_dark", "wheat_mid", "wheat_light",
    # marsh: sugarcane stalks
    "cane_dark", "cane_mid", "cane_light",
    # marsh: muddy water and wet ground
    "murk_dark", "murk_mid", "murk_light",
    # cacao volcano: warm volcanic ash and drying-yard ground
    "ash_warm_dark", "ash_warm_mid", "ash_warm_light",
]

_RGB = {
    "transparent": (255, 0, 255),

    "outline": (44, 28, 40),
    "outline_soft": (78, 54, 66),
    "shadow": (60, 44, 58),
    "black": (26, 20, 26),
    "white": (255, 255, 255),
    "cream": (255, 246, 214),
    "grey_light": (216, 216, 224),
    "grey_mid": (152, 152, 168),
    "grey_dark": (94, 94, 110),

    "brn_dark": (146, 100, 54),
    "brn_mid": (192, 142, 80),
    "brn_light": (228, 186, 122),
    "brn_belly": (248, 218, 170),
    "tan_dark": (208, 158, 98),
    "tan_mid": (240, 200, 140),
    "tan_light": (253, 228, 178),
    "tan_belly": (255, 242, 210),

    "pink_dark": (198, 98, 124),
    "pink_mid": (246, 152, 178),
    "pink_light": (255, 198, 214),
    "blush": (255, 170, 170),

    "grn_dark": (36, 94, 54),
    "grn_mid": (64, 140, 68),
    "grn_light": (112, 188, 90),
    "leaf_dark": (30, 106, 62),
    "leaf_mid": (60, 156, 84),
    "leaf_light": (120, 204, 106),
    "grass_dark": (60, 130, 64),
    "grass_mid": (100, 178, 84),
    "grass_light": (156, 216, 110),
    "trunk_dark": (88, 58, 36),
    "trunk_mid": (124, 86, 52),
    "trunk_light": (164, 120, 76),

    "sand_dark": (198, 160, 94),
    "sand_mid": (230, 198, 126),
    "sand_light": (250, 228, 166),
    "cact_dark": (46, 114, 70),
    "cact_mid": (78, 154, 90),
    "cact_light": (130, 196, 114),
    "rock_dark": (114, 90, 76),
    "rock_mid": (156, 128, 106),
    "rock_light": (198, 174, 146),
    "clay_dark": (170, 106, 64),
    "clay_mid": (214, 148, 94),

    "snow_dark": (170, 196, 226),
    "snow_mid": (216, 234, 248),
    "snow_light": (252, 254, 255),
    "ice_dark": (88, 148, 202),
    "ice_mid": (132, 194, 234),
    "ice_light": (194, 234, 254),
    "pine_dark": (30, 84, 72),
    "pine_mid": (50, 118, 98),
    "pine_light": (88, 160, 130),

    "ash_dark": (76, 64, 68),
    "ash_mid": (114, 98, 98),
    "ash_light": (160, 142, 134),
    "lava_dark": (178, 46, 26),
    "lava_mid": (234, 90, 34),
    "lava_light": (252, 154, 50),
    "ember_orange": (252, 178, 66),
    "ember_yellow": (255, 232, 132),

    "castle_dark": (80, 64, 98),
    "castle_mid": (120, 98, 142),
    "castle_light": (168, 144, 186),
    "choco_dark": (88, 52, 34),
    "choco_mid": (124, 78, 50),
    "choco_light": (166, 112, 74),
    "sponge_dark": (206, 160, 98),
    "sponge_mid": (240, 200, 140),
    "sponge_light": (255, 230, 180),
    "cream_dark": (218, 202, 234),
    "cream_mid": (248, 240, 254),
    "cream_light": (255, 255, 255),
    "cherry_dark": (178, 36, 56),
    "cherry_mid": (226, 64, 84),
    "cherry_light": (254, 124, 134),

    "red_dark": (174, 46, 50),
    "red_mid": (228, 76, 72),
    "red_light": (254, 134, 120),
    "orange_dark": (208, 106, 34),
    "orange_mid": (248, 154, 58),
    "yellow_dark": (208, 170, 42),
    "yellow_mid": (252, 216, 74),
    "yellow_light": (255, 244, 162),
    "blue_dark": (56, 86, 178),
    "blue_mid": (88, 134, 228),
    "blue_light": (142, 188, 252),
    "cyan_dark": (46, 150, 178),
    "cyan_mid": (88, 204, 226),
    "cyan_light": (164, 242, 252),
    "purple_dark": (96, 56, 154),
    "purple_mid": (148, 96, 204),
    "purple_light": (198, 154, 242),
    "green_spark": (148, 248, 142),
    "white_spark": (255, 255, 242),

    # --- fur ramps: warm brown, dark -> highlight ------------------------
    "brn_shadow": (74, 44, 30),
    "brn_deep": (110, 70, 44),
    "brn_mid2": (162, 112, 66),
    "brn_hi": (206, 156, 96),
    "brn_hl": (240, 200, 148),

    # --- fur ramps: light tan, dark -> highlight ------------------------
    "tan_shadow": (128, 88, 56),
    "tan_deep": (176, 128, 82),
    "tan_mid2": (222, 180, 122),
    "tan_hi": (246, 214, 162),
    "tan_hl": (255, 240, 206),

    "fur_black": (22, 18, 26),
    "fur_white": (250, 248, 240),
    "fur_grey": (140, 138, 148),

    # --- steel: 5 steps for blades and gun barrels ----------------------
    "steel_shadow": (48, 56, 78),
    "steel_dark": (84, 96, 124),
    "steel_mid": (140, 154, 184),
    "steel_hi": (196, 212, 236),
    "steel_glint": (248, 252, 255),

    # --- brass: revolver frame, cartridges ------------------------------
    "brass_shadow": (96, 66, 30),
    "brass_dark": (150, 106, 46),
    "brass_mid": (208, 158, 74),
    "brass_hi": (248, 214, 138),

    # --- wood: katana saya, grips, signs --------------------------------
    "wood_dark": (60, 38, 28),
    "wood_mid": (104, 68, 44),
    "wood_light": (152, 106, 68),

    # --- gun metal (darker than steel, for the revolver body) -----------
    "gun_dark": (34, 34, 42),
    "gun_mid": (66, 66, 80),
    "gun_hi": (108, 110, 128),

    # --- grip: the rubber grip on both weapons --------------------------
    "grip_dark": (58, 40, 36),
    "grip_mid": (96, 66, 58),
    "grip_hi": (140, 100, 86),

    # --- cloth ----------------------------------------------------------
    "cloth_red_sh": (120, 40, 44),
    "cloth_red": (176, 62, 62),
    "cloth_red_hi": (224, 104, 96),
    "cloth_blue_sh": (44, 62, 122),
    "cloth_blue": (74, 102, 178),
    "cloth_blue_hi": (124, 158, 226),
    "belt_dark": (52, 40, 34),
    "belt_mid": (92, 72, 56),

    # --- slash arc (bright crescent that fades at the ends) -------------
    "arc_core": (255, 255, 240),
    "arc_inner": (198, 238, 255),
    "arc_outer": (110, 186, 240),
    "arc_fade": (64, 118, 190),

    # --- fire -----------------------------------------------------------
    "fire_core": (255, 248, 190),
    "fire_inner": (255, 186, 64),
    "fire_outer": (232, 96, 32),
    "fire_smoke": (108, 78, 76),

    # --- ice ------------------------------------------------------------
    "ice_core": (255, 255, 255),
    "ice_inner": (178, 236, 255),
    "ice_outer": (86, 158, 226),
    "ice_shard": (206, 240, 255),

    # --- volt -----------------------------------------------------------
    "volt_core": (255, 255, 216),
    "volt_inner": (252, 226, 96),
    "volt_outer": (156, 140, 240),

    # --- shadow / dark magic --------------------------------------------
    "shadow_core": (226, 200, 255),
    "shadow_inner": (146, 92, 196),
    "shadow_outer": (74, 42, 108),

    # --- heal -----------------------------------------------------------
    "heal_core": (240, 255, 236),
    "heal_inner": (146, 240, 170),
    "heal_outer": (72, 184, 122),

    "ui_panel": (28, 24, 26),
    "ui_panel_light": (52, 44, 46),
    "ui_frame": (142, 110, 68),
    "ui_frame_light": (204, 168, 114),
    "ui_text": (255, 250, 240),
    "ui_text_dim": (184, 172, 164),
    "ui_gold": (240, 192, 76),
    "ui_gold_light": (255, 232, 152),
    "hud_heart": (240, 70, 88),
    "hud_heart_dark": (170, 34, 56),
    "hud_heart_empty": (98, 84, 110),

    # --- region terrain --------------------------------------------------
    # wheat: a warm gold ramp with real light/shadow so the field reads as
    # volume rather than a flat yellow sheet.
    "wheat_dark": (168, 120, 44),
    "wheat_mid": (220, 174, 76),
    "wheat_light": (250, 226, 130),
    # sugarcane: pale yellow-green, unmistakably stalk-like next to the
    # darker jungle/leaf greens.
    "cane_dark": (106, 124, 58),
    "cane_mid": (156, 178, 86),
    "cane_light": (208, 222, 130),
    # marsh water: murky olive (not blue -- it is mud, not a lake).
    "murk_dark": (46, 60, 46),
    "murk_mid": (76, 94, 68),
    "murk_light": (116, 132, 92),
    # volcanic ash, but *warm* (taupe) so the cacao grove reads distinct
    # from the old grey volcano and from the chocolate-brown soil.
    "ash_warm_dark": (98, 74, 68),
    "ash_warm_mid": (144, 116, 106),
    "ash_warm_light": (192, 168, 154),
}


def build_index():
    idx = {"transparent": 0}
    for i, n in enumerate(PALETTE_NAMES, start=1):
        idx[n] = i
    return idx


PAL = build_index()
assert len(PALETTE_NAMES) + 1 <= 256, "palette overflow (>256 colors)"


def rgb_of(name):
    return _RGB[name]


def bgr555(name):
    r, g, b = _RGB[name]
    return ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3)


# Variant color remaps: same shapes, different fur / accessories.
VARIANT_MALE = {}

VARIANT_FEMALE = {
    "brn_dark": "tan_dark",
    "brn_mid": "tan_mid",
    "brn_light": "tan_light",
    "brn_belly": "tan_belly",
    # accessory: green leaf -> pink flower
    "leaf_dark": "pink_dark",
    "leaf_mid": "pink_mid",
    "leaf_light": "pink_light",
}


def ix(name, variant=None):
    """Palette index for a color name, honoring a variant remap."""
    if variant and name in variant:
        name = variant[name]
    return PAL[name]


# ---------------------------------------------------------------------------
# Canvas + shapes
# ---------------------------------------------------------------------------

class Canvas:
    """Index-based pixel buffer. Index 0 == transparent."""

    def __init__(self, w, h):
        self.w = w
        self.h = h
        self.px = [0] * (w * h)

    # --- low level ------------------------------------------------------
    def get(self, x, y):
        if 0 <= x < self.w and 0 <= y < self.h:
            return self.px[y * self.w + x]
        return 0

    def set(self, x, y, color):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = color

    def is_empty(self, x, y):
        return self.get(x, y) == 0

    # --- shapes ---------------------------------------------------------
    def rect(self, x, y, w, h, color):
        for j in range(y, y + h):
            for i in range(x, x + w):
                self.set(i, j, color)

    def ellipse(self, cx, cy, rx, ry, color):
        if rx <= 0 or ry <= 0:
            return
        for j in range(int(cy - ry), int(cy + ry) + 1):
            for i in range(int(cx - rx), int(cx + rx) + 1):
                dx = (i - cx + 0.5) / rx
                dy = (j - cy + 0.5) / ry
                if dx * dx + dy * dy <= 1.0:
                    self.set(i, j, color)

    def circle(self, cx, cy, r, color):
        self.ellipse(cx, cy, r, r, color)

    def blob(self, cx, cy, rx, ry, color, n=4.0, squash_top=0.0):
        """Filled superellipse -- the workhorse for organic creature bodies.

        n=2 is an ellipse, n=4..6 is the chunky rounded form used by cute
        character art. `squash_top` flattens the top of the shape (0 = none,
        0.5 = strongly flattened), which is how a capybara's boxy head and a
        slime's flat-topped dome differ from a plain oval.

        Coordinates are floats and the shape is centred, so callers place parts
        by centre point -- that is what keeps eyes, muzzle and limbs aligned as
        the proportions change.
        """
        for j in range(int(cy - ry) - 1, int(cy + ry) + 2):
            for i in range(int(cx - rx) - 1, int(cx + rx) + 2):
                dx = abs((i + 0.5 - cx) / max(rx, 0.001))
                dy = abs((j + 0.5 - cy) / max(ry, 0.001))
                if squash_top > 0.0 and (j + 0.5) < cy:
                    # Flatten the upper half by compressing its y extent.
                    dy /= max(0.15, 1.0 - squash_top)
                if dx ** n + dy ** n <= 1.0:
                    self.set(i, j, color)

    def round_rect(self, x, y, w, h, r, color):
        """Rounded rectangle -- the workhorse for boxy capybara heads."""
        for j in range(y, y + h):
            for i in range(x, x + w):
                # distance into the corner region
                cx = min(i - x, x + w - 1 - i)
                cy = min(j - y, y + h - 1 - j)
                if cx < r and cy < r:
                    dx = r - cx
                    dy = r - cy
                    if dx * dx + dy * dy > r * r:
                        continue
                self.set(i, j, color)

    def hline(self, x0, x1, y, color):
        for x in range(min(x0, x1), max(x0, x1) + 1):
            self.set(x, y, color)

    def vline(self, x, y0, y1, color):
        for y in range(min(y0, y1), max(y0, y1) + 1):
            self.set(x, y, color)

    # --- composition ----------------------------------------------------
    def auto_outline(self, color_name="outline", variant=None,
                     also_inner=False):
        """Outline every empty pixel touching a filled one.

        Call this LAST: it grows the silhouette by one pixel, so leave a
        1px margin when laying out shapes.
        """
        col = ix(color_name, variant)
        src = list(self.px)
        for y in range(self.h):
            for x in range(self.w):
                if src[y * self.w + x] != 0:
                    continue
                hit = False
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < self.w and 0 <= ny < self.h:
                        if src[ny * self.w + nx] != 0:
                            hit = True
                            break
                if hit:
                    self.set(x, y, col)

    def outline_vertical_edges(self, color_name="outline", variant=None):
        """Add outline only on the left/right of filled runs (keeps tops open).

        Useful for ground tiles / flat platforms where a full outline would
        create a hard seam against neighbouring tiles.
        """
        col = ix(color_name, variant)
        src = list(self.px)
        for y in range(self.h):
            for x in range(self.w):
                if src[y * self.w + x] != 0:
                    continue
                left = self.get_src(src, x - 1, y) != 0
                right = self.get_src(src, x + 1, y) != 0
                if left or right:
                    self.set(x, y, col)

    @staticmethod
    def get_src(src, x, y):
        return 0  # placeholder (unused)

    def shade_bottom(self, color_name, variant=None, rows=1):
        """Darken the bottom `rows` pixels of each filled column (fake AO)."""
        col = ix(color_name, variant)
        for x in range(self.w):
            filled = [y for y in range(self.h) if self.get(x, y) != 0]
            if not filled:
                continue
            for y in filled[-rows:]:
                if self.get(x, y) != 0:
                    self.set(x, y, col)

    def shade_top(self, color_name, variant=None, rows=1):
        col = ix(color_name, variant)
        for x in range(self.w):
            filled = [y for y in range(self.h) if self.get(x, y) != 0]
            if not filled:
                continue
            for y in filled[:rows]:
                if self.get(x, y) != 0:
                    self.set(x, y, col)

    def replace(self, from_name, to_name, variant=None):
        a = PAL[from_name]
        b = ix(to_name, variant)
        self.px = [b if v == a else v for v in self.px]

    def mirrored(self):
        out = Canvas(self.w, self.h)
        for y in range(self.h):
            for x in range(self.w):
                out.px[y * self.w + x] = self.px[y * self.w + (self.w - 1 - x)]
        return out

    def rotated_cw(self):
        out = Canvas(self.h, self.w)
        for y in range(self.h):
            for x in range(self.w):
                out.set(self.h - 1 - y, x, self.get(x, y))
        return out

    def offset_copy(self, dx, dy):
        out = Canvas(self.w, self.h)
        for y in range(self.h):
            for x in range(self.w):
                v = self.get(x, y)
                if v:
                    out.set(x + dx, y + dy, v)
        return out

    def paste(self, other, ox=0, oy=0):
        for y in range(other.h):
            for x in range(other.w):
                v = other.get(x, y)
                if v:
                    self.set(ox + x, oy + y, v)

    def count_filled(self):
        return sum(1 for v in self.px if v)

    # --- NDS emission ---------------------------------------------------
    def to_tiles(self):
        """NDS 8bpp tiles: 8x8 blocks, left-to-right then top-to-bottom."""
        out = bytearray()
        for ty in range(0, self.h, 8):
            for tx in range(0, self.w, 8):
                for y in range(8):
                    for x in range(8):
                        out.append(self.get(tx + x, ty + y) & 0xFF)
        return bytes(out)

    def to_bmp8(self):
        """Straight 8bpp bitmap: row-major indices, no tiling."""
        return bytes((self.px[y * self.w + x] & 0xFF)
                     for y in range(self.h) for x in range(self.w))


def new_canvas(w, h):
    return Canvas(w, h)


# ---------------------------------------------------------------------------
# Emission helpers
# ---------------------------------------------------------------------------

def write_bin(path, data):
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def emit_palette(path):
    """256 x u16 BGR555 little endian; index 0 = transparent."""
    data = bytearray()
    data += struct.pack("<H", bgr555("transparent"))
    for name in PALETTE_NAMES:
        data += struct.pack("<H", bgr555(name))
    pad = 256 - len(PALETTE_NAMES) - 1
    data += b"\x00\x00" * pad
    write_bin(path, bytes(data))


def emit_palette_header(path):
    """C header with PAL_<NAME> index constants.

    Emitting these from the single source of truth means the C code can never
    drift out of sync with the palette order -- a mismatch would silently
    recolor the whole game.
    """
    lines = [
        "// Autogenerated by tools/gen_common.py -- do not edit.",
        "// Palette indices for palette.bin; index 0 is transparent.",
        "",
        "#pragma once",
        "",
        "#define PAL_TRANSPARENT 0",
    ]
    for name in PALETTE_NAMES:
        lines.append(f"#define PAL_{name.upper():<16} {PAL[name]}")
    lines.append("")
    # Highest index that is still an opaque game color (used by the shadow
    # pass to tell "ground" pixels from "already drawn entity" pixels).
    lines.append(f"#define PAL_LAST_COLOR {PAL[PALETTE_NAMES[-1]]}")
    lines.append("")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("palette header ->", path)


def emit_palette_png(path, scale=16):
    """Swatch sheet so the palette itself can be eyeballed."""
    from PIL import Image, ImageDraw
    cols = 16
    rows = (len(PALETTE_NAMES) + 1 + cols - 1) // cols
    img = Image.new("RGB", (cols * scale, rows * scale), (20, 16, 22))
    d = ImageDraw.Draw(img)
    names = [TRANSPARENT] + PALETTE_NAMES
    for n, name in enumerate(names):
        r, g, b = rgb_of(name)
        x = (n % cols) * scale
        y = (n // cols) * scale
        d.rectangle([x, y, x + scale - 2, y + scale - 2], fill=(r, g, b))
    img.save(path)
    print("palette ->", path)


def preview_scale(canvases, path, scale=4, cols=None, labels=None,
                  bg=(58, 50, 72), grid=True, title=None):
    """Contact sheet of canvases, scaled up with nearest-neighbour."""
    from PIL import Image, ImageDraw
    if not canvases:
        return
    cell_w = max(c.w for c in canvases) * scale
    cell_h = max(c.h for c in canvases) * scale
    cols = cols or min(8, len(canvases))
    rows = (len(canvases) + cols - 1) // cols
    label_h = 11 if labels else 0
    head_h = 14 if title else 0
    sheet = Image.new("RGB", (cols * cell_w, head_h + rows * (cell_h + label_h)), bg)
    d = ImageDraw.Draw(sheet)
    if title:
        d.text((4, 2), title, fill=(255, 240, 200))

    def idx_to_rgb(i):
        for name in [TRANSPARENT] + PALETTE_NAMES:
            if PAL[name] == i:
                return _RGB[name]
        return (0, 0, 0)

    for n, c in enumerate(canvases):
        img = Image.new("RGB", (c.w, c.h))
        img.putdata([bg if v == 0 else idx_to_rgb(v) for v in c.px])
        img = img.resize((c.w * scale, c.h * scale), Image.NEAREST)
        cx = (n % cols) * cell_w
        cy = head_h + (n // cols) * (cell_h + label_h)
        sheet.paste(img, (cx, cy))
        if grid:
            d.rectangle([cx, cy, cx + cell_w - 1, cy + cell_h - 1],
                        outline=(70, 62, 86))
        if labels:
            d.text((cx + 2, cy + cell_h + 1), str(labels[n])[:20],
                   fill=(240, 236, 250))
    sheet.save(path)
    print("preview ->", path)
