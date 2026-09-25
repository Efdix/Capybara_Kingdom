// assets.h -- every generated asset, exposed as C symbols.
//
// The .bin files under graphics/ and data/ are converted by bin2c at build
// time, which emits both the array and a `<name>_bin_size` macro. We include
// those generated headers directly rather than re-declaring the symbols, so
// the C side can never drift out of sync with what the generators produced.
// The Makefile adds the generated header directory to the include path and
// makes every source file depend on it.
//
// Keep the list below in sync with tools/gen_*.py.

#pragma once

#include <stdint.h>

// --- shared palette (256 x u16 BGR555) -------------------------------------
#include "palette_bin.h"

// --- characters: 8 directions x 16 frames, 32x32 8bpp ----------------------
// The two playable characters are drawn as the glyphs 卡 and 皮.
#include "hero_m_tiles_bin.h"
#include "hero_f_tiles_bin.h"

// --- monsters: 15 mobs (32x32) and 10 bosses (64x64) -----------------------
#include "mob_tiles_bin.h"
#include "boss_tiles_bin.h"

// --- skill cards: 20 x (72x96) 8bpp, one picture per card -----------------
// Twelve base cards then the eight fusion results, in CARD_FUSE_* order. The
// count is CARD_COUNT in game.h, and verify_assets.py now checks this sheet
// against that number rather than a literal, so the two cannot drift apart.
#include "card_atlas_bin.h"

// --- the kingdom's field: 24 x (16x16) 8bpp --------------------------------
// Soil (dry/wet) plus five crops at three growth stages, drawn per plot at
// runtime because a plot's picture changes every day. Layout constants live in
// source/farm_art.h, which the generator owns.
#include "farm_tiles_bin.h"


// --- regions: a 48-tile 16x16 atlas + a tile map, per region ---------------
//
// The world is much larger than the screen, so it is stored as an atlas plus
// one byte per tile position rather than as pixels. `_prop.bin` is one flag per
// atlas tile saying whether that tile is solid, which keeps the engine from
// hardcoding which tile ids are walls.
//
// Area ids: 0 hub, 1 rain, 2 farm, 3 past, 4 marsh, 5 cacao.
#include "area_hub_atlas_bin.h"
#include "area_hub_map_bin.h"
#include "area_hub_prop_bin.h"
#include "area_rain_atlas_bin.h"
#include "area_rain_map_bin.h"
#include "area_rain_prop_bin.h"
#include "area_farm_atlas_bin.h"
#include "area_farm_map_bin.h"
#include "area_farm_prop_bin.h"
#include "area_past_atlas_bin.h"
#include "area_past_map_bin.h"
#include "area_past_prop_bin.h"
#include "area_marsh_atlas_bin.h"
#include "area_marsh_map_bin.h"
#include "area_marsh_prop_bin.h"
#include "area_cacao_atlas_bin.h"
#include "area_cacao_map_bin.h"
#include "area_cacao_prop_bin.h"

#define AREA_REGION_COUNT 6

const uint8_t *area_atlas(int area_id);
const uint8_t *area_map(int area_id);
const uint8_t *area_prop(int area_id);
// Tile dimensions of an area's map buffer (the buffers carry no length).
void area_size(int area_id, int *cols, int *rows);

// --- full-screen UI art: 256x192 8bpp ---------------------------------------
// The title scene and the expedition chart. These are UI compositions rather
// than parts of the world, so they are blitted in screen space.
#include "ui_title_bin.h"
#include "ui_worldmap_bin.h"

// --- CJK font atlas: 16x16 4bpp glyphs, order given by font_chars.h ---------
#include "font_glyphs_bin.h"

// ---------------------------------------------------------------------------
// Sprite geometry
// ---------------------------------------------------------------------------

#define HERO_W        32
#define HERO_H        32
#define HERO_DIRS     8
#define HERO_FRAMES   16     // per direction: idle 4 / walk 6 / attack 4 / hurt 2

// Enemies. One 32x32 sheet holds 15 mobs with 4 frames each; bosses are 64x64
// with 8 frames each -- the four walk poses plus the four poses a duel reads
// off body language (wind-up / signature / enraged / broken). A mob is drawn at
// its MonsterDef scale, so its on-screen size is not fixed by these -- they are
// the source art's size.
#define MOB_W         32
#define MOB_H         32
#define MOB_FRAMES    4
#define BOSS_W        64
#define BOSS_H        64
#define BOSS_FRAMES   8

// Boss frame indices within a boss block. The order is fixed by
// tools/gen_monsters.py (BOSS_VARIANTS) and must match these.
#define BF_IDLE       0
#define BF_BOB        1
#define BF_ATTACK     2
#define BF_HURT       3
#define BF_WINDUP     4   // coiled back and low: about to attack
#define BF_SPECIAL_A  5   // the signature move's own posture (dig / rise)
#define BF_SPECIAL_B  6   // the enraged stance, worn through phases 2 and 3
#define BF_STAGGER    7   // on its knees: the punish window


// Character frame indices within a direction block. The sheet order is fixed by
// tools/gen_heroes.py and must match these.
#define HF_IDLE       0
#define HF_IDLE_N     4
#define HF_WALK_A     4
#define HF_WALK_N     6
#define HF_ATTACK     10
#define HF_ATTACK_N   4
#define HF_HURT       14
#define HF_HURT_N     2

// Glyph cell size and atlas metrics
#define GLYPH_CELL    16
#define GLYPH_BYTES   128    // 4bpp 16x16 = 4 NDS tiles * 32 bytes

// Palette is always 256 entries of u16.
#define PALETTE_ENTRIES 256
