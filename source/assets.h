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

// --- skill cards: 12 x (72x96) 8bpp, one picture per card -----------------
#include "card_atlas_bin.h"


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
// with 4 frames each. A mob is drawn at its MonsterDef scale, so its on-screen
// size is not fixed by these -- they are the source art's size.
#define MOB_W         32
#define MOB_H         32
#define MOB_FRAMES    4
#define BOSS_W        64
#define BOSS_H        64
#define BOSS_FRAMES   4


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
