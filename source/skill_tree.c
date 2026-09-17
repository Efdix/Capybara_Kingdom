// Passive Skill Tree (被动技能树) system.
// Unlocks permanent bonuses across runs using Boss Tokens (霸主印记).

#include <string.h>
#include "game.h"
#include "skill_tree.h"
#include "assets.h"

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
// Top screen: Sacred Skill Tree illustration
// ---------------------------------------------------------------------------

void skill_tree_draw_top(void)
{
    render_camera(0, 0);

    // Deep cosmic slate background with subtle circuit grid
    gfx_fill_rect(0, 0, AREA_W, AREA_H, PAL_UI_PANEL);

    for (int y = 0; y < AREA_H; y += 24)
        gfx_fill_rect(0, y, AREA_W, 1, PAL_GREY_DARK);
    for (int x = 0; x < AREA_W; x += 32)
        gfx_fill_rect(x, 0, 1, AREA_H, PAL_GREY_DARK);

    // Subtle background stars
    for (int i = 0; i < 24; i++)
    {
        int sx = (i * 47 + 13) % AREA_W;
        int sy = (i * 31 + 7) % AREA_H;
        gfx_fill_rect(sx, sy, 1, 1, PAL_GREY_MID);
    }

    int cx = 128, cy = 96;

    // 4 Branches: 0 Power (Top-Left), 1 Toughness (Bottom-Left),
    //             2 Agility (Top-Right), 3 Harvest (Bottom-Right)
    struct {
        uint8_t col_dark, col_lit;
        int nodes[3][2];
    } branches[4] = {
        { PAL_RED_DARK,    PAL_RED_LIGHT,     {{102, 74},  {76, 52},   {46, 32}} },
        { PAL_CYAN_DARK,   PAL_CYAN_LIGHT,    {{102, 118}, {76, 140},  {46, 160}} },
        { PAL_GRN_DARK,    PAL_GREEN_SPARK,   {{154, 74},  {180, 52},  {210, 32}} },
        { PAL_ORANGE_DARK, PAL_UI_GOLD_LIGHT, {{154, 118}, {180, 140}, {210, 160}} },
    };

    // Draw branch connecting lines
    for (int b = 0; b < 4; b++)
    {
        int lv = get_branch_lv(b);
        uint8_t lit_c = branches[b].col_lit;
        uint8_t dark_c = PAL_OUTLINE;

        int px = cx, py = cy;
        for (int t = 0; t < 3; t++)
        {
            int nx = branches[b].nodes[t][0];
            int ny = branches[b].nodes[t][1];
            bool unlocked = (lv > t);
            uint8_t line_c = unlocked ? lit_c : dark_c;

            gfx_line(px, py, nx, ny, line_c);
            if (unlocked)
            {
                gfx_line(px, py + 1, nx, ny + 1, line_c);
                // Energy particle pulsing along branch
                int spark_step = (g_frame + t * 16) % 32;
                int sx = px + ((nx - px) * spark_step) / 32;
                int sy = py + ((ny - py) * spark_step) / 32;
                gfx_circle(sx, sy, 1, PAL_WHITE_SPARK);
            }
            px = nx;
            py = ny;
        }
    }

    // Draw tier nodes (badges)
    for (int b = 0; b < 4; b++)
    {
        int lv = get_branch_lv(b);
        uint8_t lit_c = branches[b].col_lit;
        uint8_t dark_c = branches[b].col_dark;
        bool is_cur = (b == s_tree_cursor);

        for (int t = 0; t < 3; t++)
        {
            int nx = branches[b].nodes[t][0];
            int ny = branches[b].nodes[t][1];
            bool unlocked = (lv > t);
            int r = 5 + t * 2;

            if (is_cur && (t == (lv < 3 ? lv : 2)))
            {
                // Pulsing targeting reticle on the active upgrade target
                uint8_t ring_c = ((g_frame >> 3) & 1) ? PAL_WHITE_SPARK : PAL_UI_GOLD_LIGHT;
                gfx_circle_outline(nx, ny, r + 4, ring_c);
                gfx_fill_rect(nx - r - 4, ny - r - 4, 3, 2, PAL_UI_GOLD_LIGHT);
                gfx_fill_rect(nx + r + 2, ny - r - 4, 3, 2, PAL_UI_GOLD_LIGHT);
                gfx_fill_rect(nx - r - 4, ny + r + 3, 3, 2, PAL_UI_GOLD_LIGHT);
                gfx_fill_rect(nx + r + 2, ny + r + 3, 3, 2, PAL_UI_GOLD_LIGHT);
            }

            gfx_circle_outline(nx, ny, r + 1, PAL_OUTLINE);
            gfx_circle(nx, ny, r, unlocked ? dark_c : PAL_GREY_DARK);
            if (unlocked)
            {
                gfx_circle(nx, ny, r - 2, lit_c);
                gfx_fill_rect(nx - 1, ny - 1, 2, 2, PAL_WHITE_SPARK);
            }
            else
            {
                gfx_fill_rect(nx - 1, ny - 1, 2, 2, PAL_GREY_MID);
            }
        }
    }

    // Draw Center Root Core Node (水豚潜能核心)
    gfx_circle_outline(cx, cy, 15, PAL_UI_GOLD_LIGHT);
    gfx_circle(cx, cy, 13, PAL_UI_GOLD);
    gfx_circle(cx, cy, 9, PAL_YELLOW_LIGHT);
    gfx_circle(cx, cy, 5, PAL_WHITE_SPARK);
    gfx_fill_rect(cx - 2, cy - 2, 4, 4, PAL_WHITE_SPARK);

    // Peaceful capybaras meditating under the celestial energy
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
    text_clear();

    // Top Header Banner
    text_panel(0, 0, 20, 2);
    text_draw(1, 0, "【水豚神树 · 被动技能】", PAL_UI_GOLD_LIGHT);
    text_printf(12, 1, PAL_YELLOW_LIGHT, "印记:%d枚", g_boss_tokens);

    // 4 Branches listing
    for (int i = 0; i < SKILL_BRANCH_MAX; i++)
    {
        int lv = get_branch_lv(i);
        bool selected = (i == s_tree_cursor);
        int py = 2 + i * 2;

        uint8_t title_col = selected ? PAL_WHITE_SPARK : PAL_UI_GOLD;
        const char *arrow = selected ? ">" : " ";

        if (lv >= SKILL_MAX_LEVEL)
        {
            text_printf(0, py, title_col, "%s%s Lv.%d [已满级]",
                        arrow, s_branch_names[i], lv);
        }
        else
        {
            int cost = lv + 1;
            uint8_t cost_col = (g_boss_tokens >= cost) ? PAL_GREEN_SPARK : PAL_HUD_HEART;
            text_printf(0, py, title_col, "%s%s Lv.%d",
                        arrow, s_branch_names[i], lv);
            text_printf(11, py, cost_col, "(需%d印记)", cost);
        }

        // Current / Next level description line
        const char *desc = (lv >= SKILL_MAX_LEVEL)
                         ? s_branch_descs[i][3]
                         : s_branch_descs[i][lv + 1];
        text_printf(1, py + 1, selected ? PAL_CYAN_LIGHT : PAL_UI_TEXT_DIM,
                    " %s", desc);
    }

    // Bottom Action Prompt
    text_panel(0, 11, 20, 3);
    text_draw(1, 11, "上下选择 A升级 (支持触屏)", PAL_WHITE_SPARK);
    text_draw(1, 12, "按 B 返回王国城堡广场", PAL_UI_TEXT_DIM);
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
            int cost = lv + 1;
            if (g_boss_tokens >= cost)
            {
                g_boss_tokens -= cost;
                set_branch_lv(s_tree_cursor, lv + 1);
                audio_play_sfx(SND_LEVELUP);

                // Recompute hero stats
                hero_recompute_stats(0);
                hero_recompute_stats(1);
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
