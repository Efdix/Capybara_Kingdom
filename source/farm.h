// farm.h -- the kingdom's field: tilled plots, seeds and the day cycle.
//
// The cake ingredients used to drop straight off monsters. Now the monsters
// drop SEEDS, the seeds are planted in the park, and the ingredients are what
// the field pays out -- so an expedition earns you the means to farm, and the
// farm is what actually fills the cake bar.
//
// The field lives in the hub at a fixed rectangle of lawn (FARM_ORIGIN_X/Y).
// It is drawn at runtime rather than baked into the hub map, because a plot is
// a different picture every day: bare, wet, sprouting, ripe. verify_assets.py
// checks the rectangle is walkable in the generated hub map, so the field can
// never end up inside a pond or under a house.
//
// Crop index == region index, deliberately. Region r's monsters drop seed r,
// planting seed r grows crop r, and harvesting crop r banks ingredients for
// region r. One number carries the whole chain, which is what keeps the UI
// (and the player's head) from needing a lookup table.

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "game.h"
// The sheet layout (tile ids, crop/stage counts) is owned by the art
// generator, and verify_assets.py checks this file and that header agree.
#include "farm_art.h"

// ---------------------------------------------------------------------------
// Geometry
//
// The rectangle is 6x4 tiles of the hub lawn at (9, 21). Chosen because it is
// plain ground a few steps south-west of the plaza, clear of the garden paths,
// the ponds and every landmark's approach.
// ---------------------------------------------------------------------------

#define FARM_COLS      6
#define FARM_ROWS      4
#define FARM_PLOTS     (FARM_COLS * FARM_ROWS)

#define FARM_TILE_X    9     // hub tile of the field's top-left corner
#define FARM_TILE_Y    21
#define FARM_ORIGIN_X  (FARM_TILE_X * 16)
#define FARM_ORIGIN_Y  (FARM_TILE_Y * 16)
#define FARM_PX_W      (FARM_COLS * 16)
#define FARM_PX_H      (FARM_ROWS * 16)

// How many ingredients one ripe plot pays out. Three, so the cake's quota per
// region (12/15/18/21/24) comes out as 4/5/6/7/8 harvests -- a quantity the
// 24-plot field can actually supply in a couple of nights, and one that keeps
// the last region the biggest ask without making it a grind.
#define FARM_YIELD     3

// A crop is ripe after two watered days: planted at stage 0, stage 1 after the
// first night, stage 2 (harvestable) after the second.
#define FARM_STAGE_SPROUT 0
#define FARM_STAGE_GROWN  1
#define FARM_STAGE_RIPE   2

// Interaction reach, in pixels from the field's rectangle. Generous for the
// same reason HUB_REACH is: the player should never have to line up on a tile
// to press A.
#define FARM_REACH     26

// ---------------------------------------------------------------------------
// How much of each region's ingredient the cake needs
//
// Deliberately much smaller than the old "target" the field bar counts. The
// target is an expedition's pacing (how long a region takes to reach its boss);
// this is the farm's quota, and it has to be something a planting cycle can
// actually deliver. At FARM_YIELD per plot the five regions ask for
// 4/5/6/7/8 harvests, which one well-planted field supplies.
//
// (FARM_YIELD itself is defined once, with the rest of the geometry above. It
// was defined here a second time too, which meant an edit to the first one
// silently did nothing -- the same trap MAX_FLOATERS had in game.h.)
// ---------------------------------------------------------------------------

#define CAKE_NEED_0    12
#define CAKE_NEED_1    15
#define CAKE_NEED_2    18
#define CAKE_NEED_3    21
#define CAKE_NEED_4    24

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t crop;      // 0 = empty, else 1 + crop index
    uint8_t stage;     // FARM_STAGE_*
    bool    watered;   // grew tonight's stage already
} FarmPlot;

extern FarmPlot g_farm[FARM_PLOTS];
extern uint8_t  g_seeds[FARM_CROP_COUNT];             // seeds in the satchel
extern uint8_t  g_cake_have[INGREDIENT_COUNT];        // ingredients harvested
extern uint16_t g_day;                                // days since the start

// How many of region `i`'s ingredient the cake needs, and whether it is met.
int  cake_need(int region);
bool cake_region_done(int region);
bool cake_ready(void);
int  cake_done_count(void);

// Packed form for the network mirror (farm.h owns the bit layout so farm.c and
// net.c cannot disagree about it): bits 0-2 crop, bits 3-4 stage, bit 5 watered.
uint8_t farm_pack_plot(const FarmPlot *p);
void    farm_unpack_plot(FarmPlot *p, uint8_t b);

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Wipe every plot, seed and banked ingredient. Called by game_init(), so a new
// game starts with a bare field.
void farm_init(void);

// Which plot covers this world point, or -1. Purely geometric: it answers for
// points outside the field too, so the caller can decide about reach.
int  farm_plot_at(int32_t wx, int32_t wy);

// Is a capybara centred at (px, py) close enough to work the field?
bool farm_in_reach(int32_t px, int32_t py);

// Total seeds held across every crop.
int  farm_seed_total(void);

// Plots that are ripe and waiting.
int  farm_ripe_count(void);

// The single context action for one plot: harvest if ripe, water if planted
// and dry, plant if empty and the satchel has a seed. Returns the crop index
// acted on, or -1 if there was nothing to do. `announce` is false when the
// caller wants the change without a toast (the net mirror).
int  farm_plot_action(int plot, bool announce);

// Water every planted, unwatered plot at once. This is the field's quality of
// life button: twenty-four separate presses is a chore, one is a routine.
int  farm_water_all(bool announce);

// Spend the night: every watered crop advances one stage and the soil dries.
// Returns the number of plots that grew.
int  farm_advance_day(void);

// Add a harvest to the region's bank, capping at the region target.
void farm_bank(uint8_t crop, int amount);

// Draw the field, its crops and its interaction ring. World space, so it slots
// in between render_world() and the characters.
void farm_draw(void);

// The bottom screen's field panel: seeds held, day, and what the plots are
// doing. `y` is where to start the block.
void farm_draw_panel(int y);

// One-line summary for the hub's proximity prompt.
const char *farm_prompt(void);
