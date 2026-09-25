// Passive Skill Tree (被动技能树) system.
// Unlocks permanent bonuses across runs using Boss Tokens (霸主印记).

#include <stdio.h>
#include <string.h>
#include "game.h"
#include "skill_tree.h"
#include "assets.h"
#include "ui_draw.h"
#include "save.h"

SkillTree g_skill_tree = { 0, 0, 0, 0 };
uint16_t g_boss_tokens = 0;
static uint8_t s_tree_cursor = 0;

void skill_tree_init(void)
{
    // Preserves g_skill_tree and g_boss_tokens across runs
}

static uint8_t get_branch_lv(int branch)
{
    switch (branch)
    {
    case SKILL_BRANCH_POWER:   return g_skill_tree.power_lv;
    case SKILL_BRANCH_TOUGH:   return g_skill_tree.tough_lv;
    case SKILL_BRANCH_AGI:     return g_skill_tree.agi_lv;
    case SKILL_BRANCH_HARVEST: return g_skill_tree.harvest_lv;
    default:                   return 0;
    }
}

static void set_branch_lv(int branch, uint8_t lv)
{
    switch (branch)
    {
    case SKILL_BRANCH_POWER:   g_skill_tree.power_lv = lv; break;
    case SKILL_BRANCH_TOUGH:   g_skill_tree.tough_lv = lv; break;
    case SKILL_BRANCH_AGI:     g_skill_tree.agi_lv = lv; break;
    case SKILL_BRANCH_HARVEST: g_skill_tree.harvest_lv = lv; break;
    }
}

static const char *s_branch_names[SKILL_BRANCH_MAX] = {
    "力量之树",
    "坚韧之树",
    "敏捷之树",
    "丰饶之树",
};

// What the next tier of a branch costs, in 霸主印记.
//
// Shape: 1, 1, 3 -- cheap to enter, expensive to finish. The old 1, 2, 3 ramp
// put the dear step in the middle, which taxed exactly the moment a player was
// still deciding whether they liked a branch.
//
// The totals are the point. A region pays three tokens (one from 先锋, two from
// the 霸主) and there are five regions, so one complete clear is fifteen. Four
// branches at five each is twenty: a clear buys every branch's first tier plus
// three of the four capstones. That is a run that ends one deliberate gap short
// of done -- a choice the player made and can see -- where the old 24-token
// total could not be reached at all, because the only way to close a nine-token
// gap was to re-farm regions that had nothing left to give.
static int branch_cost(int lv)
{
    return (lv + 1 >= SKILL_MAX_LEVEL) ? 3 : 1;
}

static const char *s_branch_descs[SKILL_BRANCH_MAX][4] = {
    {
        "基础状态，尚未点亮",
        "尖牙强化：基础攻击力+2",
        "撕咬暴击：暴击率+15%",
        "霸主神威：全部技能伤害+30%"
    },
    {
        "基础状态，尚未点亮",
        "厚脂减伤：受击25%几率免伤",
        "巨兽体魄：最大红心上限+2",
        "佛系自愈：每10秒自动回复1心"
    },
    {
        "基础状态，尚未点亮",
        "飞奔短蹄：基础移动速度+1",
        "受创滑步：受击无敌时间+50%",
        "灵动穿梭：全部技能冷却-20%"
    },
    {
        "基础状态，尚未点亮",
        "灵敏嗅觉：拾取吸附半径+25",
        "贪吃体质：经验与掉落收益+35%",
        "天地磁吸：升级瞬吸全图经验"
    }
};

// ---------------------------------------------------------------------------
// Top screen: the sacred grove
//
// The tree is DRAWN rather than blitted: four limbs spread from one fork, one
// per branch of the skill tree, and each limb carries three fruit. A branch's
// colour here is the same colour its row wears on the bottom screen, so "the
// red limb" and 力量之树 are visibly the same thing.
//
// The sky is the kingdom's own sunset -- the family the title screen uses. This
// screen used to be a dark slate "circuit board" with a grid on it, which was
// the one surface in the game that looked like a different game.
// ---------------------------------------------------------------------------

// Each branch's dark (unlit skin) and lit (fruit) tone. Shared with the bottom
// screen's row chips so the two screens cannot drift apart.
static const uint8_t s_branch_col[SKILL_BRANCH_MAX][2] = {
    { PAL_RED_DARK,    PAL_RED_LIGHT    },   // 力量
    { PAL_CYAN_DARK,   PAL_CYAN_LIGHT   },   // 坚韧
    { PAL_GRN_DARK,    PAL_GREEN_SPARK  },   // 敏捷
    { PAL_ORANGE_DARK, PAL_UI_GOLD_LIGHT },   // 丰饶
};

// A limb: a stroke of shrinking discs, so a branch tapers the way a real one
// does. A 1px gfx_line would read as wire, not timber.
static void limb(int x0, int y0, int x1, int y1, int r0, int r1, uint8_t col)
{
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int n = (adx > ady) ? adx : ady;
    if (n < 1)
        n = 1;
    for (int i = 0; i <= n; i++)
    {
        int r = r0 + (r1 - r0) * i / n;
        if (r < 1)
            r = 1;
        gfx_circle(x0 + dx * i / n, y0 + dy * i / n, r, col);
    }
}

// One foliage clump. gfx_circle draws discs, and a canopy of discs reads as a
// lollipop; an ellipse is what makes a tree look leafy.
static void leaf_clump(int cx, int cy, int rx, int ry, uint8_t col)
{
    if (rx < 1 || ry < 1)
        return;
    for (int dy = -ry; dy <= ry; dy++)
    {
        int rem = rx * rx - (rx * rx * dy * dy) / (ry * ry);
        if (rem <= 0)
            continue;
        int hw = 0;
        while ((hw + 1) * (hw + 1) <= rem)
            hw++;
        gfx_fill_rect(cx - hw, cy + dy, hw * 2 + 1, 1, col);
    }
}

void skill_tree_draw_top(void)
{
    render_camera(0, 0);

    // --- dusk sky, the title screen's sunset family ------------------------
    static const uint8_t sky_c[5] = {
        PAL_PURPLE_DARK, PAL_PURPLE_MID, PAL_PINK_DARK,
        PAL_ORANGE_DARK, PAL_ORANGE_MID,
    };
    static const uint8_t sky_y[5] = { 30, 54, 78, 100, 118 };
    int prev = 0;
    for (int i = 0; i < 5; i++)
    {
        gfx_fill_rect(0, prev, AREA_W, sky_y[i] - prev, sky_c[i]);
        prev = sky_y[i];
    }

    // Early stars, only in the band where the sky is still dark.
    for (int i = 0; i < 18; i++)
    {
        int sx = (i * 61 + 17) % AREA_W;
        int sy = (i * 13 + 5) % 34;
        gfx_fill_rect(sx, sy, 1, 1,
                      (i & 1) ? PAL_WHITE_SPARK : PAL_PINK_LIGHT);
    }

    // --- the horizon: a ragged treeline, then the meadow -------------------
    for (int x = 0; x < AREA_W; x++)
    {
        int h = 7 + ((x * 7) % 5) + ((x * 13) % 3);   // never a flat top
        gfx_fill_rect(x, 118 - h, 1, h, PAL_PINE_DARK);
    }
    gfx_fill_rect(0, 118, AREA_W, 24, PAL_GRN_DARK);
    gfx_fill_rect(0, 142, AREA_W, AREA_H - 142, PAL_GRASS_MID);
    for (int x = 0; x < AREA_W; x += 3)
        gfx_fill_rect(x + ((x / 3) & 1), 143, 2, 1, PAL_GRASS_LIGHT);

    const int cx = 128, cy = 96;

    // --- canopy, behind everything -----------------------------------------
    static const struct { int x, y, rx, ry; uint8_t c; } canopy[] = {
        { 128,  60, 34, 22, PAL_LEAF_DARK },
        { 128,  55, 27, 17, PAL_LEAF_MID  },
        {  74,  50, 28, 18, PAL_LEAF_DARK },
        {  70,  45, 21, 13, PAL_LEAF_MID  },
        { 182,  50, 28, 18, PAL_LEAF_DARK },
        { 186,  45, 21, 13, PAL_LEAF_MID  },
        {  74, 140, 28, 17, PAL_LEAF_DARK },
        {  70, 145, 21, 12, PAL_LEAF_MID  },
        { 182, 140, 28, 17, PAL_LEAF_DARK },
        { 186, 145, 21, 12, PAL_LEAF_MID  },
    };
    for (unsigned i = 0; i < sizeof(canopy) / sizeof(canopy[0]); i++)
        leaf_clump(canopy[i].x, canopy[i].y, canopy[i].rx, canopy[i].ry,
                   canopy[i].c);

    // --- trunk: tapered, with a root flare, so it stands on the meadow -----
    for (int y = cy; y < 186; y++)
    {
        int t = y - cy;
        int half = 8 + (t * t) / 700;
        if (half > 26)
            half = 26;
        gfx_fill_rect(cx - half, y, half * 2, 1, PAL_TRUNK_MID);
        gfx_fill_rect(cx - half, y, 3, 1, PAL_TRUNK_DARK);
        gfx_fill_rect(cx + half - 4, y, 4, 1, PAL_TRUNK_LIGHT);
    }
    // Bark: a few vertical seams, so the trunk is not a flat slab.
    for (int k = -2; k <= 2; k += 2)
        gfx_fill_rect(cx + k * 6, cy + 20, 1, 60, PAL_TRUNK_DARK);

    // --- the four limbs, one per branch ------------------------------------
    struct {
        int nodes[3][2];
    } branches[4] = {
        {{{102, 74},  {76, 52},   {46, 32}}},    // 力量, upper left
        {{{102, 118}, {76, 140},  {46, 160}}},   // 坚韧, lower left
        {{{154, 74},  {180, 52},  {210, 32}}},   // 敏捷, upper right
        {{{154, 118}, {180, 140}, {210, 160}}},  // 丰饶, lower right
    };

    for (int b = 0; b < 4; b++)
    {
        int lv = get_branch_lv(b);
        int px = cx, py = cy;
        for (int t = 0; t < 3; t++)
        {
            int nx = branches[b].nodes[t][0];
            int ny = branches[b].nodes[t][1];
            bool unlocked = (lv > t);

            // An unlocked limb is timber all the way out; a locked one fades to
            // the shaded tone past the last fruit the player owns.
            limb(px, py, nx, ny, 6 - t * 2, 4 - t * 2,
                 unlocked ? PAL_TRUNK_MID : PAL_TRUNK_DARK);

            // Sap light travelling out along a live branch.
            if (unlocked)
            {
                int step = (g_frame + t * 16) % 32;
                gfx_circle(px + (nx - px) * step / 32,
                           py + (ny - py) * step / 32, 1, PAL_WHITE_SPARK);
            }
            px = nx;
            py = ny;
        }
    }

    // --- the fruit: one per tier, in the branch's own colour ---------------
    uint16_t ring_t = (uint16_t)(g_frame >> 3);
    for (int b = 0; b < 4; b++)
    {
        int lv = get_branch_lv(b);
        bool is_cur = (b == s_tree_cursor);
        uint8_t col_dark = s_branch_col[b][0];
        uint8_t col_lit = s_branch_col[b][1];

        for (int t = 0; t < 3; t++)
        {
            int nx = branches[b].nodes[t][0];
            int ny = branches[b].nodes[t][1];
            bool unlocked = (lv > t);
            int r = 5 + t * 2;

            if (is_cur && (t == (lv < 3 ? lv : 2)))
            {
                // Reticle on the tier this press would buy: the one mark on
                // this screen that says "here, next".
                uint8_t ring_c = (ring_t & 1) ? PAL_WHITE_SPARK
                                              : PAL_UI_GOLD_LIGHT;
                gfx_circle_outline(nx, ny, r + 4, ring_c);
                gfx_fill_rect(nx - r - 4, ny - r - 4, 3, 2, PAL_UI_GOLD_LIGHT);
                gfx_fill_rect(nx + r + 2, ny - r - 4, 3, 2, PAL_UI_GOLD_LIGHT);
                gfx_fill_rect(nx - r - 4, ny + r + 3, 3, 2, PAL_UI_GOLD_LIGHT);
                gfx_fill_rect(nx + r + 2, ny + r + 3, 3, 2, PAL_UI_GOLD_LIGHT);
            }

            gfx_circle_outline(nx, ny, r + 1, PAL_OUTLINE);
            if (unlocked)
            {
                gfx_circle(nx, ny, r, col_dark);
                gfx_circle(nx, ny, r - 2, col_lit);
                // A lit pip, so the fruit reads as round rather than as a chip.
                gfx_fill_rect(nx - r / 2 - 1, ny - r / 2 - 1, 2, 2,
                              PAL_WHITE_SPARK);
                // A stalk tying it to the limb it grew from.
                gfx_fill_rect(nx, ny - r - 1, 1, 2, PAL_TRUNK_DARK);
            }
            else
            {
                // A bud: unripe, dull, and visibly not yet a fruit.
                gfx_circle(nx, ny, r, PAL_GRN_DARK);
                gfx_circle(nx, ny, r - 2, PAL_GREY_MID);
            }
        }
    }

    // --- the heart of the tree, where the four limbs meet ------------------
    {
        int pulse = (ring_t & 1) ? 0 : 1;
        gfx_circle_outline(cx, cy, 16 + pulse, PAL_UI_GOLD_LIGHT);
        gfx_circle(cx, cy, 13, PAL_UI_GOLD);
        gfx_circle(cx, cy, 9, PAL_YELLOW_LIGHT);
        gfx_circle(cx, cy, 5, PAL_WHITE_SPARK);
    }

    // The two capybaras, sitting on the meadow under the tree.
    gfx_fill_rect(10, 184, 22, 3, PAL_SHADOW);
    gfx_fill_rect(224, 184, 22, 3, PAL_SHADOW);
    gfx_blit_8bpp(hero_m_tiles_bin, HERO_W, HERO_H, DIR_DOWN * HERO_FRAMES,
                  4, 156, false, true);
    gfx_blit_8bpp(hero_f_tiles_bin, HERO_W, HERO_H, DIR_DOWN * HERO_FRAMES,
                  224, 156, false, true);
}

// ---------------------------------------------------------------------------
// Bottom screen: Skill tree interactive menu
// ---------------------------------------------------------------------------

void skill_tree_draw_bottom(void)
{
    ui_field(UI_SUB);

    // Header: the tree's name and the token wallet that pays for upgrades.
    sub_fill_rect(0, 0, AREA_W, 28, PAL_UI2_LINE);
    sub_fill_rect(0, 0, AREA_W, 26, PAL_UI2_SCREEN);
    sub_fill_rect(0, 0, AREA_W, 1, PAL_UI2_BLUE_DARK);
    ui_plate_sel(UI_SUB, 4, 4, 128, 20, UI_R_TAB);
    ui_label(UI_SUB, 4, 4, 128, 20, "水豚神树", PAL_WHITE);
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "印记 %d", g_boss_tokens);
        ui_text_right(UI_SUB, 250, 14, buf, PAL_UI2_BLUE_HI);
    }

    // The four branches as rows. Each row carries the branch name, its level,
    // and the cost of the next tier in a well on the right -- a well because a
    // price is a value you read, not a button you press.
    for (int i = 0; i < SKILL_BRANCH_MAX; i++)
    {
        int lv = get_branch_lv(i);
        bool selected = (i == s_tree_cursor);
        int ry = 34 + i * 30;

        ui_btn(UI_SUB, 20, ry, 216, 28, selected ? UI_SEL : UI_ON);

        // The branch's own colour, the same one its limb wears on the top
        // screen, so "the red one" needs no reading.
        ui_round(UI_SUB, 32, ry + 10, 8, 8, 2, s_branch_col[i][0]);
        ui_round(UI_SUB, 33, ry + 11, 6, 6, 2,
                 get_branch_lv(i) > 0 ? s_branch_col[i][1]
                                      : s_branch_col[i][0]);

        char buf[48];
        snprintf(buf, sizeof(buf), "%s  Lv%d", s_branch_names[i], lv);
        ui_text_mid(UI_SUB, 46, ry + 14, buf,
                    selected ? PAL_WHITE : PAL_UI2_INK);

        if (lv >= SKILL_MAX_LEVEL)
        {
            ui_well(UI_SUB, 150, ry + 6, 76, 16, UI_R_TAB, PAL_UI2_PLATE_DEEP);
            ui_label(UI_SUB, 150, ry + 6, 76, 16, "已满级", PAL_UI2_GREEN);
        }
        else
        {
            int cost = branch_cost(lv);
            bool afford = (g_boss_tokens >= cost);
            // "需 N 印记" is four glyphs wide; at 68px the last one fell off
            // the well's right edge. 76px holds it, and ui_label centres it.
            snprintf(buf, sizeof(buf), "需%d印记", cost);
            ui_well(UI_SUB, 150, ry + 6, 76, 16, UI_R_TAB, PAL_UI2_SCREEN);
            ui_label(UI_SUB, 150, ry + 6, 76, 16, buf,
                     afford ? PAL_UI2_BLUE_HI : PAL_UI2_RED);
        }

        if (selected)
            ui_cursor(UI_SUB, 9, ry + 14, PAL_UI2_INK);
    }

    // The selected branch's next tier, described in a panel of its own: it is
    // the only text on the screen that changes with the cursor, so it gets the
    // space to be read.
    {
        int lv = get_branch_lv(s_tree_cursor);
        const char *desc = (lv >= SKILL_MAX_LEVEL)
                         ? s_branch_descs[s_tree_cursor][3]
                         : s_branch_descs[s_tree_cursor][lv + 1];
        ui_plate(UI_SUB, 20, 156, 216, 28, UI_R_PLATE);
        ui_text_mid(UI_SUB, 32, 170, desc, PAL_UI2_INK);
    }
}

// ---------------------------------------------------------------------------
// Skill tree update / input
// ---------------------------------------------------------------------------

void skill_tree_update(void)
{
    skill_tree_draw_top();
    skill_tree_draw_bottom();

    uint16_t down = input_down();

    if (down & KEY_UP)
    {
        s_tree_cursor = (s_tree_cursor + SKILL_BRANCH_MAX - 1) % SKILL_BRANCH_MAX;
        audio_play_sfx(SND_UI_MOVE);
    }
    if (down & KEY_DOWN)
    {
        s_tree_cursor = (s_tree_cursor + 1) % SKILL_BRANCH_MAX;
        audio_play_sfx(SND_UI_MOVE);
    }

    // Touch screen support
    if (input_touch_pressed())
    {
        uint16_t ty = input_touch_y();
        int row = ty / 16;
        if (row >= 2 && row <= 9)
        {
            int tapped_branch = (row - 2) / 2;
            if (tapped_branch >= 0 && tapped_branch < SKILL_BRANCH_MAX)
            {
                if (s_tree_cursor == tapped_branch)
                    down |= KEY_A; // tap selected branch to upgrade
                else
                {
                    s_tree_cursor = (uint8_t)tapped_branch;
                    audio_play_sfx(SND_UI_MOVE);
                }
            }
        }
        else if (row >= 11)
        {
            down |= KEY_B; // tapped bottom area to exit
        }
    }

    if (down & KEY_A)
    {
        int lv = get_branch_lv(s_tree_cursor);
        if (lv < SKILL_MAX_LEVEL)
        {
            int cost = branch_cost(lv);
            if (g_boss_tokens >= cost)
            {
                g_boss_tokens -= cost;
                set_branch_lv(s_tree_cursor, lv + 1);
                audio_play_sfx(SND_LEVELUP);
                // The purse is shared: in a linked session the host owns the
                // authoritative total, so this spend is reported rather than
                // only applied at home (net.c dedupes it by sequence).
                net_notify_token_delta((int16_t)-cost);

                // Recompute hero stats
                hero_recompute_stats(0);
                hero_recompute_stats(1);

                // A bought node is the most expensive thing the player does
                // with a non-renewable currency, so it goes to the cartridge
                // immediately rather than on the way out of this screen.
                save_write();
            }
            else
            {
                audio_play_sfx(SND_HURT); // Can't afford
            }
        }
    }

    if (down & KEY_B)
    {
        audio_play_sfx(SND_UI_CANCEL);
        g_state = ST_HUB;
    }
}
