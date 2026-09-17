// Sprite drawing: the two glyph characters, and placeholder circles for
// everything hostile.
//
// ENEMIES ARE CIRCLES
//
// The creatures are placeholders on purpose. The game has 48 monsters on screen
// at once and the thing that has to work right now is the *gameplay* -- can you
// tell a chaser from a shooter, a normal from a boss, a slowed one from a
// healthy one -- not whether a slime has a nice face. So every enemy is a plain
// filled circle, and the information that actually matters is carried by
// properties that do not need art:
//
//   SIZE    boss = big circle, everything else = small circle. This is the one
//           distinction a player must never miss, so it is the most obvious one.
//   COLOUR  one ramp per monster type, so a shooty cactus is never confused with
//           a dashing wasp even in a crowd.
//   RINGS   recruited / pacified / slowed / hurt get an outline in a fixed
//           colour, reusing the conventions the rest of the HUD already uses.
//
// The previous creature art is not deleted: tools/gen_monsters.py still exists
// and now writes into art_retired/, out of the compiled asset set. If the art
// comes back, the circles are one function to revert.
//
// The characters, by contrast, are real sprites -- a 32x32 sheet per character,
// four directions. See tools/gen_heroes.py.

#include <string.h>

#include "game.h"
#include "assets.h"

// ---------------------------------------------------------------------------
// Character sheets
// ---------------------------------------------------------------------------

// The glyph characters: four directions of idle / walk / bite / hurt. The row
// order matches the DIR_* constants, so a direction indexes a row directly.
static const uint8_t *const g_hero_sheets[2] = {
    hero_m_tiles_bin,
    hero_f_tiles_bin,
};

// ---------------------------------------------------------------------------
// Monster definitions
// ---------------------------------------------------------------------------

// One entry per enemy archetype. The circle's size and colour are part of the
// definition rather than of a sprite sheet, which is what lets a new enemy be
// added by adding a row here.
//
// `radius` is in pixels and is the drawn circle's radius; it is also what the
// collision and attack ranges are derived from, so a big enemy genuinely is a
// big target.
const MonsterDef g_monster_defs[] = {
    //             ai        radius  dark          fill          light        atk exp
    // 雨林
    { AI_SHOOTER,       12, PAL_GRN_DARK,  PAL_GRN_MID,   PAL_GRN_LIGHT,   4,  6 },  // 0: rain_frog (树蛙)
    { AI_DASHER,        11, PAL_YELLOW_DARK, PAL_YELLOW_MID, PAL_WHITE_SPARK, 5, 7 },  // 1: rain_wasp (飞蜂)
    { AI_CHASER,        12, PAL_LEAF_DARK, PAL_LEAF_MID,  PAL_LEAF_LIGHT,  4,  6 },  // 2: rain_slime (软泥)
    // 农场
    { AI_CHASER,        11, PAL_BRN_DARK,  PAL_BRN_MID,   PAL_TAN_BELLY,   4,  6 },  // 3: farm_rat (田鼠)
    { AI_DASHER,        12, PAL_FUR_BLACK, PAL_STEEL_DARK, PAL_STEEL_MID,  5,  7 },  // 4: farm_crow (乌鸦)
    { AI_SHOOTER,       12, PAL_CLAY_DARK, PAL_CLAY_MID,  PAL_SAND_LIGHT,  5,  7 },  // 5: farm_mudlet (泥怪)
    // 牧场
    { AI_CHASER,        12, PAL_GREY_DARK, PAL_GREY_MID,  PAL_GREY_LIGHT,  5,  7 },  // 6: past_dog (野犬)
    { AI_DASHER,        12, PAL_CREAM_MID, PAL_WHITE,     PAL_FUR_BLACK,   6,  8 },  // 7: past_ram (黑羊)
    { AI_SHOOTER,       11, PAL_STEEL_DARK, PAL_STEEL_MID, PAL_CYAN_LIGHT, 5,  7 },  // 8: past_hornet (牛虻)
    // 沼泽
    { AI_CHASER,        13, PAL_CLAY_DARK, PAL_TRUNK_DARK, PAL_PURPLE_MID, 5,  8 },  // 9: marsh_slime (淤泥怪)
    { AI_DASHER,        11, PAL_PINE_DARK, PAL_PINE_MID,  PAL_YELLOW_LIGHT, 6, 8 },  // 10: marsh_viper (毒蟒)
    { AI_SHOOTER,       13, PAL_CACT_DARK, PAL_CACT_MID,  PAL_YELLOW_MID,  6,  8 },  // 11: marsh_toad (泥蟾)
    // 火山
    { AI_CHASER,        12, PAL_LAVA_DARK, PAL_LAVA_MID,  PAL_EMBER_YELLOW, 7, 9 },  // 12: volc_imp (火灵)
    { AI_DASHER,        12, PAL_CHOCO_DARK, PAL_CHOCO_MID, PAL_RED_LIGHT,   6,  8 },  // 13: volc_bat (蝠翼)
    { AI_SHOOTER,       13, PAL_LAVA_DARK, PAL_LAVA_MID,  PAL_EMBER_ORANGE, 7, 10 }, // 14: volc_orb (火球)
};

#define MONSTER_DEF_COUNT (int)(sizeof(g_monster_defs) / sizeof(g_monster_defs[0]))

const MonsterDef *monster_def(int id)
{
    if (id >= MONSTER_DEF_COUNT)
        id = 0;
    return &g_monster_defs[id];
}


// Radius of a boss circle. Deliberately about two and a half times a normal
// enemy: at a glance across a crowded 256px screen, that gap is unambiguous.
#define BOSS_CIRCLE_R 30

// The drawn radius of any monster, boss or not. Everything that needs to know
// how big a creature looks goes through here.
int monster_draw_radius(const Monster *m)
{
    if (m->is_boss)
        return BOSS_CIRCLE_R;
    return monster_def(m->def_id)->radius;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// Pick the character's frame from its animation state. The sheet lays each
// direction out as idle(4) / walk(6) / bite(4) / hurt(2), so the walk cycle
// indexes into its own block rather than being sampled out of the idle frames.
static int hero_frame(const Hero *h)
{
    if (h->hurt_stun > 0)
    {
        // First 8 frames: shock scream (frame 0), remaining frames: shivering recovery (frame 1)
        return HF_HURT + (h->hurt_stun <= 8 ? 1 : 0);
    }
    if (h->bite_timer > 0)
    {
        // Play the bite from its start rather than holding one pose.
        int f = 10 - h->bite_timer;
        if (f < 0) f = 0;
        if (f > HF_ATTACK_N - 1) f = HF_ATTACK_N - 1;
        return HF_ATTACK + f;
    }
    if (h->moving)
        return HF_WALK_A + ((h->anim >> 3) % HF_WALK_N);
    return HF_IDLE + ((h->anim >> 4) & 3);
}

void draw_hero(int index)
{
    Hero *h = &g_heroes[index];
    if (!h->active)
        return;

    const uint8_t *tiles = g_hero_sheets[h->variant & 1];
    int frame = h->facing * HERO_FRAMES + hero_frame(h);

    // Left-facing art is stored as mirrored right-facing rows, so no runtime
    // flip is needed.
    gfx_blit_8bpp(tiles, HERO_W, HERO_H, frame, h->x, h->y, false, true);

    int32_t cx, cy;
    hero_center(h, &cx, &cy);

    // Orbiting Water Orbs (灵水环绕)
    if (h->water_orbs > 0)
    {
        static const int8_t sin_tbl[65] = {
              0,   3,   6,   9,  12,  16,  19,  22,  25,  28,  31,  34,  37,
             40,  43,  46,  49,  51,  54,  57,  60,  63,  65,  68,  71,  73,
             76,  78,  81,  83,  85,  88,  90,  92,  94,  96,  98, 100, 102,
            104, 106, 107, 109, 111, 112, 113, 115, 116, 117, 118, 120, 121,
            122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127, 127,
        };
        for (int k = 0; k < h->water_orbs; k++)
        {
            uint8_t a = (uint8_t)(h->orb_angle + k * (256 / h->water_orbs));
            int q = a & 63;
            int sx = sin_tbl[q];
            int sy = sin_tbl[(q + 64) & 63];
            if (a & 64) sx = -sx;
            if (a & 128) sy = -sy;
            int ox = cx + (sx * 22) / 127;
            int oy = cy + (sy * 22) / 127;

            gfx_circle(ox, oy, 4, PAL_CYAN_MID);
            gfx_circle(ox, oy, 2, PAL_CYAN_LIGHT);
            gfx_circle(ox, oy, 1, PAL_WHITE_SPARK);
        }
    }

    // Invulnerability flashes a ring under the character. Blinking the sprite
    // itself would be unreadable at this size and would hide the very thing the
    // player is looking at.
    if (h->invuln > 0 && ((g_frame >> 2) & 1))
        gfx_circle_outline(cx, cy, 14, PAL_WHITE_SPARK);

    // The recruit charge is a resource the player has to be able to watch, and
    // it has no card on the HUD yet at the moment it matters.
    if (h->recruit_charge > 0)
    {
        for (int i = 0; i < h->recruit_charge; i++)
            gfx_fill_rect(cx - 6 + i * 3, cy - 20, 2, 2, PAL_HEAL_INNER);
    }
}


void draw_monster(int index)
{
    Monster *m = &g_monsters[index];
    if (!m->alive)
        return;

    int32_t cx, cy;
    monster_center(m, &cx, &cy);
    int r = monster_draw_radius(m);
    bool hflip = (m->dx < 0);

    if (m->is_boss)
    {
        int f = 0;
        if (m->hurt_timer > 0)
            f = 3;
        else if (m->boss_skill_timer > 0)
            f = 2;
        else if ((g_frame >> 3) & 1)
            f = 1;

        int frame = m->boss_id * 4 + f;
        gfx_blit_8bpp(boss_tiles_bin, 64, 64, frame, cx - 32, cy - 32, hflip, true);
    }
    else
    {
        int f = 0;
        if (m->hurt_timer > 0)
            f = 3;
        else if (m->attack_timer > 0)
            f = 2;

        else if ((g_frame >> 3) & 1)
            f = 1;

        int frame = m->def_id * 4 + f;
        gfx_blit_8bpp(mob_tiles_bin, 32, 32, frame, cx - 16, cy - 16, hflip, true);
    }


    // With up to 48 circles on screen the crowd has to communicate its state
    // without a label: a recruited one gets a friendly ring, a pacified one a
    // calm halo, and a slowed one an earthy outline.
    if (m->recruited)
        gfx_circle_outline(cx, cy, r + 1, PAL_HEAL_OUTER);
    else if (m->pacify_timer > 0)
        gfx_circle_outline(cx, cy, r + 1, PAL_HEAL_INNER);
    else if (m->slow_timer > 0)
        gfx_circle_outline(cx, cy, r + 1, PAL_CLAY_DARK);

    // A shooter telegraphs its attack with a bright ring, which is the one
    // enemy behaviour a player has to be able to predict.
    if (!m->is_boss && m->attack_timer > 0 &&
        (monster_def(m->def_id)->ai == AI_SHOOTER))
        gfx_circle_outline(cx, cy, r + 3, PAL_WHITE_SPARK);

    // A dasher winding up: a double ring so it stands apart from the shooter's
    // single one.
    // Boss skill telegraphing warning ring
    if (m->is_boss && m->boss_skill > 0 && m->boss_skill_timer > 15)
    {
        uint8_t tc = ((g_frame >> 2) & 1) ? PAL_RED_LIGHT : PAL_WHITE_SPARK;
        gfx_circle_outline(m->boss_tx, m->boss_ty, 26, tc);
        gfx_circle_outline(cx, cy, r + 4, tc);
    }

    // Boss health, as a bar over the circle. Without it a boss fight has no
    // sense of progress at all.
    if (m->is_boss && m->hp_max > 0)
    {
        int w = 54;
        int x0 = cx - w / 2;
        int y0 = cy - r - 12;
        gfx_fill_rect(x0 - 1, y0 - 1, w + 2, 7, PAL_OUTLINE);
        gfx_fill_rect(x0, y0, w, 5, PAL_HUD_HEART_EMPTY);
        int fill_w = (int)((int32_t)w * m->hp / m->hp_max);
        gfx_fill_rect(x0, y0, fill_w, 5, PAL_HUD_HEART);
    }
}

// Experience motes and ingredients. Both are small, so they are drawn as tiny
// shapes rather than sprites; the bob and the colour tell them apart.
void draw_gem(int index)
{
    Gem *g = &g_gems[index];
    if (!g->alive)
        return;

    int bob = (g->bob >> 2) & 1;
    int x = g->x, y = g->y - bob;

    if (g->kind == 0)
    {
        // Experience mote: a small green spark.
        gfx_fill_rect(x - 1, y - 2, 3, 5, PAL_HEAL_OUTER);
        gfx_fill_rect(x - 2, y - 1, 5, 3, PAL_HEAL_OUTER);
        gfx_fill_rect(x - 1, y - 1, 3, 3, PAL_GREEN_SPARK);
        gfx_fill_rect(x, y - 1, 1, 1, PAL_WHITE_SPARK);
    }
    else if (g->kind == 1)
    {
        // Ingredient: a gold nugget with a highlight, deliberately larger and
        // brighter than a mote because it is what the region is about.
        gfx_fill_rect(x - 2, y - 3, 5, 7, PAL_OUTLINE);
        gfx_fill_rect(x - 3, y - 2, 7, 5, PAL_OUTLINE);
        gfx_fill_rect(x - 2, y - 2, 5, 5, PAL_UI_GOLD);
        gfx_fill_rect(x - 1, y - 2, 3, 3, PAL_UI_GOLD_LIGHT);
        gfx_fill_rect(x, y - 1, 1, 1, PAL_WHITE_SPARK);
    }
    else if (g->kind == 2)
    {
        // 霸主印记 (Boss Token): Shining royal purple and ruby crest
        gfx_fill_rect(x - 3, y - 3, 7, 7, PAL_OUTLINE);
        gfx_fill_rect(x - 2, y - 2, 5, 5, PAL_PURPLE_DARK);
        gfx_fill_rect(x - 1, y - 1, 3, 3, PAL_PURPLE_LIGHT);
        gfx_fill_rect(x, y, 1, 1, PAL_WHITE_SPARK);
        if ((g_frame >> 2) & 1)
            gfx_circle_outline(x, y, 6, PAL_UI_GOLD_LIGHT);
    }
}
