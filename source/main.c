// Entry point, state machine and main loop.
//
// Flow: title -> mode select (single / host / join) -> hub -> pick a region ->
// survive it, gathering that region's ingredient until the boss shows up and
// goes down -> the ingredient is secured -> back to the kingdom. Collect all
// five and the cake can be baked.
//
// Two consoles can play the whole thing together over the DS local wireless
// link; the host drives the run and the client mirrors it.

#include <string.h>
#include <stdio.h>

#include "game.h"
#include "assets.h"
#include "skill_tree.h"

// ---------------------------------------------------------------------------
// Small UI helpers
// ---------------------------------------------------------------------------

// Fade both screens by driving the master brightness register.
//   level: 0 = black, 16 = full brightness
// libnds' setBrightness() takes (screen, level) where screen is 1 = main,
// 2 = sub, 3 = both, and level is -16 (black) .. 0 (normal) .. +16 (white).
// Passing 3 for "both" in one call is correct; the earlier version poked the
// two screens separately with the wrong screen ids, which left the display
// stuck at its power-on black level.
static void set_brightness(int level)
{
    if (level < 0)
        level = 0;
    if (level > 16)
        level = 16;
    setBrightness(3, level - 16);
}

// ---------------------------------------------------------------------------
// Title screen
// ---------------------------------------------------------------------------

static void draw_title(void)
{
    // Background: a night sky with a cake, drawn straight into the world bitmap
    // so it shares the game's palette and look.
    for (int y = 0; y < AREA_H; y++)
    {
        uint8_t c = (y < 60) ? PAL_PURPLE_DARK :
                    (y < 110) ? PAL_CASTLE_DARK : PAL_UI_PANEL;
        gfx_fill_rect(0, y, AREA_W, 1, c);
    }
    // stars
    for (int i = 0; i < 60; i++)
    {
        int x = (i * 37) % AREA_W;
        int y = (i * 53) % 100;
        gfx_fill_rect(x, y, 2, 2, (i % 3) ? PAL_WHITE_SPARK : PAL_YELLOW_LIGHT);
    }

    // A big layer cake in the middle.
    int cx = AREA_W / 2;
    int base_y = 120;
    gfx_fill_rect(cx - 40, base_y, 80, 26, PAL_CHOCO_DARK);
    gfx_fill_rect(cx - 38, base_y + 2, 76, 22, PAL_CHOCO_MID);
    gfx_fill_rect(cx - 30, base_y - 22, 60, 24, PAL_SPONGE_DARK);
    gfx_fill_rect(cx - 28, base_y - 20, 56, 20, PAL_SPONGE_MID);
    gfx_fill_rect(cx - 20, base_y - 38, 40, 18, PAL_CREAM_DARK);
    gfx_fill_rect(cx - 18, base_y - 36, 36, 15, PAL_CREAM_MID);
    // frosting drips
    for (int i = 0; i < 8; i++)
        gfx_fill_rect(cx - 38 + i * 10, base_y - 2, 6, 6, PAL_CREAM_MID);
    // cherry + candle
    gfx_fill_rect(cx - 4, base_y - 44, 8, 7, PAL_CHERRY_MID);
    gfx_fill_rect(cx - 3, base_y - 43, 4, 4, PAL_CHERRY_LIGHT);
    gfx_fill_rect(cx - 1, base_y - 58, 3, 14, PAL_CYAN_MID);
    gfx_fill_rect(cx, base_y - 62, 1, 5, PAL_EMBER_YELLOW);
    gfx_fill_rect(cx - 2, base_y - 64, 5, 4, PAL_EMBER_ORANGE);

    // The two capybaras beside the cake, seen from behind, using the real
    // character art. Male on the left, female on the right.
    gfx_blit_8bpp(hero_m_tiles_bin, HERO_W, HERO_H, DIR_UP * HERO_FRAMES,
                  cx - 78, base_y - 2, false, true);
    gfx_blit_8bpp(hero_f_tiles_bin, HERO_W, HERO_H, DIR_UP * HERO_FRAMES,
                  cx + 46, base_y - 2, false, true);
}

static const char *title_items[] = {
    "开始游戏",
    "设置",
};
#define TITLE_COUNT 2

static void title_screen(void)
{
    // The title is drawn in SCREEN space, not world space: it is its own
    // composition and has no world bound. Pinning the camera to the origin is
    // what makes the gfx_* world-to-screen translation an identity here, so
    // the coordinates below read as plain screen pixels.
    render_camera(0, 0);
    draw_title();
    text_clear();
    text_panel(2, 1, 16, 3);
    text_draw(4, 2, "水豚生存", PAL_UI_GOLD_LIGHT);
    text_draw(3, 3, "生日蛋糕大冒险", PAL_UI_TEXT);

    if (g_ingredients_done == (1 << INGREDIENT_COUNT) - 1)
        text_draw(3, 5, "材料已集齐！", PAL_GREEN_SPARK);

    text_panel(6, 7, 8, 4);
    for (int i = 0; i < TITLE_COUNT; i++)
    {
        bool sel = (i == g_menu_cursor);
        text_printf(6, 8 + i * 2, sel ? PAL_WHITE_SPARK : PAL_UI_TEXT_DIM,
                    "%s%s", sel ? "> " : "  ", title_items[i]);
    }

    text_draw(1, 12, "上下选择 A确定 START制作名单", PAL_UI_TEXT_DIM);

    audio_play_music(MUS_TITLE);
}

// ---------------------------------------------------------------------------
// Character select
// ---------------------------------------------------------------------------

// The two capybaras. Which one you pick is not cosmetic: their ultimates do
// different things (see hero_release_ultimate), so this is where the run's
// shape gets decided.
static const char *char_names[2] = {
    "阿棕（公）",
    "阿奶（母）",
};

static const char *char_ult[2] = {
    "大招 · 怒涛冲击\n扫倒身边一圈敌人，清出退路",
    "大招 · 温柔庇护\n回满血、安抚全场、吸走所有掉落",
};

static void char_select_screen(void)
{
    // Top screen: the real sprites at 2x, so the fur, the belly and the head
    // accessory are legible enough to choose on.
    render_camera(0, 0);
    gfx_fill_rect(0, 0, AREA_W, AREA_H, PAL_UI_PANEL);
    gfx_fill_rect(0, 0, AREA_W, 18, PAL_PURPLE_DARK);

    for (int i = 0; i < 2; i++)
    {
        const uint8_t *sheet = (i == 0) ? hero_m_tiles_bin : hero_f_tiles_bin;
        bool sel = (i == g_menu_cursor);
        int x = 24 + i * 112;

        gfx_fill_rect(x, 42, 96, 104, sel ? PAL_UI_FRAME : PAL_GREY_DARK);
        gfx_fill_rect(x + 3, 45, 90, 98, PAL_UI_PANEL);
        gfx_blit_8bpp_scaled(sheet, HERO_W, HERO_H, DIR_DOWN * HERO_FRAMES,
                             x + 16, 62, false, false, 200);

        if (sel)
        {
            // A caret over the chosen one; the frame alone is too quiet at a
            // glance and this screen is the only place a choice like this is
            // ever made.
            for (int k = 0; k < 6; k++)
                gfx_fill_rect(x + 48 - 1 - k, 30 + k, 2 + k * 2, 2,
                              PAL_UI_GOLD_LIGHT);
        }
    }

    // Bottom screen: names, and what each one's ultimate actually does.
    text_clear();
    text_panel(1, 0, 18, 2);
    text_draw(2, 0, "选择角色", PAL_UI_GOLD_LIGHT);
    text_draw(13, 0, "左右选择", PAL_UI_TEXT_DIM);

    for (int i = 0; i < 2; i++)
    {
        bool sel = (i == g_menu_cursor);
        text_printf(2 + i * 10, 2, sel ? PAL_WHITE_SPARK : PAL_UI_TEXT_DIM,
                    "%s%s", sel ? "> " : "  ", char_names[i]);
    }

    text_panel(1, 5, 18, 5);
    text_draw(2, 6, char_ult[g_menu_cursor],
              (g_menu_cursor == 0) ? PAL_ORANGE_MID : PAL_CYAN_LIGHT);
    text_draw(2, 10, "左右选择 A确定 B返回", PAL_UI_TEXT_DIM);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

static const char *settings_items[] = {
    "音乐",
    "音效",
    "返回",
};
#define SETTINGS_COUNT 3

// Words rather than a bar: four steps is few enough to name, and a name is
// unambiguous where a half-filled bar is not.
static const char *vol_names[AUDIO_VOL_LEVELS] = { "关", "小", "中", "大" };

static void settings_screen(void)
{
    text_clear();
    text_panel(1, 1, 18, 9);
    text_draw(2, 2, "设置", PAL_UI_GOLD_LIGHT);

    for (int i = 0; i < SETTINGS_COUNT; i++)
    {
        bool sel = (i == g_menu_cursor);
        text_printf(2, 4 + i * 2, sel ? PAL_WHITE_SPARK : PAL_UI_TEXT_DIM,
                    "%s%s", sel ? "> " : "  ", settings_items[i]);

        if (i == 0)
            text_printf(13, 4, sel ? PAL_UI_GOLD_LIGHT : PAL_UI_TEXT_DIM,
                        "%s", vol_names[audio_get_music_volume()]);
        else if (i == 1)
            text_printf(13, 6, sel ? PAL_UI_GOLD_LIGHT : PAL_UI_TEXT_DIM,
                        "%s", vol_names[audio_get_sfx_volume()]);
    }

    text_draw(1, 11, "上下选择 左右调整 B返回", PAL_UI_TEXT_DIM);
}

// ---------------------------------------------------------------------------
// Mode select
// ---------------------------------------------------------------------------

static const char *mode_items[] = {
    "单人冒险",
    "创建派对（联机主机）",
    "加入派对（联机客机）",
};
#define MODE_COUNT 3

static void mode_select_screen(void)
{
    text_clear();
    text_panel(1, 1, 18, 8);
    text_draw(2, 2, "选择模式", PAL_UI_GOLD_LIGHT);
    for (int i = 0; i < MODE_COUNT; i++)
    {
        const char *arrow = (i == g_menu_cursor) ? "> " : "  ";
        text_printf(1, 4 + i * 2, (i == g_menu_cursor) ? PAL_WHITE_SPARK
                                                      : PAL_UI_TEXT_DIM,
                    "%s%s", arrow, mode_items[i]);
    }
    text_draw(2, 11, "上下选择 A确定 B返回", PAL_UI_TEXT_DIM);
}

// ---------------------------------------------------------------------------
// Hub (the kingdom)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Region intro / clear
// ---------------------------------------------------------------------------

static void level_intro_screen(void)
{
    const RegionDef *rg = &g_regions[g_menu_cursor];
    text_clear();
    text_panel(1, 2, 18, 9);
    text_printf(2, 3, PAL_UI_GOLD_LIGHT, "%s", rg->name);
    text_draw(2, 5, rg->intro, PAL_UI_TEXT);
    text_printf(2, 7, PAL_CYAN_LIGHT, "收集%s 迎战双霸主", rg->ingredient);
    text_printf(2, 8, PAL_YELLOW_LIGHT, "先锋：%s", rg->mid_boss_name);
    text_printf(2, 9, PAL_ORANGE_MID, "霸主：%s", rg->boss_name);
    text_draw(2, 11, "按 A 出发", PAL_WHITE_SPARK);
}

static void level_clear_screen(void)
{
    const RegionDef *rg = &g_regions[g_region_id];
    text_clear();
    text_panel(1, 3, 18, 7);
    text_draw(2, 4, "材料到手！", PAL_UI_GOLD_LIGHT);
    text_printf(2, 6, PAL_UI_TEXT, "%s：%s", rg->name, rg->ingredient);

    text_draw(2, 8, "收集进度：", PAL_UI_TEXT);
    int got = 0;
    for (int i = 0; i < INGREDIENT_COUNT; i++)
        if (g_ingredients_done & (1 << i))
            got++;
    text_printf(2, 10, PAL_GREEN_SPARK, "%d / %d", got, INGREDIENT_COUNT);

    if (got == INGREDIENT_COUNT)
        text_draw(2, 11, "材料齐了！回王国烤蛋糕 (A)", PAL_YELLOW_MID);
    else
        text_draw(2, 11, "按 A 返回王国", PAL_WHITE_SPARK);
}

// ---------------------------------------------------------------------------
// Game over
// ---------------------------------------------------------------------------

static void game_over_screen(void)
{
    text_clear();
    text_panel(1, 3, 18, 7);
    text_draw(3, 4, "被围住了…", PAL_HUD_HEART);
    text_printf(2, 6, PAL_UI_TEXT, "坚持了 %d分%d秒",
                g_run_time / 3600, (g_run_time / 60) % 60);
    text_printf(2, 8, PAL_UI_TEXT_DIM, "%s的材料没有拿到",
                g_regions[g_region_id].ingredient);
    text_draw(2, 10, "已经拿到的材料不会丢失", PAL_GREEN_SPARK);
    text_draw(3, 11, "按 A 返回王国", PAL_WHITE_SPARK);
}

// ---------------------------------------------------------------------------
// Ending
// ---------------------------------------------------------------------------

static int g_ending_timer = 0;

static int g_lobby_state = 0; // 0 = menu, 1 = connecting, 2 = failed
static int g_lobby_cursor = 0; // 0 = host, 1 = client
static int g_lobby_timer = 0;


static void ending_screen(void)
{
    g_ending_timer++;

    // Like the title, the finale is its own screen-space composition, so the
    // camera is pinned and the gfx_* calls below are plain screen pixels.
    render_camera(0, 0);

    // Animate: the oven glows, then fireworks burst and drift down.
    for (int y = 0; y < AREA_H; y++)
    {
        uint8_t c = (y < 80) ? PAL_PURPLE_DARK : PAL_CASTLE_DARK;
        gfx_fill_rect(0, y, AREA_W, 1, c);
    }

    // Fireworks: expanding rings of sparkles on a fixed schedule.
    for (int f = 0; f < 5; f++)
    {
        int t = (g_ending_timer - f * 45) % 225;
        if (t < 0 || t > 70)
            continue;
        int bx = 40 + f * 44;
        int by = 40 + ((f * 29) % 50);
        int r = t;
        for (int a = 0; a < 360; a += 12)
        {
            int dx = (r * (a % 90 - 45)) / 45;
            int dy = (r * (((a + 45) % 90) - 45)) / 45;
            uint8_t c = (f % 3 == 0) ? PAL_PINK_LIGHT :
                        (f % 3 == 1) ? PAL_CYAN_LIGHT : PAL_YELLOW_LIGHT;
            if (r < 22)
                gfx_fill_rect(bx + dx, by + dy, 2, 2, c);
        }
    }

    // The finished cake, centre stage.
    int cx = AREA_W / 2;
    int by = 128;
    gfx_fill_rect(cx - 46, by, 92, 30, PAL_CHOCO_DARK);
    gfx_fill_rect(cx - 44, by + 2, 88, 26, PAL_CHOCO_MID);
    gfx_fill_rect(cx - 34, by - 24, 68, 26, PAL_SPONGE_DARK);
    gfx_fill_rect(cx - 32, by - 22, 64, 22, PAL_SPONGE_MID);
    gfx_fill_rect(cx - 22, by - 42, 44, 20, PAL_CREAM_DARK);
    gfx_fill_rect(cx - 20, by - 40, 40, 17, PAL_CREAM_MID);
    for (int i = 0; i < 9; i++)
        gfx_fill_rect(cx - 44 + i * 10, by - 3, 7, 7, PAL_CREAM_MID);
    gfx_fill_rect(cx - 5, by - 49, 10, 8, PAL_CHERRY_MID);
    gfx_fill_rect(cx - 3, by - 47, 5, 4, PAL_CHERRY_LIGHT);

    // Candles: five, one per ingredient, all lit.
    for (int i = 0; i < INGREDIENT_COUNT; i++)
    {
        int px = cx - 20 + i * 10;
        gfx_fill_rect(px, by - 62, 3, 14,
                      (i % 2) ? PAL_CYAN_MID : PAL_PINK_MID);
        int flick = ((g_ending_timer / 6 + i) % 3);
        gfx_fill_rect(px - 1, by - 66 - flick, 5, 5, PAL_EMBER_ORANGE);
        gfx_fill_rect(px, by - 68 - flick, 3, 3, PAL_EMBER_YELLOW);
    }

    // The two capybaras celebrating beside the cake.
    int hop = ((g_ending_timer / 12) % 2) ? -3 : 0;
    gfx_blit_8bpp(hero_m_tiles_bin, HERO_W, HERO_H, DIR_UP * HERO_FRAMES,
                  cx - 88, by + 2 + hop, false, true);
    gfx_blit_8bpp(hero_f_tiles_bin, HERO_W, HERO_H, DIR_UP * HERO_FRAMES,
                  cx + 56, by + 2 - hop, false, true);

    if (g_ending_timer > 120)
        audio_play_music(MUS_BIRTHDAY);

    text_clear();
    if (g_ending_timer > 90)
    {
        text_panel(1, 2, 18, 4);
        text_draw(3, 3, "生日快乐！", PAL_UI_GOLD_LIGHT);
        text_draw(2, 5, "蛋糕烤好啦，一起吃吧", PAL_UI_TEXT);
    }
    if (g_ending_timer > 260)
    {
        text_panel(1, 7, 18, 5);
        text_draw(2, 8, "谢谢你们陪我一起冒险", PAL_PINK_LIGHT);
        text_draw(2, 10, "愿每一天都像今天一样开心", PAL_CYAN_LIGHT);
        text_draw(3, 11, "按 A 继续", PAL_UI_TEXT_DIM);
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    render_init();
    text_init();
    audio_init();
    game_init();

    // The local player is capybara 0 (male); the partner is 1 (female).
    g_local_hero = 0;
    g_heroes[0].active = true;
    g_heroes[0].variant = 0;

    g_state = ST_TITLE;
    g_menu_cursor = 0;

    int fade = 0;   // start black, fade in

    while (1)
    {
        swiWaitForVBlank();
        input_poll();

        uint16_t down = input_down();

        switch (g_state)
        {
        case ST_TITLE:
            title_screen();
            if (fade < 16)
                fade++;
            if (down & KEY_UP)
            {
                g_menu_cursor = (g_menu_cursor + TITLE_COUNT - 1) % TITLE_COUNT;
                audio_play_sfx(SND_UI_MOVE);
            }
            if (down & KEY_DOWN)
            {
                g_menu_cursor = (g_menu_cursor + 1) % TITLE_COUNT;
                audio_play_sfx(SND_UI_MOVE);
            }
            if (down & KEY_A)
            {
                audio_play_sfx(SND_UI_CONFIRM);
                if (g_menu_cursor == 0)
                {
                    g_menu_cursor = 0;
                    g_state = ST_CHAR_SELECT;
                }
                else
                {
                    g_menu_cursor = 0;
                    g_state = ST_SETTINGS;
                }
            }
            if (down & KEY_START)
                g_state = ST_CREDITS;
            break;

        case ST_CHAR_SELECT:
            char_select_screen();
            if (down & (KEY_LEFT | KEY_RIGHT))
            {
                g_menu_cursor ^= 1;
                audio_play_sfx(SND_UI_MOVE);
            }
            if (down & KEY_B)
            {
                audio_play_sfx(SND_UI_CANCEL);
                g_menu_cursor = 0;
                g_state = ST_TITLE;
            }
            if (down & KEY_A)
            {
                audio_play_sfx(SND_UI_CONFIRM);
                // The pick decides which capybara this console drives; when a
                // partner joins they take the other one, so the pair is always
                // one of each no matter which side chose first.
                g_heroes[0].variant = (uint8_t)g_menu_cursor;
                g_menu_cursor = 0;
                g_state = ST_MODE_SELECT;
            }
            break;

        case ST_SETTINGS:
            settings_screen();
            if (down & KEY_UP)
            {
                g_menu_cursor = (g_menu_cursor + SETTINGS_COUNT - 1) %
                                SETTINGS_COUNT;
                audio_play_sfx(SND_UI_MOVE);
            }
            if (down & KEY_DOWN)
            {
                g_menu_cursor = (g_menu_cursor + 1) % SETTINGS_COUNT;
                audio_play_sfx(SND_UI_MOVE);
            }
            if (down & (KEY_LEFT | KEY_RIGHT))
            {
                int step = (down & KEY_RIGHT) ? 1 : -1;
                if (g_menu_cursor == 0)
                    audio_set_music_volume(audio_get_music_volume() + step);
                else if (g_menu_cursor == 1)
                    audio_set_sfx_volume(audio_get_sfx_volume() + step);
                audio_play_sfx(SND_UI_MOVE);
            }
            if (down & (KEY_B | KEY_A))
            {
                audio_play_sfx(SND_UI_CANCEL);
                g_menu_cursor = 0;
                g_state = ST_TITLE;
            }
            break;

        case ST_MODE_SELECT:
            mode_select_screen();
            if (down & KEY_UP)
            {
                g_menu_cursor = (g_menu_cursor + MODE_COUNT - 1) % MODE_COUNT;
                audio_play_sfx(SND_UI_MOVE);
            }
            if (down & KEY_DOWN)
            {
                g_menu_cursor = (g_menu_cursor + 1) % MODE_COUNT;
                audio_play_sfx(SND_UI_MOVE);
            }
            if (down & KEY_B)
            {
                audio_play_sfx(SND_UI_CANCEL);
                g_menu_cursor = 0;
                g_state = ST_CHAR_SELECT;
            }
            if (down & KEY_A)
            {
                audio_play_sfx(SND_UI_CONFIRM);
                if (g_menu_cursor == 0)
                {
                    g_multiplayer = false;
                    g_state = ST_HUB;
                    g_menu_cursor = 0;
                    game_start_hub();
                }
                else
                {
                    g_multiplayer = true;
                    g_is_host = (g_menu_cursor == 1);

                    // The partner always takes the capybara this console did
                    // not: a pair is one of each, so both ultimates are always
                    // in play and the combo is always available.
                    g_heroes[1].active = true;
                    g_heroes[1].variant = g_heroes[0].variant ^ 1;
                    g_heroes[1].remote = true;
                    hero_recompute_stats(1);
                    g_heroes[1].x = WORLD_W / 2 + 8;
                    g_heroes[1].y = WORLD_H / 2;
                    g_heroes[1].net_x = g_heroes[1].x;
                    g_heroes[1].net_y = g_heroes[1].y;

                    if (g_is_host ? net_start_host() : net_start_client())
                    {
                        g_state = ST_LOBBY;
                    }
                    else
                    {
                        // No link: fall back to solo play rather than
                        // stranding the player in a dead menu.
                        g_multiplayer = false;
                        g_heroes[1].active = false;
                        g_state = ST_HUB;
                        g_menu_cursor = 0;
                        game_start_hub();
                    }
                }
            }
            break;

        case ST_LOBBY:
            if (g_lobby_state == 0)
            {
                text_clear();
                text_panel(1, 2, 18, 9);
                text_draw(2, 3, "【信鸽联络站 · 无线联机】", PAL_CYAN_LIGHT);
                text_draw(2, 5, g_lobby_cursor == 0 ? "> 1. 创建队伍 (主机)" : "  1. 创建队伍 (主机)",
                          g_lobby_cursor == 0 ? PAL_WHITE_SPARK : PAL_UI_TEXT);
                text_draw(2, 7, g_lobby_cursor == 1 ? "> 2. 加入队伍 (客机)" : "  2. 加入队伍 (客机)",
                          g_lobby_cursor == 1 ? PAL_WHITE_SPARK : PAL_UI_TEXT);
                text_draw(2, 9, "按 A 确定  按 B 返回大厅", PAL_UI_TEXT_DIM);

                if (down & (KEY_UP | KEY_DOWN))
                {
                    g_lobby_cursor ^= 1;
                    audio_play_sfx(SND_UI_MOVE);
                }
                if (down & KEY_B)
                {
                    audio_play_sfx(SND_UI_CANCEL);
                    g_state = ST_HUB;
                    game_start_hub();
                }
                if (down & KEY_A)
                {
                    audio_play_sfx(SND_UI_CONFIRM);
                    g_is_host = (g_lobby_cursor == 0);
                    g_lobby_timer = 0;
                    bool ok = g_is_host ? net_start_host() : net_start_client();
                    if (ok)
                        g_lobby_state = 1;
                    else
                        g_lobby_state = 2;
                }
            }
            else if (g_lobby_state == 1)
            {
                text_clear();
                text_panel(1, 2, 18, 9);
                g_lobby_timer++;
                text_printf(2, 3, PAL_CYAN_LIGHT, "正在搜索联机… (%d秒)", g_lobby_timer / 60);
                text_draw(2, 5, g_is_host ? "主机: 正在广播房间等待加入"
                                          : "客机: 正在扫描寻找主机房间", PAL_UI_TEXT);
                text_draw(2, 7, "信道: CH 10  协议: CapyLink", PAL_UI_TEXT_DIM);
                text_draw(2, 9, "按 B 随时取消并返回大厅", PAL_YELLOW_LIGHT);

                net_update();
                if (net_ready())
                {
                    audio_play_sfx(SND_UI_CONFIRM);
                    g_multiplayer = true;
                    g_heroes[1].active = true;
                    g_heroes[1].variant = g_heroes[0].variant ^ 1;
                    g_heroes[1].remote = true;
                    hero_recompute_stats(1);
                    g_state = ST_HUB;
                    game_start_hub();
                }
                if (down & KEY_B)
                {
                    net_shutdown();
                    g_multiplayer = false;
                    audio_play_sfx(SND_UI_CANCEL);
                    g_state = ST_HUB;
                    game_start_hub();
                }
                if (g_lobby_timer > 900)
                {
                    g_lobby_state = 2;
                }
            }
            else
            {
                text_clear();
                text_panel(1, 2, 18, 9);
                text_draw(2, 3, "未检测到可用无线网络", PAL_HUD_HEART);
                text_draw(2, 5, "请确认无线开关已开启", PAL_UI_TEXT);
                text_draw(2, 7, "（若在模拟器请开启Wifi）", PAL_UI_TEXT_DIM);
                text_draw(2, 9, "按 B 键返回王国广场", PAL_WHITE_SPARK);

                if (down & (KEY_B | KEY_A))
                {
                    net_shutdown();
                    g_multiplayer = false;
                    audio_play_sfx(SND_UI_CANCEL);
                    g_state = ST_HUB;
                    game_start_hub();
                }
            }
            break;

        case ST_HUB:
            hub_update();
            net_update();

            // Check touch shortcuts along the bottom
            if (input_touch_pressed() && input_touch_y() >= 150)
            {
                int tx = input_touch_x();
                if (tx < 64)
                {
                    // Touch: 出征选关
                    audio_play_sfx(SND_UI_CONFIRM);
                    g_state = ST_LEVEL_INTRO;
                }
                else if (tx < 128)
                {
                    // Touch: 无线联机
                    audio_play_sfx(SND_UI_CONFIRM);
                    g_lobby_state = 0;
                    g_state = ST_LOBBY;
                }
                else if (tx < 192)
                {
                    // Touch: 被动神树
                    audio_play_sfx(SND_UI_CONFIRM);
                    g_state = ST_SKILL_TREE;
                }
                else
                {
                    // Touch: 烤箱进度
                    if (g_ingredients_done == ((1 << INGREDIENT_COUNT) - 1))
                    {
                        audio_play_sfx(SND_DOOR);
                        g_state = ST_ENDING;
                        g_ending_timer = 0;
                        fade = 0;
                        audio_play_music(MUS_BIRTHDAY);
                    }
                    else
                    {
                        audio_play_sfx(SND_UI_MOVE);
                    }
                }
            }

            // Keyboard/D-pad interactions
            if (down & KEY_X)
            {
                audio_play_sfx(SND_UI_CONFIRM);
                g_state = ST_SKILL_TREE;
            }

            if (down & KEY_A)
            {
                int near = hub_near_landmark();
                if (near == 1)
                {
                    // North Gate -> Expedition Select
                    audio_play_sfx(SND_UI_CONFIRM);
                    g_state = ST_LEVEL_INTRO;
                }
                else if (near == 2)
                {
                    // West Post -> Wireless Multiplayer
                    audio_play_sfx(SND_UI_CONFIRM);
                    g_lobby_state = 0;
                    g_state = ST_LOBBY;
                }
                else if (near == 3)
                {
                    // East Tree -> Passive Skill Tree
                    audio_play_sfx(SND_UI_CONFIRM);
                    g_state = ST_SKILL_TREE;
                }
                else if (near == 4)
                {
                    // Center Oven -> Bake cake if ready
                    if (g_ingredients_done == ((1 << INGREDIENT_COUNT) - 1))
                    {
                        audio_play_sfx(SND_DOOR);
                        g_state = ST_ENDING;
                        g_ending_timer = 0;
                        fade = 0;
                        audio_play_music(MUS_BIRTHDAY);
                    }
                    else
                    {
                        audio_play_sfx(SND_UI_MOVE);
                    }
                }
            }

            if ((down & KEY_Y) &&
                g_ingredients_done == ((1 << INGREDIENT_COUNT) - 1))
            {
                audio_play_sfx(SND_DOOR);
                g_state = ST_ENDING;
                g_ending_timer = 0;
                fade = 0;
                audio_play_music(MUS_BIRTHDAY);
            }
            break;

        case ST_SKILL_TREE:
            skill_tree_update();
            break;

        case ST_LEVEL_INTRO:
            level_intro_screen();
            net_update();
            if (down & KEY_UP)
            {
                g_menu_cursor = (g_menu_cursor + INGREDIENT_COUNT - 1) % INGREDIENT_COUNT;
                audio_play_sfx(SND_UI_MOVE);
            }
            if (down & KEY_DOWN)
            {
                g_menu_cursor = (g_menu_cursor + 1) % INGREDIENT_COUNT;
                audio_play_sfx(SND_UI_MOVE);
            }
            if (down & KEY_A)
            {
                audio_play_sfx(SND_DOOR);
                game_start_region(g_menu_cursor);
                g_state = ST_PLAY;
            }
            if (down & KEY_B)
            {
                g_state = ST_HUB;
                game_start_hub();
            }
            break;

        case ST_PLAY:
            game_update();
            net_update();
#ifdef AUTOPLAY
            // Debug: summon the region boss immediately, so the boss fight can
            // be verified without first farming enough ingredient drops.
            if (down & KEY_SELECT)
            {
                g_ingredient_have = g_regions[g_region_id].target;
                audio_play_sfx(SND_UI_CONFIRM);
            }
#endif
            break;

        case ST_CARD_PICK:
            // The world keeps drawing behind the picker so the level-up stays
            // visible, but the simulation is paused: this is the one moment the
            // player gets to stop moving and think.
            card_menu_draw();
            card_menu_input();
            break;

        case ST_LEVEL_CLEAR:
            level_clear_screen();
            net_update();
            if (down & KEY_A)
            {
                audio_play_sfx(SND_UI_CONFIRM);
                audio_play_music(MUS_FIELD);
                g_state = ST_HUB;
                game_start_hub();
            }
            break;

        case ST_GAME_OVER:
            game_over_screen();
            if (down & KEY_A)
            {
                audio_play_sfx(SND_UI_CONFIRM);
                audio_play_music(MUS_FIELD);
                g_state = ST_HUB;
                g_mid_boss_out = false;
                g_boss_out = false;
            }
            break;

        case ST_ENDING:
            if (fade < 16)
                fade++;
            ending_screen();
            if (g_ending_timer > 260 && (down & KEY_A))
                g_state = ST_CREDITS;
            break;

        case ST_CREDITS:
            text_clear();
            text_panel(1, 2, 18, 9);
            text_draw(3, 3, "水豚生存 · 生日快乐", PAL_UI_GOLD_LIGHT);
            text_draw(2, 5, "制作：BlocksDS 像素小分队", PAL_UI_TEXT);
            text_draw(2, 7, "献给一起冒险的你们", PAL_PINK_LIGHT);
            text_draw(2, 9, "生日快乐！", PAL_YELLOW_LIGHT);
            text_draw(2, 11, "按 B 返回标题", PAL_UI_TEXT_DIM);
            audio_play_music(MUS_BIRTHDAY);
            if (down & KEY_B)
            {
                g_state = ST_TITLE;
                g_menu_cursor = 0;
            }
            break;

        default:
            g_state = ST_TITLE;
            break;
        }

        set_brightness(fade);
        render_flush();
        text_flush();
    }

    return 0;
}
