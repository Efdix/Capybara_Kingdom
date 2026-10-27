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

    # --- palette v2 additions (also appended, same reason) ----------------
    # plank / boardwalk: a warmer, more saturated timber than `wood_*`, which
    # is the dark bark brown used for trunks and fences.
    "plank_shadow", "plank_dark", "plank_mid", "plank_light",
    # roof tiles: terracotta and the deep teal-green glazed roof used on the
    # larger civic buildings.
    "tile_dark", "tile_mid", "tile_light",
    "slate_dark", "slate_mid", "slate_light",
    # stone: cut masonry, brighter and cooler than `rock_*` (which is a
    # weathered boulder ramp) so walls read as built rather than natural.
    "stone_shadow", "stone_dark", "stone_mid", "stone_light",
    # window glow: the warm lamp light that makes a village look inhabited.
    "lamp_glow", "lamp_core",
    # water clarification: a saturated tropical lagoon, distinct from the
    # greenish `cyan_*` used for magic effects.
    "sea_deep", "sea_mid", "sea_light", "foam",
    # world map: paper, ink, route dots and the five biome washes.
    "map_paper", "map_paper_shade", "map_ink", "map_route",
    "map_grass", "map_grass_ho", "map_forest", "map_forest_ho",
    "map_desert", "map_desert_ho", "map_snow", "map_snow_ho",
    "map_ash", "map_ash_ho", "map_sea", "map_sea_ho",
    "map_marker", "map_marker_ho",

    # --- ui v3: the Black 2 / White 2 plate kit (appended, same reason) ---
    #
    # The storybook `ui_*` block above is a dark indigo panel with bronze trim.
    # This block replaces it for everything the player presses: the Black 2 /
    # White 2 menu language is *light* -- white rounded plates on a blue field,
    # hard navy outlines, and one saturated blue that means "this is the thing
    # you are about to press".
    #
    # Both sets stay in the palette because the older one still colours
    # world-space markers and the card art; nothing that uses the new kit should
    # reach for the old names.
    "ui2_bg", "ui2_bg_hi",                                  # the screen field
    "ui2_dot",                                              # field texture
    "ui2_line", "ui2_line_soft",                            # navy outlines
    "ui2_plate_hi", "ui2_plate", "ui2_plate_lo",            # plate gradient
    "ui2_plate_deep",                                       # recessed / pressed
    "ui2_screen",                                           # dark readout well
    "ui2_blue_dark", "ui2_blue", "ui2_blue_hi", "ui2_blue_glow",
    "ui2_ink", "ui2_ink_dim", "ui2_shadow",
    # Semantic accents for values drawn on the light plates: the palette's
    # existing *_dark tones are tuned for sprites on terrain and go muddy
    # against a white panel, so the UI gets its own readable set.
    "ui2_red", "ui2_green", "ui2_amber", "ui2_violet", "ui2_cyan",

    # --- ui v4: title dusk + chart dressing (appended, same reason) --------
    #
    # The title screen is a proper sunset scene now, and the old table had no
    # between-step from `purple_dark` to `orange_mid`: quantising a dusk
    # gradient through that gap went muddy. These are the missing dusk ramp
    # (deep indigo -> violet -> plum -> rose -> ember -> gold), the two muted
    # greens the dusk meadow needs, and two scorched-paper tones for the
    # expedition chart's burnt edges.
    "dusk_deep",        # sky top
    "dusk_indigo",      # sky band 2 / tree + castle silhouette fill
    "dusk_violet",      # sky band 3 / nearest ridge
    "dusk_plum",        # sky band 4 / mid ridge + far clouds
    "haze_mauve",       # far ridge haze against the horizon glow
    "sunset_rose",      # sky band 5
    "sunset_ember",     # sky band 6 / backlit rims
    "sunset_gold",      # horizon band / lit cloud edges / glow mid tone
    "cloud_dusk",       # darkest silhouette (clouds, hill castle, tree)
    "dusk_grass",       # meadow at dusk
    "dusk_grass_deep",  # meadow shadow / foreground grass band
    "map_burn",         # chart: scorched edge mid tone
    "map_burn_deep",    # chart: scorched edge darkest
]

# Palette v2 -- "sunlit storybook".
#
# The rule the whole table follows: every material gets a four-stop ramp whose
# shadow end is *cool and chromatic* rather than merely dark, and whose
# highlight end is near-white. Flat mid-tone-only art is what made the old
# version read as washed out; a material only looks solid once its darkest stop
# and its lightest stop are far enough apart to see at a glance on a DS panel.
# Outlines are a warm plum-black rather than neutral black, so a chunky
# silhouette never punches a cold hole through a warm scene.
_RGB = {
    "transparent": (255, 0, 255),

    "outline": (58, 34, 52),
    "outline_soft": (100, 64, 86),
    "shadow": (74, 52, 84),
    "black": (32, 22, 34),
    "white": (255, 255, 255),
    "cream": (255, 246, 214),
    "grey_light": (228, 230, 242),
    "grey_mid": (160, 164, 188),
    "grey_dark": (92, 96, 124),

    "brn_dark": (150, 96, 46),
    "brn_mid": (206, 148, 74),
    "brn_light": (242, 194, 118),
    "brn_belly": (255, 226, 172),
    "tan_dark": (222, 164, 92),
    "tan_mid": (250, 208, 138),
    "tan_light": (255, 232, 178),
    "tan_belly": (255, 246, 214),

    "pink_dark": (214, 92, 128),
    "pink_mid": (255, 148, 184),
    "pink_light": (255, 200, 220),
    "blush": (255, 160, 164),

    "grn_dark": (30, 106, 54),
    "grn_mid": (66, 168, 74),
    "grn_light": (126, 214, 96),
    "leaf_dark": (26, 122, 66),
    "leaf_mid": (62, 180, 88),
    "leaf_light": (134, 226, 108),
    "grass_dark": (56, 140, 62),
    "grass_mid": (108, 196, 76),
    "grass_light": (168, 232, 106),
    "trunk_dark": (94, 56, 32),
    "trunk_mid": (136, 88, 48),
    "trunk_light": (186, 130, 72),

    "sand_dark": (216, 158, 82),
    "sand_mid": (244, 206, 128),
    "sand_light": (255, 236, 172),
    "cact_dark": (40, 122, 74),
    "cact_mid": (72, 170, 92),
    "cact_light": (126, 210, 116),
    "rock_dark": (116, 88, 82),
    "rock_mid": (168, 134, 118),
    "rock_light": (214, 186, 158),
    "clay_dark": (188, 106, 56),
    "clay_mid": (232, 152, 88),

    "snow_dark": (146, 186, 232),
    "snow_mid": (208, 232, 252),
    "snow_light": (252, 254, 255),
    "ice_dark": (62, 128, 200),
    "ice_mid": (116, 188, 240),
    "ice_light": (192, 234, 255),
    "pine_dark": (22, 88, 78),
    "pine_mid": (42, 130, 106),
    "pine_light": (86, 176, 142),

    "ash_dark": (74, 62, 74),
    "ash_mid": (118, 100, 108),
    "ash_light": (170, 148, 146),
    "lava_dark": (186, 44, 22),
    "lava_mid": (244, 96, 28),
    "lava_light": (255, 162, 44),
    "ember_orange": (255, 186, 62),
    "ember_yellow": (255, 238, 138),

    "castle_dark": (78, 62, 104),
    "castle_mid": (124, 100, 152),
    "castle_light": (180, 154, 200),
    "choco_dark": (96, 52, 30),
    "choco_mid": (136, 80, 46),
    "choco_light": (184, 120, 74),
    "sponge_dark": (214, 164, 94),
    "sponge_mid": (246, 204, 136),
    "sponge_light": (255, 234, 180),
    "cream_dark": (224, 206, 244),
    "cream_mid": (250, 242, 255),
    "cream_light": (255, 255, 255),
    "cherry_dark": (188, 30, 56),
    "cherry_mid": (238, 66, 86),
    "cherry_light": (255, 128, 136),

    "red_dark": (182, 40, 48),
    "red_mid": (238, 74, 70),
    "red_light": (255, 138, 118),
    "orange_dark": (216, 100, 26),
    "orange_mid": (255, 156, 52),
    "yellow_dark": (216, 172, 36),
    "yellow_mid": (255, 220, 68),
    "yellow_light": (255, 248, 170),
    "blue_dark": (48, 84, 190),
    "blue_mid": (86, 138, 238),
    "blue_light": (146, 196, 255),
    "cyan_dark": (36, 156, 190),
    "cyan_mid": (86, 214, 236),
    "cyan_light": (172, 246, 255),
    "purple_dark": (98, 52, 164),
    "purple_mid": (154, 96, 216),
    "purple_light": (206, 158, 248),
    "green_spark": (150, 252, 140),
    "white_spark": (255, 255, 246),

    # --- fur ramps: warm brown, dark -> highlight ------------------------
    "brn_shadow": (78, 44, 30),
    "brn_deep": (118, 70, 40),
    "brn_mid2": (172, 114, 62),
    "brn_hi": (216, 162, 96),
    "brn_hl": (248, 208, 152),

    # --- fur ramps: light tan, dark -> highlight ------------------------
    "tan_shadow": (136, 88, 54),
    "tan_deep": (186, 132, 80),
    "tan_mid2": (236, 188, 122),
    "tan_hi": (252, 220, 168),
    "tan_hl": (255, 242, 210),

    "fur_black": (28, 20, 30),
    "fur_white": (252, 250, 244),
    "fur_grey": (152, 150, 166),

    # --- steel: 5 steps for blades and gun barrels ----------------------
    "steel_shadow": (44, 54, 82),
    "steel_dark": (80, 96, 130),
    "steel_mid": (140, 158, 194),
    "steel_hi": (202, 218, 244),
    "steel_glint": (250, 253, 255),

    # --- brass: revolver frame, cartridges ------------------------------
    "brass_shadow": (96, 62, 22),
    "brass_dark": (156, 106, 38),
    "brass_mid": (218, 164, 66),
    "brass_hi": (252, 222, 138),

    # --- wood: katana saya, grips, signs --------------------------------
    "wood_dark": (62, 38, 26),
    "wood_mid": (108, 68, 42),
    "wood_light": (160, 110, 68),

    # --- gun metal (darker than steel, for the revolver body) -----------
    "gun_dark": (32, 32, 44),
    "gun_mid": (66, 68, 88),
    "gun_hi": (112, 116, 140),

    # --- grip: the rubber grip on both weapons --------------------------
    "grip_dark": (58, 38, 34),
    "grip_mid": (100, 66, 56),
    "grip_hi": (146, 102, 84),

    # --- cloth ----------------------------------------------------------
    "cloth_red_sh": (124, 32, 44),
    "cloth_red": (188, 56, 62),
    "cloth_red_hi": (238, 100, 96),
    "cloth_blue_sh": (38, 58, 130),
    "cloth_blue": (72, 104, 190),
    "cloth_blue_hi": (128, 164, 236),
    "belt_dark": (52, 38, 34),
    "belt_mid": (94, 72, 56),

    # --- slash arc (bright crescent that fades at the ends) -------------
    "arc_core": (255, 255, 244),
    "arc_inner": (204, 242, 255),
    "arc_outer": (108, 190, 248),
    "arc_fade": (58, 116, 196),

    # --- fire -----------------------------------------------------------
    "fire_core": (255, 250, 196),
    "fire_inner": (255, 190, 62),
    "fire_outer": (238, 94, 28),
    "fire_smoke": (116, 80, 78),

    # --- ice ------------------------------------------------------------
    "ice_core": (255, 255, 255),
    "ice_inner": (182, 240, 255),
    "ice_outer": (80, 158, 232),
    "ice_shard": (212, 244, 255),

    # --- volt -----------------------------------------------------------
    "volt_core": (255, 255, 220),
    "volt_inner": (255, 230, 92),
    "volt_outer": (154, 140, 248),

    # --- shadow / dark magic --------------------------------------------
    "shadow_core": (230, 204, 255),
    "shadow_inner": (150, 92, 202),
    "shadow_outer": (72, 40, 112),

    # --- heal -----------------------------------------------------------
    "heal_core": (242, 255, 238),
    "heal_inner": (146, 246, 174),
    "heal_outer": (66, 192, 124),

    # UI: a deep indigo-plum panel with bronze/gold trim. Warm-dark rather
    # than neutral-dark so panels sit *in* the world's colour range instead of
    # on top of it, and so the gold reads as the same family as torchlight.
    "ui_panel": (34, 26, 44),
    "ui_panel_light": (62, 46, 76),
    "ui_frame": (150, 106, 56),
    "ui_frame_light": (222, 178, 106),
    "ui_text": (255, 250, 238),
    "ui_text_dim": (176, 166, 190),
    "ui_gold": (250, 196, 68),
    "ui_gold_light": (255, 236, 148),
    "hud_heart": (248, 66, 94),
    "hud_heart_dark": (176, 28, 58),
    "hud_heart_empty": (86, 74, 104),

    # --- region terrain --------------------------------------------------
    # wheat: a warm gold ramp with real light/shadow so the field reads as
    # volume rather than a flat yellow sheet.
    "wheat_dark": (186, 124, 34),
    "wheat_mid": (238, 182, 62),
    "wheat_light": (255, 232, 128),
    # sugarcane: pale yellow-green, unmistakably stalk-like next to the
    # darker jungle/leaf greens.
    "cane_dark": (98, 128, 44),
    "cane_mid": (154, 190, 70),
    "cane_light": (212, 230, 118),
    # marsh water: murky olive (not blue -- it is mud, not a lake).
    "murk_dark": (44, 62, 52),
    "murk_mid": (74, 100, 74),
    "murk_light": (116, 142, 98),
    # volcanic ash, but *warm* (taupe) so the cacao grove reads distinct
    # from the old grey volcano and from the chocolate-brown soil.
    "ash_warm_dark": (104, 74, 66),
    "ash_warm_mid": (152, 116, 100),
    "ash_warm_light": (202, 172, 152),

    # --- palette v2 additions -------------------------------------------
    # Boardwalk timber: warmer, lighter and far more saturated than `wood_*`,
    # which is the near-black bark brown used for trunks and fence posts. The
    # kingdom is built on planks, so this is the most-seen material in the hub.
    "plank_shadow": (104, 60, 30),
    "plank_dark": (150, 92, 44),
    "plank_mid": (198, 132, 64),
    "plank_light": (240, 180, 100),

    # Roof tiles: terracotta and a deep glazed teal. Two roof families is what
    # lets a row of houses look like a village rather than a copy-paste.
    "tile_dark": (150, 52, 44),
    "tile_mid": (208, 84, 62),
    "tile_light": (248, 128, 92),
    "slate_dark": (30, 84, 92),
    "slate_mid": (52, 128, 132),
    "slate_light": (92, 178, 172),

    # Cut masonry: brighter and cooler than `rock_*`, which is a weathered
    # boulder ramp. Walls that are built should not look like scenery.
    "stone_shadow": (108, 104, 118),
    "stone_dark": (150, 146, 162),
    "stone_mid": (196, 194, 208),
    "stone_light": (238, 238, 248),

    # Window light. One warm pair is enough to make a whole village look
    # inhabited at night, and it is the only place in the game that uses it.
    "lamp_glow": (255, 196, 74),
    "lamp_core": (255, 244, 176),

    # Tropical lagoon: saturated blue, deliberately distinct from the
    # greenish `cyan_*` ramp the magic effects use.
    "sea_deep": (26, 92, 168),
    "sea_mid": (48, 148, 216),
    "sea_light": (108, 204, 244),
    "foam": (226, 250, 255),

    # World map: a parchment chart with one wash per biome. Parchment rather
    # than satellite view because the map is a menu, and a menu must be
    # readable at a glance over the five destinations it offers.
    "map_paper": (238, 216, 168),
    "map_paper_shade": (204, 174, 126),
    "map_ink": (78, 54, 48),
    "map_route": (168, 108, 62),
    "map_grass": (140, 196, 96),
    "map_grass_ho": (186, 224, 122),
    "map_forest": (56, 140, 78),
    "map_forest_ho": (92, 182, 100),
    "map_desert": (238, 200, 118),
    "map_desert_ho": (255, 228, 158),
    "map_snow": (206, 230, 250),
    "map_snow_ho": (240, 250, 255),
    "map_ash": (146, 100, 96),
    "map_ash_ho": (196, 140, 122),
    "map_sea": (72, 158, 216),
    "map_sea_ho": (126, 200, 240),
    "map_marker": (238, 68, 76),
    "map_marker_ho": (255, 168, 150),

    # --- ui v3: the DS-era Nintendo menu kit ------------------------------
    #
    # A light blue field, near-white plates, and a hard navy outline around
    # everything. Two constraints shaped these values:
    #
    #   * The blues are desaturated. A fully saturated accent is what makes a
    #     UI look like a web page; DS menus keep their blue closer to slate.
    #   * Every plate tone is far enough from its neighbour to survive a
    #     checkerboard seam, because that is how this kit gets smooth edges out
    #     of a small palette.
    #
    # The ink is navy rather than black: pure black on a blue-white plate reads
    # as a hole punched through it.
    "ui2_bg": (150, 178, 214),
    "ui2_bg_hi": (186, 208, 236),
    "ui2_dot": (162, 188, 220),
    "ui2_line": (38, 54, 92),
    "ui2_line_soft": (122, 146, 184),
    "ui2_plate_hi": (255, 255, 255),
    "ui2_plate": (240, 245, 252),
    "ui2_plate_lo": (218, 230, 246),
    "ui2_plate_deep": (140, 166, 202),
    "ui2_screen": (34, 52, 96),
    "ui2_blue_dark": (28, 66, 128),
    "ui2_blue": (56, 110, 184),
    "ui2_blue_hi": (108, 162, 220),
    "ui2_blue_glow": (196, 220, 246),
    "ui2_ink": (30, 46, 84),
    "ui2_ink_dim": (118, 138, 172),
    "ui2_shadow": (26, 38, 70),

    # UI accents, tuned to stay legible as ink on a near-white plate.
    "ui2_red": (196, 58, 62),
    "ui2_green": (44, 132, 84),
    "ui2_amber": (194, 126, 26),
    "ui2_violet": (108, 78, 168),
    "ui2_cyan": (36, 130, 158),

    # --- ui v4: title dusk + chart dressing --------------------------------
    # A dusk ramp with real steps (the old table jumped straight from violet
    # to orange), two desaturated greens so the meadow sits inside the sunset
    # instead of shouting over it, and scorched paper for the chart's burnt
    # edges.
    "dusk_deep": (44, 24, 66),
    "dusk_indigo": (68, 38, 90),
    "dusk_violet": (102, 54, 104),
    "dusk_plum": (140, 66, 96),
    "haze_mauve": (172, 100, 112),
    "sunset_rose": (212, 92, 62),
    "sunset_ember": (246, 138, 48),
    "sunset_gold": (255, 214, 120),
    "cloud_dusk": (36, 24, 52),
    "dusk_grass": (46, 64, 48),
    "dusk_grass_deep": (26, 40, 32),
    "map_burn": (134, 88, 52),
    "map_burn_deep": (90, 56, 40),
}


# ---------------------------------------------------------------------------
# Palette v5 -- the Pokemon (GBA) retune.
#
# Only RGB values change here, never the ORDER: the palette table is full
# (252/256 slots) and every name is referenced by generated C code, so new
# colors are impossible and reordering would silently recolor the game. The
# retunes below are the ones the Pokemon-style repaint actually needs:
#
#   * outline goes near-black and neutral. The old plum (58,34,52) read as a
#     purple haze around every sprite; GBA Pokemon outlines are crisp dark
#     gray-browns that sit behind the fill, and the crispness is half of what
#     makes those sprites read at 32px.
#   * the fur ramps lose their grey undertone: chestnut mid stops going muddy
#     against grass.
# ----------------------------------------------------------------------------
_RETUNE = {
    "outline": (46, 38, 46),
    "outline_soft": (92, 74, 88),
    "brn_mid2": (180, 120, 62),
    "brn_hi": (224, 170, 100),
    "tan_mid2": (240, 194, 126),
    "tan_hi": (255, 226, 172),
    "grass_mid": (116, 200, 82),
    "grass_light": (176, 236, 112),
    "grn_mid": (72, 172, 78),
    "grn_light": (136, 220, 100),
    "leaf_mid": (68, 184, 92),
    "leaf_light": (140, 230, 110),
    "sea_deep": (32, 96, 176),
    "sea_mid": (56, 152, 220),
    "sea_light": (116, 208, 248),
    "murk_light": (124, 152, 104),
}
for _n, _v in _RETUNE.items():
    _RGB[_n] = _v


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

    # --- craft layer ------------------------------------------------------
    # The shared vocabulary every painter builds on. These exist so a dozen
    # independent scripts light their subjects the same way: a 1px dark
    # outline (auto_outline), a bottom-of-form cel shadow, a top-left rim
    # light, eyes that carry a specular dot, and dithered gradients for the
    # places real art would airbrush. Consistency across painters is what
    # makes 25 independently drawn creatures read as one game.

    def dither_rect(self, x, y, w, h, color, parity=0):
        """Checker dither fill: 50% coverage of `color` over what is there.

        The cheapest honest gradient a no-alpha canvas can do. Stacking two
        of these at different radii with different parities gives the soft
        radial falloff a glow needs.
        """
        col = ix(color) if isinstance(color, str) else color
        for j in range(y, y + h):
            for i in range(x, x + w):
                if ((i + j) & 1) == parity:
                    self.set(i, j, col)

    def dither_ellipse(self, cx, cy, rx, ry, color, parity=0):
        """Checker-dithered ellipse -- the dither_rect of round shapes."""
        col = ix(color) if isinstance(color, str) else color
        if rx <= 0 or ry <= 0:
            return
        for j in range(int(cy - ry), int(cy + ry) + 1):
            for i in range(int(cx - rx), int(cx + rx) + 1):
                dx = (i - cx + 0.5) / rx
                dy = (j - cy + 0.5) / ry
                if dx * dx + dy * dy <= 1.0 and ((i + j) & 1) == parity:
                    self.set(i, j, col)

    def grad_ellipse(self, cx, cy, rx, ry, colors):
        """A radial gradient out of 2-4 tones, dithered at the seams.

        colors runs core -> edge. Each ring is solid out to its radius, then
        dithered across the band to the next tone, which is exactly how the
        eye reads 'glow' at 8bpp with no alpha.
        """
        n = len(colors)
        for k, name in enumerate(colors):
            rk = rx * (1.0 - k / n)
            ryk = ry * (1.0 - k / n)
            if k == 0:
                self.ellipse(cx, cy, rk, ryk, ix(name))
            else:
                self.dither_ellipse(cx, cy, rk, ryk, ix(name),
                                    parity=(k + int(cx) + int(cy)) & 1)

    def rim_light(self, color_name="white", variant=None):
        """Light the top-left silhouette edge, one pixel, in place.

        Replaces the first interior pixel that touches empty space from
        above or from the left -- the two sides the scene's key light comes
        from. Skips pixels that are already the outline colour, so it never
        eats a deliberate dark edge.
        """
        col = ix(color_name, variant)
        edge = ix("outline", variant)
        src = list(self.px)
        for y in range(self.h):
            for x in range(self.w):
                if src[y * self.w + x] in (0, edge):
                    continue
                up = y > 0 and src[(y - 1) * self.w + x] == 0
                left = x > 0 and src[y * self.w + x - 1] == 0
                if up or left:
                    self.set(x, y, col)

    def cel_shade(self, color_name, variant=None, rows=2):
        """Darken the bottom `rows` pixels of every filled column.

        This is the shadow half of the two-plane lighting the prop and
        creature painters share: a top-lit form needs its underside to fall
        away or the shape reads as a sticker.
        """
        col = ix(color_name, variant)
        for x in range(self.w):
            filled = [y for y in range(self.h) if self.get(x, y) != 0]
            if not filled:
                continue
            for y in filled[-rows:]:
                if self.get(x, y) != 0:
                    self.set(x, y, col)

    def sparkle(self, cx, cy, r, color, core=None):
        """A four-point star: the one 'magic' mark every FX reuses.

        Cross arms of length r, diagonal dots at half r, and a bright core
        pixel. Used at r=2 on pickups and r=4 on ultimates.
        """
        col = ix(color) if isinstance(color, str) else color
        cc = ix(core) if isinstance(core, str) else core
        self.vline(cx, cy - r, cy + r, col)
        self.hline(cx - r, cx + r, cy, col)
        if r >= 3:
            for d in (1, r - 1):
                self.set(cx + d, cy + d, col)
                self.set(cx - d, cy + d, col)
                self.set(cx + d, cy - d, col)
                self.set(cx - d, cy - d, col)
        if cc is not None:
            self.set(cx, cy, cc)

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


def paint_eye(c, cx, cy, rx, ry, iris="black", sclera=None, spec="white",
              lid=None, variant=None):
    """One eye, drawn the same way on every creature in the game.

    A sclera disc (when asked), an iris that fills most of it, a black pupil
    when the iris is a colour rather than black, and -- the part that makes
    the difference between 'mark' and 'alive' -- a single white specular dot
    fixed to the upper-left of the pupil. `lid` draws a heavy upper edge for
    angry creatures; `iris="black"` skips the pupil step entirely.
    """
    if sclera:
        c.ellipse(cx, cy, rx + 0.6, ry + 0.6, ix("outline", variant))
        c.ellipse(cx, cy, rx, ry, ix(sclera, variant))
    if iris == "black":
        c.ellipse(cx, cy, rx * 0.7, ry * 0.7, ix("black"))
    else:
        c.ellipse(cx, cy, rx * 0.9, ry * 0.9, ix(iris, variant))
        c.ellipse(cx + 0.4, cy + 0.3, rx * 0.45, ry * 0.45, ix("black"))
    if spec:
        c.set(int(cx - rx * 0.4), int(cy - ry * 0.5), ix(spec, variant))
    if lid:
        c.hline(int(cx - rx), int(cx + rx * 0.6), int(cy - ry),
                ix(lid, variant))


def new_canvas(w, h):
    return Canvas(w, h)


# ---------------------------------------------------------------------------
# Pokemon-style vocabulary (palette v5 painters)
#
# The GBA Pokemon look rests on four habits the old storybook painters did
# not have:
#
#   1. chibi proportions -- the head is the widest shape on the sprite, and
#      the head/body silhouette break is visible from the front;
#   2. big glossy eyes -- an eye is a *feature* (sclera, pupil, two glints),
#      not a dot; a creature reads as alive through its eyes first;
#   3. hard-edged two-plane shading -- the shadow is a shape with a contour,
#      not a gradient;
#   4. a crisp near-black outline hugging a convex silhouette.
#
# pk_eye(), pk_shade_bottom() and pk_rim() below are how the repainted hero,
# monster and world painters all keep to those habits without copying each
# other's code.
# ---------------------------------------------------------------------------

def pk_eye(c, cx, cy, rx, ry, mode="open", iris=None, variant=None,
           sclera="white"):
    """One Pokemon eye. mode: open | blink | hurt.

    Open is a sclera disc, a pupil that fills most of it (a dark pupil on
    white, or a pupil inside a coloured iris), a big square glint at the
    upper-left and a small one at the lower-right -- the double shine that
    makes GBA sprites glossy. Blink draws a closed lid; hurt draws >_<.
    """
    ink = ix("fur_black")
    cx, cy = int(round(cx)), int(round(cy))
    if mode == "open" and rx <= 1.5:
        # Too small for sclera+iris detail: a bead pupil with one glint is
        # all that survives at this scale (GBA tiny sprites do the same).
        c.circle(cx, cy, rx + 0.4, ink)
        c.set(int(cx - rx * 0.4), int(cy - ry * 0.5),
              ix("white", variant))
        return
    if mode == "blink":
        c.hline(cx - int(rx) - 1, cx + int(rx) + 1, cy, ink)
        c.set(cx, cy + 1, ink)
        return
    if mode == "hurt":
        sgn = 1 if cx < c.w // 2 else -1
        for k, dx in enumerate((-1, 0, 1)):
            c.set(cx + sgn * (1 - abs(dx)), cy - 1 + k, ink)
        return
    if sclera:
        c.ellipse(cx, cy, rx + 0.7, ry + 0.7, ix("outline", variant))
        c.ellipse(cx, cy, rx + 0.2, ry + 0.2, ix(sclera, variant))
    if iris:
        c.ellipse(cx, cy, rx * 0.95, ry * 0.95, ix(iris, variant))
        c.ellipse(cx + 0.3, cy + 0.4, rx * 0.5, ry * 0.55, ink)
    else:
        c.ellipse(cx + 0.2, cy + 0.3, rx * 0.62, ry * 0.66, ink)
    if sclera or iris:
        c.set(int(cx - rx * 0.4), int(cy - ry * 0.45), ix("white", variant))
        c.set(int(cx + rx * 0.5), int(cy + ry * 0.35), ix("white", variant))


def pk_shade_bottom(c, color_name, variant=None, rows=2):
    """The shadow plane: darken the bottom `rows` filled pixels of each column.

    Same mechanism as Canvas.cel_shade but it *stops at interior columns*:
    a column whose filled run is only 1-2 px tall (thin limbs, ear tips) is
    left alone, so spindly details do not vanish into the shadow tone.
    """
    col = ix(color_name, variant)
    for x in range(c.w):
        filled = [y for y in range(c.h) if c.get(x, y) != 0]
        if len(filled) <= rows + 1:
            continue
        for y in filled[-rows:]:
            c.set(x, y, col)


def pk_shade_side(c, color_name, variant=None, cols=1):
    """Shade the right-hand interior edge (the side away from the key light).

    One pass over filled pixels whose right neighbour is empty or shaded:
    this is what turns a flat fill into a lit form without a gradient.
    """
    col = ix(color_name, variant)
    edge = ix("outline", variant)
    src = list(c.px)
    for y in range(c.h):
        for x in range(c.w):
            v = src[y * c.w + x]
            if v in (0, col, edge):
                continue
            nx = x + cols
            if nx >= c.w or src[y * c.w + nx] == 0:
                c.set(x, y, col)


def pk_rim(c, color_name, variant=None, protected=()):
    """Light the top-left interior silhouette edge, one pixel.

    Like Canvas.rim_light but with a skip-set (far limbs painted in the deep
    tone stay dark) and an optional skip of the outline colour.
    """
    col = ix(color_name, variant)
    edge = ix("outline", variant)
    src = list(c.px)
    for y in range(c.h):
        for x in range(c.w):
            v = src[y * c.w + x]
            if v == 0 or v in protected or v == col:
                continue
            up = y > 0 and src[(y - 1) * c.w + x] == 0
            left = x > 0 and src[y * c.w + x - 1] == 0
            if up or left:
                c.set(x, y, col)


def pk_outline_grow(c, color_name="outline", variant=None):
    """auto_outline, spelled for the new painters."""
    c.auto_outline(color_name, variant)


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
