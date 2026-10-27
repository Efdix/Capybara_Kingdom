// Sprite drawing: the two capybaras, the enemies, and the pickups.
//
// Enemies are drawn from real sprite sheets -- 15 mobs on a 32x32 grid and 10
// bosses on a 64x64 one -- resampled per MonsterDef.scale so a brute genuinely
// looks bigger than a swarm rather than merely being labelled one. What the
// player has to read off a crowd of 48 is still the creature's *state*, and
// that is carried by rings drawn over the art (recruited / pacified / slowed /
// telegraphing), not by the art itself.
//
// The earlier circle placeholders are gone; tools/gen_monsters.py regenerates
// these sheets and writes its older output into art_retired/, which is not part
// of the compiled asset set.
//
// The characters are a 32x32 sheet each, 8 directions x 16 frames. See
// tools/gen_heroes.py.

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

// One entry per enemy archetype. Every region fields three residents, but no
// two of the fifteen behave alike: each mob carries its own AI, its own stats
// and its own ingredient drop rate, so every region asks for different movement
// and every mob is worth reading before you fight it.
//
//   雨林   飞蜂 weaves in fast, 树蛙 lobs arcing shots, 软泥 splits in two
//   农场   田鼠 steals an ingredient and flees, 乌鸦 dive-bombs, 泥怪 erupts
//   牧场   野犬 hunts in a pack, 黑羊 charges long, 牛虻 hastes its allies
//   沼泽   毒蟒 spits bursts, 泥蟾 lobs two-shot volleys, 淤泥怪 splits
//   火山   蝠翼 weaves, 火球 detonates on contact, 火灵 charges through you
//
// `radius` drives collision and reach; `scale` drives how big it looks, so the
// size the player sees is the size the collision believes in. `drop_pct` is the
// ingredient chance: brutes are the best ingredient source, swarm the worst,
// which makes "which crowd do I wade into" a real choice.
const MonsterDef g_monster_defs[] = {
    //       ai          radius scale speed dmg  hp  exp drop
    // --- 雨林 ---
    { AI_LOBBER,       12,   100,    1,  1,  16,  20,  28 },  // 0: 树蛙
    { AI_ZIGZAG,        7,    70,    2,  1,   8,  12,  16 },  // 1: 飞蜂
    { AI_SPLITTER,     15,   140,    1,  2,  36,  30,  38 },  // 2: 软泥
    // --- 农场 ---
    { AI_STEALER,       7,    72,    3,  1,  10,  18,   0 },  // 3: 田鼠
    { AI_DIVER,        11,    95,    2,  1,  18,  24,  26 },  // 4: 乌鸦
    { AI_NOVA,         17,   140,    1,  2,  46,  34,  40 },  // 5: 泥怪
    // --- 牧场 ---
    { AI_PACK,          8,    82,    2,  1,  12,  15,  20 },  // 6: 野犬
    { AI_CHARGER,      16,   135,    1,  2,  40,  34,  36 },  // 7: 黑羊
    { AI_SUPPORT,      11,   100,    2,  1,  16,  26,  28 },  // 8: 牛虻
    // --- 沼泽 ---
    { AI_SPLITTER,     16,   145,    1,  2,  52,  36,  40 },  // 9: 淤泥怪
    { AI_SPITTER,       9,    88,    2,  1,  11,  16,  22 },  // 10: 毒蟒
    { AI_LOBBER,       13,   115,    1,  1,  20,  26,  30 },  // 11: 泥蟾
    // --- 火山 ---
    { AI_CHARGER,      15,   130,    2,  2,  44,  36,  38 },  // 12: 火灵
    { AI_ZIGZAG,        7,    72,    3,  1,   9,  14,  18 },  // 13: 蝠翼
    { AI_BOMBER,       10,   105,    1,  2,  14,  24,  28 },  // 14: 火球
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
// how big a creature looks goes through here. Splitter offspring and elites
// scale off the def's own radius, so the collided size always matches the
// drawn size.
int monster_draw_radius(const Monster *m)
{
    if (m->is_boss)
        return BOSS_CIRCLE_R;
    int r = monster_def(m->def_id)->radius;
    int pct = m->scale_pct ? m->scale_pct : 100;
    r = (r * pct) / 100;
    if (m->elite)
        r += 2;
    return r;
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
    {
        // Distance, not frames: see HERO_WALK_STEP_PX. `anim` still counts
        // frames, and the speed multiplies here, so the stride stays the same
        // length whether the capybara is at 3 pixels a frame or at 6.
        int phase = (int)(h->anim * h->move_speed) / HERO_WALK_STEP_PX;
        return HF_WALK_A + (phase % HF_WALK_N);
    }

    // The idle cycle runs on the wall clock. It used to be sampled from the
    // same counter the walk advanced -- a counter that only ever incremented
    // while moving -- so four idle poses existed and a standing capybara never
    // got past whichever one the last step happened to leave it on. Offsetting
    // by variant keeps the pair from breathing in unison.
    return HF_IDLE + ((((g_frame >> 4) + (h->variant & 1) * 2)) & 3);
}

void draw_hero(int index)
{
    Hero *h = &g_heroes[index];
    if (!h->active)
        return;

    const uint8_t *tiles = g_hero_sheets[h->variant & 1];
    int frame = h->facing * HERO_FRAMES + hero_frame(h);

    // The first three frames of the hit reaction are drawn as a bright
    // silhouette. This is derived from the invulnerability window the blow just
    // opened rather than from hurt_stun, because 厚皮 sets hurt_stun to zero --
    // keying off that would have made the players who invested in the tanky
    // line the only ones who never saw that they had been hit.
    int inv_start = (g_skill_tree.agi_lv >= 2) ? 90 : 60;
    if (h->invuln + 3 > inv_start)
        gfx_flash_begin(PAL_WHITE_SPARK);

    // Left-facing art is stored as mirrored right-facing rows, so no runtime
    // flip is needed.
    gfx_blit_8bpp(tiles, HERO_W, HERO_H, frame, h->x, h->y, false, true);
    gfx_flash_end();

    int32_t cx, cy;
    hero_center(h, &cx, &cy);

    // Orbiting Water Orbs (灵水环绕)
    if (h->water_orbs > 0)
    {
        for (int k = 0; k < h->water_orbs; k++)
        {
            uint8_t a = (uint8_t)(h->orb_angle + k * (256 / h->water_orbs));
            int dx, dy;
            gfx_dir_256(a, &dx, &dy);
            int ox = cx + (dx * 22) / 127;
            int oy = cy + (dy * 22) / 127;

            gfx_circle(ox, oy, 5, PAL_CYAN_DARK);
            gfx_circle(ox, oy, 4, PAL_CYAN_MID);
            gfx_circle(ox, oy, 2, PAL_CYAN_LIGHT);
            gfx_circle(ox, oy, 1, PAL_WHITE_SPARK);
        }
    }

    // The ultimate release. It is the biggest thing the player does, and
    // without something drawn for it the only feedback was the screen shake --
    // which reads as being hit, not as hitting.
    //
    // ult_flash counts down from 30, so `t` runs 0..29 across the release and
    // drives both the expansion of the rings and the travel of the sparks. The
    // two capybaras set off different colours -- the shockwave is hot white and
    // amber, the shelter is soft green -- so in a two-player run a partner's
    // ultimate is identifiable from across the arena.
    if (h->ult_flash > 0)
    {
        int t = 30 - (int)h->ult_flash;
        bool shelter = (h->variant & 1) != 0;

        for (int k = 0; k < 3; k++)
        {
            int r = 12 + k * 15 + t * 3;
            int phase = (t + k * 5) % 12;
            uint8_t c;
            if (shelter)
                c = (phase < 4) ? PAL_HEAL_CORE
                                : (phase < 8 ? PAL_HEAL_INNER : PAL_HEAL_OUTER);
            else
                c = (phase < 4) ? PAL_ARC_CORE
                                : (phase < 8 ? PAL_ORANGE_MID : PAL_LAVA_DARK);
            gfx_circle_outline(cx, cy, r, c);
            gfx_circle_outline(cx, cy, r - 1, c);
        }

        // Sparks flying outward along sixteen spokes.
        int sr = 20 + t * 4;
        for (int a = 0; a < 256; a += 16)
        {
            int dx, dy;
            gfx_dir_256(a, &dx, &dy);
            gfx_fill_rect(cx + (dx * sr) / 127, cy + (dy * sr) / 127,
                          2, 2, PAL_WHITE_SPARK);
        }
        // Chunks of the burst tumbling out behind the light: four heavy
        // specks that lag and drop, which is what gives the release weight.
        int dr = sr + 6;
        for (int a = 0; a < 256; a += 64)
        {
            int dx, dy;
            gfx_dir_256(a + 32, &dx, &dy);
            gfx_fill_rect(cx + (dx * dr) / 127,
                          cy + (dy * dr) / 127 + t / 2, 2, 1,
                          shelter ? PAL_HEAL_INNER : PAL_ORANGE_MID);
        }
        // A bright core for the first few frames only, so the release reads as
        // an instant rather than as a slow bloom.
        if (t < 6)
            gfx_circle(cx, cy, 16 - t * 2,
                       shelter ? PAL_HEAL_CORE : PAL_ARC_CORE);
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
        // Bosses carry eight poses. Which one shows is the whole read of the
        // fight, so the order of these tests is the order of importance:
        // broken > winding up > mid-attack > enraged > hurt > walking.
        //
        // It starts at BF_IDLE rather than being left to the chain. As a bare
        // `int f;` the chain had a hole in it: a phase-1 boss with every timer
        // at zero, on a frame where the bob bit was clear, matched no branch at
        // all -- and `frame = boss_id * 8 + <garbage>` then indexed anywhere in
        // the sheet. Resting is the correct answer for that boss, so say so.
        int f = BF_IDLE;
        if (m->stagger_timer > 0)
            f = BF_STAGGER;
        else if (m->hurt_timer > 0)
            f = BF_HURT;
        else if (m->boss_skill_timer > 0)
            // The field boss telegraphs for 40 frames and lands the blow at
            // 20: the back half of that is a coiled wind-up, the front half is
            // the strike itself. Splitting on the same boundary the damage
            // uses is what keeps the pose honest.
            f = (m->boss_skill_timer > 20) ? BF_WINDUP : BF_SPECIAL_A;
        else if (m->attack_timer > 0)
            f = BF_ATTACK;
        else if (m->phase > 0 && ((g_frame >> 3) & 1) == 0)
            f = BF_SPECIAL_B;   // phase 2+: the enraged stance, half the time
        else if ((g_frame >> 3) & 1)
            f = BF_BOB;

        int frame = m->boss_id * BOSS_FRAMES + f;
        // The first frames of a hurt reaction are drawn as a bright silhouette.
        // A boss is 64x64 and mostly body, so "that landed" has to be said
        // unambiguously or it is lost under the health bar.
        if (m->hurt_timer > 5)
            gfx_flash_begin(PAL_WHITE_SPARK);
        gfx_blit_8bpp(boss_tiles_bin, 64, 64, frame, cx - 32, cy - 32, hflip, true);
        gfx_flash_end();
    }
    else
    {
        int f = 0;
        if (m->hurt_timer > 0)
            f = 3;
        else if (m->attack_timer > 0)
            f = 2;

        else if (((g_frame + m->bob_phase) >> 3) & 1)
            f = 1;

        AiType ai = monster_def(m->def_id)->ai;

        // Splitter offspring and elites redraw the same sheet at their own
        // size, so a small slime genuinely looks like a small slime.
        int pct = m->scale_pct ? (int)m->scale_pct
                               : (int)monster_def(m->def_id)->scale;

        // The mud nova's swell IS its telegraph: the eruption is only
        // answerable if the inflation is visible, so the body grows as the
        // blow nears -- drawn wider than tall, which reads as pressure.
        if (ai == AI_NOVA && m->state == 1 && m->state_timer > 0)
        {
            int t = 36 - m->state_timer;   // 0..36 across the wind-up
            pct += (t * 26) / 36;
        }

        int drawn = (MOB_W * pct) / 100;
        int frame = m->def_id * 4 + f;
        // Two or three frames of silhouette on contact. This is the single
        // clearest "I hit that one" the crowd can give, and with forty of them
        // on screen the alternative is trusting the player to spot which of the
        // forty just lost health.
        if (m->hurt_timer > 5)
            gfx_flash_begin(PAL_WHITE_SPARK);
        gfx_blit_8bpp_scaled(mob_tiles_bin, MOB_W, MOB_H, frame,
                             cx - drawn / 2, cy - drawn / 2, hflip, true, pct);
        gfx_flash_end();

        // Melee wind-ups that the crowd would otherwise hide. The charge and
        // the dive both lock their aim before they move, so the warning is a
        // ring on the target -- closing as the move nears -- plus, for the
        // charge, the lane it will cross.
        if ((ai == AI_CHARGER || ai == AI_DIVER) && m->state == 1 &&
            m->state_timer > 0)
        {
            int windup = (ai == AI_CHARGER) ? 45 : 35;
            int rr = 9 + (m->state_timer * 15) / windup;
            uint8_t hot = ((g_frame >> 2) & 1) ? PAL_RED_LIGHT
                                               : PAL_WHITE_SPARK;
            gfx_circle_outline(m->boss_tx, m->boss_ty, rr, hot);
            gfx_circle_outline(m->boss_tx, m->boss_ty, rr - 2, PAL_RED_DARK);
            if (ai == AI_CHARGER)
                gfx_line(cx, cy, m->boss_tx, m->boss_ty, PAL_RED_DARK);
        }
        if (ai == AI_NOVA && m->state == 1 && m->state_timer > 0)
        {
            uint8_t hot = ((g_frame >> 2) & 1) ? PAL_RED_LIGHT
                                               : PAL_WHITE_SPARK;
            gfx_circle_outline(cx, cy, r + 2, hot);
            gfx_circle_outline(cx, cy, r + 5, PAL_RED_DARK);
        }
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

    // Elites wear a gold outline and a three-point crown: a crowd of 40 has to
    // show its VIP without a tooltip, and "gold means worth killing" is the
    // one rule the whole drop economy agrees on.
    if (m->elite)
    {
        gfx_circle_outline(cx, cy, r + 2,
                           ((g_frame >> 3) & 1) ? PAL_UI_GOLD_LIGHT
                                                : PAL_UI_GOLD);
        int cy0 = cy - r - 8;
        gfx_fill_rect(cx - 5, cy0 + 2, 11, 2, PAL_UI_GOLD);
        gfx_fill_rect(cx - 5, cy0 - 1, 2, 3, PAL_UI_GOLD);
        gfx_fill_rect(cx - 1, cy0 - 2, 2, 4, PAL_UI_GOLD);
        gfx_fill_rect(cx + 3, cy0 - 1, 2, 3, PAL_UI_GOLD);
        gfx_fill_rect(cx - 1, cy0 - 1, 2, 2, PAL_UI_GOLD_LIGHT);
    }

    // A hasted mob sparks: the support buff is visible from the far side of a
    // fight, which is what makes the hornet a priority target.
    if (m->haste_timer > 0 && ((g_frame >> 2) & 1))
    {
        gfx_fill_rect(cx - r - 3, cy - 2, 2, 2, PAL_UI_GOLD_LIGHT);
        gfx_fill_rect(cx + r + 1, cy - 2, 2, 2, PAL_UI_GOLD_LIGHT);
    }

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

// ---------------------------------------------------------------------------
// Telegraphs (预警)
//
// Every hostile move lays one of these down before it fires, and the shape is
// the same in every region so the vocabulary transfers: a ring means "this
// lands here", a lane means "this crosses here", an arc means "this sweeps
// here". They are drawn as outlines that fill as their timer drains, so the
// remaining life is legible without a HUD -- the player reads "not yet, not
// yet, NOW" off the shape itself.
// ---------------------------------------------------------------------------
void draw_telegraph(int index)
{
    Telegraph *t = &g_telegraphs[index];
    if (!t->alive || t->life == 0)
        return;

    if (t->delay > 0)
    {
        // Held back: part of a chain whose turn has not come. Shown faintly,
        // because knowing a combo is coming is not the same as being told
        // where its first beat lands.
        gfx_circle_outline(t->x, t->y, t->radius, PAL_UI2_SHADOW);
        return;
    }

    // `fill` runs 0..100 across the shape's life, and is what turns a static
    // outline into a countdown: the shape closes on itself as the move nears.
    int total = t->life_max ? t->life_max : 1;
    int elapsed = total - t->life;
    int fill = elapsed * 100 / total;
    uint8_t hot = ((g_frame >> 2) & 1) ? PAL_RED_LIGHT : PAL_WHITE_SPARK;

    switch (t->kind)
    {
    case TG_CHAIN:
    case TG_CIRCLE:
    {
        gfx_circle_outline(t->x, t->y, t->radius, hot);
        int inner = t->radius * fill / 100;
        if (inner > 1)
            gfx_circle_outline(t->x, t->y, inner, PAL_RED_DARK);
        if (fill > 80)
            gfx_circle_outline(t->x, t->y, t->radius - 1, PAL_WHITE_SPARK);
        break;
    }
    case TG_LINE:
    {
        // A lane on the floor, filling along its length toward the far end.
        int y0 = (int)t->y;
        int h = t->radius ? t->radius : 16;
        int x0 = (int)t->x, x1 = (int)t->x2;
        if (x1 < x0) { int s = x0; x0 = x1; x1 = s; }
        gfx_rect_outline(x0, y0 - h, x1 - x0, h, hot);
        int w = (x1 - x0) * fill / 100;
        if (w > 1)
            gfx_fill_rect(x0, y0 - h + 2, w, h - 4, PAL_RED_DARK);
        break;
    }
    case TG_ARC:
    default:
    {
        // A sweep: stamped circles along the swing path, so the arc reads as
        // travelling and "step behind it" is what the shape suggests.
        int x0 = (int)t->x, y0 = (int)t->y;
        int x1 = (int)t->x2, y1 = (int)t->y2;
        for (int a = 0; a < 6; a++)
        {
            int px = x0 + (x1 - x0) * a / 6;
            int py = y0 + (y1 - y0) * a / 6;
            (void)py;
            gfx_circle_outline(px, y1, t->radius * (100 - a * 8) / 100, hot);
        }
        if (fill > 70)
            gfx_line(x0, y0, x1, DUEL_GROUND_Y, PAL_WHITE_SPARK);
        break;
    }
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
    else if (g->kind == GEM_SEED)
    {
        // A seed: a fat amber pip with a pale sprout at the tip. Deliberately
        // smaller than the old ingredient nugget, because there are many more
        // of them -- and green at the top, which is what marks it as something
        // that grows rather than something you hand in.
        gfx_fill_rect(x - 2, y - 1, 4, 4, PAL_OUTLINE);
        gfx_fill_rect(x - 1, y, 2, 3, PAL_UI_GOLD);
        gfx_fill_rect(x - 1, y + 1, 1, 2, PAL_UI_GOLD_LIGHT);
        gfx_fill_rect(x, y - 2, 1, 1, PAL_GREEN_SPARK);
        gfx_fill_rect(x - 1, y - 3, 1, 1, PAL_LEAF_LIGHT);
    }
    else if (g->kind == 3)
    {
        // Heart drop: the only mid-run sustain besides level-ups, so it is
        // drawn in the same shape the HUD uses and pulses to be findable in a
        // crowd of green motes.
        uint8_t fill = ((g_frame >> 3) & 1) ? PAL_HUD_HEART : PAL_HUD_HEART_DARK;
        gfx_fill_rect(x - 3, y - 2, 2, 2, PAL_OUTLINE);
        gfx_fill_rect(x + 1, y - 2, 2, 2, PAL_OUTLINE);
        gfx_fill_rect(x - 3, y - 1, 6, 3, PAL_OUTLINE);
        gfx_fill_rect(x - 2, y + 2, 4, 1, PAL_OUTLINE);
        gfx_fill_rect(x - 1, y + 3, 2, 1, PAL_OUTLINE);
        gfx_fill_rect(x - 2, y - 1, 1, 1, fill);
        gfx_fill_rect(x + 1, y - 1, 1, 1, fill);
        gfx_fill_rect(x - 2, y, 4, 2, fill);
        gfx_fill_rect(x - 1, y + 2, 2, 1, fill);
        gfx_fill_rect(x - 2, y - 1, 1, 1, PAL_PINK_LIGHT);
    }
}
