// farm.c -- the kingdom's field.
//
// See farm.h for the design. This file owns three things: the plot array, the
// three context actions (plant / water / harvest), and the drawing of the
// field in the world and its panel on the bottom screen.
//
// The field is drawn rather than baked into the hub map so that a plot can
// change under the player's feet. That also means the terrain under it stays
// lawn, which is what the player walks on -- the plots are decoration plus a
// hitbox, never collision. A field you can't walk across would make the park
// worse to move around in, and nothing about farming needs collision.

#include <stdio.h>
#include <string.h>

#include "game.h"
#include "assets.h"
#include "farm.h"
#include "farm_art.h"
#include "ui_draw.h"

FarmPlot g_farm[FARM_PLOTS];
uint8_t  g_seeds[FARM_CROP_COUNT];
uint8_t  g_cake_have[INGREDIENT_COUNT];
uint16_t g_day = 1;

static const uint8_t *farm_tiles(void) { return farm_tiles_bin; }

// The cake's quota per region, indexed by region == crop id. Kept as a table
// rather than as arithmetic over the region target because the two numbers mean
// different things now: the target is an expedition's length, this is a
// planting cycle's output.
static const uint8_t s_cake_need[INGREDIENT_COUNT] = {
    CAKE_NEED_0, CAKE_NEED_1, CAKE_NEED_2, CAKE_NEED_3, CAKE_NEED_4,
};

int cake_need(int region)
{
    if (region < 0 || region >= INGREDIENT_COUNT)
        return 0;
    return s_cake_need[region];
}

bool cake_region_done(int region)
{
    if (region < 0 || region >= INGREDIENT_COUNT)
        return true;
    return g_cake_have[region] >= s_cake_need[region];
}

bool cake_ready(void)
{
    for (int i = 0; i < INGREDIENT_COUNT; i++)
        if (!cake_region_done(i))
            return false;
    return true;
}

int cake_done_count(void)
{
    int n = 0;
    for (int i = 0; i < INGREDIENT_COUNT; i++)
        if (cake_region_done(i))
            n++;
    return n;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

void farm_init(void)
{
    memset(g_farm, 0, sizeof(g_farm));
    memset(g_seeds, 0, sizeof(g_seeds));
    memset(g_cake_have, 0, sizeof(g_cake_have));
    g_day = 1;
}

uint8_t farm_pack_plot(const FarmPlot *p)
{
    return (uint8_t)((p->crop & 0x07) | ((p->stage & 0x03) << 3) |
                     (p->watered ? 0x20 : 0));
}

void farm_unpack_plot(FarmPlot *p, uint8_t b)
{
    p->crop = b & 0x07;
    p->stage = (b >> 3) & 0x03;
    p->watered = (b & 0x20) != 0;
    if (p->crop == 0)
    {
        p->stage = 0;
        p->watered = false;
    }
}

int farm_plot_at(int32_t wx, int32_t wy)
{
    int lx = (int)(wx - FARM_ORIGIN_X);
    int ly = (int)(wy - FARM_ORIGIN_Y);
    if (lx < 0 || ly < 0 || lx >= FARM_PX_W || ly >= FARM_PX_H)
        return -1;
    return (ly / 16) * FARM_COLS + (lx / 16);
}

bool farm_in_reach(int32_t px, int32_t py)
{
    // Distance to the field's rectangle rather than to its centre: the field
    // is 96 px wide, so a centre test would make the far corners unreachable
    // and the near ones too forgiving.
    int dx = 0, dy = 0;
    if (px < FARM_ORIGIN_X)                 dx = (int)(FARM_ORIGIN_X - px);
    else if (px > FARM_ORIGIN_X + FARM_PX_W) dx = (int)(px - FARM_ORIGIN_X - FARM_PX_W);
    if (py < FARM_ORIGIN_Y)                 dy = (int)(FARM_ORIGIN_Y - py);
    else if (py > FARM_ORIGIN_Y + FARM_PX_H) dy = (int)(py - FARM_ORIGIN_Y - FARM_PX_H);
    return dx * dx + dy * dy <= FARM_REACH * FARM_REACH;
}

int farm_seed_total(void)
{
    int n = 0;
    for (int i = 0; i < FARM_CROP_COUNT; i++)
        n += g_seeds[i];
    return n;
}

int farm_ripe_count(void)
{
    int n = 0;
    for (int i = 0; i < FARM_PLOTS; i++)
        if (g_farm[i].crop && g_farm[i].stage >= FARM_STAGE_RIPE)
            n++;
    return n;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

// First crop index with a seed in the satchel, or -1. Planting uses this
// rather than a picker: with at most five seed types and a field that is
// usually part-planted, "plant what I have" is what the player wants almost
// every time, and it keeps the field playable with one button.
static int first_seed_crop(void)
{
    for (int i = 0; i < FARM_CROP_COUNT; i++)
        if (g_seeds[i] > 0)
            return i;
    return -1;
}

int farm_plot_action(int plot, bool announce)
{
    if (plot < 0 || plot >= FARM_PLOTS)
        return -1;
    FarmPlot *p = &g_farm[plot];

    // Ripe: harvest.
    if (p->crop && p->stage >= FARM_STAGE_RIPE)
    {
        int crop = p->crop - 1;
        farm_bank((uint8_t)crop, FARM_YIELD);
        p->crop = 0;
        p->stage = 0;
        p->watered = false;
        tutorial_notify(TUT_EV_HARVESTED);
        if (announce)
        {
            audio_play_sfx(SND_HEART);
            char buf[64];
            snprintf(buf, sizeof(buf), "收获%s +%d  (%d/%d)",
                     g_regions[crop].ingredient, FARM_YIELD,
                     (int)g_cake_have[crop], cake_need(crop));
            hub_toast(buf, PAL_UI_GOLD_LIGHT, 150);
        }
        return crop;
    }

    // Planted but dry: water it. Unwatered crops do not grow, which is the
    // only rule that makes the field a routine rather than a decoration.
    if (p->crop && !p->watered)
    {
        p->watered = true;
        tutorial_notify(TUT_EV_WATERED);
        if (announce)
        {
            audio_play_sfx(SND_HEAL);
            hub_toast("浇水了", PAL_UI2_CYAN, 90);
        }
        return p->crop - 1;
    }

    // Empty: plant the first seed we hold.
    if (!p->crop)
    {
        int crop = first_seed_crop();
        if (crop < 0)
        {
            if (announce)
            {
                audio_play_sfx(SND_UI_MOVE);
                hub_toast("没有种子", PAL_UI2_RED, 110);
            }
            return -1;
        }
        g_seeds[crop]--;
        p->crop = (uint8_t)(crop + 1);
        p->stage = FARM_STAGE_SPROUT;
        p->watered = false;
        tutorial_notify(TUT_EV_PLANTED);
        if (announce)
        {
            audio_play_sfx(SND_UI_CONFIRM);
            char buf[64];
            snprintf(buf, sizeof(buf), "种下 %s", g_regions[crop].ingredient);
            hub_toast(buf, PAL_UI2_GREEN, 110);
        }
        return crop;
    }

    // Planted, watered, still growing.
    if (announce)
    {
        audio_play_sfx(SND_UI_MOVE);
        hub_toast("还在长呢", PAL_UI2_INK_DIM, 100);
    }
    return -1;
}

int farm_water_all(bool announce)
{
    int n = 0;
    for (int i = 0; i < FARM_PLOTS; i++)
    {
        if (g_farm[i].crop && !g_farm[i].watered)
        {
            g_farm[i].watered = true;
            n++;
        }
    }
    if (n > 0)
        tutorial_notify(TUT_EV_WATERED);
    if (announce)
    {
        if (n > 0)
        {
            audio_play_sfx(SND_HEAL);
            char buf[64];
            snprintf(buf, sizeof(buf), "给 %d 块地浇了水", n);
            hub_toast(buf, PAL_UI2_CYAN, 130);
        }
        else
        {
            audio_play_sfx(SND_UI_MOVE);
            hub_toast("没有需要浇水的地", PAL_UI2_INK_DIM, 110);
        }
    }
    return n;
}

int farm_advance_day(void)
{
    int grew = 0;
    for (int i = 0; i < FARM_PLOTS; i++)
    {
        FarmPlot *p = &g_farm[i];
        if (!p->crop)
            continue;
        // A watered crop grows one stage and the watering is spent; an
        // unwatered one simply waits. That is the whole day cycle.
        if (p->watered && p->stage < FARM_STAGE_RIPE)
        {
            p->stage++;
            grew++;
        }
        p->watered = false;
    }
    g_day++;
    if (grew > 0)
        tutorial_notify(TUT_EV_SLEPT);
    return grew;
}

void farm_bank(uint8_t crop, int amount)
{
    if (crop >= INGREDIENT_COUNT || amount <= 0)
        return;
    int need = s_cake_need[crop];
    int have = g_cake_have[crop];
    if (have >= need)
        return;          // already met; the surplus is not carried anywhere
    if (have + amount > need)
        amount = need - have;
    g_cake_have[crop] = (uint8_t)(have + amount);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void blit_tile(int tile, int32_t wx, int32_t wy, bool shadow)
{
    if (shadow)
        gfx_blit_8bpp(farm_tiles(), 16, 16, tile, wx, wy, false, true);
    else
        gfx_blit_8bpp(farm_tiles(), 16, 16, tile, wx, wy, false, false);
}

// Which tile a plot shows: soil, plus the crop at its current stage.
static int plot_tile(const FarmPlot *p)
{
    if (!p->crop)
        return p->watered ? FARM_TILE_SOIL_WET : FARM_TILE_SOIL_DRY;
    int stage = p->stage;
    if (stage >= FARM_STAGE_COUNT)
        stage = FARM_STAGE_COUNT - 1;
    return FARM_TILE_CROP(p->crop - 1, stage);
}

void farm_draw(void)
{
    if (!gfx_onscreen(FARM_ORIGIN_X, FARM_ORIGIN_Y, FARM_PX_W, FARM_PX_H))
        return;

    bool near = false;
    if (g_heroes[g_local_hero].active)
    {
        int32_t cx, cy;
        hero_center(&g_heroes[g_local_hero], &cx, &cy);
        near = farm_in_reach(cx, cy);
    }

    for (int i = 0; i < FARM_PLOTS; i++)
    {
        int px = FARM_ORIGIN_X + (i % FARM_COLS) * 16;
        int py = FARM_ORIGIN_Y + (i / FARM_COLS) * 16;
        const FarmPlot *p = &g_farm[i];

        // Soil first as a flat blit, then the crop with a shadow so a plant
        // stands on the bed instead of floating over it.
        int tile = plot_tile(p);
        if (p->crop)
        {
            blit_tile(p->watered ? FARM_TILE_SOIL_WET : FARM_TILE_SOIL_DRY,
                      px, py, false);
            blit_tile(tile, px, py, true);
        }
        else
        {
            blit_tile(tile, px, py, false);
        }

        // Ripe: a pulsing ring, so the one thing you can act on announces
        // itself from across the park.
        if (p->crop && p->stage >= FARM_STAGE_RIPE)
        {
            bool on = ((g_frame >> 3) & 1) != 0;
            if (on)
                gfx_circle_outline(px + 8, py + 8, 7, PAL_UI_GOLD_LIGHT);
        }
    }

    // A fence line drawn as a thin inked border: outlines only, so the plots
    // keep their own colour and the field reads as one tended bed.
    if (near)
        gfx_rect_outline(FARM_ORIGIN_X - 2, FARM_ORIGIN_Y - 2,
                         FARM_PX_W + 4, FARM_PX_H + 4, PAL_UI_GOLD_LIGHT);
    else
        gfx_rect_outline(FARM_ORIGIN_X - 2, FARM_ORIGIN_Y - 2,
                         FARM_PX_W + 4, FARM_PX_H + 4, PAL_OUTLINE);
}

// ---------------------------------------------------------------------------
// Bottom-screen panel
// ---------------------------------------------------------------------------

// The field panel: the day, the seed satchel, and how the field is doing. It
// is deliberately a readout rather than a grid of touch targets -- the plots
// are worked by standing next to them and pressing A, and a second way to do
// the same thing would only be a second thing to keep in sync.
void farm_draw_panel(int y)
{
    ui_plate(UI_SUB, 8, y, 96, 60, UI_R_PLATE);
    ui_text(UI_SUB, 14, y + 6, "菜地", PAL_UI2_INK_DIM);

    char buf[48];
    snprintf(buf, sizeof(buf), "第 %d 天", g_day);
    ui_text(UI_SUB, 14, y + 20, buf, PAL_UI2_INK);

    int ripe = farm_ripe_count();
    int planted = 0;
    for (int i = 0; i < FARM_PLOTS; i++)
        if (g_farm[i].crop)
            planted++;

    snprintf(buf, sizeof(buf), "可收 %d", ripe);
    ui_text(UI_SUB, 14, y + 32, buf, ripe ? PAL_UI2_GREEN : PAL_UI2_INK_DIM);
    snprintf(buf, sizeof(buf), "已种 %d/%d", planted, FARM_PLOTS);
    ui_text(UI_SUB, 14, y + 44, buf, PAL_UI2_INK_DIM);

    // Seed satchel: one chip per crop, dimmed at zero. The crop's own colour is
    // what ties this column to the plot art in the field above.
    ui_plate(UI_SUB, 108, y, 140, 60, UI_R_PLATE);
    ui_text(UI_SUB, 114, y + 6, "种子袋", PAL_UI2_INK_DIM);
    for (int i = 0; i < FARM_CROP_COUNT; i++)
    {
        int cx = 114 + i * 27;
        int cy = y + 20;
        bool have = g_seeds[i] > 0;
        ui_well(UI_SUB, cx, cy, 24, 34, UI_R_TAB, PAL_UI2_SCREEN);
        ui_label(UI_SUB, cx, cy + 2, 24, 12,
                 g_regions[i].name,
                 have ? PAL_WHITE : PAL_UI2_INK_DIM);
        snprintf(buf, sizeof(buf), "%d", g_seeds[i]);
        ui_label(UI_SUB, cx, cy + 16, 24, 14, buf,
                 have ? PAL_UI_GOLD_LIGHT : PAL_UI2_INK_DIM);
    }
}

const char *farm_prompt(void)
{
    int ripe = farm_ripe_count();
    int seeds = farm_seed_total();
    if (ripe > 0)
        return "按A收获成熟的作物";
    if (seeds > 0)
        return "按A播种 · 按B浇水";
    return "空着的地 · 先去远征捡种子";
}
