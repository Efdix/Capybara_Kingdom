// Core gameplay: capybara movement and automatic cards, monster AI, endless
// spawning, exp and ingredient pickups, and the bottom-screen HUD.
//
// Update order per frame: input -> capybara movement -> cards -> projectiles ->
// monsters -> pickups -> effects -> HUD. Keeping the simulation in one place
// means the multiplayer layer only has to serialize state, not re-implement
// rules.
//
// This is a survivors-like, so the player never presses an attack button: the
// capybara bites, slams and sprays on its own cooldowns and the player's job is
// to move. That is why every card in g_card_defs carries a cooldown and why
// this file owns all of the firing decisions.

#include <string.h>
#include <stdio.h>

#include "game.h"
#include "assets.h"

static void spawn_monster(uint8_t def_id, int32_t x, int32_t y, bool boss, uint8_t boss_id);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int iabs(int v) { return v < 0 ? -v : v; }

// Small xorshift PRNG. libc rand() pulls in more code than we need, and a fixed
// seed sequence is fine for spawn positions and AI jitter.
static uint32_t g_rng = 0x2F6E2B1u;

static uint32_t my_rand(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static int32_t dist2(int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
    int32_t dx = ax - bx;
    int32_t dy = ay - by;
    return dx * dx + dy * dy;
}

void hero_center(const Hero *h, int32_t *cx, int32_t *cy)
{
    *cx = h->x + HERO_SPRITE_W / 2;
    *cy = h->y + HERO_SPRITE_H / 2;
}

void monster_center(const Monster *m, int32_t *cx, int32_t *cy)
{
    // Enemies are circles now, so the centre is just its position: the drawn
    // shape is centred on it rather than filling a sprite box. Keeping this
    // function as the single definition of "where a monster is" is what lets
    // the collision, targeting and drawing code stay independent of that.
    *cx = m->x;
    *cy = m->y;
}

// The drawn and collided size of an enemy. Bosses are the big circle class;
// everything else uses its archetype's radius.
static int monster_size(const Monster *m)
{
    return monster_draw_radius(m) * 2;
}

// Is this monster a threat right now? Pacified and recruited ones are not,
// which is what keeps 安抚 and 感化 from being strictly worse than a damage
// card.
static bool monster_is_hostile(const Monster *m)
{
    return m->alive && !m->recruited && m->pacify_timer == 0;
}

static bool hero_hitbox_ok(const Hero *h, int nx, int ny)
{
    return !collision_rect(nx + HERO_HITBOX_OX, ny + HERO_HITBOX_OY,
                           HERO_HITBOX_W, HERO_HITBOX_H);
}

// Circle-vs-point overlap. Every area card uses this, so the cards stay
// independent of each monster's size class.
static bool in_radius(int32_t x, int32_t y, int32_t cx, int32_t cy, int r)
{
    return dist2(x, y, cx, cy) <= (int32_t)r * r;
}

// ---------------------------------------------------------------------------
// Effects and pickups
// ---------------------------------------------------------------------------

void spawn_effect(uint8_t kind, int32_t x, int32_t y, uint8_t life,
                  uint8_t radius)
{
    for (int i = 0; i < MAX_EFFECTS; i++)
    {
        Effect *e = &g_effects[i];
        if (e->alive)
            continue;
        e->alive = true;
        e->kind = kind;
        e->x = x;
        e->y = y;
        e->life = life;
        e->life_max = life;
        e->radius = radius;
        return;
    }
}

void spawn_gem(uint8_t kind, int32_t x, int32_t y, uint16_t value)
{
    for (int i = 0; i < MAX_GEMS; i++)
    {
        Gem *g = &g_gems[i];
        if (g->alive)
            continue;
        g->alive = true;
        g->kind = kind;
        g->x = x;
        g->y = y;
        g->value = value;
        g->bob = (uint8_t)(my_rand() & 15);
        return;
    }
}

static void spawn_projectile(uint8_t kind, int32_t x, int32_t y,
                             int16_t vx, int16_t vy, uint8_t life,
                             uint16_t power, uint8_t radius)
{
    for (int i = 0; i < MAX_PROJECTILES; i++)
    {
        Projectile *p = &g_projectiles[i];
        if (p->alive)
            continue;
        p->alive = true;
        p->kind = kind;
        p->x = x;
        p->y = y;
        p->vx = vx;
        p->vy = vy;
        p->life = life;
        p->power = power;
        p->radius = radius;
        return;
    }
}

// ---------------------------------------------------------------------------
// Card levels -> effective numbers
//
// Every card scales along the same three axes, which is what lets one function
// serve all twelve: cooldown shortens, power rises, radius grows. Level 5 is
// handled per card, because that is where a card should become a different
// thing rather than merely a bigger one.
// ---------------------------------------------------------------------------

static int card_level(const Hero *h, int id) { return h->cards[id]; }

static uint16_t card_cd(const Hero *h, int id)
{
    int lvl = card_level(h, id);
    if (lvl <= 0)
        lvl = 1;
    uint16_t base = g_card_defs[id].cooldown;
    if (g_skill_tree.agi_lv >= 3)
        base = (uint16_t)(base - (base * 20) / 100); // 敏捷之树Lv3: 冷却-20%
    // Shorter as it levels, but never below a quarter of the base.
    uint16_t cut = (uint16_t)((base * (lvl - 1)) / 8);
    return (uint16_t)(base - cut);
}

static int card_power(const Hero *h, int id)
{
    int lvl = card_level(h, id);
    if (lvl <= 0)
        lvl = 1;
    int p = g_card_defs[id].power;
    p += (p * (lvl - 1)) / 2;      // +50% of base per level
    p += h->attack;
    int fang = card_level(h, CARD_FANG);
    if (fang > 0)
        p += (p * fang * 8) / 100; // 利爪: +8% per level to everything
    if (g_skill_tree.power_lv >= 3)
        p += (p * 30) / 100;       // 力量之树Lv3: 全技能威力+30%
    return p;
}

static int card_radius(const Hero *h, int id)
{
    int lvl = card_level(h, id);
    if (lvl <= 0)
        lvl = 1;
    int r = g_card_defs[id].radius;
    return r + (r * (lvl - 1)) / 5;
}

// ---------------------------------------------------------------------------
// Stat recomputation for the passive cards
// ---------------------------------------------------------------------------

void hero_recompute_stats(int hero_index)
{
    Hero *h = &g_heroes[hero_index];
    if (!h->active)
        return;

    int hide = card_level(h, CARD_HIDE);
    int swift = card_level(h, CARD_SWIFT);
    int magnet = card_level(h, CARD_MAGNET);

    // 厚皮: +1 max heart per level. 坚韧之树Lv2: +2颗心
    int hm = 5 + hide + (g_skill_tree.tough_lv >= 2 ? 2 : 0);
    if (hm > HERO_MAX_HEARTS)
        hm = HERO_MAX_HEARTS;
    h->hearts_max = (uint8_t)hm;
    if (h->hearts > h->hearts_max)
        h->hearts = h->hearts_max;
    if (h->hearts == 0)
        h->hearts = h->hearts_max;

    // 快腿: +1 speed per two levels. 敏捷之树Lv1: +1 speed
    int sp = HERO_BASE_SPEED + (swift + 1) / 2 + (g_skill_tree.agi_lv >= 1 ? 1 : 0);
    if (sp > 6)
        sp = 6;
    h->move_speed = (uint16_t)sp;

    // 磁吸: 28 px base, +10 per level. 丰饶之树Lv1: +25px
    h->pickup_r = (uint16_t)(28 + magnet * 10 + (g_skill_tree.harvest_lv >= 1 ? 25 : 0));

    // Base damage: 利爪 drives. 力量之树Lv1: +2 attack
    h->attack = (uint16_t)(3 + card_level(h, CARD_FANG) * 2 + (g_skill_tree.power_lv >= 1 ? 2 : 0));

    // 水球数量绑定灵水卡牌等级 (Lv1: 1球, Lv2: 2球 ... Lv5: 5球)
    int orbs = card_level(h, CARD_MUD);
    h->water_orbs_max = (uint8_t)orbs;
}

void hero_assign_slot(int hero_index, int slot, uint8_t card_id)
{
    if (hero_index < 0 || hero_index >= 2 || slot < 0 || slot >= SKILL_SLOT_COUNT)
        return;
    Hero *h = &g_heroes[hero_index];
    if (card_id != SKILL_SLOT_NONE)
    {
        for (int s = 0; s < SKILL_SLOT_COUNT; s++)
        {
            if (s != slot && h->skill_slots[s] == card_id)
            {
                h->skill_slots[s] = h->skill_slots[slot];
                break;
            }
        }
    }
    h->skill_slots[slot] = card_id;
}

void hero_take_card(int hero_index, int card_id)
{
    Hero *h = &g_heroes[hero_index];
    if (!h->active || card_id < 0 || card_id >= CARD_COUNT)
        return;
    if (h->cards[card_id] >= CARD_MAX_LEVEL)
        return;

    h->cards[card_id]++;
    hero_recompute_stats(hero_index);

    // Auto-slot active card to first empty slot if not already equipped
    if (g_card_defs[card_id].kind != KIND_PASSIVE)
    {
        bool already_slotted = false;
        for (int s = 0; s < SKILL_SLOT_COUNT; s++)
        {
            if (h->skill_slots[s] == card_id)
            {
                already_slotted = true;
                break;
            }
        }
        if (!already_slotted)
        {
            for (int s = 0; s < SKILL_SLOT_COUNT; s++)
            {
                if (h->skill_slots[s] == SKILL_SLOT_NONE)
                {
                    h->skill_slots[s] = (uint8_t)card_id;
                    break;
                }
            }
        }
    }

    // A newly taken 厚皮 should hand over the heart it just granted, or the
    // card feels like it did nothing until the next level-up.
    if (card_id == CARD_HIDE && h->hearts < h->hearts_max)
        h->hearts++;

    if (hero_index == g_local_hero)
        audio_play_sfx(SND_LEVELUP);
}

// ---------------------------------------------------------------------------
// Experience, level-ups and the card offer
// ---------------------------------------------------------------------------

static void vacuum_all_gems(int32_t px, int32_t py)
{
    for (int i = 0; i < MAX_GEMS; i++)
    {
        if (g_gems[i].alive)
        {
            g_gems[i].x = px;
            g_gems[i].y = py;
        }
    }
}
static void hero_gain_exp(int index, int amount)
{
    Hero *h = &g_heroes[index];
    if (!h->active)
        return;

    // 磁吸's ultimate raises experience gained. 丰饶之树Lv2: 经验收益+35%
    if (card_level(h, CARD_MAGNET) >= CARD_MAX_LEVEL)
        amount += amount / 3;
    if (g_skill_tree.harvest_lv >= 2)
        amount += (amount * 35) / 100;

    h->exp += (uint16_t)amount;

    // Each level costs 35% more than the last. The curve is deliberately
    // shallow so a run keeps handing out cards.
    while (h->exp >= h->exp_need)
    {
        h->exp -= h->exp_need;
        h->level++;
        h->exp_need += (uint16_t)(h->exp_need * 35 / 100);

        // A level-up is a full heal, which is the only sustain besides 感化's
        // ultimate.
        h->hearts = h->hearts_max;

        int32_t cx, cy;
        hero_center(h, &cx, &cy);
        spawn_effect(FX_LEVELUP, cx, cy, 40, 22);
        if (g_skill_tree.harvest_lv >= 3)
            vacuum_all_gems(cx, cy);

        if (index == g_local_hero)
        {
            audio_play_sfx(SND_LEVELUP);
            card_menu_roll(index);
            g_state = ST_CARD_PICK;
            break;   // the picker pauses the run; further levels wait
        }
    }
}

// ---------------------------------------------------------------------------
// Monster death and damage
// ---------------------------------------------------------------------------

static void monster_die(Monster *m)
{
    bool was_boss = m->is_boss;
    bool was_ally = m->recruited;

    m->alive = false;

    // Recruited allies drop nothing: 感化 converts a kill into a permanent
    // ally, and paying exp for it too would make the card a farming tool.
    if (was_ally)
        return;

    int32_t cx, cy;
    monster_center(m, &cx, &cy);

    if (g_local_hero < 2 && g_heroes[g_local_hero].active)
        hero_gain_exp(g_local_hero, m->is_boss ? 120 : 8);

    // An experience mote, plus an ingredient drop on a fraction of kills, so
    // the boss gate is a matter of surviving long enough rather than of
    // grinding one particular enemy type.
    spawn_gem(0, cx, cy, 1);
    if (!was_boss && (my_rand() % 100) < 34)
        spawn_gem(1, cx + (int)(my_rand() % 9) - 4, cy, 1);

    if (was_boss)
    {
        const RegionDef *rg = &g_regions[g_region_id];
        if (m->boss_id == rg->boss_id)
        {
            // Final Boss defeated! Drops 2 Boss Tokens (霸主印记)
            spawn_gem(2, cx - 8, cy, 1);
            spawn_gem(2, cx + 8, cy, 1);
            if (g_skill_tree.harvest_lv >= 3)
                vacuum_all_gems(cx, cy);

            g_ingredients_done |= (uint8_t)(1 << g_region_id);
            audio_play_music(MUS_VICTORY);
            audio_play_sfx(SND_EXPLOSION);
            g_state = ST_LEVEL_CLEAR;
        }
        else
        {
            // Mid Boss defeated! Drops 1 Boss Token (霸主印记)
            spawn_gem(2, cx, cy, 1);
            if (g_skill_tree.harvest_lv >= 3)
                vacuum_all_gems(cx, cy);

            for (int k = 0; k < 5; k++)
                spawn_gem(0, cx + (int)(my_rand() % 21) - 10, cy + (int)(my_rand() % 21) - 10, 1);
            audio_play_sfx(SND_EXPLOSION);
            if (!g_boss_out)
                audio_play_music(MUS_BATTLE);
        }
    }
    else
    {
        audio_play_sfx(SND_ENEMY_DIE);
    }
}

static void damage_monster(Monster *m, int damage, int knock_from_x,
                           int knock_from_y, bool knockback)
{
    if (!m->alive)
        return;

    if (damage <= 0)
        damage = 1;

    // 利爪's ultimate: a flat crit chance on everything. 力量之树Lv2: +15%暴击
    if (card_level(&g_heroes[g_local_hero], CARD_FANG) >= CARD_MAX_LEVEL &&
        (my_rand() % 100) < 25)
        damage *= 2;
    else if (g_skill_tree.power_lv >= 2 && (my_rand() % 100) < 15)
        damage *= 2;

    if (m->hp <= damage)
    {
        m->hp = 0;
        monster_die(m);
        return;
    }

    m->hp -= (uint16_t)damage;
    m->hurt_timer = 8;

    // Knockback away from the attacker, unless it is a boss (they resist).
    if (knockback && !m->is_boss)
    {
        int32_t cx, cy;
        monster_center(m, &cx, &cy);
        int dx = (cx > knock_from_x) ? 5 : -5;
        int dy = (cy > knock_from_y) ? 5 : -5;
        if (iabs(cx - knock_from_x) < 6)
            dx = 0;
        if (iabs(cy - knock_from_y) < 6)
            dy = 0;
        int size = monster_size(m);
        if (!collision_rect(m->x + dx + 6, m->y + dy + 6,
                            size - 12, size - 12))
        {
            m->x += dx;
            m->y += dy;
        }
    }
}

// Damage every hostile monster inside a radius of (cx, cy).
static int damage_around(int32_t cx, int32_t cy, int radius, int power,
                         bool knockback)
{
    int hits = 0;
    int32_t r2 = (int32_t)radius * radius;
    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        Monster *m = &g_monsters[i];
        if (!m->alive || m->recruited)
            continue;
        int32_t mx, my;
        monster_center(m, &mx, &my);
        if (dist2(mx, my, cx, cy) <= r2)
        {
            damage_monster(m, power, cx, cy, knockback);
            hits++;
        }
    }
    return hits;
}

// ---------------------------------------------------------------------------
// Capybara damage
// ---------------------------------------------------------------------------

static void hero_take_damage(int index, int damage, int from_x, int from_y)
{
    Hero *h = &g_heroes[index];
    if (!h->active || h->invuln > 0)
        return;

    // 快腿's ultimate: brief immunity while charging.
    if (card_level(h, CARD_SWIFT) >= CARD_MAX_LEVEL && h->ram_timer > 0)
        return;

    int32_t cx, cy;
    hero_center(h, &cx, &cy);

    // 坚韧之树Lv1: 25%几率厚脂减伤，完全抵消此次伤害！
    if (g_skill_tree.tough_lv >= 1 && (my_rand() % 100) < 25)
    {
        spawn_effect(FX_RAM_DUST, cx, cy, 12, 16);
        return;
    }

    if (h->hearts > 0)
        h->hearts--;

    // 敏捷之树Lv2: 受击无敌时间延长至90帧(+50%)
    h->invuln = (g_skill_tree.agi_lv >= 2) ? 90 : 60;
    h->hurt_stun = 16;
    audio_play_sfx(SND_HURT);

    // 受击反馈：屏幕轻微震动 + 头顶惊慌受击汗滴特效
    if (index == g_local_hero)
        g_screen_shake = 5;
    spawn_effect(FX_HIT, cx, cy - 8, 14, 10);

    // 击退受力反馈
    int dx = (cx > from_x) ? 12 : -12;
    int dy = (cy > from_y) ? 12 : -12;
    if (iabs(cx - from_x) < 8) dx = 0;
    if (iabs(cy - from_y) < 8) dy = 0;
    if (hero_hitbox_ok(h, h->x + dx, h->y + dy))
    {
        h->x += dx;
        h->y += dy;
    }

    // 厚皮's ultimate reflects damage back at whatever just bit us.
    if (card_level(h, CARD_HIDE) >= CARD_MAX_LEVEL)
    {
        for (int i = 0; i < MAX_MONSTERS; i++)
        {
            Monster *m = &g_monsters[i];
            if (!m->alive || m->recruited)
                continue;
            int32_t mx, my;
            monster_center(m, &mx, &my);
            if (dist2(mx, my, from_x, from_y) < 40 * 40)
                damage_monster(m, 4, cx, cy, true);
        }
    }

    if (h->hearts == 0 && index == g_local_hero)
    {
        // Soft failure, kept from the original design: going down ends the run
        // but never the game. Ingredients already secured stay secured, so a
        // birthday playthrough cannot be bricked by one bad region.
        audio_play_music(MUS_VICTORY);
        g_state = ST_GAME_OVER;
    }
}

// ---------------------------------------------------------------------------
// The automatic bite: the one attack that is always running
// ---------------------------------------------------------------------------

static void hero_auto_bite(int index)
{
    Hero *h = &g_heroes[index];
    if (!h->active || h->hurt_stun > 0)
        return;

    int32_t cx, cy;
    hero_center(h, &cx, &cy);

    int level = card_level(h, CARD_TEETH);
    int range = BITE_RANGE + level * 4;
    int32_t tx, ty;
    int target = nearest_monster(cx, cy, range, &tx, &ty);
    if (target < 0)
        return;

    // Face the target so the bite pose and the damage read as aimed.
    int dx = tx - cx, dy = ty - cy;
    if (iabs(dx) > iabs(dy))
        h->facing = dx < 0 ? DIR_LEFT : DIR_RIGHT;
    else
        h->facing = dy < 0 ? DIR_UP : DIR_DOWN;

    // The base bite is weak; it exists so the capybara is never helpless and so
    // the character art has something to do before any card is taken.
    int power = 4 + h->attack;
    if (level > 0)
        power += card_power(h, CARD_TEETH) / 2;
    if (card_level(h, CARD_FANG) >= CARD_MAX_LEVEL && (my_rand() % 100) < 25)
        power *= 2;

    spawn_effect(FX_BITE, tx, ty, 8, 6);

    // 尖牙 at level 5 bites three times; below that it is a single bite on the
    // nearest target. Either way the target is hit if it is still in range.
    int bites = (level >= CARD_MAX_LEVEL) ? 3 : 1;
    for (int b = 0; b < bites; b++)
    {
        int32_t mx, my;
        if (nearest_monster(cx, cy, range, &mx, &my) < 0)
            break;
        int hit = 0;
        for (int i = 0; i < MAX_MONSTERS; i++)
        {
            Monster *m = &g_monsters[i];
            if (!m->alive || m->recruited)
                continue;
            int32_t ax, ay;
            monster_center(m, &ax, &ay);
            if (in_radius(ax, ay, mx, my, 14))
            {
                damage_monster(m, power, cx, cy, true);
                hit = 1;
                break;
            }
        }
        if (!hit)
            break;
    }

    h->bite_timer = 10;
    h->cooldown[CARD_TEETH] = (level > 0) ? card_cd(h, CARD_TEETH)
                                          : BITE_COOLDOWN;
    if (index == g_local_hero)
        audio_play_sfx(SND_ATTACK);
}

static void fire_card(int index, int card_id);

static void hero_manual_bite(int index)


{
    Hero *h = &g_heroes[index];
    if (!h->active || h->hurt_stun > 0)
        return;

    int32_t cx, cy;
    hero_center(h, &cx, &cy);

    int level = card_level(h, CARD_TEETH);
    int range = BITE_RANGE + level * 5;
    int32_t tx = cx, ty = cy;

    // Direction vector from facing
    int fx = 0, fy = 0;
    if (h->facing == DIR_LEFT) fx = -1;
    else if (h->facing == DIR_RIGHT) fx = 1;
    else if (h->facing == DIR_UP) fy = -1;
    else if (h->facing == DIR_DOWN) fy = 1;
    else if (h->facing == DIR_DOWN_LEFT)  { fx = -1; fy = 1; }
    else if (h->facing == DIR_DOWN_RIGHT) { fx = 1;  fy = 1; }
    else if (h->facing == DIR_UP_LEFT)    { fx = -1; fy = -1; }
    else if (h->facing == DIR_UP_RIGHT)   { fx = 1;  fy = -1; }

    int target = nearest_monster(cx, cy, range, &tx, &ty);
    if (target < 0)
    {
        tx = cx + fx * range;
        ty = cy + fy * range;
    }

    int power = 5 + h->attack;
    if (level > 0)
        power += card_power(h, CARD_TEETH) / 2;

    spawn_effect(FX_BITE, tx, ty, 8, 8);
    damage_around(tx, ty, 20, power, true);

    h->bite_timer = 12;
    h->cooldown[CARD_TEETH] = 22; // ~0.35s cd
    if (index == g_local_hero)
        audio_play_sfx(SND_ATTACK);
}

static void fire_card(int index, int card_id);

void hero_cast_slot(int index, int slot)
{
    if (index < 0 || index >= 2 || slot < 0 || slot >= SKILL_SLOT_COUNT)
        return;
    Hero *h = &g_heroes[index];
    if (!h->active || h->hurt_stun > 0 || h->remote)
        return;

    uint8_t cid = h->skill_slots[slot];
    if (cid == SKILL_SLOT_NONE || cid >= CARD_COUNT)
        return;

    if (h->cooldown[cid] > 0)
        return;

    if (cid == CARD_TEETH)
    {
        hero_manual_bite(index);
    }
    else
    {
        if (card_level(h, cid) <= 0)
            return;
        fire_card(index, cid);
        h->cooldown[cid] = card_cd(h, cid);
        if (index == g_local_hero)
            audio_play_sfx(SND_ATTACK);
    }
}

static int s_touch_drag_from = -1;

static void hero_handle_active_skills(int index)
{
    Hero *h = &g_heroes[index];
    if (!h->active || h->hurt_stun > 0 || h->remote)
        return;

    uint16_t down = input_down();
    uint16_t held = input_held();

    // 1. Hardware Buttons: A, B, X, Y (plus shoulder triggers L/R)
    if (down & KEY_A)
        hero_cast_slot(index, SKILL_SLOT_A);
    if (down & (KEY_B | KEY_L))
        hero_cast_slot(index, SKILL_SLOT_B);
    if (down & KEY_X)
        hero_cast_slot(index, SKILL_SLOT_X);
    if ((down & (KEY_Y | KEY_R)) || ((held & (KEY_Y | KEY_R)) && (g_frame % 20 == 0)))
        hero_cast_slot(index, SKILL_SLOT_Y);

    // 2. Touch Screen: tap slot box to cast, drag to another slot to swap!
    if (input_touch_pressed())
    {
        for (int s = 0; s < 4; s++)
        {
            int sx = 4 + s * 62;
            if (input_touch_in(sx, 148, 58, 40))
            {
                s_touch_drag_from = s;
                hero_cast_slot(index, s);
                break;
            }
        }
    }
    else if (!input_touch_pressed() && s_touch_drag_from >= 0)
    {
        uint16_t tx = input_touch_x();
        uint16_t ty = input_touch_y();
        if (ty >= 145)
        {
            int drop_slot = -1;
            for (int s = 0; s < 4; s++)
            {
                int sx = 4 + s * 62;
                if (tx >= sx && tx <= sx + 58)
                {
                    drop_slot = s;
                    break;
                }
            }
            if (drop_slot >= 0 && drop_slot != s_touch_drag_from)
            {
                uint8_t temp = h->skill_slots[s_touch_drag_from];
                h->skill_slots[s_touch_drag_from] = h->skill_slots[drop_slot];
                h->skill_slots[drop_slot] = temp;
                audio_play_sfx(SND_UI_CONFIRM);
            }
        }
        s_touch_drag_from = -1;
    }
}


// ---------------------------------------------------------------------------
// Cards
//
// Each active card has a case here. They all share a shape: find targets with
// nearest_monster or damage_around, apply damage, spawn the effect that makes it
// readable, and set the cooldown.
// ---------------------------------------------------------------------------

static void fire_card(int index, int card_id)
{
    Hero *h = &g_heroes[index];
    int lvl = card_level(h, card_id);
    if (lvl <= 0)
        return;

    int32_t cx, cy;
    hero_center(h, &cx, &cy);
    int power = card_power(h, card_id);
    int radius = card_radius(h, card_id);
    bool local = (index == g_local_hero);

    switch (card_id)
    {
    case CARD_TAIL:
    {
        if (local) audio_play_sfx(SND_ATTACK);
        spawn_effect(FX_TAIL, cx, cy, 12, (uint8_t)radius);
        damage_around(cx, cy, radius, power, true);

        // Ultimate: whatever it sweeps is left stunned.
        if (lvl >= CARD_MAX_LEVEL)
        {
            for (int i = 0; i < MAX_MONSTERS; i++)
            {
                Monster *m = &g_monsters[i];
                if (!m->alive || m->recruited)
                    continue;
                int32_t mx, my;
                monster_center(m, &mx, &my);
                if (in_radius(mx, my, cx, cy, radius))
                {
                    m->state = 1;
                    m->state_timer = 40;
                }
            }
        }
        break;
    }

    case CARD_RAM:
    {
        int32_t tx, ty;
        int dx = 0, dy = 0;
        if (nearest_monster(cx, cy, 120, &tx, &ty) >= 0)
        {
            dx = tx - cx;
            dy = ty - cy;
        }
        else
        {
            if (h->facing == DIR_LEFT) dx = -1;
            else if (h->facing == DIR_RIGHT) dx = 1;
            else if (h->facing == DIR_UP) dy = -1;
            else if (h->facing == DIR_DOWN) dy = 1;
            else if (h->facing == DIR_DOWN_LEFT)  { dx = -1; dy = 1; }
            else if (h->facing == DIR_DOWN_RIGHT) { dx = 1;  dy = 1; }
            else if (h->facing == DIR_UP_LEFT)    { dx = -1; dy = -1; }
            else if (h->facing == DIR_UP_RIGHT)   { dx = 1;  dy = -1; }
        }
        int len = (iabs(dx) > iabs(dy)) ? iabs(dx) : iabs(dy);
        len = len ? len : 1;
        h->ram_dx = (int8_t)((dx * 3) / len);
        h->ram_dy = (int8_t)((dy * 3) / len);
        if (h->ram_dx == 0 && h->ram_dy == 0)
            h->ram_dy = 2;
        h->ram_timer = (uint16_t)(16 + lvl * 2);
        if (h->invuln < 8)
            h->invuln = 8;
        if (local) audio_play_sfx(SND_ATTACK);
        spawn_effect(FX_RAM_DUST, cx, cy, 18, (uint8_t)(radius + 6));
        break;
    }

    case CARD_WATER:
    {
        // Multi-tier water gun / water jet / water cannon in 8 directions!
        int16_t vx = 0, vy = 0, sp = 7;
        if (h->facing == DIR_LEFT) vx = -sp;
        else if (h->facing == DIR_RIGHT) vx = sp;
        else if (h->facing == DIR_UP) vy = -sp;
        else if (h->facing == DIR_DOWN) vy = sp;
        else if (h->facing == DIR_DOWN_LEFT)  { vx = -5; vy = 5; }
        else if (h->facing == DIR_DOWN_RIGHT) { vx = 5;  vy = 5; }
        else if (h->facing == DIR_UP_LEFT)    { vx = -5; vy = -5; }
        else if (h->facing == DIR_UP_RIGHT)   { vx = 5;  vy = -5; }

        if (lvl == 1)
        {
            spawn_projectile(PROJ_WATER, cx, cy, vx, vy, 55, power, 5);
        }
        else if (lvl == 2)
        {
            spawn_projectile(PROJ_WATER, cx, cy, vx, vy, 65, power, 7);
        }
        else if (lvl == 3)
        {
            int ox = (vy != 0) ? 6 : 0;
            int oy = (vx != 0) ? 6 : 0;
            spawn_projectile(PROJ_WATER, cx - ox, cy - oy, vx, vy, 65, power, 6);
            spawn_projectile(PROJ_WATER, cx + ox, cy + oy, vx, vy, 65, power, 6);
        }
        else if (lvl == 4)
        {
            spawn_projectile(PROJ_WATER, cx, cy, vx, vy, 75, power, 10);
        }
        else
        {
            // Lv5: Super Water Cannon!
            int ox = (vy != 0) ? 10 : 0;
            int oy = (vx != 0) ? 10 : 0;
            spawn_projectile(PROJ_WATER, cx - ox, cy - oy, (int16_t)(vx * 9 / 7), (int16_t)(vy * 9 / 7), 80, power, 9);
            spawn_projectile(PROJ_WATER, cx, cy, (int16_t)(vx * 9 / 7), (int16_t)(vy * 9 / 7), 80, (uint16_t)(power * 13 / 10), 13);
            spawn_projectile(PROJ_WATER, cx + ox, cy + oy, (int16_t)(vx * 9 / 7), (int16_t)(vy * 9 / 7), 80, power, 9);
            spawn_effect(FX_RAM_DUST, cx, cy, 18, 30);
            if (local) g_screen_shake = 3;
        }

        if (local) audio_play_sfx(SND_MAGIC_FIRE);
        break;
    }


    case CARD_MUD: // 灵水 (Orbiting Water Orbs burst pulse)
    {
        int orbs = h->water_orbs;
        if (orbs <= 0) orbs = 1;
        spawn_effect(FX_PACIFY, cx, cy, 16, (uint8_t)(radius + orbs * 6));
        damage_around(cx, cy, radius + orbs * 6, power, true);
        if (local) audio_play_sfx(SND_MAGIC_ICE);
        break;
    }

    case CARD_DUNG: // 便便陷阱 (Ground Dung Trap)
    {
        spawn_effect(FX_DUNG_TRAP, cx, cy, (uint8_t)(200 + lvl * 50), (uint8_t)(radius + lvl * 2));
        for (int i = MAX_EFFECTS - 1; i >= 0; i--)
        {
            if (g_effects[i].alive && g_effects[i].kind == FX_DUNG_TRAP && g_effects[i].x == cx && g_effects[i].y == cy)
            {
                g_effects[i].power = (uint16_t)power;
                break;
            }
        }
        if (local) audio_play_sfx(SND_ATTACK);
        break;
    }

    case CARD_PACIFY:
    {
        // Calm the crowd: everything nearby stops attacking for a while. This
        // turns the capybara's real-world reputation into a mechanic.
        spawn_effect(FX_PACIFY, cx, cy, 40, (uint8_t)radius);
        for (int i = 0; i < MAX_MONSTERS; i++)
        {
            Monster *m = &g_monsters[i];
            if (!m->alive || m->recruited)
                continue;
            int32_t mx, my;
            monster_center(m, &mx, &my);
            if (!in_radius(mx, my, cx, cy, radius))
                continue;

            m->pacify_timer = (uint16_t)(150 + lvl * 60);

            // The ultimate turns the calm into a betrayal: cowed enemies start
            // hitting each other.
            if (lvl >= CARD_MAX_LEVEL)
            {
                for (int j = 0; j < MAX_MONSTERS; j++)
                {
                    Monster *o = &g_monsters[j];
                    if (!o->alive || o == m || o->recruited)
                        continue;
                    int32_t ox, oy;
                    monster_center(o, &ox, &oy);
                    if (in_radius(ox, oy, mx, my, 40))
                        damage_monster(o, power + 6, mx, my, false);
                }
            }
        }
        if (local) audio_play_sfx(SND_HEAL);
        break;
    }

    case CARD_RECRUIT:
    {
        // 感化 charges up while an enemy is nearby and converts one when full.
        // The card only arms itself when there is something to work on, so a
        // capybara standing alone does not burn the cooldown for nothing.
        int32_t tx, ty;
        if (nearest_monster(cx, cy, radius, &tx, &ty) < 0)
            return;

        if (h->recruit_charge < 5)
        {
            h->recruit_charge++;
            // A soft tick, not the confirm sound: this fires repeatedly while
            // charging and would otherwise be the loudest thing in the game.
            if (local && h->recruit_charge == 1)
                audio_play_sfx(SND_UI_MOVE);
            break;
        }

        for (int i = 0; i < MAX_MONSTERS; i++)
        {
            Monster *m = &g_monsters[i];
            if (!m->alive || m->recruited || m->is_boss)
                continue;
            int32_t mx, my;
            monster_center(m, &mx, &my);
            if (!in_radius(mx, my, tx, ty, 24))
                continue;

            // Turn it: it heals to full so it is useful immediately rather
            // than dying on arrival.
            m->recruited = true;
            m->hp = m->hp_max;
            m->pacify_timer = 0;
            m->state = 0;
            m->state_timer = 0;
            m->attack_timer = 0;
            h->recruit_charge = 0;
            spawn_effect(FX_PACIFY, mx, my, 30, 16);
            if (lvl >= CARD_MAX_LEVEL && h->hearts < h->hearts_max)
                h->hearts++;
            if (local) audio_play_sfx(SND_HEAL);
            break;
        }
        break;
    }

    default:
        break;   // passives have no active effect
    }

    h->cooldown[card_id] = card_cd(h, card_id);
}

// ---------------------------------------------------------------------------
// Capybara update
// ---------------------------------------------------------------------------

static void hero_update(int index)
{
    Hero *h = &g_heroes[index];
    if (!h->active)
        return;

    if (h->invuln > 0) h->invuln--;
    if (h->bite_timer > 0) h->bite_timer--;
    if (h->hurt_stun > 0) h->hurt_stun--;
    if (h->pacify_timer > 0) h->pacify_timer--;
    for (int i = 0; i < CARD_COUNT; i++)
        if (h->cooldown[i] > 0)
            h->cooldown[i]--;

    // 坚韧之树Lv3: 佛系自愈，每10秒(600帧)自动回复1颗心
    if (g_skill_tree.tough_lv >= 3 && (g_frame % 600) == 0 && h->hearts < h->hearts_max)
    {
        h->hearts++;
        int32_t hcx, hcy;
        hero_center(h, &hcx, &hcy);
        spawn_effect(FX_LEVELUP, hcx, hcy, 20, 12);
        audio_play_sfx(SND_HEAL);
    }

    // 灵水环绕: 水球周身旋转公转、碰触伤敌消耗、按时恢复
    int orbs_lvl = card_level(h, CARD_MUD);
    if (orbs_lvl > 0)
    {
        h->water_orbs_max = (uint8_t)orbs_lvl;
        if (h->water_orbs == 0 && h->orb_regen_timer == 0)
            h->water_orbs = h->water_orbs_max;

        h->orb_angle = (uint8_t)(h->orb_angle + 4);

        if (h->water_orbs < h->water_orbs_max)
        {
            if (h->orb_regen_timer > 0)
                h->orb_regen_timer--;
            else
            {
                h->water_orbs++;
                h->orb_regen_timer = (orbs_lvl >= CARD_MAX_LEVEL) ? 45 : 90;
                int32_t ocx, ocy;
                hero_center(h, &ocx, &ocy);
                spawn_effect(FX_PACIFY, ocx, ocy, 10, 16);
            }
        }

        if (h->water_orbs > 0)
        {
            static const int8_t s_orb_sin[65] = {
                  0,   3,   6,   9,  12,  16,  19,  22,  25,  28,  31,  34,  37,
                 40,  43,  46,  49,  51,  54,  57,  60,  63,  65,  68,  71,  73,
                 76,  78,  81,  83,  85,  88,  90,  92,  94,  96,  98, 100, 102,
                104, 106, 107, 109, 111, 112, 113, 115, 116, 117, 118, 120, 121,
                122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127, 127,
            };
            int32_t ocx, ocy;
            hero_center(h, &ocx, &ocy);
            bool orb_hit = false;
            for (int k = 0; k < h->water_orbs; k++)
            {
                uint8_t a = (uint8_t)(h->orb_angle + k * (256 / h->water_orbs));
                int q = a & 63;
                int sx = s_orb_sin[q];
                int sy = s_orb_sin[(q + 64) & 63];
                if (a & 64) sx = -sx;
                if (a & 128) sy = -sy;
                int32_t ox = ocx + (sx * 22) / 127;
                int32_t oy = ocy + (sy * 22) / 127;

                for (int m = 0; m < MAX_MONSTERS; m++)
                {
                    Monster *mo = &g_monsters[m];
                    if (!mo->alive || mo->recruited)
                        continue;
                    int32_t mx, my;
                    monster_center(mo, &mx, &my);
                    if (dist2(ox, oy, mx, my) < 14 * 14)
                    {
                        damage_monster(mo, card_power(h, CARD_MUD), ocx, ocy, true);
                        spawn_effect(FX_BITE, ox, oy, 10, 8);
                        audio_play_sfx(SND_MAGIC_ICE);
                        h->water_orbs--;
                        h->orb_regen_timer = (orbs_lvl >= CARD_MAX_LEVEL) ? 45 : 90;
                        orb_hit = true;
                        break;
                    }
                }
                if (orb_hit)
                    break;
            }
        }
    }

    // The charge (冲撞) overrides walking while it lasts.
    if (h->ram_timer > 0)
    {
        h->ram_timer--;
        int dx = h->ram_dx * 3;
        int dy = h->ram_dy * 3;
        if (hero_hitbox_ok(h, h->x + dx, h->y + dy))
        {
            h->x += dx;
            h->y += dy;
        }
        else
        {
            h->ram_timer = 0;
        }
        h->moving = true;
        h->anim++;

        int32_t cx, cy;
        hero_center(h, &cx, &cy);
        int lvl = card_level(h, CARD_RAM);
        damage_around(cx, cy, card_radius(h, CARD_RAM),
                      card_power(h, CARD_RAM), true);
        // Ultimate: a shockwave every few frames of the charge.
        if (lvl >= CARD_MAX_LEVEL && (h->ram_timer % 6) == 0)
        {
            spawn_effect(FX_RAM_DUST, cx, cy, 14, 26);
            damage_around(cx, cy, 34, card_power(h, CARD_RAM) / 2, true);
        }
    }
    else if (h->remote)
    {
        // The remote capybara's position arrives over the network; we only
        // drive the animation from how far it moved.
        int32_t dx = h->net_x - h->x;
        int32_t dy = h->net_y - h->y;
        // Interpolate toward the reported position to smooth 30 Hz updates.
        h->x += dx / 3;
        h->y += dy / 3;
        h->moving = (iabs(dx) + iabs(dy)) > 2;
        if (h->moving)
            h->anim++;
        h->facing = h->net_facing;
        return;
    }
    else if (h->hurt_stun > 0)
    {
        return;   // no control while stunned
    }
    else
    {
        uint16_t keys = input_held();
        int dx = 0, dy = 0;

        if (keys & KEY_LEFT)  dx -= 1;
        if (keys & KEY_RIGHT) dx += 1;
        if (keys & KEY_UP)    dy -= 1;
        if (keys & KEY_DOWN)  dy += 1;

        if (dx < 0 && dy < 0)      h->facing = DIR_UP_LEFT;
        else if (dx > 0 && dy < 0) h->facing = DIR_UP_RIGHT;
        else if (dx < 0 && dy > 0) h->facing = DIR_DOWN_LEFT;
        else if (dx > 0 && dy > 0) h->facing = DIR_DOWN_RIGHT;
        else if (dx < 0)           h->facing = DIR_LEFT;
        else if (dx > 0)           h->facing = DIR_RIGHT;
        else if (dy < 0)           h->facing = DIR_UP;
        else if (dy > 0)           h->facing = DIR_DOWN;

        int speed = h->move_speed;

        // Axis-separated movement so sliding along walls feels right.
        if (dx != 0)
        {
            int nx = h->x + dx * speed;
            if (hero_hitbox_ok(h, nx, h->y))
                h->x = nx;
        }
        if (dy != 0)
        {
            int ny = h->y + dy * speed;
            if (hero_hitbox_ok(h, h->x, ny))
                h->y = ny;
        }

        h->moving = (dx != 0 || dy != 0);
        if (h->moving)
            h->anim++;

    }

    if (h->x < 0) h->x = 0;
    if (h->y < 0) h->y = 0;
    if (h->x > WORLD_W - HERO_SPRITE_W) h->x = WORLD_W - HERO_SPRITE_W;
    if (h->y > WORLD_H - HERO_SPRITE_H) h->y = WORLD_H - HERO_SPRITE_H;
}

// Fire every card whose cooldown has elapsed. The bite is deliberately NOT
// here: it is the always-on auto-attack and lives in hero_auto_bite, because it
// has to work before any card is taken.
static void hero_fire_cards(int index)
{
    Hero *h = &g_heroes[index];
    if (!h->active || h->hurt_stun > 0 || h->remote)
        return;

    for (int id = 0; id < CARD_COUNT; id++)
    {
        if (h->cards[id] <= 0)
            continue;
        if (g_card_defs[id].kind == KIND_PASSIVE)
            continue;

        // Don't auto-fire skills that are equipped in manual ABXY slots!
        bool is_slotted = false;
        for (int s = 0; s < SKILL_SLOT_COUNT; s++)
        {
            if (h->skill_slots[s] == id)
            {
                is_slotted = true;
                break;
            }
        }
        if (is_slotted)
            continue;

        if (h->cooldown[id] > 0)
            continue;
        fire_card(index, id);
    }
}

// ---------------------------------------------------------------------------
// Targeting
// ---------------------------------------------------------------------------

int nearest_hero(int px, int py, int32_t *out_dist)
{
    int best = -1;
    int32_t bestd = 0;
    for (int i = 0; i < 2; i++)
    {
        if (!g_heroes[i].active)
            continue;
        int32_t cx, cy;
        hero_center(&g_heroes[i], &cx, &cy);
        int32_t d = dist2(px, py, cx, cy);
        if (best < 0 || d < bestd)
        {
            best = i;
            bestd = d;
        }
    }
    if (out_dist)
        *out_dist = bestd;
    return best;
}

int nearest_monster(int32_t px, int32_t py, int32_t max_dist,
                    int32_t *out_x, int32_t *out_y)
{
    int best = -1;
    int32_t bestd = max_dist * max_dist;
    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        Monster *m = &g_monsters[i];
        // Pacified monsters are still valid TARGETS -- 安抚 stops them
        // attacking, it does not make them invulnerable. Excluding them here
        // left a crowd of unkillable creatures wandering the arena, since the
        // auto-attack picks its target with this function.
        if (!m->alive || m->recruited)
            continue;
        int32_t mx, my;
        monster_center(m, &mx, &my);
        int32_t d = dist2(px, py, mx, my);
        if (d <= bestd)
        {
            bestd = d;
            best = i;
            if (out_x) *out_x = mx;
            if (out_y) *out_y = my;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Monster AI
// ---------------------------------------------------------------------------

static void monster_move_toward(Monster *m, int32_t tx, int32_t ty, int speed)
{
    int size = monster_size(m);
    int32_t cx, cy;
    monster_center(m, &cx, &cy);

    int dx = 0, dy = 0;
    if (iabs(tx - cx) > 2)
        dx = (tx > cx) ? speed : -speed;
    if (iabs(ty - cy) > 2)
        dy = (ty > cy) ? speed : -speed;

    // Monsters use a smaller box than their sprite so they fit through gaps.
    int box = size - 12;
    int ox = 6, oy = 6;

    if (dx != 0 && !collision_rect(m->x + ox + dx, m->y + oy, box, box))
    {
        m->x += dx;
        m->dx = dx < 0 ? -1 : 1;
    }
    else if (dx != 0 && dy == 0)
    {
        dy = (my_rand() & 1) ? speed : -speed;
    }
    if (dy != 0 && !collision_rect(m->x + ox, m->y + oy + dy, box, box))
    {
        m->y += dy;
        m->dy = dy < 0 ? -1 : 1;
    }
}

// A recruited ally picks the nearest hostile monster and bites it.
static void ally_update(Monster *m)
{
    int best = -1;
    int32_t bestd = 0;
    int32_t cx, cy;
    monster_center(m, &cx, &cy);

    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        Monster *o = &g_monsters[i];
        if (!o->alive || o == m || o->recruited)
            continue;
        int32_t ox, oy;
        monster_center(o, &ox, &oy);
        int32_t d = dist2(cx, cy, ox, oy);
        if (best < 0 || d < bestd)
        {
            best = i;
            bestd = d;
        }
    }
    if (best < 0)
        return;

    Monster *t = &g_monsters[best];
    int32_t tx, ty;
    monster_center(t, &tx, &ty);
    monster_move_toward(m, tx, ty, 1);

    int touch = monster_size(m) / 2 + 8;
    if (dist2(cx, cy, tx, ty) < touch * touch && m->attack_timer == 0)
    {
        damage_monster(t, 6, cx, cy, true);
        m->attack_timer = 30;
    }
}

static void damage_around_heroes(int32_t cx, int32_t cy, int radius, int dmg)
{
    int32_t r2 = (int32_t)radius * radius;
    for (int i = 0; i < 2; i++)
    {
        if (!g_heroes[i].active)
            continue;
        int32_t hx, hy;
        hero_center(&g_heroes[i], &hx, &hy);
        if (dist2(hx, hy, cx, cy) <= r2)
            hero_take_damage(i, dmg, cx, cy);
    }
}

static void execute_boss_skill(Monster *m, int32_t cx, int32_t cy)
{
    int b = m->boss_id;
    int32_t tx = m->boss_tx, ty = m->boss_ty;
    int dx = tx - cx, dy = ty - cy;
    int len = (iabs(dx) > iabs(dy)) ? iabs(dx) : iabs(dy);
    len = len ? len : 1;

    switch (b)
    {
    case 0: // 雨林巨猿 (Rainforest Ape)
        if (m->boss_skill == 1) // 撼地重砸
        {
            spawn_effect(FX_RAM_DUST, cx, cy, 20, 65);
            damage_around_heroes(cx, cy, 65, 1);
            g_screen_shake = 4;
            audio_play_sfx(SND_EXPLOSION);
        }
        else if (m->boss_skill == 2) // 狂怒飞扑
        {
            m->x = tx - 16;
            m->y = ty - 16;
            spawn_effect(FX_MUD, tx, ty, 60, 26);
            damage_around_heroes(tx, ty, 36, 1);
            g_screen_shake = 3;
            audio_play_sfx(SND_EXPLOSION);
        }
        else // 野果投掷
        {
            spawn_projectile(PROJ_ENEMY, cx, cy, (int16_t)((dx * 4) / len), (int16_t)((dy * 4) / len), 90, 1, 5);
            spawn_projectile(PROJ_ENEMY, cx, cy, (int16_t)(((dx + dy/3) * 4) / len), (int16_t)(((dy - dx/3) * 4) / len), 90, 1, 5);
            spawn_projectile(PROJ_ENEMY, cx, cy, (int16_t)(((dx - dy/3) * 4) / len), (int16_t)(((dy + dx/3) * 4) / len), 90, 1, 5);
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        break;

    case 1: // 树精长老 (Treant Elder)
        if (m->boss_skill == 1) // 飞叶旋风
        {
            int sp = 3;
            int dirs[8][2] = {{sp,0},{-sp,0},{0,sp},{0,-sp},{sp*2/3,sp*2/3},{-sp*2/3,sp*2/3},{sp*2/3,-sp*2/3},{-sp*2/3,-sp*2/3}};
            for (int k = 0; k < 8; k++)
                spawn_projectile(PROJ_ENEMY, cx, cy, dirs[k][0], dirs[k][1], 80, 1, 4);
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        else if (m->boss_skill == 2) // 地刺树根
        {
            spawn_effect(FX_BITE, tx, ty, 16, 20);
            damage_around_heroes(tx, ty, 28, 1);
            audio_play_sfx(SND_ATTACK);
        }
        else // 爆裂毒果
        {
            spawn_effect(FX_STINK, tx, ty, 90, 24);
            damage_around_heroes(tx, ty, 26, 1);
            audio_play_sfx(SND_EXPLOSION);
        }
        break;

    case 2: // 狂暴野猪 (Raging Boar)
        if (m->boss_skill == 1) // 麦浪狂冲
        {
            int dash_x = (dx * 50) / len;
            int dash_y = (dy * 50) / len;
            m->x += dash_x;
            m->y += dash_y;
            spawn_effect(FX_RAM_DUST, cx, cy, 20, 25);
            damage_around_heroes(m->x + 16, m->y + 16, 34, 1);
            g_screen_shake = 3;
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 2) // 獠牙挑飞
        {
            spawn_effect(FX_TAIL, cx, cy, 14, 30);
            damage_around_heroes(cx, cy, 32, 1);
            audio_play_sfx(SND_ATTACK);
        }
        else // 掘地飞泥
        {
            spawn_projectile(PROJ_MUD, cx, cy, (int16_t)((-dx * 3) / len), (int16_t)((-dy * 3) / len), 70, 1, 6);
            spawn_projectile(PROJ_MUD, cx, cy, (int16_t)((dy * 3) / len), (int16_t)((-dx * 3) / len), 70, 1, 6);
            spawn_projectile(PROJ_MUD, cx, cy, (int16_t)((-dy * 3) / len), (int16_t)((dx * 3) / len), 70, 1, 6);
            audio_play_sfx(SND_HIT);
        }
        break;

    case 3: // 硕鼠大王 (Rat King)
        if (m->boss_skill == 1) // 鼠群召集
        {
            spawn_monster(3, cx + 18, cy, false, 0);
            spawn_monster(3, cx - 18, cy, false, 0);
            audio_play_sfx(SND_PICKUP);
        }
        else if (m->boss_skill == 2) // 金麦圣光
        {
            for (int k = 1; k <= 3; k++)
                spawn_projectile(PROJ_ENEMY, cx + (dx * k * 8) / len, cy + (dy * k * 8) / len, (int16_t)((dx * 5) / len), (int16_t)((dy * 5) / len), 80, 1, 4);
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        else // 巨尾重扫
        {
            spawn_effect(FX_TAIL, cx, cy, 16, 44);
            damage_around_heroes(cx, cy, 44, 1);
            audio_play_sfx(SND_ATTACK);
        }
        break;

    case 4: // 夜狼领袖 (Night Wolf Leader)
        if (m->boss_skill == 1) // 残影双连突
        {
            m->x += (dx * 45) / len;
            m->y += (dy * 45) / len;
            spawn_effect(FX_RAM_DUST, cx, cy, 14, 20);
            damage_around_heroes(m->x + 16, m->y + 16, 28, 1);
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 2) // 苍狼长啸
        {
            spawn_effect(FX_PACIFY, cx, cy, 20, 60);
            audio_play_sfx(SND_HURT);
        }
        else // 霜月爪刃
        {
            spawn_projectile(PROJ_ENEMY, cx, cy, (int16_t)((dx * 4) / len), (int16_t)((dy * 4) / len), 85, 1, 5);
            spawn_projectile(PROJ_ENEMY, cx, cy, (int16_t)(((dx + dy/2) * 4) / len), (int16_t)(((dy - dx/2) * 4) / len), 85, 1, 4);
            spawn_projectile(PROJ_ENEMY, cx, cy, (int16_t)(((dx - dy/2) * 4) / len), (int16_t)(((dy + dx/2) * 4) / len), 85, 1, 4);
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        break;

    case 5: // 狂暴黑牛 (Raging Bull)
        if (m->boss_skill == 1) // 狂牛冲锋
        {
            m->x += (dx * 60) / len;
            m->y += (dy * 60) / len;
            spawn_effect(FX_RAM_DUST, cx, cy, 24, 30);
            damage_around_heroes(m->x + 16, m->y + 16, 36, 1);
            g_screen_shake = 4;
            audio_play_sfx(SND_EXPLOSION);
        }
        else if (m->boss_skill == 2) // 巨蹄震荡
        {
            spawn_effect(FX_RAM_DUST, cx, cy, 20, 70);
            damage_around_heroes(cx, cy, 70, 1);
            g_screen_shake = 3;
            audio_play_sfx(SND_EXPLOSION);
        }
        else // 巨角旋风
        {
            spawn_effect(FX_TAIL, cx, cy, 16, 42);
            damage_around_heroes(cx, cy, 42, 1);
            audio_play_sfx(SND_ATTACK);
        }
        break;

    case 6: // 泥沼巨蟾 (Swamp Giant Toad)
        if (m->boss_skill == 1) // 毒泥喷射
        {
            spawn_projectile(PROJ_ENEMY, cx, cy, (int16_t)((dx * 3) / len), (int16_t)((dy * 3) / len), 90, 1, 6);
            spawn_projectile(PROJ_ENEMY, cx, cy, (int16_t)(((dx + dy/3) * 3) / len), (int16_t)(((dy - dx/3) * 3) / len), 90, 1, 5);
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        else if (m->boss_skill == 2) // 巨蟾压顶
        {
            m->x = tx - 16;
            m->y = ty - 16;
            spawn_effect(FX_MUD, tx, ty, 70, 30);
            damage_around_heroes(tx, ty, 38, 1);
            g_screen_shake = 4;
            audio_play_sfx(SND_EXPLOSION);
        }
        else // 毒瘴弥漫
        {
            spawn_effect(FX_STINK, cx, cy, 100, 36);
            damage_around_heroes(cx, cy, 36, 1);
            audio_play_sfx(SND_HIT);
        }
        break;

    case 7: // 凯门鳄王 (Caiman King)
        if (m->boss_skill == 1) // 死亡翻滚
        {
            m->x += (dx * 50) / len;
            m->y += (dy * 50) / len;
            spawn_effect(FX_BITE, m->x + 16, m->y + 16, 14, 28);
            damage_around_heroes(m->x + 16, m->y + 16, 36, 1);
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 2) // 鳄尾掀浪
        {
            spawn_projectile(PROJ_WATER, cx, cy, (int16_t)((dx * 4) / len), (int16_t)((dy * 4) / len), 70, 1, 8);
            spawn_projectile(PROJ_WATER, cx, cy, (int16_t)(((dx + dy/3) * 4) / len), (int16_t)(((dy - dx/3) * 4) / len), 70, 1, 7);
            spawn_projectile(PROJ_WATER, cx, cy, (int16_t)(((dx - dy/3) * 4) / len), (int16_t)(((dy + dx/3) * 4) / len), 70, 1, 7);
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        else // 泥潭潜伏
        {
            m->x = tx + (dx < 0 ? 30 : -30);
            m->y = ty;
            spawn_effect(FX_MUD, m->x + 16, m->y + 16, 50, 24);
            damage_around_heroes(m->x + 16, m->y + 16, 30, 1);
            audio_play_sfx(SND_ATTACK);
        }
        break;

    case 8: // 焦糖熔岩巨兽 (Caramel Lava Golem)
        if (m->boss_skill == 1) // 熔岩重拳
        {
            spawn_effect(FX_RAM_DUST, cx, cy, 20, 50);
            damage_around_heroes(cx, cy, 50, 1);
            g_screen_shake = 4;
            audio_play_sfx(SND_EXPLOSION);
        }
        else if (m->boss_skill == 2) // 地裂火波
        {
            for (int k = 1; k <= 3; k++)
            {
                int fx = cx + (dx * k * 14) / len;
                int fy = cy + (dy * k * 14) / len;
                spawn_effect(FX_BITE, fx, fy, 16, 14);
                damage_around_heroes(fx, fy, 20, 1);
            }
            audio_play_sfx(SND_EXPLOSION);
        }
        else // 天降火雨
        {
            for (int k = -1; k <= 1; k++)
                spawn_projectile(PROJ_ENEMY, tx + k * 20, ty - 40, 0, 4, 40, 1, 6);
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        break;

    case 9: // 蛋糕魔王 (Cake Lord)
        if (m->boss_skill == 1) // 八方樱桃炸弹
        {
            int sp = 3;
            int dirs[8][2] = {{sp,0},{-sp,0},{0,sp},{0,-sp},{sp*2/3,sp*2/3},{-sp*2/3,sp*2/3},{sp*2/3,-sp*2/3},{-sp*2/3,-sp*2/3}};
            for (int k = 0; k < 8; k++)
                spawn_projectile(PROJ_ENEMY, cx, cy, dirs[k][0], dirs[k][1], 75, 1, 5);
            audio_play_sfx(SND_EXPLOSION);
        }
        else if (m->boss_skill == 2) // 高压奶油光束
        {
            for (int k = 0; k < 4; k++)
                spawn_projectile(PROJ_ENEMY, cx, cy, (int16_t)(((dx + k * 4) * 4) / len), (int16_t)(((dy + k * 4) * 4) / len), 80, 1, 5);
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        else // 恶魔蛋糕下砸
        {
            spawn_effect(FX_RAM_DUST, cx, cy, 26, 80);
            damage_around_heroes(cx, cy, 80, 1);
            g_screen_shake = 6;
            audio_play_sfx(SND_EXPLOSION);
        }
        break;
    }
}

static void boss_update(Monster *m, int32_t cx, int32_t cy, int32_t hx, int32_t hy, int speed, int hi, int touch)
{
    if (m->boss_skill_cd > 0)
        m->boss_skill_cd--;

    if (m->boss_skill > 0)
    {
        if (m->boss_skill_timer > 0)
            m->boss_skill_timer--;

        if (m->boss_skill_timer == 20)
            execute_boss_skill(m, cx, cy);

        if (m->boss_skill_timer == 0)
        {
            m->boss_skill = 0;
            m->boss_skill_cd = (uint16_t)(60 + (my_rand() % 60));
        }
        return;
    }

    monster_move_toward(m, hx, hy, speed);

    if (m->boss_skill_cd == 0)
    {
        m->boss_skill = (uint8_t)(1 + (my_rand() % 3));
        m->boss_skill_timer = 40;
        m->boss_tx = hx;
        m->boss_ty = hy;
        audio_play_sfx(SND_UI_MOVE);
    }

    if (dist2(cx, cy, hx, hy) < touch * touch && m->attack_timer == 0)
    {
        hero_take_damage(hi, 1, cx, cy);
        m->attack_timer = 45;
    }
}

static void monster_update(int index)
{
    Monster *m = &g_monsters[index];
    if (!m->alive)
        return;

    if (m->hurt_timer > 0) m->hurt_timer--;
    if (m->attack_timer > 0) m->attack_timer--;
    if (m->state_timer > 0) m->state_timer--;
    if (m->slow_timer > 0) m->slow_timer--;
    if (m->pacify_timer > 0) m->pacify_timer--;
    if (m->rot_timer > 0)
    {
        m->rot_timer--;
        // Rotting damage, applied on a slow tick so it reads as a status.
        if ((m->rot_timer % 24) == 0)
            damage_monster(m, 3, m->x, m->y, false);
    }
    m->anim++;

    if (m->recruited)
    {
        ally_update(m);
        return;
    }

    // Standing in mud stops movement for as long as the pool is underfoot.
    int speed = (m->slow_timer > 0) ? 0 : 1;

    int32_t cx, cy;
    monster_center(m, &cx, &cy);

    // A pacified monster wanders instead of hunting: it stays on the field, so
    // 安抚 is not just a worse kill card, but it stops being a threat.
    if (m->pacify_timer > 0)
    {
        if ((m->anim % 40) == 0 || (m->dx == 0 && m->dy == 0))
        {
            m->dx = (int8_t)((int)(my_rand() % 3) - 1);
            m->dy = (int8_t)((int)(my_rand() % 3) - 1);
        }
        int size = monster_size(m);
        int nx = m->x + m->dx * 2;
        int ny = m->y + m->dy * 2;
        if (!collision_rect(nx + 6, ny + 6, size - 12, size - 12))
        {
            m->x = nx;
            m->y = ny;
        }
        else
        {
            m->dx = (int8_t)(-(int)m->dx);
            m->dy = (int8_t)(-(int)m->dy);
        }
        return;
    }

    // Stunned by 铁尾's ultimate.
    if (m->state == 1 && m->state_timer > 0)
        return;

    int hi = nearest_hero(cx, cy, NULL);
    if (hi < 0)
        return;
    int32_t hx, hy;
    hero_center(&g_heroes[hi], &hx, &hy);

    int size = monster_size(m);
    int touch = (size / 2) + 8;

    if (m->is_boss)
    {
        boss_update(m, cx, cy, hx, hy, speed, hi, touch);
        return;
    }

    switch (monster_def(m->def_id)->ai)
    {
    case AI_CHASER:
    {
        if (dist2(cx, cy, hx, hy) < 200 * 200)
            monster_move_toward(m, hx, hy, speed);

        if (dist2(cx, cy, hx, hy) < touch * touch && m->attack_timer == 0)
        {
            hero_take_damage(hi, 1, cx, cy);
            m->attack_timer = 45;
        }
        break;
    }
    case AI_DASHER:
    {
        if (m->state == 0)
        {
            monster_move_toward(m, hx, hy, speed);
            if (m->state_timer == 0 && dist2(cx, cy, hx, hy) < 90 * 90)
            {
                m->state = 2;
                m->state_timer = 40;   // wind-up before the lunge
            }
        }
        else
        {
            if (m->state_timer > 20)
            {
                // wind-up: face the target, don't move
            }
            else if (m->state_timer > 0)
            {
                int32_t dx = hx - cx, dy = hy - cy;
                int len = (iabs(dx) > iabs(dy)) ? iabs(dx) : iabs(dy);
                len = len ? len : 1;
                int mx = (dx * 4) / len;
                int my = (dy * 4) / len;
                if (!collision_rect(m->x + 6 + mx, m->y + 6 + my,
                                    size - 12, size - 12))
                {
                    m->x += mx;
                    m->y += my;
                }
                m->dx = mx < 0 ? -1 : 1;
            }
            else
            {
                m->state = 0;
                m->state_timer = 60 + (my_rand() % 60);
            }

            if (dist2(cx, cy, hx, hy) < touch * touch && m->attack_timer == 0)
            {
                hero_take_damage(hi, 1, cx, cy);
                m->attack_timer = 45;
            }
        }
        break;
    }
    case AI_SHOOTER:
    {
        int32_t d2 = dist2(cx, cy, hx, hy);
        // Keep a comfortable distance: retreat when too close, close in when
        // too far, fire on a timer that shortens as difficulty rises.
        if (d2 < 60 * 60)
            monster_move_toward(m, cx * 2 - hx, cy * 2 - hy, 1);
        else if (d2 > 100 * 100)
            monster_move_toward(m, hx, hy, 1);

        if (m->attack_timer == 0 && d2 < 140 * 140)
        {
            int32_t dx = hx - cx, dy = hy - cy;
            int len = (iabs(dx) > iabs(dy)) ? iabs(dx) : iabs(dy);
            len = len ? len : 1;
            spawn_projectile(PROJ_ENEMY, cx, cy,
                             (int16_t)((dx * 3) / len),
                             (int16_t)((dy * 3) / len), 110, 1, 4);
            int cd = 90 - g_difficulty * 5;
            m->attack_timer = (uint16_t)(cd < 30 ? 30 : cd);
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        break;
    }
    case AI_WANDER:
    case AI_ALLY:
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Projectiles
// ---------------------------------------------------------------------------

static void projectile_update(void)
{
    for (int i = 0; i < MAX_PROJECTILES; i++)
    {
        Projectile *p = &g_projectiles[i];
        if (!p->alive)
            continue;

        p->x += p->vx;
        p->y += p->vy;

        if (p->life > 0)
            p->life--;
        if (p->life == 0)
        {
            p->alive = false;
            continue;
        }

        // Terrain collision.
        if (collision_at(p->x, p->y))
        {
            if (p->kind == PROJ_DUNG)
            {
                spawn_effect(FX_STINK, p->x, p->y, 80, (uint8_t)p->radius);
                damage_around(p->x, p->y, p->radius, p->power, true);
                audio_play_sfx(SND_HIT);
            }
            else if (p->kind == PROJ_WATER)
            {
                spawn_effect(FX_MUD, p->x, p->y, 40, 14);
            }
            p->alive = false;
            continue;
        }

        if (p->kind == PROJ_ENEMY)
        {
            for (int h = 0; h < 2; h++)
            {
                Hero *hero = &g_heroes[h];
                if (!hero->active)
                    continue;
                int32_t cx, cy;
                hero_center(hero, &cx, &cy);
                if (dist2(p->x, p->y, cx, cy) < 10 * 10)
                {
                    hero_take_damage(h, 1, p->x, p->y);
                    p->alive = false;
                    break;
                }
            }
            continue;
        }

        // Capybara projectiles hit hostile monsters.
        for (int m = 0; m < MAX_MONSTERS; m++)
        {
            Monster *mo = &g_monsters[m];
            if (!mo->alive || mo->recruited)
                continue;
            int32_t cx, cy;
            monster_center(mo, &cx, &cy);
            int hit_r = monster_size(mo) / 2 + p->radius;
            if (dist2(p->x, p->y, cx, cy) >= hit_r * hit_r)
                continue;

            if (p->kind == PROJ_DUNG)
            {
                // Dung bursts on the first thing it hits, and the burst is
                // bigger than the ball.
                spawn_effect(FX_STINK, p->x, p->y, 80, (uint8_t)p->radius);
                damage_around(p->x, p->y, p->radius, p->power, true);
                // Ultimate: the cloud leaves what it touches rotting.
                if (card_level(&g_heroes[g_local_hero], CARD_DUNG)
                    >= CARD_MAX_LEVEL)
                {
                    for (int k = 0; k < MAX_MONSTERS; k++)
                    {
                        Monster *o = &g_monsters[k];
                        if (!o->alive || o->recruited)
                            continue;
                        int32_t ox, oy;
                        monster_center(o, &ox, &oy);
                        if (in_radius(ox, oy, p->x, p->y, p->radius + 22))
                            o->rot_timer = 90;
                    }
                }
                p->alive = false;
            }
            else
            {
                damage_monster(mo, p->power, p->x, p->y, true);
                // The water jet pierces everything; that is its identity.
                if (p->kind != PROJ_WATER)
                    p->alive = false;
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Effects: the mud pool and the stink cloud are gameplay, not decoration
// ---------------------------------------------------------------------------

static void effect_update(void)
{
    for (int i = 0; i < MAX_EFFECTS; i++)
    {
        Effect *e = &g_effects[i];
        if (!e->alive)
            continue;

        if (e->life > 0)
            e->life--;
        if (e->life == 0)
        {
            e->alive = false;
            continue;
        }

        if (e->kind == FX_MUD)
        {
            // Mud stops whatever stands in it, and at the card's ultimate it
            // rots them as well.
            int lvl = card_level(&g_heroes[g_local_hero], CARD_MUD);
            for (int m = 0; m < MAX_MONSTERS; m++)
            {
                Monster *mo = &g_monsters[m];
                if (!mo->alive || mo->recruited)
                    continue;
                int32_t mx, my;
                monster_center(mo, &mx, &my);
                if (in_radius(mx, my, e->x, e->y, e->radius))
                {
                    mo->slow_timer = 6;
                    if (lvl >= CARD_MAX_LEVEL)
                        mo->rot_timer = 40;
                }
            }
        }
        else if (e->kind == FX_STINK)
        {
            // The cloud keeps damaging while it lasts.
            if ((e->life % 20) == 0)
                damage_around(e->x, e->y, e->radius, 4, false);
        }
        else if (e->kind == FX_DUNG_TRAP)
        {
            // 地面便便陷阱：敌人踩到后受到中毒伤害并减速，随后炸开成恶臭毒雾
            for (int m = 0; m < MAX_MONSTERS; m++)
            {
                Monster *mo = &g_monsters[m];
                if (!mo->alive || mo->recruited)
                    continue;
                int32_t mx, my;
                monster_center(mo, &mx, &my);
                if (in_radius(mx, my, e->x, e->y, e->radius))
                {
                    int dmg = e->power > 0 ? e->power : 12;
                    damage_monster(mo, dmg, e->x, e->y, true);
                    mo->rot_timer = 100; // 中毒
                    mo->slow_timer = 90; // 减速
                    spawn_effect(FX_STINK, e->x, e->y, 60, (uint8_t)(e->radius + 6));
                    audio_play_sfx(SND_EXPLOSION);
                    e->alive = false;
                    break;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Pickups
// ---------------------------------------------------------------------------

static void pickup_update(void)
{
    for (int i = 0; i < MAX_GEMS; i++)
    {
        Gem *g = &g_gems[i];
        if (!g->alive)
            continue;

        g->bob++;

        for (int h = 0; h < 2; h++)
        {
            Hero *hero = &g_heroes[h];
            if (!hero->active || hero->remote)
                continue;

            int32_t cx, cy;
            hero_center(hero, &cx, &cy);
            int32_t r = hero->pickup_r;
            int32_t d2 = dist2(g->x, g->y, cx, cy);

            if (d2 >= r * r)
                continue;

            // 磁吸 pulls the mote in rather than requiring an exact walk-over.
            if (d2 > 16)
            {
                int32_t dx = cx - g->x;
                int32_t dy = cy - g->y;
                int len = (iabs(dx) > iabs(dy)) ? iabs(dx) : iabs(dy);
                if (len > 0)
                {
                    g->x += (dx / len) * 2;
                    g->y += (dy / len) * 2;
                }
            }

            if (d2 < 12 * 12)
            {
                g->alive = false;
                if (g->kind == 0)
                {
                    hero_gain_exp(h, g->value);
                    if (h == g_local_hero)
                        audio_play_sfx(SND_PICKUP);
                }
                else if (g->kind == 1)
                {
                    // An ingredient: this is what summons the region boss.
                    if (g_ingredient_have < 255)
                        g_ingredient_have++;
                    if (h == g_local_hero)
                        audio_play_sfx(SND_HEART);
                }
                else if (g->kind == 2)
                {
                    // 霸主印记：用于解锁被动技能树
                    g_boss_tokens += g->value;
                    if (h == g_local_hero)
                        audio_play_sfx(SND_LEVELUP);
                }
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Spawning
// ---------------------------------------------------------------------------

static bool spot_free(int32_t x, int32_t y, int size)
{
    return !collision_rect(x + 6, y + 6, size - 12, size - 12);
}

static void spawn_monster(uint8_t def_id, int32_t x, int32_t y, bool boss,
                          uint8_t boss_id)
{
    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        Monster *m = &g_monsters[i];
        if (m->alive)
            continue;

        memset(m, 0, sizeof(*m));
        m->alive = true;
        m->def_id = def_id;
        m->x = x;
        m->y = y;
        m->is_boss = boss;
        m->boss_id = boss_id;
        m->dx = 1;

        // Monsters scale with elapsed difficulty, not with the character's
        // level: this is an endless run, so the clock is what eventually wins.
        if (boss)
        {
            m->hp_max = (uint16_t)(160 + g_difficulty * 70 + g_region_id * 60);
            m->radius = monster_draw_radius(m);
        }
        else
        {
            m->hp_max = (uint16_t)(10 + g_difficulty * 14 + g_region_id * 8);
            m->radius = monster_draw_radius(m);
        }
        m->hp = m->hp_max;
        return;
    }
}

// 16 evenly spaced unit directions, scaled by 64. A table rather than a
// sine call: the spawn ring only needs "roughly evenly spread", and this keeps
// floating point out of the simulation.
static const int8_t g_dir16_x[16] = {
     64,  59,  45,  24,   0, -24, -45, -59,
    -64, -59, -45, -24,   0,  24,  45,  59,
};
static const int8_t g_dir16_y[16] = {
      0,  24,  45,  59,  64,  59,  45,  24,
      0, -24, -45, -59, -64, -59, -45, -24,
};

static void spawn_at_edge(uint8_t def_id)
{
    // Spawn on a ring around the local character rather than at the edge of a
    // fixed screen.
    //
    // The arena used to be exactly one screen, so "spawn at the screen edge"
    // was a meaningful instruction -- every spawn was guaranteed to be just
    // outside the view and would walk in. With a world that is 8x8 screens,
    // the edge of the world is two thousand pixels away, so that rule would
    // mean the player never sees an enemy again. The equivalent rule now is a
    // ring just outside the CAMERA, centred on the player whose screen has to
    // stay busy.
    int32_t px = WORLD_W / 2, py = WORLD_H / 2;
    if (g_heroes[g_local_hero].active)
        hero_center(&g_heroes[g_local_hero], &px, &py);
    else if (g_heroes[0].active)
        hero_center(&g_heroes[0], &px, &py);

    // Just off the short edge of the viewport, so a spawn is never visible.
    int rmin = AREA_H / 2 + 10;
    int rmax = AREA_H / 2 + 60;

    for (int tries = 0; tries < 30; tries++)
    {
        int d = (int)(my_rand() % 16);
        int r = rmin + (int)(my_rand() % (rmax - rmin));
        int32_t x = px + (int32_t)g_dir16_x[d] * r / 64;
        int32_t y = py + (int32_t)g_dir16_y[d] * r / 64;

        // Keep the ring inside the world; the border is solid anyway, so a
        // spawn outside it would simply never arrive.
        if (x < 40 || y < 40 || x > WORLD_W - 40 || y > WORLD_H - 40)
            continue;

        if (spot_free(x, y, 28))
        {
            spawn_monster(def_id, x, y, false, 0);
            return;
        }
    }
}

static int alive_hostile_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_MONSTERS; i++)
        if (monster_is_hostile(&g_monsters[i]))
            n++;
    return n;
}

// ---------------------------------------------------------------------------
// Region flow
// ---------------------------------------------------------------------------

// Frames until the next spawn batch. A file-scope variable rather than a
// function-static so game_start_region can reset it: a leftover countdown from
// the previous region would otherwise carry into the next one.
static uint16_t g_spawn_timer = 0;

void game_start_region(int region_index)
{
    if (region_index < 0 || region_index >= INGREDIENT_COUNT)
        return;

    const RegionDef *rg = &g_regions[region_index];

    g_region_id = (uint8_t)region_index;
    g_ingredient_have = 0;
    g_mid_boss_out = false;
    g_boss_out = false;
    g_run_time = 0;
    g_difficulty = 0;
    g_spawn_timer = 30;   // a moment of calm before the first wave

    memset(g_monsters, 0, sizeof(g_monsters));
    memset(g_projectiles, 0, sizeof(g_projectiles));
    memset(g_gems, 0, sizeof(g_gems));
    memset(g_effects, 0, sizeof(g_effects));

    // Bind the region's world, then put everyone at the middle of it. The
    // spawn clearing is kept walkable by the generator, so no collision test
    // is needed here -- but the camera is snapped, not lerped, so the first
    // frame of a region never shows the previous region's corner.
    const uint8_t *atlas = area_atlas(rg->area_id);
    const uint8_t *map = area_map(rg->area_id);
    const uint8_t *prop = area_prop(rg->area_id);
    render_set_world(atlas, map, prop);

    int32_t cx = WORLD_W / 2;
    int32_t cy = WORLD_H / 2;

    for (int i = 0; i < 2; i++)
    {
        Hero *h = &g_heroes[i];
        if (!h->active)
            continue;
        h->x = cx - HERO_SPRITE_W / 2 + (i ? 40 : -40);
        h->y = cy - HERO_SPRITE_H / 2;
        h->facing = DIR_UP;
        h->invuln = 90;
        h->ram_timer = 0;
        h->hurt_stun = 0;
        h->bite_timer = 0;
        h->recruit_charge = 0;
        for (int c = 0; c < CARD_COUNT; c++)
            h->cooldown[c] = 0;
        h->net_x = h->x;
        h->net_y = h->y;
        hero_recompute_stats(i);
    }

    // The camera follows the local character; centring it here is what makes
    // the region open on the player instead of at a world corner.
    Hero *local = &g_heroes[g_local_hero];
    if (local->active)
    {
        int32_t hx, hy;
        hero_center(local, &hx, &hy);
        render_camera_center_on(hx, hy);
    }
    else
    {
        render_camera_center_on(cx, cy);
    }

    audio_play_music(MUS_BATTLE);
}

// The spawn interval tightens with difficulty; this is the pressure valve of an
// endless run.
static uint16_t spawn_interval(void)
{
    int iv = SPAWN_BASE -
             g_difficulty * (SPAWN_BASE - SPAWN_MIN) / DIFFICULTY_MAX;
    if (iv < SPAWN_MIN)
        iv = SPAWN_MIN;
    return (uint16_t)iv;
}

static void difficulty_update(void)
{
    uint8_t d = (uint8_t)(g_run_time / DIFFICULTY_STEP);
    if (d > DIFFICULTY_MAX)
        d = DIFFICULTY_MAX;
    if (d != g_difficulty)
    {
        g_difficulty = d;
        if (d == DIFFICULTY_MAX)
            audio_play_sfx(SND_EXPLOSION);   // the run has peaked; hold on
    }
}

static void spawn_update(void)
{
    const RegionDef *rg = &g_regions[g_region_id];

    // Phase 1: Mid-boss appears when half of the ingredient target is gathered
    if (!g_mid_boss_out && g_ingredient_have >= (rg->target / 2))
    {
        g_mid_boss_out = true;
        int32_t px = WORLD_W / 2, py = WORLD_H / 2;
        if (g_heroes[g_local_hero].active)
            hero_center(&g_heroes[g_local_hero], &px, &py);

        int32_t bx = px + (AREA_W / 2 + 40), by = py;
        if (bx > WORLD_W - 60) bx = px - (AREA_W / 2 + 40);
        if (bx < 60) bx = 60;
        if (by < 60) by = 60;
        if (by > WORLD_H - 60) by = WORLD_H - 60;

        spawn_monster(0, bx, by, true, rg->mid_boss_id);
        audio_play_music(MUS_BOSS);
    }

    // Phase 2: Final Boss appears once full ingredient target has been gathered
    if (!g_boss_out && g_ingredient_have >= rg->target)
    {
        g_boss_out = true;

        int32_t px = WORLD_W / 2, py = WORLD_H / 2;
        if (g_heroes[g_local_hero].active)
            hero_center(&g_heroes[g_local_hero], &px, &py);

        int32_t bx = px, by = py - (AREA_H / 2 + 40);
        if (bx < 60) bx = 60;
        if (by < 60) by = py + (AREA_H / 2 + 40);
        if (by > WORLD_H - 60) by = WORLD_H - 60;

        spawn_monster(0, bx, by, true, rg->boss_id);
        audio_play_music(MUS_BOSS);
    }

    if (g_spawn_timer > 0)
    {
        g_spawn_timer--;
        return;
    }
    g_spawn_timer = spawn_interval();

    // Progressive mob variety:
    // Difficulty 0-2: Chaser
    // Difficulty 3-5: Chaser + Dasher
    // Difficulty 6+: Full squad (Chaser + Dasher + Shooter)
    int max_choices = 1;
    if (g_difficulty >= 3) max_choices = 2;
    if (g_difficulty >= 6) max_choices = rg->mob_count;
    if (max_choices > rg->mob_count) max_choices = rg->mob_count;

    int group = 1 + g_difficulty / 3;
    for (int i = 0; i < group; i++)
    {
        if (alive_hostile_count() >= MAX_MONSTERS - 4)
            break;
        int choice = my_rand() % max_choices;
        uint8_t def = rg->mob_ids[choice];
        spawn_at_edge(def);
    }
}

// ---------------------------------------------------------------------------
// Bottom-screen HUD
// ---------------------------------------------------------------------------

static void draw_heart(int x, int y, uint8_t fill)
{
    sub_fill_rect(x, y + 1, 7, 5, PAL_OUTLINE);
    sub_fill_rect(x + 1, y, 5, 7, PAL_OUTLINE);
    sub_fill_rect(x + 1, y + 1, 5, 4, fill);
    sub_fill_rect(x + 2, y + 2, 2, 2, PAL_PINK_LIGHT);
}

// A minimap of the whole region on the bottom screen, with the player on it.
//
// With a world 8x8 screens in size, the player has no way to know where they
// are or where they have been without one -- and a survivors-like is played by
// kiting, which means deliberately running a long way. The map is drawn by
// sampling the tile map on a coarse grid: 128x96 tiles down to 64x40 cells, so
// one cell is two tiles. That is cheap enough to redraw every frame and detailed
// enough to show the shape of the terrain.
#define MINIMAP_X      152
#define MINIMAP_Y      72
#define MINIMAP_W      64
#define MINIMAP_H      40
#define MINIMAP_SX     2        // world tiles per minimap cell

static void draw_minimap(void)
{
    sub_rect_outline(MINIMAP_X - 1, MINIMAP_Y - 1, MINIMAP_W + 2, MINIMAP_H + 2,
                     PAL_UI_FRAME);
    sub_fill_rect(MINIMAP_X, MINIMAP_Y, MINIMAP_W, MINIMAP_H, PAL_UI_PANEL);

    const uint8_t *map = area_map(g_regions[g_region_id].area_id);
    const uint8_t *prop = area_prop(g_regions[g_region_id].area_id);
    if (!map || !prop)
        return;

    for (int cy = 0; cy < MINIMAP_H; cy++)
    {
        for (int cx = 0; cx < MINIMAP_W; cx++)
        {
            // Sample the top-left tile of each cell; a finer average would cost
            // more than the extra detail is worth at this size.
            int tx = cx * MINIMAP_SX;
            int ty = cy * (WORLD_ROWS / MINIMAP_H);
            if (tx >= WORLD_COLS || ty >= WORLD_ROWS)
                continue;
            unsigned id = map[ty * WORLD_COLS + tx];
            uint8_t col;
            if (id >= ATLAS_TILES)
                col = PAL_UI_PANEL;
            else if (prop[id])
                col = PAL_UI_TEXT_DIM;          // walls and props
            else if (id >= 14 && id <= 17)
                col = PAL_CYAN_DARK;            // water
            else if (id >= 12 && id <= 13)
                col = PAL_SAND_MID;             // paths
            else
                col = PAL_GRN_DARK;             // open ground
            sub_fill_rect(MINIMAP_X + cx, MINIMAP_Y + cy, 1, 1, col);
        }
    }

    // The player, as a bright dot. Clamped to the map frame so it stays visible
    // at the very edge of the world.
    if (g_heroes[g_local_hero].active)
    {
        int32_t hx, hy;
        hero_center(&g_heroes[g_local_hero], &hx, &hy);
        int mx = MINIMAP_X + (int)(hx / TILE_PX) / MINIMAP_SX;
        int my = MINIMAP_Y + (int)(hy / TILE_PX) /
                 (WORLD_ROWS / MINIMAP_H);
        if (mx < MINIMAP_X) mx = MINIMAP_X;
        if (mx > MINIMAP_X + MINIMAP_W - 2) mx = MINIMAP_X + MINIMAP_W - 2;
        if (my < MINIMAP_Y) my = MINIMAP_Y;
        if (my > MINIMAP_Y + MINIMAP_H - 2) my = MINIMAP_Y + MINIMAP_H - 2;
        sub_fill_rect(mx - 1, my - 1, 3, 3, PAL_WHITE_SPARK);
        sub_fill_rect(mx, my, 1, 1, PAL_HUD_HEART);
    }

    // The boss, when there is one, so the player can choose to engage or run.
    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        Monster *m = &g_monsters[i];
        if (!m->alive || !m->is_boss)
            continue;
        int mx = MINIMAP_X + (int)(m->x / TILE_PX) / MINIMAP_SX;
        int my = MINIMAP_Y + (int)(m->y / TILE_PX) / (WORLD_ROWS / MINIMAP_H);
        if (mx < MINIMAP_X) mx = MINIMAP_X;
        if (mx > MINIMAP_X + MINIMAP_W - 2) mx = MINIMAP_X + MINIMAP_W - 2;
        if (my < MINIMAP_Y) my = MINIMAP_Y;
        if (my > MINIMAP_Y + MINIMAP_H - 2) my = MINIMAP_Y + MINIMAP_H - 2;
        sub_fill_rect(mx - 1, my - 1, 3, 3, PAL_HUD_HEART);
        break;
    }

    text_draw(19, 5, "地图", PAL_UI_TEXT_DIM);
}

// Arrows at the screen edge pointing at anything important that is off screen.
//
// In a world this large, "where is the boss" is a question the player cannot
// answer from what is drawn, and the boss appearing is the event the whole
// region builds up to.
static void draw_offscreen_markers(void)
{
    int cam_x, cam_y;
    render_camera_get(&cam_x, &cam_y);

    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        Monster *m = &g_monsters[i];
        if (!m->alive || !m->is_boss)
            continue;

        int sx = m->x - cam_x;
        int sy = m->y - cam_y;
        if (sx >= 0 && sx < AREA_W && sy >= 0 && sy < AREA_H)
            return;   // it is on screen; nothing to point at

        // Clamp the marker onto the screen border, inset so the arrow is not
        // half off the edge.
        int mx = sx < 8 ? 8 : (sx > AREA_W - 8 ? AREA_W - 8 : sx);
        int my = sy < 8 ? 8 : (sy > AREA_H - 8 ? AREA_H - 8 : sy);

        gfx_fill_rect(mx - 5, my - 5, 10, 10, PAL_OUTLINE);
        gfx_fill_rect(mx - 4, my - 4, 8, 8, PAL_HUD_HEART);
        gfx_fill_rect(mx - 2, my - 2, 4, 4, PAL_WHITE_SPARK);
        return;
    }
}

static uint8_t card_kind_color(int kind)
{
    switch (kind)
    {
    case KIND_PHYS:    return PAL_ORANGE_MID;
    case KIND_MAGIC:   return PAL_CYAN_LIGHT;
    case KIND_SUPPORT: return PAL_GREEN_SPARK;
    default:           return PAL_UI_GOLD_LIGHT;
    }
}

// The bottom screen during play: hearts, level and exp, ingredient progress,
// and the cards currently held.
static void hud_draw(void)
{
    text_clear();
    text_panel(0, 0, 20, 2);

    Hero *h = &g_heroes[g_local_hero];
    const RegionDef *rg = &g_regions[g_region_id];

    // Region + elapsed survival time, which is the run's real score.
    text_printf(1, 0, PAL_UI_GOLD_LIGHT, "%s %d:%02d", rg->name,
                g_run_time / 3600, (g_run_time / 60) % 60);

    text_printf(12, 1, PAL_UI_TEXT_DIM, "难度%d", g_difficulty + 1);

    // Hearts, two rows of six so twelve still fit.
    for (int i = 0; i < h->hearts_max && i < HERO_MAX_HEARTS; i++)
    {
        uint8_t fill = (i < h->hearts) ? PAL_HUD_HEART : PAL_HUD_HEART_EMPTY;
        draw_heart(8 + (i % 6) * 9, 20 + (i / 6) * 9, fill);
    }

    text_printf(1, 3, PAL_UI_TEXT, "等级 %d", h->level);
    text_printf(9, 3, PAL_UI_TEXT_DIM, "坐标 %d,%d",
                (int)(h->x / TILE_PX), (int)(h->y / TILE_PX));

    // Experience bar.
    sub_rect_outline(58, 24, 76, 8, PAL_OUTLINE);
    sub_fill_rect(59, 25, 74, 6, PAL_UI_PANEL);
    if (h->exp_need > 0)
    {
        int w = (74 * h->exp) / h->exp_need;
        if (w > 74) w = 74;
        sub_fill_rect(59, 25, w, 6, PAL_GREEN_SPARK);
    }

    // Ingredient progress toward the boss.
    text_printf(1, 5, PAL_UI_TEXT_DIM, "收集%s", rg->ingredient);
    for (int i = 0; i < rg->target; i++)
    {
        uint8_t c = (i < g_ingredient_have) ? PAL_UI_GOLD : PAL_HUD_HEART_EMPTY;
        int gx = 62 + (i % 12) * 8;
        int gy = 40 + (i / 12) * 8;
        sub_fill_rect(gx, gy, 6, 6, c);
    }

    if (g_boss_out)
        text_draw(1, 7, "首领出现了！", PAL_HUD_HEART);

    // The map of the region, which is the only way to know where you are in a
    // world this size.
    draw_minimap();

    // Held cards, colour-coded by kind. Four per row, kept out of the map's
    // column so the two never overlap.
    text_draw(1, 9, "卡牌", PAL_UI_TEXT_DIM);
    int shown = 0;
    for (int i = 0; i < CARD_COUNT; i++)
    {
        if (h->cards[i] <= 0)
            continue;
        int col = 5 + (shown % 3) * 3;
        int row = 10 + (shown / 3);
        uint8_t c = card_kind_color(g_card_defs[i].kind);
        text_printf(col, row, c, "%s%d", g_card_defs[i].name, h->cards[i]);
        shown++;
    }
    if (shown == 0)
        text_draw(5, 10, "升级就能获得卡牌", PAL_UI_TEXT_DIM);

    // Active Skill Slots Touch Bar (y: 154..186)
    Hero *hl = &g_heroes[g_local_hero];
    static const char *s_hud_btn_names[4] = { "A", "B", "X", "Y" };
    int col_hud_step[4] = { 1, 6, 11, 16 };

    for (int s = 0; s < 4; s++)
    {
        int sx = 4 + s * 62;
        uint8_t cid = hl->skill_slots[s];
        bool has_sk = (cid != SKILL_SLOT_NONE && cid < CARD_COUNT);
        bool ready = has_sk && (hl->cooldown[cid] == 0) && (card_level(hl, cid) > 0 || cid == CARD_TEETH);

        sub_rect_outline(sx, 154, 58, 32, PAL_OUTLINE);
        sub_fill_rect(sx + 1, 155, 56, 30, ready ? PAL_UI_PANEL : PAL_GREY_DARK);

        if (has_sk)
        {
            uint8_t col = ready ? card_kind_color(g_card_defs[cid].kind) : PAL_UI_TEXT_DIM;
            text_printf(col_hud_step[s], 11, ready ? PAL_UI_GOLD_LIGHT : PAL_UI_TEXT_DIM, "%s键位", s_hud_btn_names[s]);
            text_printf(col_hud_step[s], 12, col, "%s", g_card_defs[cid].name);

            // Cooldown meter
            if (!ready && hl->cooldown[cid] > 0)
            {
                int max_cd = card_cd(hl, cid);
                if (max_cd > 0)
                {
                    int cd_w = (54 * (max_cd - hl->cooldown[cid])) / max_cd;
                    if (cd_w > 54) cd_w = 54;
                    sub_fill_rect(sx + 2, 182, cd_w, 2, PAL_CYAN_LIGHT);
                }
            }
        }
        else
        {
            text_printf(col_hud_step[s], 11, PAL_UI_TEXT_DIM, "%s键位", s_hud_btn_names[s]);
            text_printf(col_hud_step[s], 12, PAL_UI_TEXT_DIM, "空位");
        }
    }
}

// ---------------------------------------------------------------------------
// Frame entry points
// ---------------------------------------------------------------------------

void game_init(void)
{
    memset(g_heroes, 0, sizeof(g_heroes));
    memset(g_monsters, 0, sizeof(g_monsters));
    memset(g_projectiles, 0, sizeof(g_projectiles));
    memset(g_gems, 0, sizeof(g_gems));
    memset(g_effects, 0, sizeof(g_effects));

    for (int i = 0; i < 2; i++)
    {
        Hero *h = &g_heroes[i];
        h->level = 1;
        h->exp = 0;
        h->exp_need = 12;
        h->hearts_max = 5;
        h->hearts = 5;
        h->attack = 3;
        h->move_speed = HERO_BASE_SPEED;
        h->pickup_r = 28;
        for (int c = 0; c < CARD_COUNT; c++)
            h->cards[c] = 0;
        for (int s = 0; s < SKILL_SLOT_COUNT; s++)
            h->skill_slots[s] = SKILL_SLOT_NONE;
        h->skill_slots[SKILL_SLOT_A] = CARD_TEETH;
        h->active = false;
    }

    g_ingredients_done = 0;
    g_ingredient_have = 0;
    g_mid_boss_out = false;
    g_boss_out = false;
    g_run_time = 0;
    g_difficulty = 0;
}

void game_update(void)
{
    g_frame++;
    g_run_time++;

    difficulty_update();

    // Capybaras first: their new positions matter for monster targeting.
    hero_update(0);
    hero_update(1);

    // The host owns monster simulation; in single player the local console
    // always is the authority.
    if (!g_multiplayer || g_is_host)
    {
        spawn_update();
        for (int i = 0; i < MAX_MONSTERS; i++)
            monster_update(i);
        projectile_update();
        effect_update();
        pickup_update();
    }

    // Cards fire on their own timers, then the always-on bite. Both run for
    // every capybara this console simulates.
    hero_handle_active_skills(g_local_hero);
    hero_fire_cards(g_local_hero);
    if (!g_multiplayer && g_heroes[g_local_hero ^ 1].active)
        hero_fire_cards(g_local_hero ^ 1);

    if (g_heroes[g_local_hero].cooldown[CARD_TEETH] == 0)
        hero_auto_bite(g_local_hero);
    if (!g_multiplayer && g_heroes[g_local_hero ^ 1].active &&
        g_heroes[g_local_hero ^ 1].cooldown[CARD_TEETH] == 0)
        hero_auto_bite(g_local_hero ^ 1);

    // --- camera ---
    // Follow the local character, softened so the view does not jitter on a
    // single-frame twitch. The camera is centred on the player and then
    // clamped to the world by render_camera(), which is also what keeps the
    // viewport from showing anything outside the map.
    if (g_heroes[g_local_hero].active)
    {
        int32_t hx, hy;
        hero_center(&g_heroes[g_local_hero], &hx, &hy);

        int cam_x, cam_y;
        render_camera_get(&cam_x, &cam_y);
        int want_x = hx - AREA_W / 2;
        int want_y = hy - AREA_H / 2;
        // Ease toward the target; the divisor is the smoothing, and 2 keeps
        // the player near the centre without a visible lag.
        render_camera(cam_x + (want_x - cam_x) / 2,
                      cam_y + (want_y - cam_y) / 2);
    }

    // Composite the world. Only the visible tiles are drawn, so this cost is
    // the same whether the region is one screen or a hundred.
    const RegionDef *rg = &g_regions[g_region_id];
    render_set_world(area_atlas(rg->area_id), area_map(rg->area_id),
                     area_prop(rg->area_id));
    render_world();

    if (g_heroes[g_local_hero].active)
    {
        // Ground layer: mud pools, stink clouds and rings sit under everything.
        for (int i = 0; i < MAX_EFFECTS; i++)
        {
            Effect *e = &g_effects[i];
            if (!e->alive)
                continue;
            switch (e->kind)
            {
            case FX_MUD:
                gfx_circle(e->x, e->y, e->radius,
                           (e->life % 8 < 4) ? PAL_CLAY_DARK : PAL_CLAY_MID);
                gfx_circle(e->x, e->y, e->radius - 3, PAL_TRUNK_MID);
                break;
            case FX_STINK:
                gfx_circle(e->x, e->y, e->radius, PAL_PINE_DARK);
                gfx_circle(e->x, e->y, e->radius - 2, PAL_CACT_DARK);
                break;
            case FX_RAM_DUST:
                gfx_circle_outline(e->x, e->y, e->radius, PAL_SAND_DARK);
                break;
            case FX_PACIFY:
                gfx_circle_outline(e->x, e->y, e->radius, PAL_HEAL_INNER);
                break;
            case FX_DUNG_TRAP:
                gfx_circle(e->x, e->y, 4, PAL_CHOCO_DARK);
                gfx_circle(e->x, e->y, 3, PAL_CHOCO_MID);
                gfx_circle(e->x - 1, e->y - 1, 1, PAL_GREEN_SPARK);
                break;
            case FX_LEVELUP:
                gfx_circle_outline(e->x, e->y, e->radius, PAL_UI_GOLD_LIGHT);
                break;
            default:
                break;
            }
        }

        for (int i = 0; i < MAX_GEMS; i++)
            draw_gem(i);

        // The crowd, then the projectiles over it, then the characters on top
        // so the player is never hidden by their own fight.
        for (int i = 0; i < MAX_MONSTERS; i++)
            draw_monster(i);

        for (int i = 0; i < MAX_PROJECTILES; i++)
        {
            Projectile *p = &g_projectiles[i];
            if (!p->alive)
                continue;
            switch (p->kind)
            {
            case PROJ_WATER:
                gfx_circle(p->x, p->y, p->radius, PAL_CYAN_MID);
                gfx_circle(p->x, p->y,
                           p->radius > 3 ? p->radius - 3 : 1, PAL_CYAN_LIGHT);
                gfx_circle(p->x, p->y, 1, PAL_WHITE_SPARK);
                break;
            case PROJ_DUNG:
                gfx_circle(p->x, p->y, p->radius > 4 ? 4 : p->radius,
                           PAL_CHOCO_DARK);
                gfx_circle(p->x, p->y, 2, PAL_CHOCO_MID);
                break;
            case PROJ_ENEMY:
                gfx_fill_rect(p->x - 3, p->y - 3, 6, 6, PAL_PURPLE_DARK);
                gfx_fill_rect(p->x - 2, p->y - 2, 4, 4, PAL_PURPLE_MID);
                gfx_fill_rect(p->x - 1, p->y - 1, 2, 2, PAL_PURPLE_LIGHT);
                break;
            default:
                break;
            }
        }

        // The bite and tail effects are drawn over the crowd so they read.
        for (int i = 0; i < MAX_EFFECTS; i++)
        {
            Effect *e = &g_effects[i];
            if (!e->alive)
                continue;
            if (e->kind == FX_BITE)
            {
                gfx_circle(e->x, e->y, 4, PAL_WHITE_SPARK);
                gfx_circle_outline(e->x, e->y, 6, PAL_OUTLINE);
            }
            else if (e->kind == FX_TAIL)
            {
                gfx_circle_outline(e->x, e->y, e->radius, PAL_WHITE_SPARK);
                gfx_circle_outline(e->x, e->y, e->radius - 3, PAL_ARC_INNER);
            }
        }

        draw_hero(0);
        draw_hero(1);

        // Off-screen indicators: a boss that has appeared but is out of view
        // is otherwise invisible, and "the boss is somewhere over there" is
        // information the player cannot play without.
        draw_offscreen_markers();
    }

    hud_draw();
}


// ---------------------------------------------------------------------------
// Walkable Hub Town (水豚王国广场)
// ---------------------------------------------------------------------------

void game_start_hub(void)
{
    g_region_id = 0;
    g_mid_boss_out = false;
    g_boss_out = false;
    g_ingredient_have = 0;

    memset(g_monsters, 0, sizeof(g_monsters));
    memset(g_projectiles, 0, sizeof(g_projectiles));
    memset(g_gems, 0, sizeof(g_gems));
    memset(g_effects, 0, sizeof(g_effects));

    render_set_world(area_atlas(0), area_map(0), area_prop(0));

    Hero *h0 = &g_heroes[0];
    h0->active = true;
    h0->remote = false;
    h0->variant = 0;
    h0->x = WORLD_W / 2;
    h0->y = WORLD_H / 2;
    h0->facing = DIR_DOWN;
    h0->invuln = 0;
    h0->hurt_stun = 0;
    h0->ram_timer = 0;
    hero_recompute_stats(0);

    if (g_multiplayer)
    {
        Hero *h1 = &g_heroes[1];
        h1->active = true;
        h1->variant = 1;
        h1->x = WORLD_W / 2 + 32;
        h1->y = WORLD_H / 2;
        h1->facing = DIR_DOWN;
        h1->invuln = 0;
        h1->hurt_stun = 0;
        h1->ram_timer = 0;
        hero_recompute_stats(1);
    }
    else
    {
        g_heroes[1].active = false;
    }

    render_camera_center_on(h0->x + HERO_SPRITE_W / 2, h0->y + HERO_SPRITE_H / 2);
    audio_play_music(MUS_FIELD);
}

int hub_near_landmark(void)
{
    Hero *h = &g_heroes[g_local_hero];
    if (!h->active)
        return 0;

    int32_t hx, hy;
    hero_center(h, &hx, &hy);

    // 1: North Gate (出征之门) at (1024, 640)
    if (dist2(hx, hy, 1024, 640) < 50 * 50)
        return 1;

    // 2: West Post (信鸽信使) at (896, 768)
    if (dist2(hx, hy, 896, 768) < 50 * 50)
        return 2;

    // 3: East Tree (神圣世界树) at (1152, 768)
    if (dist2(hx, hy, 1152, 768) < 50 * 50)
        return 3;

    // 4: Center/South Oven (蛋糕烤箱) at (1024, 850)
    if (dist2(hx, hy, 1024, 850) < 50 * 50)
        return 4;

    return 0;
}

void hub_update(void)
{
    g_frame++;

    // Brisk comfortable stroll speed in town
    g_heroes[0].move_speed = 4;
    g_heroes[1].move_speed = 4;

    hero_update(0);
    if (g_multiplayer && g_heroes[1].active)
        hero_update(1);

    // Follow camera
    Hero *h = &g_heroes[g_local_hero];
    if (h->active)
    {
        int32_t hx, hy;
        hero_center(h, &hx, &hy);
        int cam_x, cam_y;
        render_camera_get(&cam_x, &cam_y);
        int want_x = hx - AREA_W / 2;
        int want_y = hy - AREA_H / 2;
        render_camera(cam_x + (want_x - cam_x) / 2,
                      cam_y + (want_y - cam_y) / 2);
    }

    render_set_world(area_atlas(0), area_map(0), area_prop(0));
    render_world();

    // Draw landmarks glowing auras in the Hub
    // North Gate: Stone arch portal
    gfx_fill_rect(1024 - 24, 640 - 18, 48, 14, PAL_GREY_DARK);
    gfx_fill_rect(1024 - 20, 640 - 16, 40, 10, PAL_UI_GOLD_LIGHT);
    gfx_circle_outline(1024, 640, 18, ((g_frame >> 3) & 1) ? PAL_WHITE_SPARK : PAL_UI_GOLD);

    // West Post: Pigeon Postal Station
    gfx_fill_rect(896 - 10, 768 - 14, 20, 18, PAL_CYAN_DARK);
    gfx_circle(896, 768 - 16, 6, PAL_CYAN_LIGHT);
    gfx_circle_outline(896, 768, 14, ((g_frame >> 3) & 1) ? PAL_CYAN_LIGHT : PAL_WHITE_SPARK);

    // East Tree: Glowing World Tree Aura
    gfx_circle(1152, 768 - 12, 20, PAL_LEAF_DARK);
    gfx_circle(1152, 768 - 12, 14, PAL_LEAF_LIGHT);
    gfx_circle(1152, 768 - 12, 7, PAL_GREEN_SPARK);
    gfx_circle_outline(1152, 768, 18, ((g_frame >> 3) & 1) ? PAL_GREEN_SPARK : PAL_UI_GOLD);

    // South Cake Oven
    gfx_fill_rect(1024 - 16, 850 - 10, 32, 20, PAL_RED_DARK);
    gfx_fill_rect(1024 - 12, 850 - 6, 24, 12, PAL_EMBER_ORANGE);

    // Draw heroes
    draw_hero(0);
    if (g_multiplayer && g_heroes[1].active)
        draw_hero(1);

    hub_draw();
}

void hub_draw(void)
{
    text_clear();
    text_panel(0, 0, 20, 2);

    text_draw(1, 0, "水豚王国广场", PAL_UI_GOLD_LIGHT);
    text_printf(12, 0, PAL_UI_GOLD, "印记:%d", g_boss_tokens);
    text_draw(1, 1, "十字键漫步 · 走近地标交互", PAL_UI_TEXT_DIM);

    // Proximity prompt
    int near = hub_near_landmark();
    if (near == 1)
        text_draw(1, 3, "★ 出征之门: 按 A 挑选关卡", PAL_WHITE_SPARK);
    else if (near == 2)
        text_draw(1, 3, "★ 信鸽使者: 按 A 无线联机", PAL_CYAN_LIGHT);
    else if (near == 3)
        text_draw(1, 3, "★ 神圣神树: 按 A 打开技能树", PAL_GREEN_SPARK);
    else if (near == 4)
    {
        if (g_ingredients_done == ((1 << INGREDIENT_COUNT) - 1))
            text_draw(1, 3, "★ 蛋糕烤箱: 材料齐备！按 Y 庆典", PAL_YELLOW_MID);
        else
            text_draw(1, 3, "★ 蛋糕烤箱: 收集5种食材制作蛋糕", PAL_UI_TEXT_DIM);
    }
    else
    {
        text_draw(1, 3, "探索王国广场 走近地标交互", PAL_UI_TEXT_DIM);
    }

    // Ingredients progress list
    text_draw(1, 5, "庆典蛋糕食材:", PAL_UI_GOLD_LIGHT);
    for (int i = 0; i < INGREDIENT_COUNT; i++)
    {
        bool done = (g_ingredients_done & (1 << i)) != 0;
        uint8_t col = done ? PAL_GREEN_SPARK : PAL_UI_TEXT_DIM;
        text_printf(1 + i * 4, 6, col, "%s%s",
                    done ? "●" : "○", g_regions[i].ingredient);
    }

    // Landmark direction guide
    text_draw(1, 8, "北:出征选关  西:无线联机", PAL_UI_TEXT);
    text_draw(1, 9, "东:被动神树  南:庆典烤箱", PAL_UI_TEXT);
    text_draw(13, 9, "按X开神树", PAL_UI_TEXT_DIM);

    // Bottom touch buttons (y: 154..186)
    sub_rect_outline(4, 154, 58, 32, PAL_OUTLINE);
    sub_fill_rect(5, 155, 56, 30, PAL_UI_PANEL);
    text_draw(1, 12, "出征选关", PAL_UI_GOLD_LIGHT);

    sub_rect_outline(66, 154, 58, 32, PAL_OUTLINE);
    sub_fill_rect(67, 155, 56, 30, PAL_UI_PANEL);
    text_draw(6, 12, "无线联机", PAL_CYAN_LIGHT);

    sub_rect_outline(128, 154, 58, 32, PAL_OUTLINE);
    sub_fill_rect(129, 155, 56, 30, PAL_UI_PANEL);
    text_draw(11, 12, "被动神树", PAL_GREEN_SPARK);

    sub_rect_outline(190, 154, 62, 32, PAL_OUTLINE);
    sub_fill_rect(191, 155, 60, 30, PAL_UI_PANEL);
    text_draw(16, 12, "烤箱庆典", PAL_YELLOW_LIGHT);
}
