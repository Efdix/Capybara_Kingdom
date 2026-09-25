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
#include "hub_layout.h"
#include "farm.h"
#include "worldmap_nodes.h"
#include "ui_draw.h"
#include "save.h"
#include "perf.h"

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

// Every panel, button, bar and cursor on every screen now comes from
// ui_draw.c, so this file only lays things out. The old hand-rolled nested
// rectangles that used to live here are gone: a "button" built out of two
// fills at each call site is why the interface read as text in a box.

static void draw_title(float t)
{
    // The scene itself is a generated image (tools/gen_ui.py), blitted in
    // screen space. Only the animated parts -- the celebrate-hop, the candle
    // flicker and the drifting sparkles -- are drawn here.
    render_ui_image(ui_title_bin);

    int hop = ((int)(t / 20) & 1) ? -2 : 0;
    gfx_blit_8bpp(hero_m_tiles_bin, HERO_W, HERO_H, DIR_DOWN * HERO_FRAMES,
                  96, 138 + hop, false, true);
    gfx_blit_8bpp(hero_f_tiles_bin, HERO_W, HERO_H, DIR_DOWN * HERO_FRAMES,
                  128, 138 - hop, false, true);

    // Candle flicker on the cake, sitting just above the flame in the art.
    if (((int)(t / 6) & 1))
        gfx_fill_rect(127, 62, 2, 2, PAL_WHITE_SPARK);

    // Sparkles drifting up out of the village, so the sky is never static.
    for (int i = 0; i < 14; i++)
    {
        int x = (i * 37 + 11) % AREA_W;
        int y = 54 - (int)(t * 0.6f + i * 9) % 54;
        uint8_t c = (i % 3) ? PAL_WHITE_SPARK : PAL_YELLOW_LIGHT;
        gfx_fill_rect(x, y, 1, 1, c);
    }
}

// The title's menu entries.
//
// The list is built from whether a save exists rather than being a fixed table,
// because "continue" and "start over" are only meaningful next to each other:
// shown alone, either one is a lie about what is on the cartridge. A player with
// a save gets three rows and an explicit choice; a player without one gets the
// two they always had.
#define TITLE_IDX_CONTINUE 0
#define TITLE_IDX_NEW      1

// Set the moment 重新开始 is highlighted and A pressed; a second A commits.
// It is also cleared on every other title action, so the question never
// outlives the screen that asked it.
static bool s_confirm_new = false;

static int title_item_count(void)
{
    return save_has_data() ? 3 : 2;
}

static const char *title_item_label(int i)
{
    if (save_has_data())
    {
        switch (i)
        {
        case TITLE_IDX_CONTINUE: return "继续冒险";
        case TITLE_IDX_NEW:      return "重新开始";
        default:                 return "设置";
        }
    }
    return (i == 0) ? "开始游戏" : "设置";
}

// Everything a fresh file starts over with. game_init() is what clears the meta
// layer, so it has to run again here -- it already ran at boot, before the save
// was loaded so the title screen could show the real cake progress, and by now
// its work has been overwritten by that load.
static void title_start_new_game(void)
{
    game_init();
    save_clear();
    g_local_hero = 0;
    g_heroes[0].active = true;
    g_heroes[0].variant = 0;
}

// Write the meta layer. Called at every point that changes it, which is a
// handful of frames a session -- an EEPROM page write is about a millisecond,
// so there is no reason to batch them behind an explicit "save" the player has
// to remember. The moments that matter are the ones where the player would be
// upset to lose the last few minutes: planting, sleeping, banked ingredients,
// a bought skill, a cleared region, a volume change.
static void save_meta(void)
{
    save_write();
}

// Tear the wireless session down completely: the link, the shared-session
// flags and the partner capybara. Every exit path from multiplayer goes
// through this, so no route can leave a ghost partner walking around or the
// radio stack open under the hub. Safe to call when not in a session.
static void leave_multiplayer(void)
{
    if (!g_multiplayer && !g_heroes[1].active)
        return;
    net_shutdown();
    g_multiplayer = false;
    g_is_host = false;
    g_heroes[1].active = false;
    g_heroes[1].remote = false;
    hero_recompute_stats(1);
}

// The link died mid-session (net.c watches both directions). Whatever the
// two players were doing, the honest outcome is the same: say so, fold the
// session, and let this console keep going alone. A run in progress returns
// to the kingdom rather than freezing in a battlefield whose other half has
// stopped simulating. (net.c calls this once per lost session.)
void multiplayer_link_lost(void)
{
    bool midrun = (g_state == ST_PLAY || g_state == ST_DUEL);
    leave_multiplayer();
    save_write();
    if (midrun)
    {
        game_start_hub();
        g_state = ST_HUB;
        hub_toast("与伙伴失去联系，回到王国继续", PAL_UI2_RED, 240);
    }
    else
    {
        hub_toast("无线连接已断开", PAL_UI2_RED, 240);
    }
    audio_play_sfx(SND_UI_CANCEL);
}

// The actions behind the mode-select confirmation, factored out so the
// prologue can interrupt and then replay them: the story page runs BEFORE any
// radio comes up, so a first-timer reading it can never stall a live link.
static int g_lobby_state;   // defined with the other lobby state, below
static void mode_enter(int mode)
{
    if (mode == 0)
    {
        g_multiplayer = false;
        g_state = ST_HUB;
        g_menu_cursor = 0;
        game_start_hub();
        return;
    }

    g_multiplayer = true;
    g_is_host = (mode == 1);

    // The partner always takes the capybara this console did not: a pair is
    // one of each, so both ultimates are always in play and the combo is
    // always available.
    g_heroes[1].active = true;
    g_heroes[1].variant = g_heroes[0].variant ^ 1;
    g_heroes[1].remote = true;
    hero_recompute_stats(1);
    // Placed in the kingdom's coordinates: this runs before game_start_hub(),
    // which binds the hub's (smaller) world.
    g_heroes[1].x = HUB_W / 2;
    g_heroes[1].y = HUB_H / 2;
    g_heroes[1].net_x = g_heroes[1].x;
    g_heroes[1].net_y = g_heroes[1].y;

    if (g_is_host ? net_start_host() : net_start_client())
    {
        g_state = ST_LOBBY;
    }
    else
    {
        // No link. Say so on the failure screen instead of quietly turning
        // the request into a solo game: a player who asked for multiplayer is
        // told their radio did not come up, and B from there folds the
        // half-built session properly.
        leave_multiplayer();
        g_lobby_state = 2;
        g_state = ST_LOBBY;
    }
}

// ---------------------------------------------------------------------------
// Prologue (序章)
//
// Three pages between "pick your capybara" and the kingdom, shown once per
// save file: the cold oven, the old baker's complaint, and the reason two
// capybaras are walking out the gate. It is the only modal thing the story
// ever does, it is skippable at every frame, and a file that has played at
// all never sees it again (see tutorial_boot / TUT_INTRO).
//
// The cake the whole game walks toward is not named here on purpose: the
// kingdom says "an important day is coming" and keeps the rest to itself
// until the oven finally says it out loud.
// ---------------------------------------------------------------------------

static int s_intro_page = 0;            // 0..2
static int8_t s_intro_next_mode = -1;   // mode-select entry to resume after

static const char *const intro_titles[3] = {
    "湿地的清晨",
    "老面包师的话",
    "出发",
};

static const char *const intro_pages[3][6] = {
    {
        "水豚王国有一座老烤箱。",
        "它烤过王国所有的节日，",
        "庆典的、丰收的、雨季结束的。",
        "可今年，烤箱冷了。",
        NULL,
        NULL,
    },
    {
        "「五位霸主把甜味都带走了。」",
        "树精抱走了果子，鼠王囤起面粉，",
        "黑牛占着牧场，鳄王守着甘蔗。",
        "火山里那位最过分：连『生日』",
        "这个词都不许人说。",
        NULL,
    },
    {
        "「重要的日子快到了。去把它们",
        "请回来吧——不用打倒它们，",
        "水豚最擅长的，是陪着谁，",
        "等它消气。」",
        NULL,
        NULL,
    },
};

// The top screen's scene, one sketch per page, all of it flat-colour rects
// and circles in screen space -- the same material the ending scene draws
// with. Each page answers the page before it: the cold oven, the complaint,
// the gate opening.
static void intro_scene(void)
{
    if (s_intro_page == 1)
    {
        // The bakery interior, dusk-warm. The oven is dark: that is the page.
        for (int y = 0; y < AREA_H; y++)
        {
            uint8_t c = (y < 60) ? PAL_CASTLE_DARK
                                 : (y < 150 ? PAL_CHOCO_DARK : PAL_SHADOW);
            gfx_fill_rect(0, y, AREA_W, 1, c);
        }
        // The oven: a dark arch with a cold mouth, one ember line dying.
        gfx_fill_rect(88, 66, 80, 84, PAL_CHOCO_MID);
        gfx_fill_rect(96, 74, 64, 68, PAL_CHOCO_DARK);
        gfx_fill_rect(104, 96, 48, 38, PAL_OUTLINE);
        gfx_fill_rect(104, 130, 48, 4, PAL_EMBER_ORANGE);
        // A shelf of empty plates overhead.
        gfx_fill_rect(30, 40, 196, 3, PAL_CHOCO_MID);
        for (int i = 0; i < 5; i++)
            gfx_fill_rect(40 + i * 40, 30, 22, 8, PAL_SPONGE_DARK);
        // The old baker, small against the dark, facing the cold mouth.
        int bob = ((g_frame / 24) & 1) ? -1 : 0;
        gfx_blit_8bpp(hero_m_tiles_bin, HERO_W, HERO_H,
                      DIR_DOWN * HERO_FRAMES, 46, 118 + bob, false, true);
    }
    else
    {
        // Morning over the wetland: pale sky, low sun, the lake band and the
        // boardwalk the whole kingdom hangs from. Page 2 stands at the gate.
        for (int y = 0; y < AREA_H; y++)
        {
            uint8_t c = (y < 70) ? PAL_CYAN_LIGHT
                                 : (y < 96 ? PAL_CYAN_MID
                                           : (y < 160 ? PAL_GRN_MID
                                                      : PAL_GRN_DARK));
            gfx_fill_rect(0, y, AREA_W, 1, c);
        }
        gfx_circle(196, 34, 14, PAL_YELLOW_LIGHT);
        gfx_circle(196, 34, 9, PAL_EMBER_YELLOW);
        gfx_fill_rect(0, 96, AREA_W, 8, PAL_UI2_BLUE);
        gfx_fill_rect(0, 104, AREA_W, 2, PAL_UI2_BLUE_HI);
        // Boardwalk planks running to the horizon.
        for (int i = 0; i < 9; i++)
            gfx_fill_rect(20 + i * 26, 132, 14, 3, PAL_SPONGE_MID);

        if (s_intro_page == 0)
        {
            // One capybara on the bank, looking at the cold kingdom.
            int bob = ((g_frame / 20) & 1) ? -1 : 0;
            gfx_blit_8bpp(hero_f_tiles_bin, HERO_W, HERO_H,
                          DIR_DOWN * HERO_FRAMES, 116, 108 + bob, false, true);
        }
        else
        {
            // The gate: two pillars and a lintel, and the pair already
            // halfway to it.
            gfx_fill_rect(52, 84, 16, 76, PAL_SPONGE_DARK);
            gfx_fill_rect(188, 84, 16, 76, PAL_SPONGE_DARK);
            gfx_fill_rect(44, 72, 168, 14, PAL_SPONGE_DARK);
            gfx_fill_rect(48, 75, 160, 3, PAL_SPONGE_MID);
            int bob = ((g_frame / 16) & 1) ? -1 : 0;
            gfx_blit_8bpp(hero_m_tiles_bin, HERO_W, HERO_H,
                          DIR_RIGHT * HERO_FRAMES, 108, 122 + bob,
                          false, true);
            gfx_blit_8bpp(hero_f_tiles_bin, HERO_W, HERO_H,
                          DIR_RIGHT * HERO_FRAMES, 132, 122 - bob,
                          false, true);
        }
    }
}

// Defined further down with the shared UI helpers; the prologue's hint row
// and the market screen both sit earlier in the file than the definition.
static void hint_strip(UiScreen scr, int y, const char *const *keys,
                       const char *const *labels, int n);

static void intro_screen(void)
{
    render_camera(0, 0);
    intro_scene();

    ui_field(UI_SUB);
    {
        char buf[40];
        snprintf(buf, sizeof(buf), "序章 · %s", intro_titles[s_intro_page]);
        ui_header_tab(UI_SUB, 12, 10, 232, buf);
    }

    ui_plate(UI_SUB, 12, 42, 232, 98, UI_R_PLATE);
    // One text row per line: at the two-cell pitch a five-line page would run
    // past the plate's bottom edge, and the pages are written to be read as
    // lines, not as airy paragraphs.
    int row = 4;
    for (int i = 0; i < 6 && intro_pages[s_intro_page][i]; i++)
    {
        text_draw_wrapped(2, row, 18, intro_pages[s_intro_page][i],
                          PAL_UI2_INK);
        row += 1;
    }

    // The one lesson the story sneaks in: on the last page, where "what do I
    // do" is about to be asked, the answer rides along with the plot. It sits
    // in the one text row between the plate and the hint strip.
    if (s_intro_page == 2)
        ui_text_mid(UI_SUB, 20, 142,
                    "方向键移动 · 水豚自己会战斗", PAL_UI2_INK_DIM);

    {
        static const char *const labels[] = { "继续", "跳过" };
        ui_plate(UI_SUB, 20, 158, 216, 26, UI_R_PLATE);
        hint_strip(UI_SUB, 163,
                   (const char *const[]){ "A", "B" }, labels, 2);
    }
}

// Leave the prologue the way it came: mark the file as having heard the
// story, then finish whatever the mode-select press started.
static void intro_finish(void)
{
    tutorial_mark(TUT_INTRO);
    audio_play_sfx(SND_UI_CONFIRM);
    mode_enter((int)s_intro_next_mode);
}

// ---------------------------------------------------------------------------
// The duel's controls card (决斗说明)
//
// The duel is a second control scheme dropped on the player at the loudest
// moment of the run: the arena scrolls sideways, A stops being a skill and
// becomes a jump, and the boss telegraphs everything. Once per save, the
// first duel opens with one static card that says so -- the one place this
// game allows a modal, because a line of toast cannot carry a whole scheme.
// Any key starts the fight; every duel after the first skips straight in.
// ---------------------------------------------------------------------------

static void duel_card_screen(void)
{
    // The top screen keeps whatever the leap-out beat left there: the boss
    // mid-lunge is exactly the poster this card should hang over.
    ui_field(UI_SUB);
    ui_header_tab(UI_SUB, 12, 10, 232, "决斗 · 最后只剩你们两个");

    ui_plate(UI_SUB, 12, 40, 232, 84, UI_R_PLATE);
    static const char *const lines[] = {
        "A 跳跃（长按跳得更高）",
        "B 翻滚：有无敌帧，空中还能用一次",
        "看前摇再动：红线＝冲锋，落点圈＝砸落",
        "爆圈＝别贴身；66%/33% 血它会变招加速",
        "技能格用触屏或 X / Y，R 放大招",
    };
    int row = 3;
    for (int i = 0; i < 5; i++)
    {
        text_draw_wrapped(2, row, 18, lines[i], PAL_UI2_INK);
        row += 1;
    }

    {
        static const char *const labels[] = { "开始" };
        ui_plate(UI_SUB, 20, 158, 216, 26, UI_R_PLATE);
        hint_strip(UI_SUB, 163,
                   (const char *const[]){ "A" }, labels, 1);
    }
}

// ---------------------------------------------------------------------------
// The market (集市): surplus ingredients buy boss tokens.
//
// Materials bank past the cake's quota used to evaporate and the token purse
// topped out five short of the full tree, so a finished player had exactly
// one post-game activity and it was re-fighting regions for nothing. The
// market turns the surplus into the currency the tree wants: any five
// ingredients trade for one token, taken from the biggest piles first.
// ---------------------------------------------------------------------------

#define MARKET_TRADE_N  5

// Defined further down with the other shared UI helpers; the market screen
// below is the first user in this file.
static void hint_strip(UiScreen scr, int y, const char *const *keys,
                       const char *const *labels, int n);

static int market_surplus(void)
{
    int total = 0;
    for (int i = 0; i < INGREDIENT_COUNT; i++)
        total += g_cake_have[i];
    return total;
}

static bool market_exchange(void)
{
    if (market_surplus() < MARKET_TRADE_N)
        return false;
    for (int k = 0; k < MARKET_TRADE_N; k++)
    {
        int best = 0;
        for (int i = 1; i < INGREDIENT_COUNT; i++)
            if (g_cake_have[i] > g_cake_have[best])
                best = i;
        g_cake_have[best]--;
    }
    g_boss_tokens++;
    return true;
}

static void market_screen(void)
{
    text_clear();
    ui_field(UI_MAIN);
    ui_field(UI_SUB);
    ui_header_tab(UI_SUB, 12, 10, 232, "集市 · 材料换印记");
    ui_plate(UI_SUB, 20, 40, 216, 60, UI_R_PLATE);
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "印记 %d", (int)g_boss_tokens);
        ui_text_mid(UI_SUB, 40, 50, buf, PAL_UI2_BLUE_HI);
        snprintf(buf, sizeof(buf), "可交换的材料：%d 份", market_surplus());
        ui_text_mid(UI_SUB, 40, 74, buf, PAL_UI2_INK);
    }
    ui_btn(UI_SUB, 20, 110, 216, 30, g_menu_cursor == 0 ? UI_SEL : UI_ON);
    ui_label_st(UI_SUB, 20, 110, 216, 30, "兑换 1 印记（5 份材料）",
                g_menu_cursor == 0 ? UI_SEL : UI_ON);
    if (g_menu_cursor == 0)
        ui_cursor(UI_SUB, 9, 125, PAL_UI2_INK);
    ui_btn(UI_SUB, 20, 148, 216, 30, g_menu_cursor == 1 ? UI_SEL : UI_ON);
    ui_label_st(UI_SUB, 20, 148, 216, 30, "离开",
                g_menu_cursor == 1 ? UI_SEL : UI_ON);
    if (g_menu_cursor == 1)
        ui_cursor(UI_SUB, 9, 163, PAL_UI2_INK);
    hint_strip(UI_SUB, 185,
               (const char *const[]){ "A", "B" },
               (const char *const[]){ "兑换", "离开" }, 2);
}

// One row of a menu: a plate carrying its label, with the cursor beside it when
// selected. Shared by the title, mode, settings and pause menus so that a menu
// entry looks the same wherever the player meets one.
//
// `hint` is the small right-aligned note on the row (a value, a state); pass
// NULL for none.
void menu_row(UiScreen scr, int y, int x, int w, int h, const char *label,
              const char *hint, bool sel)
{
    ui_btn(scr, x, y, w, h, sel ? UI_SEL : UI_ON);
    ui_text_mid(scr, x + 14, y + h / 2, label,
                sel ? PAL_WHITE : PAL_UI2_INK);
    if (hint)
        ui_text_right(scr, x + w - 14, y + h / 2, hint,
                      sel ? PAL_UI2_BLUE_GLOW : PAL_UI2_INK_DIM);
    if (sel)
        ui_cursor(scr, x - 11, y + h / 2, PAL_UI2_INK);
}

// The hint strip along the bottom of a menu screen: key caps and what they do.
//
// `keys` and `labels` are parallel arrays of `n` entries, where a key is one or
// two characters and a label is a short verb. Laying them out from a table
// rather than hand-placing each cap is what keeps the whole game's control
// hints on one baseline -- and it is the only reason adding a button later does
// not mean re-measuring four screens.
static void hint_strip(UiScreen scr, int y, const char *const *keys,
                       const char *const *labels, int n)
{
    int gap_key_label = 6;    // between a cap and its verb
    int gap_pair = 16;        // between two cap+verb pairs

    // Measure first so the row can be centred as a whole. Cap widths come from
    // ui_keycap_w, so a two-glyph key ("←→") gets a wider cap instead of
    // clipping.
    int total = 0;
    for (int i = 0; i < n; i++)
    {
        total += ui_keycap_w(keys[i]) + gap_key_label +
                 text_utf8_width(labels[i]) - 3;
        if (i + 1 < n)
            total += gap_pair;
    }

    int x = (AREA_W - total) / 2;
    for (int i = 0; i < n; i++)
    {
        int cap_w = ui_keycap_w(keys[i]);
        // The A button is the confirm key everywhere in this game, so it gets
        // the accent cap; every other key is neutral.
        bool primary = (keys[i][0] == 'A' && keys[i][1] == '\0');
        ui_keycap(scr, x, y, cap_w, 16, keys[i], primary);
        x += cap_w + gap_key_label;
        ui_text_mid(scr, x, y + 8, labels[i], PAL_UI2_INK);
        x += text_utf8_width(labels[i]) - 3 + gap_pair;
    }
}

// Where entry `i` of `n` sits. Three rows need a tighter pitch than two: the
// rows and the status panel below them are competing for the same 192 pixels,
// and at the two-row pitch the third row overlapped the panel.
static void title_row_rect(int i, int n, int *y, int *h)
{
    if (n >= 3) { *h = 30; *y = 26 + i * 32; }
    else        { *h = 38; *y = 40 + i * 46; }
}

static void title_screen(void)
{
    static float t = 0;
    t += 1.0f;

    // The title draws in SCREEN space: pinning the camera to the origin makes
    // the gfx_* world-to-screen translation an identity here.
    render_camera(0, 0);
    draw_title(t);

    // The name plate on the TOP screen, over the sky band the scene art leaves
    // clear. The dark well carries the name and the light plate under it the
    // subtitle, which is the same dark-readout-on-a-light-plate pairing the
    // in-play HUD uses.
    ui_plate(UI_MAIN, 28, 10, 200, 50, UI_R_PLATE);
    ui_well(UI_MAIN, 34, 15, 188, 22, UI_R_TAB, PAL_UI2_SCREEN);
    ui_label(UI_MAIN, 34, 15, 188, 22, "水豚生存", PAL_WHITE);
    ui_label(UI_MAIN, 28, 38, 200, 16, "生日蛋糕大冒险", PAL_UI2_INK);

    if (cake_ready())
        ui_text_lit(UI_MAIN, 74, 66, "★ 材料已集齐，可以烤蛋糕了",
                    PAL_YELLOW_LIGHT);

    ui_field(UI_SUB);

    // The menu: the last row is always 设置, so the count is all the handler
    // needs to know which index means what.
    int n_items = title_item_count();
    if (g_menu_cursor >= n_items)
        g_menu_cursor = n_items - 1;
    for (int i = 0; i < n_items; i++)
    {
        int y, h;
        title_row_rect(i, n_items, &y, &h);
        menu_row(UI_SUB, y, 20, 216, h, title_item_label(i), NULL,
                 i == g_menu_cursor);
    }

    // A two-line status panel under the menu: the run's own state, and whether
    // this cartridge will actually remember any of it.
    //
    // The save line is not decoration. The game's whole rhythm assumes yesterday
    // is still there -- crops need two watered nights, the spring advances the
    // calendar -- and until now it never said whether that promise was being
    // kept. Saying so plainly is worth one row of pixels.
    {
        ui_plate(UI_SUB, 20, 122, 216, 32, UI_R_PLATE);
        char buf[64];
        // The cake's own progress: harvested ingredients, not regions cleared.
        // The two used to be the same number; now a region can be cleared
        // without its crops being grown, and the title screen is about the cake.
        int got = cake_done_count();
        snprintf(buf, sizeof(buf), "蛋糕材料  %d / %d", got, INGREDIENT_COUNT);
        ui_text_mid(UI_SUB, 34, 131, buf, PAL_UI2_INK);
        for (int i = 0; i < INGREDIENT_COUNT; i++)
            ui_bullet(UI_SUB, 168 + i * 14, 127, cake_region_done(i));

        if (!save_available())
            ui_text_mid(UI_SUB, 34, 145, "存档：此卡带无存档芯片",
                        PAL_UI2_RED);
        else if (save_has_data())
            ui_text_mid(UI_SUB, 34, 145, "存档：已记录", PAL_UI2_GREEN);
        else
            ui_text_mid(UI_SUB, 34, 145, "存档：空", PAL_UI2_INK_DIM);
    }

    // The control hint along the bottom. Three entries, laid out by hint_strip
    // so this row and every other screen's sit on the same baseline. START is
    // spelled out: a bare "S" names a key this console does not have.
    {
        static const char *const labels[] = { "选择", "确定", "名单" };
        ui_plate(UI_SUB, 20, 158, 216, 26, UI_R_PLATE);
        hint_strip(UI_SUB, 163,
                   (const char *const[]){ "↑↓", "A", "ST" }, labels, 3);
    }

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

static void char_select_screen(float t)
{
    // Top screen: the two capybaras on show cards, so the fur, the belly and
    // the head accessory are legible enough to choose on. Drawn in screen space
    // with the camera pinned, like every other menu.
    render_camera(0, 0);

    // The field, so the top screen matches the bottom one instead of being a
    // different material. ui_field draws through the sub-screen primitives by
    // default, so this calls the top-screen equivalent directly.
    for (int y = 0; y < AREA_H; y++)
    {
        uint8_t c = (y < 56) ? PAL_UI2_BG_HI
                             : (y < 120 ? PAL_UI2_BG : PAL_UI2_PLATE_DEEP);
        gfx_fill_rect(0, y, AREA_W, 1, c);
    }

    for (int i = 0; i < 2; i++)
    {
        const uint8_t *sheet = (i == 0) ? hero_m_tiles_bin : hero_f_tiles_bin;
        bool sel = (i == g_menu_cursor);
        int x = 26 + i * 104;
        int lift = sel ? -2 : 0;

        // A show card: the same plate the menus use, with the character on a
        // soft pool of light. Selected cards rise and take the accent border,
        // so the choice reads without a separate cursor.
        gfx_fill_rect(x - 2, 40 + lift - 2, 92, 118, PAL_UI2_LINE);
        gfx_fill_rect(x, 40 + lift, 88, 114,
                      sel ? PAL_UI2_BLUE : PAL_UI2_PLATE_DEEP);
        gfx_fill_rect(x + 2, 42 + lift, 84, 110,
                      sel ? PAL_UI2_PLATE : PAL_UI2_PLATE_LO);
        gfx_fill_rect(x + 2, 42 + lift, 84, 1, PAL_UI2_PLATE_HI);

        // A pool of light under the character, so it stands on something.
        gfx_circle(x + 44, 132 + lift, 25,
                   sel ? PAL_UI2_BLUE_GLOW : PAL_UI2_PLATE_DEEP);
        gfx_blit_8bpp_scaled(sheet, HERO_W, HERO_H, DIR_DOWN * HERO_FRAMES,
                             x + 12, 64 + lift, false, false, 200);

        if (sel)
        {
            // A bobbing caret over the chosen one, so the selection is
            // unmistakable even in a still screenshot. The kit's caret, not a
            // hand-rolled triangle: it is the same "this one" mark every other
            // screen uses, which is what makes it read at a glance.
            int bob = ((int)t / 8) & 1;
            ui_caret_down(UI_MAIN, x + 44, 26 + bob, PAL_UI2_BLUE);
        }
    }

    // Bottom screen: the two names as tabs, then what each one's ultimate
    // actually does. Only the selected ultimate is described, so the panel
    // never has to fit both.
    ui_field(UI_SUB);
    ui_header_tab(UI_SUB, 12, 10, 232, "选择角色");

    for (int i = 0; i < 2; i++)
    {
        bool sel = (i == g_menu_cursor);
        int bx = 20 + i * 112;
        ui_btn(UI_SUB, bx, 40, 104, 30, sel ? UI_SEL : UI_ON);
        ui_label_st(UI_SUB, bx, 40, 104, 30, char_names[i],
                    sel ? UI_SEL : UI_ON);
        // No cursor here on purpose: the two tabs are side by side with an
        // 8px gap, and an 11px arrow between them would sit on the neighbour.
        // The blue fill is the selection marker.
    }

    ui_plate(UI_SUB, 20, 80, 216, 72, UI_R_PLATE);
    // The ultimate's name and its effect are one string in the source, split by
    // a newline; drawing the halves separately puts each on a chosen row rather
    // than on the 14px cell grid the text layer would otherwise impose.
    {
        const char *txt = char_ult[g_menu_cursor];
        const char *nl = strchr(txt, '\n');
        char head[64];
        size_t n = nl ? (size_t)(nl - txt) : strlen(txt);
        if (n >= sizeof(head))
            n = sizeof(head) - 1;
        memcpy(head, txt, n);
        head[n] = '\0';

        ui_text_mid(UI_SUB, 34, 92, head,
                    (g_menu_cursor == 0) ? PAL_UI2_AMBER : PAL_UI2_CYAN);
        if (nl)
            ui_text_mid(UI_SUB, 34, 112, nl + 1, PAL_UI2_INK);
        // Centred on the plate, not merely offset to the middle of the screen:
        // ui_text_mid's x is the LEFT edge, so the footnote needs measuring.
        ui_text_mid(UI_SUB, 20 + (216 - text_utf8_width("一个清场，一个续航")) / 2,
                    134, "一个清场，一个续航", PAL_UI2_INK_DIM);
    }

    {
        static const char *const labels[] = { "选择", "确定", "返回" };
        ui_plate(UI_SUB, 20, 158, 216, 26, UI_R_PLATE);
        hint_strip(UI_SUB, 163,
                   (const char *const[]){ "←→", "A", "B" }, labels, 3);
    }
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

// One settings row: a plate carrying the label, and the value drawn in a dark
// well on its right. The well is what makes the value look adjustable rather
// than merely displayed -- the same "value goes in here" cue the bars use.
static void settings_row(int y, const char *label, const char *value,
                         bool sel)
{
    int h = 30;
    ui_btn(UI_SUB, 20, y, 216, h, sel ? UI_SEL : UI_ON);
    ui_text_mid(UI_SUB, 34, y + h / 2, label,
                sel ? PAL_WHITE : PAL_UI2_INK);

    ui_well(UI_SUB, 132, y + 6, 74, h - 12, UI_R_TAB,
            sel ? PAL_UI2_SCREEN : PAL_UI2_PLATE_DEEP);
    ui_label(UI_SUB, 132, y + 6, 74, h - 12, value,
             sel ? PAL_UI2_BLUE_HI : PAL_UI2_INK);

    // Arrows on either side of the well, lit only on the selected row: they are
    // the "left/right does something here" hint, and lighting them on every row
    // would just add noise.
    uint8_t ac = sel ? PAL_WHITE : PAL_UI2_INK_DIM;
    ui_cursor_l(UI_SUB, 118, y + h / 2, ac);
    ui_cursor(UI_SUB, 218, y + h / 2, ac);

    if (sel)
        ui_cursor(UI_SUB, 9, y + h / 2, PAL_UI2_INK);
}

static void settings_screen(void)
{
    ui_field(UI_SUB);
    ui_header_tab(UI_SUB, 12, 10, 232, "设置");

    settings_row(44, settings_items[0],
                 vol_names[audio_get_music_volume()], g_menu_cursor == 0);
    settings_row(80, settings_items[1],
                 vol_names[audio_get_sfx_volume()], g_menu_cursor == 1);

    // The third row is the exit, so it is a plain button with no value well.
    menu_row(UI_SUB, 116, 20, 216, 30, settings_items[2], NULL,
             g_menu_cursor == 2);

    {
        static const char *const labels[] = { "选择", "调整", "返回" };
        ui_plate(UI_SUB, 20, 156, 216, 26, UI_R_PLATE);
        hint_strip(UI_SUB, 161,
                   (const char *const[]){ "↑↓", "←→", "B" }, labels, 3);
    }
}

// ---------------------------------------------------------------------------
// Pause (暂停)
//
// START in the field opens a small panel instead of leaving the run to guess
// work. In single player the world holds still behind it; over a link the
// field keeps moving under the panel -- the clock belongs to both players,
// so one console may not stop it -- and the row that walks home is dimmed,
// because leaving mid-link would strand the partner.
//
// 回王国 is the polite way out the game never had: leaving a run used to mean
// finding a crowd and dying in it, which taught players that the road home
// runs through a defeat screen. Walking out is brighter than that, and the
// seeds come home either way.
// ---------------------------------------------------------------------------

static bool s_paused = false;
static int  s_pause_cursor = 0;
static bool s_pause_help = false;

#define PAUSE_COUNT 5

static void pause_retreat(void)
{
    s_paused = false;
    // Same bookkeeping as coming home a winner: the trip out is a day's
    // work, the field turns over, and everything banked goes to the card.
    audio_play_music(MUS_FIELD);
    farm_advance_day();
    save_meta();
    game_start_hub();
    g_state = ST_HUB;
    hub_toast("先回王国了 · 作物夜里长大一阶", PAL_UI2_GREEN, 200);
}

static void pause_controls_screen(void)
{
    ui_field(UI_SUB);
    ui_header_tab(UI_SUB, 12, 10, 232, "操作说明");

    ui_plate(UI_SUB, 12, 40, 232, 84, UI_R_PLATE);
    static const char *const lines[] = {
        "野战：方向键走位，水豚自动战斗",
        "A/B/X/Y 技能格 · R 大招 · 触屏同效",
        "决斗：A 跳，B 翻滚（有无敌帧）",
        "主城：A 进地标 · 菜地里 B 浇整片",
        "START 暂停；升级时三选一",
    };
    int row = 3;
    for (int i = 0; i < 5; i++)
    {
        text_draw_wrapped(2, row, 18, lines[i], PAL_UI2_INK);
        row += 1;
    }

    {
        static const char *const labels[] = { "返回" };
        ui_plate(UI_SUB, 20, 158, 216, 26, UI_R_PLATE);
        hint_strip(UI_SUB, 163,
                   (const char *const[]){ "B" }, labels, 1);
    }
}

static void pause_draw(void)
{
    // No field(UISUB) clear here: over a link this panel floats on a live
    // HUD, so it draws opaque plates over what is already there.
    ui_plate(UI_SUB, 24, 34, 208, 124, UI_R_PLATE);
    for (int i = 0; i < PAUSE_COUNT; i++)
    {
        bool sel = (i == s_pause_cursor);
        bool dim = (i == 4 && g_multiplayer);
        int y = 44 + i * 22;
        const char *label;
        const char *hint = NULL;
        switch (i)
        {
        case 0: label = "继续"; break;
        case 1: label = "操作说明"; break;
        case 2: label = "音乐";
                hint = vol_names[audio_get_music_volume()]; break;
        case 3: label = "音效";
                hint = vol_names[audio_get_sfx_volume()]; break;
        default: label = g_multiplayer ? "回王国（联机中不可）" : "回王国"; break;
        }

        ui_btn(UI_SUB, 36, y, 184, 20, sel && !dim ? UI_SEL : UI_ON);
        if (dim)
            ui_round(UI_SUB, 37, y + 1, 182, 18, UI_R_BTN - 1,
                     PAL_UI2_PLATE_DEEP);
        ui_text_mid(UI_SUB, 48, y + 10, label,
                    dim ? PAL_UI2_INK_DIM : (sel ? PAL_WHITE : PAL_UI2_INK));
        if (hint)
            ui_text_right(UI_SUB, 208, y + 10, hint,
                          sel ? PAL_UI2_BLUE_GLOW : PAL_UI2_INK_DIM);
        if (sel && !dim)
            ui_cursor(UI_SUB, 26, y + 10, PAL_UI2_INK);
    }
}

static void pause_update(uint16_t down)
{
    if (s_pause_help)
    {
        pause_controls_screen();
        if (down & (KEY_A | KEY_B | KEY_START))
        {
            s_pause_help = false;
            audio_play_sfx(SND_UI_CANCEL);
        }
        return;
    }

    pause_draw();

    if (down & KEY_UP)
    {
        s_pause_cursor = (s_pause_cursor + PAUSE_COUNT - 1) % PAUSE_COUNT;
        audio_play_sfx(SND_UI_MOVE);
    }
    if (down & KEY_DOWN)
    {
        s_pause_cursor = (s_pause_cursor + 1) % PAUSE_COUNT;
        audio_play_sfx(SND_UI_MOVE);
    }
    if (down & (KEY_LEFT | KEY_RIGHT))
    {
        int step = (down & KEY_RIGHT) ? 1 : -1;
        if (s_pause_cursor == 2)
            audio_set_music_volume(audio_get_music_volume() + step);
        else if (s_pause_cursor == 3)
            audio_set_sfx_volume(audio_get_sfx_volume() + step);
        else
            return;
        audio_play_sfx(SND_UI_MOVE);
    }
    if (down & KEY_A)
    {
        switch (s_pause_cursor)
        {
        case 0:
            s_paused = false;
            audio_play_sfx(SND_UI_CANCEL);
            break;
        case 1:
            s_pause_help = true;
            audio_play_sfx(SND_UI_CONFIRM);
            break;
        case 4:
            if (!g_multiplayer)
            {
                audio_play_sfx(SND_UI_CONFIRM);
                pause_retreat();
            }
            else
                audio_play_sfx(SND_HURT);
            break;
        default:
            break;
        }
    }
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
    ui_field(UI_SUB);
    ui_header_tab(UI_SUB, 12, 10, 232, "选择模式");

    // Three rows, each tall enough for a full-width Chinese label. The mode
    // names are long ("创建派对（联机主机）"), so the label is centred in the
    // plate rather than left-aligned like the shorter menu rows elsewhere.
    for (int i = 0; i < MODE_COUNT; i++)
    {
        bool sel = (i == g_menu_cursor);
        int y = 44 + i * 40;
        ui_btn(UI_SUB, 20, y, 216, 32, sel ? UI_SEL : UI_ON);
        ui_label_st(UI_SUB, 20, y, 216, 32, mode_items[i],
                    sel ? UI_SEL : UI_ON);
        if (sel)
            ui_cursor(UI_SUB, 9, y + 16, PAL_UI2_INK);
    }

    {
        static const char *const labels[] = { "选择", "确定", "返回" };
        ui_plate(UI_SUB, 20, 166, 216, 26, UI_R_PLATE);
        hint_strip(UI_SUB, 171,
                   (const char *const[]){ "↑↓", "A", "B" }, labels, 3);
    }
}

// ---------------------------------------------------------------------------
// Hub (the kingdom)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Region intro / clear
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The world map (远征地图)
//
// This is the level select. The chart is a generated image; the C side adds
// the five node markers, the route highlight for the current selection and the
// cursor, then describes the highlighted region on the bottom screen.
//
// Navigation is not a cursor through a list: the nodes sit where their biomes
// are on the chart, and left/right walks the ring of five. That is why the
// generator emits the node positions -- a hardcoded table here would drift
// from the art the first time a biome moved.
// ---------------------------------------------------------------------------

// Order the nodes are visited in, which is also the order the regions are
// defined in (g_regions). Kept as data rather than as arithmetic on the node
// table so reordering the chart cannot silently reorder the regions.
static const uint8_t map_node_order[INGREDIENT_COUNT] = { 0, 1, 2, 3, 4 };

static bool region_cleared(int region_id)
{
    return (g_ingredients_done & (1 << region_id)) != 0;
}

// How dangerous a region is, as a tier from 0 upward. A region starts the
// difficulty clock at its own index (see game_start_region), so the numbers are
// the index -- but reading it through a function keeps the two from drifting if
// that ever changes.
static int region_threat(int region_id)
{
    return region_id;
}

// Which region the game would suggest next: the first one not yet cleared. The
// regions are laid out from gentlest to harshest, so "first uncleared" is also
// "the one the ramp is aiming at".
//
// It is a suggestion and nothing more. A hard gate here would break the promise
// the whole design rests on -- that a bad night never blocks the pair, that the
// field and the cake survive a wipe, and that they can go and farm seeds
// somewhere easier if they want to. Telling the player which door is heaviest
// respects them; locking four of the five doors does not.
static int region_recommended(void)
{
    for (int i = 0; i < INGREDIENT_COUNT; i++)
        if (!region_cleared(i))
            return i;
    return -1;
}

// The threat readout: filled pips, one per tier, in a colour that climbs from
// green to red. One glyph repeated, so it reads at a glance and needs nothing
// the font does not already have.
static void region_threat_label(int region_id, char *buf, size_t n)
{
    int t = region_threat(region_id);
    if (t < 0)
        t = 0;
    if (t > INGREDIENT_COUNT - 1)
        t = INGREDIENT_COUNT - 1;
    int k = 0;
    k += snprintf(buf + k, n - (size_t)k, "危险 ");
    for (int i = 0; i <= t && (size_t)k + 4 < n; i++)
        k += snprintf(buf + k, n - (size_t)k, "★");
}

static uint8_t region_threat_colour(int region_id)
{
    switch (region_threat(region_id))
    {
    case 0:  return PAL_GRN_LIGHT;
    case 1:  return PAL_YELLOW_LIGHT;
    case 2:  return PAL_UI2_AMBER;
    case 3:  return PAL_ORANGE_MID;
    default: return PAL_RED_LIGHT;
    }
}

static void world_map_touch_bar(void);

static void draw_world_map(float t)
{
    render_camera(0, 0);
    render_ui_image(ui_worldmap_bin);

    // A banner across the top, so the map is titled and the sea band the
    // generator left there is used rather than wasted. The banner is the same
    // tab treatment the menus use, so the chart reads as one of the game's
    // screens rather than as a picture the game happens to show.
    // The banner is taller than the one-line version it replaced: the threat
    // line needs a row of its own, and at the old height it hung off the
    // plate's bottom edge and came out clipped against the border.
    ui_plate(UI_MAIN, 28, 2, 200, 44, UI_R_PLATE);
    ui_well(UI_MAIN, 32, 5, 192, 20, UI_R_TAB, PAL_UI2_SCREEN);
    ui_text(UI_MAIN, 40, 6, "远征地图", PAL_WHITE);
    ui_text_right(UI_MAIN, 216, 15, "选一处地方", PAL_UI2_BLUE_HI);

    // The threat of the highlighted region, and whether it is the one the ramp
    // is pointing at. The five regions are not interchangeable and the chart
    // gives no clue which is which -- someone could walk into the volcano first
    // and conclude the game is unfair rather than that they skipped four doors.
    {
        char buf[64];
        region_threat_label(g_menu_cursor, buf, sizeof(buf));
        uint8_t col = region_threat_colour(g_menu_cursor);
        ui_text(UI_MAIN, 40, 28, buf, col);

        if (g_menu_cursor == region_recommended())
            ui_text_right(UI_MAIN, 216, 28, "推荐下一站", PAL_UI2_GREEN);
        else if (!region_cleared(g_menu_cursor) &&
                 region_recommended() >= 0 &&
                 region_threat(g_menu_cursor) > region_threat(region_recommended()))
            ui_text_right(UI_MAIN, 216, 28, "难度偏高", PAL_UI2_RED);
    }

    // The castle, marked as home so it is never mistaken for a destination.
    {
        int hx = WORLD_MAP_HOME_X, hy = WORLD_MAP_HOME_Y;
        gfx_circle_outline(hx, hy, 20, PAL_UI2_BLUE);
        gfx_circle_outline(hx, hy, 21, PAL_UI2_LINE);
    }

    // Route from home to every node: dim by default, lit for the selection.
    for (int i = 0; i < INGREDIENT_COUNT; i++)
    {
        int nx = g_world_map_node_xy[map_node_order[i]][0];
        int ny = g_world_map_node_xy[map_node_order[i]][1];
        bool sel = (map_node_order[i] == g_menu_cursor);
        uint8_t c = sel ? PAL_UI2_BLUE_HI : PAL_MAP_ROUTE;
        // A dotted trail, stepped along the same line the generator drew its
        // own trail on so the two lie on top of each other.
        int steps = 24;
        for (int k = 1; k < steps; k++)
        {
            int x = WORLD_MAP_HOME_X +
                    ((nx - WORLD_MAP_HOME_X) * k) / steps;
            int y = WORLD_MAP_HOME_Y +
                    ((ny - WORLD_MAP_HOME_Y) * k) / steps;
            if (k % 2)
                continue;
            gfx_fill_rect(x, y, 2, 2, c);
        }
    }

    // The nodes themselves.
    for (int i = 0; i < INGREDIENT_COUNT; i++)
    {
        int rid = map_node_order[i];
        int nx = g_world_map_node_xy[rid][0];
        int ny = g_world_map_node_xy[rid][1];
        bool sel = (rid == g_menu_cursor);
        bool done = region_cleared(rid);

        // A pin in the menu language: navy outline, white plate, and the
        // state carried by the fill -- the same blue as a selected button,
        // the same green as a cleared tab. The old concentric rings read as
        // a camera lens rather than as a place you can go.
        gfx_circle(nx, ny, 9, PAL_UI2_LINE);
        gfx_circle(nx, ny, 8, done ? PAL_GRN_DARK
                                   : (sel ? PAL_UI2_BLUE : PAL_WHITE));
        gfx_circle(nx, ny, 5, done ? PAL_LEAF_LIGHT
                                   : (sel ? PAL_UI2_BLUE_HI
                                          : PAL_UI2_PLATE_LO));
        if (!sel && !done)
            gfx_circle(nx, ny, 2, PAL_UI2_INK_DIM);

        if (sel)
        {
            // A rotating ring of sparks: the one moving thing on the screen,
            // so "this is what A will start" is never in question.
            int r = 13 + (int)(3.0f * ((t / 12) - (int)(t / 12)));
            for (int a = 0; a < 256; a += 16)
            {
                int dx, dy;
                gfx_dir_256((int)(a + t * 2), &dx, &dy);
                gfx_fill_rect(nx + (dx * r) / 127, ny + (dy * r) / 127,
                              2, 2, PAL_WHITE_SPARK);
            }
            gfx_circle_outline(nx, ny, 11, PAL_UI2_BLUE_GLOW);
        }

        if (done)
        {
            // A check, drawn as two strokes so it reads at 8 px.
            gfx_fill_rect(nx - 4, ny, 2, 2, PAL_WHITE_SPARK);
            gfx_fill_rect(nx - 2, ny + 2, 2, 2, PAL_WHITE_SPARK);
            gfx_fill_rect(nx - 2, ny, 2, 2, PAL_WHITE_SPARK);
            gfx_fill_rect(nx, ny - 2, 2, 2, PAL_WHITE_SPARK);
            gfx_fill_rect(nx, ny - 4, 2, 2, PAL_WHITE_SPARK);
        }

        // Threat pips under the pin: one per tier of the difficulty clock this
        // region starts at. All five are on one screen, so this is the one place
        // the chart can say "that one is the hard one" before it is chosen --
        // which is the whole reason the recommendation above exists.
        {
            int t0 = region_threat(rid);
            int w = (t0 + 1) * 4;
            int px = nx - w / 2;
            for (int k = 0; k <= t0; k++)
                gfx_fill_rect(px + k * 4, ny + 12, 3, 2,
                              region_threat_colour(rid));
        }
    }
}

static void world_map_screen(float t)
{
    draw_world_map(t);

    const RegionDef *rg = &g_regions[g_menu_cursor];
    bool done = region_cleared(g_menu_cursor);

    ui_field(UI_SUB);

    // The region's name, with its state as a chip on the right. Only these two
    // change as the cursor moves, so they are the loudest things on the screen.
    // A third state exists between unexplored and cleared: a duel lost here
    // left the boss summoned and waiting, and entering re-fights it directly.
    bool pending = (g_duel_pending == (int8_t)g_menu_cursor);
    ui_plate(UI_SUB, 6, 4, 244, 68, UI_R_PLATE);
    ui_well(UI_SUB, 10, 8, 236, 22, UI_R_TAB, PAL_UI2_SCREEN);
    ui_text(UI_SUB, 18, 9, rg->name, PAL_WHITE);
    ui_well(UI_SUB, 190, 8, 56, 22, UI_R_TAB,
            done ? PAL_GRN_DARK
                 : (pending ? PAL_UI2_AMBER : PAL_UI2_PLATE_DEEP));
    ui_label(UI_SUB, 190, 8, 56, 22,
             done ? "★已收集" : (pending ? "霸主候场" : "未探索"),
             done ? PAL_WHITE
                  : (pending ? PAL_UI2_SCREEN : PAL_UI2_INK_DIM));
    if (pending)
        ui_text_right(UI_SUB, 242, 34, "进入后直接再战", PAL_UI2_AMBER);

    // The flavour line, wrapped rather than trusted to fit: a clipped line
    // loses the words that describe the place. 19 columns stops the text at
    // the plate's inner margin; 20 ran the ink onto the plate's own outline.
    {
        char buf[160];
        size_t n = strlen(rg->intro);
        if (n >= sizeof(buf))
            n = sizeof(buf) - 1;
        memcpy(buf, rg->intro, n);
        buf[n] = '\0';
        text_draw_wrapped(1, 3, 19, buf, PAL_UI2_INK);
    }

    // The ingredient and the two bosses, as a labelled list.
    ui_plate(UI_SUB, 6, 76, 244, 62, UI_R_PLATE);

    struct { const char *label; const char *value; uint8_t col; } rows[3] = {
        { "采集", rg->ingredient,  PAL_UI2_CYAN  },
        { "先锋", rg->mid_boss_name, PAL_UI2_AMBER },
        { "霸主", rg->boss_name,   PAL_UI2_RED   },
    };
    for (int i = 0; i < 3; i++)
    {
        int ry = 82 + i * 19;
        ui_well(UI_SUB, 12, ry, 40, 16, UI_R_TAB, PAL_UI2_PLATE_DEEP);
        ui_label(UI_SUB, 12, ry, 40, 16, rows[i].label, PAL_UI2_INK);
        ui_text_mid(UI_SUB, 60, ry + 8, rows[i].value, rows[i].col);
    }
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "需 %d 个", rg->target);
        ui_text_right(UI_SUB, 242, 90, buf, PAL_UI2_INK_DIM);
    }

    // Touch: the five nodes as buttons along the bottom, plus the start button.
    // The chart on the top screen is the picture; this is the part a finger
    // can actually hit.
    world_map_touch_bar();
}

// Touch-only navigation for the map screen: five region tabs and a start
// button. Returns 1 if a region was started, 0 otherwise.
static int world_map_input(void)
{
    if (!input_touch_pressed())
        return 0;

    int tx = input_touch_x();
    int ty = input_touch_y();

    if (ty >= 146)
    {
        if (tx < 52)
            return 1;                       // 出发

        // Same rectangles world_map_touch_bar() paints.
        static const int tab_x[INGREDIENT_COUNT] = { 54, 92, 130, 168, 206 };
        static const int tab_w[INGREDIENT_COUNT] = { 34, 34, 34, 34, 48 };
        for (int i = 0; i < INGREDIENT_COUNT; i++)
        {
            if (tx >= tab_x[i] && tx < tab_x[i] + tab_w[i])
            {
                if (g_menu_cursor != i)
                {
                    g_menu_cursor = (uint8_t)i;
                    audio_play_sfx(SND_UI_MOVE);
                }
                break;
            }
        }
    }
    return 0;
}

static void world_map_touch_bar(void)
{
    // The five region tabs. A cleared one is green, the selected one is blue,
    // and everything else is a plain plate -- three states that have to be
    // distinguishable at a glance, which is why they differ in fill and not
    // only in outline.
    for (int i = 0; i < INGREDIENT_COUNT; i++)
    {
        int bx;
        bool sel = (g_menu_cursor == i);
        bool done = region_cleared(i);
        UiState st = sel ? UI_SEL : UI_ON;

        // Tab widths come from the names: four regions have two-character
        // names and one has four, so a fixed pitch would run the last label off
        // the screen. The same table drives the hit test below.
        static const int tab_w[INGREDIENT_COUNT] = { 34, 34, 34, 34, 48 };
        static const int tab_x[INGREDIENT_COUNT] = { 54, 92, 130, 168, 206 };
        int w = tab_w[i];
        bx = tab_x[i];

        ui_btn(UI_SUB, bx, 146, w, 40, st);
        if (!sel && done)
        {
            // Cleared: recolour the plate green rather than adding a badge.
            ui_round(UI_SUB, bx + 1, 147, w - 2, 38, UI_R_BTN - 1,
                     PAL_GRN_DARK);
        }
        ui_label(UI_SUB, bx, 147, w, 38, g_regions[i].name,
                 sel || done ? PAL_WHITE : PAL_UI2_INK);
    }

    // The start button: the only fully-blue control in the strip, so the thing
    // that launches a run is unmistakable.
    ui_btn(UI_SUB, 4, 146, 46, 40, UI_SEL);
    ui_label(UI_SUB, 4, 146, 46, 40, "出发", PAL_WHITE);
}

static void level_clear_screen(void)
{
    const RegionDef *rg = &g_regions[g_region_id];
    ui_field(UI_SUB);

    // The result plate, centred: a win is announced by the header tab and the
    // ingredient counter filling up, not by decoration.
    ui_header_tab(UI_SUB, 28, 26, 200, "霸主消气了！");

    ui_plate(UI_SUB, 28, 56, 200, 96, UI_R_PLATE);
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s · %s", rg->name, rg->ingredient);
        ui_text_mid(UI_SUB, 40, 70, buf, PAL_UI2_INK);
    }

    // The run's tally as five bullets, so "what is left" is countable at a
    // glance rather than read as a fraction. This is the CAKE's progress, not
    // the region's: an expedition hands over seeds, and it is the field back
    // home that turns them into the ingredients these bullets are counting.
    int got = cake_done_count();
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "击退 %d 只来犯者", (int)g_region_kills);
        ui_text_mid(UI_SUB, 40, 86, buf, PAL_UI2_INK_DIM);
    }
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "蛋糕进度  %d / %d", got, INGREDIENT_COUNT);
        ui_text_mid(UI_SUB, 40, 100, buf, PAL_UI2_INK);
    }
    for (int i = 0; i < INGREDIENT_COUNT; i++)
        ui_bullet(UI_SUB, 84 + i * 20, 112, cake_region_done(i));

    // The one honest line the boss says when the anger finally breaks. The
    // bosses are characters -- name cards, epithets, written patterns -- so
    // their last beat is a line, not just a loot burst. This is where the
    // prologue's promise ("you don't have to kill them") quietly pays off.
    ui_text_mid(UI_SUB, 40, 128, rg->appeased, PAL_UI_GOLD_LIGHT);

    ui_label(UI_SUB, 28, 158, 200, 20,
             cake_ready() ? "材料齐了！回王国烤蛋糕"
                          : "按 A 回王国 · 把种子种下去",
             PAL_UI2_BLUE_DARK);
}

// ---------------------------------------------------------------------------
// Game over
// ---------------------------------------------------------------------------

static void game_over_screen(void)
{
    const RegionDef *rg = &g_regions[g_region_id];
    ui_field(UI_SUB);

    // Same plate as the win screen, on purpose: a loss should look like the
    // same event with different news, not like a different game. A duel loss
    // says so by name -- the boss is still standing, and saying which one
    // turns "I died" into "the rematch is waiting".
    ui_header_tab(UI_SUB, 28, 26, 200,
                  (g_duel_pending == (int8_t)g_region_id) ? "决斗未成…"
                                                          : "被围住了…");

    ui_plate(UI_SUB, 28, 56, 200, 78, UI_R_PLATE);
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "坚持了 %d分%d秒",
                 g_run_time / 3600, (g_run_time / 60) % 60);
        ui_text_mid(UI_SUB, 40, 72, buf, PAL_UI2_INK);
    }
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "击败了 %d 只怪物", (int)g_region_kills);
        ui_text_mid(UI_SUB, 40, 90, buf, PAL_UI2_INK_DIM);
    }
    if (g_duel_pending == (int8_t)g_region_id)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s 还在等你们", rg->boss_name);
        ui_text_mid(UI_SUB, 40, 106, buf, PAL_UI_GOLD_LIGHT);
        ui_text_mid(UI_SUB, 40, 118, "再选这里，直接再战", PAL_UI2_GREEN);
    }
    else
    {
        ui_text_mid(UI_SUB, 40, 106, "这趟没能打完", PAL_UI2_INK_DIM);
        ui_text_mid(UI_SUB, 40, 116, "捡到的种子和收成都还在", PAL_UI2_GREEN);
    }

    ui_label(UI_SUB, 28, 142, 200, 20, "按 A 返回王国", PAL_UI2_BLUE_DARK);
}

// ---------------------------------------------------------------------------
// Ending
// ---------------------------------------------------------------------------

static int g_ending_timer = 0;

// The credits run in two beats: the roll, then one quiet page addressed to
// the person the cake was for. Reset wherever the credits are re-entered.
static int s_credits_phase = 0;

static int g_lobby_state = 0; // 0 = menu, 1 = connecting, 2 = failed
static int g_lobby_cursor = 0; // 0 = host, 1 = client
static int g_lobby_timer = 0;


// The finale's top-screen composition: night sky, fireworks, the cake on the
// lawn, the two capybaras. Split from ending_screen() so the credits can keep
// the scene alive behind them instead of freezing on the last ending frame.
static void ending_scene(void)
{
    // Night sky over a dark lawn. The old backdrop was a void: the cake
    // simply floated in flat purple, so it never read as standing anywhere.
    for (int y = 0; y < AREA_H; y++)
    {
        uint8_t c = (y < 80) ? PAL_PURPLE_DARK
                             : (y < 158 ? PAL_CASTLE_DARK : PAL_GRN_DARK);
        gfx_fill_rect(0, y, AREA_W, 1, c);
    }
    gfx_fill_rect(0, 158, AREA_W, 1, PAL_OUTLINE);

    // Fireworks: radial bursts on a fixed schedule. Each shell expands for 40
    // frames, then fades to its mid tone while its rays wink out one by one;
    // the angle offsets keep a burst ragged, like sparks rather than polygons.
    for (int f = 0; f < 5; f++)
    {
        int t = (g_ending_timer - f * 45) % 225;
        if (t < 0 || t > 70)
            continue;
        int bx = 40 + f * 44;
        int by = 40 + ((f * 29) % 50);
        int r = (t < 40) ? t : 40;
        uint8_t c;
        switch (f % 3)
        {
        case 0: c = (t < 40) ? PAL_PINK_LIGHT : PAL_PINK_MID; break;
        case 1: c = (t < 40) ? PAL_CYAN_LIGHT : PAL_CYAN_MID; break;
        default: c = (t < 40) ? PAL_YELLOW_LIGHT : PAL_YELLOW_MID; break;
        }
        for (int a = 0; a < 256; a += 7)
        {
            if (((a + f * 5) & 31) < t / 8)
                continue;   // rays go dark one by one as the shell fades
            int dx, dy;
            gfx_dir_256((a + f * 13) & 255, &dx, &dy);
            int rr = r + ((a * 11) & 7) - 3;
            if (rr < 2)
                continue;
            gfx_fill_rect(bx + dx * rr / 127, by + dy * rr / 127, 2, 2, c);
        }
    }

    // The finished cake, centre stage, with a shadow so it sits on the lawn.
    int cx = AREA_W / 2;
    int by = 128;
    gfx_fill_rect(cx - 44, by + 29, 88, 4, PAL_SHADOW);
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
    // A sixth candle, gold and unasked-for: the volcano boss's. It is the one
    // who had never been told what a birthday was, and its candle only
    // exists once that particular anger is appeased.
    if (g_ingredients_done & (1 << 4))
    {
        int px = cx + 30;
        gfx_fill_rect(px, by - 62, 3, 14, PAL_YELLOW_LIGHT);
        int flick = ((g_ending_timer / 6 + 5) % 3);
        gfx_fill_rect(px - 1, by - 66 - flick, 5, 5, PAL_EMBER_ORANGE);
        gfx_fill_rect(px, by - 68 - flick, 3, 3, PAL_EMBER_YELLOW);
    }

    // The appeased bosses, come down from their regions to the party they
    // were never invited to. Only the ones whose anger broke are here, small
    // and seated on the lawn in front of the capybaras -- close enough to the
    // cake, and to the candle that finally got lit for one of them.
    for (int i = 0; i < INGREDIENT_COUNT; i++)
    {
        if (!(g_ingredients_done & (1 << i)))
            continue;
        int bx = 17 + i * 49;
        int bob = ((g_ending_timer / 18 + i) & 1) ? -1 : 0;
        gfx_fill_rect(bx - 2, by + 56, 30, 3, PAL_SHADOW);
        gfx_blit_8bpp_scaled(boss_tiles_bin, 64, 64,
                             g_regions[i].boss_id * BOSS_FRAMES + BF_IDLE,
                             bx, by + 32 + bob, false, true, 40);
    }

    // The two capybaras celebrating in front of the cake, facing the player:
    // a birthday is for the people watching. Shadows keep them on the lawn.
    int hop = ((g_ending_timer / 12) % 2) ? -3 : 0;
    gfx_fill_rect(cx - 84, by + 30, 26, 4, PAL_SHADOW);
    gfx_fill_rect(cx + 58, by + 30, 26, 4, PAL_SHADOW);
    gfx_blit_8bpp(hero_m_tiles_bin, HERO_W, HERO_H, DIR_DOWN * HERO_FRAMES,
                  cx - 88, by + 4 + hop, false, true);
    gfx_blit_8bpp(hero_f_tiles_bin, HERO_W, HERO_H, DIR_DOWN * HERO_FRAMES,
                  cx + 56, by + 4 - hop, false, true);
}

static void ending_screen(void)
{
    g_ending_timer++;

    // Like the title, the finale is its own screen-space composition, so the
    // camera is pinned and the gfx_* calls below are plain screen pixels.
    render_camera(0, 0);
    ending_scene();

    if (g_ending_timer > 120)
        audio_play_music(MUS_BIRTHDAY);

    ui_field(UI_SUB);
    if (g_ending_timer > 90)
    {
        ui_plate(UI_SUB, 24, 22, 208, 44, UI_R_PLATE);
        ui_label(UI_SUB, 24, 30, 208, 22, "生日快乐！", PAL_UI2_BLUE_DARK);
        ui_label(UI_SUB, 24, 48, 208, 16, "蛋糕烤好啦，一起吃吧", PAL_UI2_INK);
    }
    if (g_ending_timer > 260)
    {
        // Rows sized to the plate: the closing line used to land its ink on
        // the plate's bottom seam, which read as a clipping bug.
        ui_plate(UI_SUB, 24, 78, 208, 62, UI_R_PLATE);
        ui_label(UI_SUB, 24, 84, 208, 18, "谢谢你们陪我一起冒险",
                 PAL_UI2_VIOLET);
        ui_label(UI_SUB, 24, 102, 208, 18, "愿每一天都像今天一样开心",
                 PAL_UI2_CYAN);
        ui_label(UI_SUB, 24, 120, 208, 16, "按 A 继续", PAL_UI2_INK_DIM);
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

    // Load the meta layer before the first frame, so the title screen can show
    // the cake progress that is actually on the cartridge and offer "continue"
    // only when there is something to continue. game_init() has already run and
    // its wipe is what a "new game" will fall back to.
    save_load();
    // With the save's progress known: a veteran's file silences the
    // first-run guidance chain before the first frame is drawn.
    tutorial_boot();

    // The local player is capybara 0 (male); the partner is 1 (female).
    g_local_hero = 0;
    g_heroes[0].active = true;
    g_heroes[0].variant = 0;

    g_state = ST_TITLE;
    g_menu_cursor = 0;

    int fade = 0;   // start black, fade in

    // Buttons pressed during a hitstop. A frozen frame runs no state handler,
    // so a press that begins and ends inside those few frames would be an edge
    // nobody ever saw; it is banked here and handed to the first frame that
    // actually runs. Without this, every freeze is a small chance to eat the
    // input the player pressed in reaction to the very hit that caused it.
    uint16_t latched_down = 0;

    while (1)
    {
        perf_wait_vblank();
        input_poll();
        perf_frame_begin();

        uint16_t down = input_down();

        // Hitstop. Holding the picture is all this needs to do: every screen
        // draws into the same two shadow buffers, and render_flush() copies
        // those buffers to VRAM whether or not anything redrew them.
        if (g_hitstop > 0)
        {
            g_hitstop--;
            latched_down |= down;
            set_brightness(fade);
            perf_draw_hud();
            render_flush();
            text_flush();
            // A frozen frame still pays for its flush, so it still counts.
            perf_frame_end();
            continue;
        }
        down |= latched_down;
        latched_down = 0;

        // A state change cancels the pause: dying while the panel was up (a
        // linked field keeps moving under it) must not bring the panel back
        // on top of the next screen.
        {
            static GameState s_prev_state = ST_BOOT;
            if (g_state != s_prev_state)
            {
                s_prev_state = g_state;
                s_paused = false;
                s_pause_help = false;
            }
        }

        switch (g_state)
        {
        case ST_TITLE:
            title_screen();
            // 重新开始 eats the whole cartridge's progress, so it asks once.
            // The question is an overlay on the menu itself: A once arms it,
            // A again commits, B walks it back.
            if (s_confirm_new)
            {
                ui_plate(UI_SUB, 28, 78, 200, 40, UI_R_PLATE);
                ui_text_mid(UI_SUB, 40, 88, "确定重新开始吗？", PAL_UI2_RED);
                ui_text_mid(UI_SUB, 40, 104, "存档会被清空 · A 确定 / B 取消",
                            PAL_UI2_INK);
            }
            if (fade < 16)
                fade++;
            if (down & KEY_UP)
            {
                int n = title_item_count();
                g_menu_cursor = (g_menu_cursor + n - 1) % n;
                audio_play_sfx(SND_UI_MOVE);
            }
            if (down & KEY_DOWN)
            {
                int n = title_item_count();
                g_menu_cursor = (g_menu_cursor + 1) % n;
                audio_play_sfx(SND_UI_MOVE);
            }
            if (down & KEY_B && s_confirm_new)
            {
                s_confirm_new = false;
                audio_play_sfx(SND_UI_CANCEL);
            }
            if (down & KEY_A)
            {
                int sel = g_menu_cursor;
                if (s_confirm_new)
                {
                    s_confirm_new = false;
                    audio_play_sfx(SND_UI_CONFIRM);
                    title_start_new_game();
                    g_state = ST_CHAR_SELECT;
                    break;
                }
                audio_play_sfx(SND_UI_CONFIRM);
                g_menu_cursor = 0;
                if (save_has_data())
                {
                    if (sel == TITLE_IDX_CONTINUE)
                        g_state = ST_CHAR_SELECT;
                    else if (sel == TITLE_IDX_NEW)
                        s_confirm_new = true;
                    else
                        g_state = ST_SETTINGS;
                }
                else
                {
                    g_state = (sel == 0) ? ST_CHAR_SELECT : ST_SETTINGS;
                }
            }
            if (down & KEY_START)
                g_state = ST_CREDITS;
            break;

        case ST_CHAR_SELECT:
        {
            static float t = 0;
            t += 1.0f;
            char_select_screen(t);
        }
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
                save_meta();
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
#ifndef AUTOPLAY
                // A file that has heard no story yet hears it once, before
                // the kingdom opens -- and before any radio comes up, so the
                // pages can never stall a live session. The guest (cursor 2)
                // skips it: the host's screen is already telling the tale,
                // and nobody needs it twice in one room.
                if (g_menu_cursor <= 1 && !tutorial_done(TUT_INTRO))
                {
                    s_intro_next_mode = (int8_t)g_menu_cursor;
                    s_intro_page = 0;
                    g_state = ST_INTRO;
                    break;
                }
#endif
                mode_enter((int)g_menu_cursor);
            }
            break;

        case ST_INTRO:
            intro_screen();
            if (down & KEY_A)
            {
                // Page by page; the last page's A is the walk to the kingdom.
                if (s_intro_page < 2)
                {
                    s_intro_page++;
                    audio_play_sfx(SND_UI_MOVE);
                }
                else
                    intro_finish();
            }
            if (down & KEY_B)
                intro_finish();   // B skips what remains; B on page 1 ends it
            break;

        case ST_LOBBY:
            if (g_lobby_state == 0)
            {
                text_clear();
                ui_field(UI_MAIN);   // the top screen otherwise keeps whatever
                ui_field(UI_SUB);    // the boot splash left there -- a dead black
                ui_header_tab(UI_SUB, 12, 10, 232, "信鸽联络站 · 无线联机");
                ui_btn(UI_SUB, 20, 46, 216, 30,
                       g_lobby_cursor == 0 ? UI_SEL : UI_ON);
                ui_label_st(UI_SUB, 20, 46, 216, 30, "创建队伍（主机）",
                            g_lobby_cursor == 0 ? UI_SEL : UI_ON);
                if (g_lobby_cursor == 0)
                    ui_cursor(UI_SUB, 9, 61, PAL_UI2_INK);
                ui_btn(UI_SUB, 20, 84, 216, 30,
                       g_lobby_cursor == 1 ? UI_SEL : UI_ON);
                ui_label_st(UI_SUB, 20, 84, 216, 30, "加入队伍（客机）",
                            g_lobby_cursor == 1 ? UI_SEL : UI_ON);
                if (g_lobby_cursor == 1)
                    ui_cursor(UI_SUB, 9, 99, PAL_UI2_INK);
                ui_plate(UI_SUB, 20, 152, 216, 26, UI_R_PLATE);
                hint_strip(UI_SUB, 157,
                           (const char *const[]){ "↑↓", "A", "B" },
                           (const char *const[]){ "选择", "确定", "返回" }, 3);

                if (down & (KEY_UP | KEY_DOWN))
                {
                    g_lobby_cursor ^= 1;
                    audio_play_sfx(SND_UI_MOVE);
                }
                if (down & KEY_B)
                {
                    audio_play_sfx(SND_UI_CANCEL);
                    // Leaving the lobby folds any session this console is
                    // still carrying: without this, a player who connected
                    // once and came back to the pigeon tower left a ghost
                    // partner wandering the kingdom under an open radio.
                    leave_multiplayer();
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
                ui_field(UI_MAIN);
                ui_field(UI_SUB);
                ui_header_tab(UI_SUB, 12, 10, 232, "正在搜索联机…");
                g_lobby_timer++;
                ui_plate(UI_SUB, 20, 48, 216, 66, UI_R_PLATE);
                {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%d 秒", g_lobby_timer / 60);
                    ui_text_mid(UI_SUB, 40, 66, buf, PAL_UI2_BLUE_DARK);
                }
                ui_text_mid(UI_SUB, 40, 90,
                            g_is_host ? "主机：正在广播房间等待加入"
                                      : "客机：正在扫描寻找主机房间",
                            PAL_UI2_INK);
                ui_text_mid(UI_SUB, 40, 108, "信道 CH 10 · 协议 CapyLink",
                            PAL_UI2_INK_DIM);
                // A pulse on the tab, so "still working" is visible without
                // reading the counter.
                if ((g_lobby_timer >> 4) & 1)
                    ui_round(UI_SUB, 118, 122, 20, 4, 2, PAL_UI2_BLUE);

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
                    leave_multiplayer();
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
                ui_field(UI_MAIN);
                ui_field(UI_SUB);
                ui_header_tab(UI_SUB, 12, 10, 232, "联机失败");
                ui_plate(UI_SUB, 20, 48, 216, 66, UI_R_PLATE);
                ui_text_mid(UI_SUB, 40, 66, "未检测到可用无线网络",
                            PAL_UI2_RED);
                ui_text_mid(UI_SUB, 40, 88, "请确认无线开关已开启",
                            PAL_UI2_INK);
                ui_text_mid(UI_SUB, 40, 106, "（模拟器请开启 Wifi）",
                            PAL_UI2_INK_DIM);
                ui_label(UI_SUB, 20, 152, 216, 24, "按 B 返回王国广场",
                         PAL_UI2_BLUE_DARK);

                if (down & (KEY_B | KEY_A))
                {
                    leave_multiplayer();
                    audio_play_sfx(SND_UI_CANCEL);
                    g_state = ST_HUB;
                    game_start_hub();
                }
            }
            break;

        case ST_HUB:
            hub_update();
            net_update();

            // Check touch shortcuts along the bottom. The strip is four
            // buttons of 59px at a 63px pitch starting at x = 4 -- the same
            // rectangles hub_draw() paints, so a tap can never land between
            // what is drawn and what responds.
            if (input_touch_pressed() && input_touch_y() >= 150)
            {
                int tx = input_touch_x();
                int slot = (tx < 4) ? -1 : (tx - 4) / 63;
                if (slot >= 0 && slot < 4 && tx < 4 + slot * 63 + 59)
                {
                    if (slot == 0)
                    {
                        // Touch: 出征选关
                        audio_play_sfx(SND_UI_CONFIRM);
                        tutorial_notify(TUT_EV_REGION_ENTER);
                        g_state = ST_WORLD_MAP;
                    }
                    else if (slot == 1)
                    {
                        // Touch: 无线联机
                        audio_play_sfx(SND_UI_CONFIRM);
                        g_lobby_state = 0;
                        g_state = ST_LOBBY;
                    }
                    else if (slot == 2)
                    {
                        // Touch: 被动神树
                        audio_play_sfx(SND_UI_CONFIRM);
                        g_state = ST_SKILL_TREE;
                    }
                    else
                    {
                        // Touch: 烤箱进度
                        if (cake_ready())
                        {
                            audio_play_sfx(SND_DOOR);
                            tutorial_notify(TUT_EV_OVEN);
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
            }

            // Keyboard/D-pad interactions
#ifdef AUTOPLAY_FARM
            // Debug: hand over a satchel of seeds so the farm route can plant
            // something. Without it the route would only ever exercise the
            // "no seed" refusal, which is the one path that matters least.
            if (down & KEY_SELECT)
            {
                for (int i = 0; i < FARM_CROP_COUNT; i++)
                    g_seeds[i] = 8;
                hub_toast("调试：种子袋已装满", PAL_UI2_GREEN, 120);
                audio_play_sfx(SND_UI_CONFIRM);
            }
#endif
#ifdef AUTOPLAY
            // Debug: drop straight into the first region. The hub's walking
            // route depends on the map layout, which moves whenever the kingdom
            // is reworked, so the scripted route cannot be relied on to reach a
            // fight. L is used rather than SELECT because SELECT is already the
            // "secure every ingredient" shortcut the ending route depends on,
            // and one key cannot mean two things in the same state.
            if (down & KEY_L)
            {
                audio_play_sfx(SND_UI_CONFIRM);
                game_start_region(0);
                g_state = ST_PLAY;
            }
            // Debug: secure every ingredient, so the --ending route can bake
            // without playing five regions first. The route still presses
            // SELECT in the hub; when the kingdom was reworked this handler
            // was the part that went missing, and the finale check stalled.
            if (down & KEY_SELECT)
            {
                static const uint8_t need[INGREDIENT_COUNT] = {
                    CAKE_NEED_0, CAKE_NEED_1, CAKE_NEED_2,
                    CAKE_NEED_3, CAKE_NEED_4,
                };
                for (int i = 0; i < INGREDIENT_COUNT; i++)
                {
                    g_cake_have[i] = need[i];
                    g_ingredients_done |= (uint8_t)(1 << i);
                }
                audio_play_sfx(SND_UI_CONFIRM);
            }
#endif
#ifdef AUTOPLAY_UI
            // Debug: the hub is where the UI route parks, so the presentation
            // screens are one key away from here. START -> level clear,
            // L -> game over, R -> ending (the credits follow it).
            if (down & KEY_START)
            {
                g_state = ST_LEVEL_CLEAR;
                audio_play_sfx(SND_UI_CONFIRM);
            }
            if (down & KEY_L)
            {
                g_state = ST_GAME_OVER;
                audio_play_sfx(SND_UI_CONFIRM);
            }
            if (down & KEY_R)
            {
                g_state = ST_ENDING;
                g_ending_timer = 0;
                audio_play_sfx(SND_UI_CONFIRM);
            }
#endif
            if (down & KEY_X)
            {
                audio_play_sfx(SND_UI_CONFIRM);
                g_state = ST_SKILL_TREE;
            }

            // --- the field --------------------------------------------------
            //
            // The plots are worked in the world, not in a menu: walk up to the
            // bed and press A. That keeps farming on the same input as every
            // other hub interaction, so there is nothing new to learn -- and it
            // means the field can be tended while walking past it on the way to
            // somewhere else, which is what makes a daily routine pleasant
            // rather than a chore.
            //
            // B waters the whole bed at once. Twenty-four separate presses
            // would be the single most tedious thing in the game, and there is
            // no decision in it -- a crop either needs water or it does not.
            bool farm_here = false;
            if (g_heroes[g_local_hero].active)
            {
                int32_t hx, hy;
                hero_center(&g_heroes[g_local_hero], &hx, &hy);
                farm_here = farm_in_reach(hx, hy);
            }

            if (farm_here && (down & KEY_A))
            {
                int32_t hx, hy;
                hero_center(&g_heroes[g_local_hero], &hx, &hy);
                int plot = farm_plot_at(hx, hy);
                // Standing between plots (or on the fence line) is normal, so
                // fall back to the nearest one rather than doing nothing.
                if (plot < 0)
                {
                    int best = -1, best_d = 0x7FFFFFFF;
                    for (int i = 0; i < FARM_PLOTS; i++)
                    {
                        int px = FARM_ORIGIN_X + (i % FARM_COLS) * 16 + 8;
                        int py = FARM_ORIGIN_Y + (i / FARM_COLS) * 16 + 8;
                        int dx = px - (int)hx, dy = py - (int)hy;
                        int d = dx * dx + dy * dy;
                        if (d < best_d) { best_d = d; best = i; }
                    }
                    plot = best;
                }
                // Crop index 0 is a real answer, so "nothing happened" is -1.
                if (farm_plot_action(plot, true) >= 0)
                    save_meta();
            }
            if (farm_here && (down & KEY_B))
            {
                if (farm_water_all(true) > 0)
                    save_meta();
            }

            if (down & KEY_A)
            {
                int near = farm_here ? -1 : hub_near_landmark();
                if (near == HUB_LM_GATE_ID)
                {
                    // 出征之门 -> the world map
                    audio_play_sfx(SND_UI_CONFIRM);
                    tutorial_notify(TUT_EV_REGION_ENTER);
                    save_meta();
                    g_state = ST_WORLD_MAP;
                }
                else if (near == HUB_LM_POST_ID)
                {
                    // 信鸽塔 -> wireless
                    audio_play_sfx(SND_UI_CONFIRM);
                    g_lobby_state = 0;
                    g_state = ST_LOBBY;
                }
                else if (near == HUB_LM_TREE_ID)
                {
                    // 神树 -> passive skills
                    audio_play_sfx(SND_UI_CONFIRM);
                    g_state = ST_SKILL_TREE;
                }
                else if (near == HUB_LM_OVEN_ID)
                {
                    // 烤箱 -> bake, but only once there is something to bake.
                    if (cake_ready())
                    {
                        audio_play_sfx(SND_DOOR);
                        tutorial_notify(TUT_EV_OVEN);
                        save_meta();
                        g_state = ST_ENDING;
                        g_ending_timer = 0;
                        fade = 0;
                        audio_play_music(MUS_BIRTHDAY);
                    }
                    else
                    {
                        audio_play_sfx(SND_UI_MOVE);
                        tutorial_notify(TUT_EV_OVEN);
                        hub_toast("材料还没凑齐，回去种地吧", PAL_UI2_RED, 150);
                    }
                }
                else if (near == HUB_LM_SPRING_ID)
                {
                    // 温泉 -> sleep. The spring is where the day turns over:
                    // it is the one landmark that was pure flavour before, and
                    // "rest here and let the crops grow" is the most natural
                    // thing a hot spring can do.
                    int grew = farm_advance_day();
                    audio_play_sfx(SND_DOOR);
                    char buf[64];
                    if (grew > 0)
                        snprintf(buf, sizeof(buf), "睡了一觉 · %d 块地长大了", grew);
                    else
                        snprintf(buf, sizeof(buf), "睡了一觉 · 第 %d 天", g_day);
                    hub_toast(buf, PAL_UI2_GREEN, 180);
                    save_meta();
                }
                else if (near == HUB_LM_MARKET_ID)
                {
                    // 集市 -> the exchange: surplus ingredients buy the
                    // tokens the skill tree wants. A landmark used to
                    // acknowledge the press and do nothing.
                    audio_play_sfx(SND_UI_CONFIRM);
                    g_menu_cursor = 0;
                    g_state = ST_MARKET;
                }
                else if (near)
                {
                    audio_play_sfx(SND_UI_MOVE);
                }
            }

            if ((down & KEY_Y) && cake_ready())
            {
                audio_play_sfx(SND_DOOR);
                tutorial_notify(TUT_EV_OVEN);
                save_meta();
                g_state = ST_ENDING;
                g_ending_timer = 0;
                fade = 0;
                audio_play_music(MUS_BIRTHDAY);
            }
            break;

        case ST_MARKET:
            market_screen();
            if (down & KEY_UP)
            {
                g_menu_cursor = 0;
                audio_play_sfx(SND_UI_MOVE);
            }
            if (down & KEY_DOWN)
            {
                g_menu_cursor = 1;
                audio_play_sfx(SND_UI_MOVE);
            }
            if (down & KEY_A)
            {
                if (g_menu_cursor == 0)
                {
                    if (market_exchange())
                    {
                        audio_play_sfx(SND_LEVELUP);
                        save_meta();
                    }
                    else
                        audio_play_sfx(SND_HURT);
                }
                else
                {
                    audio_play_sfx(SND_UI_CANCEL);
                    g_state = ST_HUB;
                }
            }
            if (down & KEY_B)
            {
                audio_play_sfx(SND_UI_CANCEL);
                g_state = ST_HUB;
            }
            if (input_touch_pressed() && g_menu_cursor == 0 &&
                input_touch_in(20, 110, 216, 30))
            {
                if (market_exchange())
                {
                    audio_play_sfx(SND_LEVELUP);
                    save_meta();
                }
                else
                    audio_play_sfx(SND_HURT);
            }
            else if (input_touch_pressed() &&
                     input_touch_in(20, 148, 216, 30))
            {
                audio_play_sfx(SND_UI_CANCEL);
                g_state = ST_HUB;
            }
            break;

        case ST_SKILL_TREE:
            skill_tree_update();
            break;

        case ST_LEVEL_INTRO:
        case ST_WORLD_MAP:
        {
            // Both states draw the same screen: ST_LEVEL_INTRO is the name the
            // older save/link code knows, ST_WORLD_MAP is the chart. Keeping
            // them on one code path is what stops the two from diverging.
            static float map_t = 0;
            map_t += 1.0f;
            world_map_screen(map_t);
            net_update();

            if (down & (KEY_LEFT | KEY_UP))
            {
                g_menu_cursor = (g_menu_cursor + INGREDIENT_COUNT - 1) %
                                INGREDIENT_COUNT;
                audio_play_sfx(SND_UI_MOVE);
            }
            if (down & (KEY_RIGHT | KEY_DOWN))
            {
                g_menu_cursor = (g_menu_cursor + 1) % INGREDIENT_COUNT;
                audio_play_sfx(SND_UI_MOVE);
            }
            if ((down & KEY_A) || world_map_input())
            {
                audio_play_sfx(SND_DOOR);
                map_t = 0;
                game_start_region(g_menu_cursor);
                g_state = ST_PLAY;
            }
            if (down & KEY_B)
            {
                audio_play_sfx(SND_UI_CANCEL);
                map_t = 0;
                g_state = ST_HUB;
                game_start_hub();
            }
            break;
        }

        case ST_PLAY:
#ifndef AUTOPLAY
            // START is the pause. In single player the world holds still; in
            // a linked session the panel floats over a field that keeps
            // moving, because one console may not stop the shared clock.
            if (down & KEY_START)
            {
                s_paused = !s_paused;
                s_pause_cursor = 0;
                s_pause_help = false;
                audio_play_sfx(s_paused ? SND_UI_MOVE : SND_UI_CANCEL);
            }
            if (s_paused && !g_multiplayer)
            {
                pause_update(down);
                break;
            }
#endif
            game_update();
            net_update();
#ifndef AUTOPLAY
            if (s_paused)
                pause_draw();
#endif
#ifdef AUTOPLAY
            // Debug: summon the region boss immediately, so the boss fight can
            // be verified without first farming enough ingredient drops.
            if (down & KEY_SELECT)
            {
                g_seed_have = g_regions[g_region_id].target;
                audio_play_sfx(SND_UI_CONFIRM);
            }
#ifdef AUTOPLAY_UI
            // Debug: jump straight to the presentation screens. A UI review
            // needs to see them without first surviving a region (or, for the
            // game-over plate, without deliberately dying), and the scripted
            // routes cannot reach them any other way.
            if (down & KEY_START)
            {
                g_state = ST_LEVEL_CLEAR;
                audio_play_sfx(SND_UI_CONFIRM);
            }
            if (down & KEY_L)
            {
                g_state = ST_GAME_OVER;
                audio_play_sfx(SND_UI_CONFIRM);
            }
            if (down & KEY_R)
            {
                g_state = ST_ENDING;
                g_ending_timer = 0;
                audio_play_sfx(SND_UI_CONFIRM);
            }
#endif
#endif
            break;

        case ST_DUEL:
        {
            // The side-on 1v1 against the region boss. duel_update() owns the
            // whole frame -- movement, the boss brain, drawing -- and hands
            // back to the normal flow (ST_LEVEL_CLEAR / ST_GAME_OVER) from
            // inside the damage paths.
#ifndef AUTOPLAY
            static bool s_duel_card_tried = false;
            static bool s_duel_card_up = false;
            if (!s_duel_card_tried)
            {
                s_duel_card_tried = true;
                s_duel_card_up = !tutorial_done(TUT_DUEL_CARD);
            }
            if (s_duel_card_up)
            {
                duel_card_screen();
                net_update();   // the link stays fed while this console reads
                if (down & (KEY_A | KEY_START | KEY_B))
                {
                    s_duel_card_up = false;
                    tutorial_mark(TUT_DUEL_CARD);
                    audio_play_sfx(SND_UI_CONFIRM);
                }
                break;
            }
#endif
            duel_update();
            net_update();
            break;
        }

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
                // Coming home ends the day, exactly as sleeping at the spring
                // does. An expedition is a day's work, so returning should turn
                // the calendar over -- otherwise the field only advances if the
                // player remembers to go and sleep, and a player who has just
                // fought a boss will not.
                farm_advance_day();
                save_meta();
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
                // Everything the run was holding is dropped, but the field and
                // the cake are not -- so this is exactly the moment to make sure
                // they are on the cartridge rather than only in RAM.
                save_meta();
                g_state = ST_HUB;
                // Every other road home rebinds the hub world; this one used to
                // just clear the two boss flags, which left the map bound to the
                // region the player died in and the camera smaller than the
                // world it was clamping against.
                game_start_hub();
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
        {
            // The finale scene keeps playing behind the roll: the fireworks
            // and candle flicker are driven by the same timer, so the top
            // screen stays alive instead of freezing on the last ending frame.
            g_ending_timer++;
            render_camera(0, 0);
            ending_scene();

            ui_field(UI_SUB);
            if (s_credits_phase == 0)
            {
                ui_plate(UI_SUB, 24, 28, 208, 116, UI_R_PLATE);
                ui_label(UI_SUB, 24, 40, 208, 22, "水豚生存 · 生日快乐",
                         PAL_UI2_BLUE_DARK);
                ui_label(UI_SUB, 24, 66, 208, 18, "制作：BlocksDS",
                         PAL_UI2_INK);
                ui_label(UI_SUB, 24, 84, 208, 18, "像素小分队",
                         PAL_UI2_INK);
                ui_label(UI_SUB, 24, 104, 208, 18, "献给一起冒险的你们",
                         PAL_UI2_VIOLET);
                ui_label(UI_SUB, 24, 122, 208, 18, "生日快乐！", PAL_UI2_AMBER);
                ui_label(UI_SUB, 24, 152, 208, 14, "按 B 继续",
                         PAL_UI2_INK_DIM);
                if (down & KEY_B)
                {
                    // The roll is not the last word: one quiet page after it,
                    // addressed to the person the cake was for.
                    s_credits_phase = 1;
                    audio_play_sfx(SND_UI_MOVE);
                }
            }
            else
            {
                ui_plate(UI_SUB, 24, 52, 208, 88, UI_R_PLATE);
                ui_label(UI_SUB, 24, 64, 208, 18,
                         "这一局，从头到尾，", PAL_UI2_INK);
                ui_label(UI_SUB, 24, 82, 208, 18,
                         "都是为你准备的礼物。", PAL_UI2_VIOLET);
                ui_label(UI_SUB, 24, 108, 208, 22, "生日快乐。",
                         PAL_UI2_AMBER);
                ui_label(UI_SUB, 24, 152, 208, 14, "按 B 返回标题",
                         PAL_UI2_INK_DIM);
                if (down & KEY_B)
                {
                    s_credits_phase = 0;
                    g_state = ST_TITLE;
                    g_menu_cursor = 0;
                }
            }
            audio_play_music(MUS_BIRTHDAY);
            break;
        }

        default:
            g_state = ST_TITLE;
            break;
        }

        set_brightness(fade);
#ifdef AUTOPLAY_UI
        // Screen-capture diagnostics: if the run stalls, a shot shows where.
        top_text_printf(0, 23, PAL_WHITE, "st=%d hs=%d step=%d left=%d f=%d",
                        (int)g_state, (int)g_hitstop,
                        autoplay_debug_step(), autoplay_debug_left(),
                        autoplay_debug_frame());
#endif
        // The readout has to land in the shadow buffer before the flush that
        // presents it, or the next frame's redraw wipes it before anyone sees
        // it.
        perf_draw_hud();
        render_flush();
        text_flush();
        perf_frame_end();
    }

    return 0;
}
