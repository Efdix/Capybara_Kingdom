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
#include "hub_layout.h"
#include "farm.h"
#include "ui_draw.h"

static Monster *spawn_monster(uint8_t def_id, int32_t x, int32_t y, bool boss,
                              uint8_t boss_id);
static void spawn_splitter_children(const Monster *parent);
static void apply_elite_roll(Monster *m, int pct);
static bool duel_is_final_boss(const Monster *m);
void duel_begin_leap_out(void);

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

// A 64-step sine table, 0..127. Shared by the orbiting water orbs and by any
// AI that needs a wobble (the weaving swarmers) without pulling in libm.
static const int8_t s_sin64[65] = {
      0,   3,   6,   9,  12,  16,  19,  22,  25,  28,  31,  34,  37,
     40,  43,  46,  49,  51,  54,  57,  60,  63,  65,  68,  71,  73,
     76,  78,  81,  83,  85,  88,  90,  92,  94,  96,  98, 100, 102,
    104, 106, 107, 109, 111, 112, 113, 115, 116, 117, 118, 120, 121,
    122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127, 127,
};

// Full-wave sine: 0..255 in, -127..127 out.
static int sin8(int step)
{
    int q = step & 63;
    int v = s_sin64[q];
    if (step & 64) v = -v;
    if (step & 128) return -v;
    return v;
}

// ---------------------------------------------------------------------------
// Floating combat numbers
//
// Every landed hit pops a small number off the target. They are drawn with a
// 3x5 built-in digit font rather than the CJK glyph atlas: a crowd of thirty
// hits a second has to cost almost nothing, and white numbers with gold
// double-size crits are the whole vocabulary the player needs.
// ---------------------------------------------------------------------------

static Floater g_floaters[MAX_FLOATERS];

// 3x5 digits, one byte per row, bits left to right.
static const uint8_t s_digit_glyph[10][5] = {
    { 0x7, 0x5, 0x5, 0x5, 0x7 },   // 0
    { 0x2, 0x6, 0x2, 0x2, 0x7 },   // 1
    { 0x7, 0x1, 0x7, 0x4, 0x7 },   // 2
    { 0x7, 0x1, 0x7, 0x1, 0x7 },   // 3
    { 0x5, 0x5, 0x7, 0x1, 0x1 },   // 4
    { 0x7, 0x4, 0x7, 0x1, 0x7 },   // 5
    { 0x7, 0x4, 0x7, 0x5, 0x7 },   // 6
    { 0x7, 0x1, 0x2, 0x2, 0x2 },   // 7
    { 0x7, 0x5, 0x7, 0x5, 0x7 },   // 8
    { 0x7, 0x5, 0x7, 0x1, 0x7 },   // 9
};

void spawn_floater(int32_t x, int32_t y, int16_t value, uint8_t kind)
{
    for (int i = 0; i < MAX_FLOATERS; i++)
    {
        Floater *f = &g_floaters[i];
        if (f->alive)
            continue;
        f->alive = true;
        f->kind = kind;
        f->x = x + (int32_t)(my_rand() % 9) - 4;
        f->y = y - 10;
        f->value = value;
        f->life = (kind == FLOATER_CRIT) ? 34 : 24;
        return;
    }
}

// Draw the floaters. Called inside the world pass, after the camera has placed
// the frame, so world coordinates are already screen coordinates.
static void floaters_draw(void)
{
    for (int i = 0; i < MAX_FLOATERS; i++)
    {
        Floater *f = &g_floaters[i];
        if (!f->alive)
            continue;

        int t = (f->kind == FLOATER_CRIT) ? 34 : 24;
        int age = t - f->life;
        // Rise fast at first, then hang: the pop is what catches the eye.
        int y = f->y - age - (age > 8 ? 0 : (8 - age) / 2);
        int px = f->x;

        uint8_t col = PAL_WHITE_SPARK;
        if (f->kind == FLOATER_CRIT) col = PAL_UI_GOLD_LIGHT;
        if (f->kind == FLOATER_HEAL) col = PAL_GREEN_SPARK;
        if (f->kind == FLOATER_SEED) col = PAL_UI_GOLD;

        if (f->kind == FLOATER_HEAL)
        {
            // A green plus, then the amount.
            gfx_fill_rect(px - 6, y - 1, 3, 1, col);
            gfx_fill_rect(px - 5, y - 2, 1, 3, col);
            px += 2;
        }
        else if (f->kind == FLOATER_SEED)
        {
            // A little amber pip, so a seed pickup reads differently from a
            // heart or a damage number at a glance.
            gfx_fill_rect(px - 6, y - 1, 2, 2, col);
            gfx_fill_rect(px - 5, y - 2, 1, 1, PAL_YELLOW_LIGHT);
            px += 2;
        }

        int v = f->value;
        char buf[6];
        int n = 0;
        if (v <= 0) buf[n++] = '0';
        while (v > 0 && n < 5) { buf[n++] = (char)('0' + v % 10); v /= 10; }

        int big = (f->kind == FLOATER_CRIT) ? 2 : 1;   // crits print double
        int dx = px - (n * 4 * big) / 2;
        for (int d = n - 1; d >= 0; d--)
        {
            const uint8_t *gl = s_digit_glyph[buf[d] - '0'];
            for (int row = 0; row < 5; row++)
                for (int cbit = 0; cbit < 3; cbit++)
                    if (gl[row] & (4 >> cbit))
                        gfx_fill_rect(dx + cbit * big, y + row * big, big, big,
                                      col);
            dx += 4 * big;
        }

        if (--f->life == 0)
            f->alive = false;
    }
}

// ---------------------------------------------------------------------------
// Event toasts (横幅播报)
//
// One line of news across the bottom screen's header: elite spawns, raid
// waves, stolen ingredients, difficulty steps, boss entrances. A single slot
// -- the newest event wins -- because two overlapping banners would both be
// unread.
// ---------------------------------------------------------------------------

#define TOAST_MAX_LIFE 240

static struct {
    const char *text;
    uint8_t     color;
    uint16_t    life;
} g_toast;

static void show_toast(const char *text, uint8_t color, uint16_t life)
{
    g_toast.text = text;
    g_toast.color = color;
    g_toast.life = life > TOAST_MAX_LIFE ? TOAST_MAX_LIFE : life;
}

// The same banner, for screens outside the field. The hub's field panel and the
// farm's own actions need to say something, and the bottom screen has exactly
// one place to say it -- so they write to the same slot rather than inventing a
// second banner that would overlap the first.
void hub_toast(const char *text, uint8_t color, uint16_t life)
{
    show_toast(text, color, life);
}

// Raid-wave (兽潮) state. A wave is a 10-second spike of spawns led by
// elites; when it breaks it pays out loot around the player. The countdown
// runs only inside a live region, so the hub and the duel leave it alone.
static uint16_t g_wave_cd = WAVE_FIRST;
static uint16_t g_wave_active = 0;

// ---------------------------------------------------------------------------
// The boss duel (霸主决战)
// ---------------------------------------------------------------------------

// Duel phases, in order. The leap-out beat plays inside the field phase (the
// boss jumps at the camera while the small monsters are cleared); the rest run
// inside ST_DUEL.
#define DUEL_OFF        0
#define DUEL_LEAP_OUT   1   // in-field: the boss leaps at the screen, mobs poof
#define DUEL_INTRO      2   // arena: the boss crashes down and roars
#define DUEL_FIGHT      3

// Boss duel FSM states.
//
// The first twelve are the original movement beats. The rest exist because the
// fight now has to do things the original wheel did not: turn a phase over,
// fall down when its poise breaks, and run named moves that are not simply
// "dash, leap or shoot".
#define DB_IDLE      0   // deciding
#define DB_WALK      1   // closing the gap
#define DB_DASH_WIND 2
#define DB_DASH      3
#define DB_LEAP_WIND 4
#define DB_LEAP_RISE 5
#define DB_LEAP_FALL 6
#define DB_LAND      7
#define DB_VOLLEY    8
#define DB_BURST_WIND 9
#define DB_BURST     10
#define DB_RECOVER   11
#define DB_ROAR      12  // phase change: invulnerable, screaming, unattackable
#define DB_STAGGER   13  // poise broken: on its knees, takes bonus damage
#define DB_WIND      14  // generic wind-up for the named moves below
#define DB_ACT       15  // generic execution of the named move
#define DB_DIG       16  // underground: untargetable, tracking the hero
#define DB_SWEEP     17  // a horizontal beam or tail arc crossing the arena

// ---------------------------------------------------------------------------
// The move vocabulary
//
// A boss does not pick attacks at random; it walks a written pattern. That is
// the whole difference between a fight the player learns and a slot machine.
// Every move here is a shape the player can answer, and each one exists to be
// somebody's problem: MV_LEAP punishes standing still, MV_SWEEP punishes
// jumping early, MV_DIG punishes camping a corner, and so on.
// ---------------------------------------------------------------------------
#define MV_NONE     0
#define MV_DASH     1   // charge across the arena along a marked lane
#define MV_LEAP     2   // leap and slam, two ground waves race out
#define MV_VOLLEY   3   // spread shot from the mouth
#define MV_BURST    4   // proximity nova: punishes hugging
#define MV_SWEEP    5   // arc sweep along the ground: jump it
#define MV_SPIKES   6   // a chain of spikes erupting in sequence
#define MV_LOB      7   // arcing shots into marked spots
#define MV_DIG      8   // burrow under the arena and surface beneath the hero
#define MV_LASER    9   // a horizontal beam that sweeps the arena
#define MV_RAIN    10   // marked impacts falling from above
#define MV_SUMMON  11   // call an add to the arena

// One entry of a boss's pattern. `chain` lets two moves be welded together --
// a dash that becomes a leap, a volley that becomes a sweep -- which is how a
// boss escalates without needing a second state machine.
typedef struct {
    uint8_t  move;      // MV_*
    uint8_t  phase;     // earliest phase this move appears in (0/1/2)
    uint16_t wind;      // telegraph length, in frames (>= TELEGRAPH_MIN)
    uint16_t act;       // execution length
    uint16_t rec;       // recovery: the window the player attacks into
    uint8_t  chain;     // MV_* to run straight after, or MV_NONE
} DuelMove;

// Per-region duel tuning, one row per FINAL boss (the five 霸主). The mid-boss
// fights stay in the top-down field; only the region climax becomes a duel.
typedef struct {
    uint8_t scale_pct;      // how big the 64x64 boss art is drawn
    uint8_t walk_speed;
    uint8_t dash_speed;
    uint8_t contact_dmg;    // touch, dash and slam damage, in hearts
    uint8_t volley_n;       // projectiles per volley (phase adds one)
    uint8_t proj_speed;
    uint8_t proj_a, proj_b, proj_c;   // region-flavoured projectile colours
    bool    has_burst;      // proximity nova, the anti-hugging measure
    uint8_t land_pause;     // extra recovery frames after a slam
    // The pattern this boss walks, and how many entries it has. Entries above
    // the boss's current phase are skipped, so turning a phase over does not
    // just speed the fight up -- it adds sentences to the boss's vocabulary.
    const DuelMove *pattern;
    uint8_t pattern_n;
    // Health. Tuned by working backwards from the damage a built-up capybara
    // actually lands in a duel: cards at level 3-5 plus a couple of tree
    // passives put sustained output around 25-35 a second, and each fight is
    // meant to run 60-110 seconds INCLUDING the phase-change roars, the
    // staggers the player earns, and the mistakes. The old 600-1300 ladder
    // died in under half a minute and the third phase was over before it
    // started; this ladder keeps every fight long enough to see its whole
    // pattern. (boss_balance_check.py rule 7 holds this band.)
    uint16_t hp;
} DuelDef;

// --- the five patterns -----------------------------------------------------
// Written as rotations, not as priority lists: the boss walks them in order, so
// a player who watches carefully can predict the next beat. Phase gates are the
// escalation, and every recovery is the player's turn.

// 树精长老: the teaching fight. Slow, wide, forgiving.
static const DuelMove g_pat_treant[] = {
    { MV_SWEEP,  0, 34, 18, 56, MV_NONE  },   // 藤鞭横扫
    { MV_VOLLEY, 0, 26, 16, 40, MV_NONE  },   // 叶刃扇射
    { MV_SPIKES, 0, 30, 24, 50, MV_NONE  },   // 根刺迸发
    { MV_LOB,    1, 28, 20, 46, MV_NONE  },   // 种子轰炸
    { MV_SWEEP,  1, 26, 18, 24, MV_SWEEP },   // 横扫二连
    { MV_SPIKES, 2, 24, 30, 52, MV_NONE  },   // 古树之怒
    { MV_BURST,  2, 20, 12, 46, MV_NONE  },   // 践踏新星
};

// 硕鼠大王: burrows, so the player cannot just stand at the far end.
static const DuelMove g_pat_ratking[] = {
    { MV_DASH,   0, 24, 40, 34, MV_NONE   },  // 冲刺啃咬
    { MV_VOLLEY, 0, 22, 14, 32, MV_NONE   },  // 麦袋抛掷
    { MV_SWEEP,  0, 26, 16, 40, MV_NONE   },  // 巨尾重扫
    { MV_DIG,    1, 30, 60, 40, MV_NONE   },  // 掘地潜行
    { MV_SUMMON, 1, 30, 20, 44, MV_NONE   },  // 鼠群召集
    { MV_DIG,    2, 24, 40, 24, MV_DIG    },  // 连环挖掘
    { MV_RAIN,   2, 26, 34, 44, MV_NONE   },  // 落尘雨
};

// 狂暴黑牛: the pressure fight. Dashes that demand the roll, and a feint that
// punishes greed -- the charge that catches you when you move in to punish it.
static const DuelMove g_pat_bull[] = {
    { MV_DASH,   0, 30, 44, 60, MV_NONE   },  // 狂牛冲锋 (walls end in a stun)
    { MV_SWEEP,  0, 24, 16, 36, MV_NONE   },  // 角挑
    { MV_LEAP,   0, 24, 30, 30, MV_NONE   },  // 跳跃践踏
    { MV_DASH,   1, 26, 40, 32, MV_LEAP   },  // 冲锋接跳劈: the feint
    { MV_DASH,   2, 22, 44, 26, MV_DASH   },  // 三连狂冲
    { MV_BURST,  2, 20, 12, 40, MV_NONE   },  // 贴身震地
};

// 凯门鳄王: water and ambush. The burrow is vertical here -- it surfaces under
// your feet rather than crossing the arena.
static const DuelMove g_pat_caiman[] = {
    { MV_DASH,   0, 28, 40, 40, MV_NONE   },  // 死亡翻滚
    { MV_VOLLEY, 0, 26, 18, 36, MV_NONE   },  // 鳄尾掀浪
    { MV_DIG,    0, 30, 54, 42, MV_NONE   },  // 潜水突袭
    { MV_BURST,  1, 22, 12, 44, MV_NONE   },  // 漩涡弹幕
    { MV_DASH,   1, 24, 34, 24, MV_DASH   },  // 折返二连
    { MV_DIG,    2, 24, 44, 22, MV_DIG    },  // 三重潜水
    { MV_SWEEP,  2, 22, 18, 40, MV_NONE   },  // 尾扫新星
};

// 蛋糕魔王: the final exam. Everything the other four taught, faster, with a
// laser that owns the ground and minions that punish tunnel vision.
static const DuelMove g_pat_cakelord[] = {
    { MV_LASER,  0, 34, 44, 40, MV_NONE   },  // 奶油光束
    { MV_RAIN,   0, 26, 32, 38, MV_NONE   },  // 樱桃炸弹雨
    { MV_LEAP,   0, 24, 30, 26, MV_NONE   },  // 恶魔下砸
    { MV_SUMMON, 0, 30, 20, 40, MV_NONE   },  // 召唤火球
    { MV_VOLLEY, 1, 24, 20, 34, MV_NONE   },  // 樱桃环形弹幕
    { MV_LEAP,   1, 22, 28, 24, MV_SWEEP  },  // 下砸接横扫
    { MV_LASER,  2, 24, 40, 22, MV_LASER  },  // 双光束
    { MV_RAIN,   2, 20, 40, 34, MV_NONE   },  // 真·魔王狂欢
};

static const DuelDef g_duel_defs[INGREDIENT_COUNT] = {
    // 树精长老: slow, root volleys, punishing slam, punishes hugging
    { 200, 1, 4, 1, 3, 2, PAL_GRN_DARK, PAL_GRN_MID, PAL_LEAF_LIGHT, true,
      20, g_pat_treant, 7, 1300 },
    // 硕鼠大王: fast dashes, quick triple shot, shallow leaps
    { 180, 2, 5, 1, 3, 3, PAL_TAN_DARK, PAL_WHEAT_MID, PAL_SAND_LIGHT, false,
      12, g_pat_ratking, 7, 1650 },
    // 狂暴黑牛: the charger -- long dashes, heavy contact, big shockwaves
    { 215, 2, 6, 2, 2, 3, PAL_BRN_DEEP, PAL_BRN_MID2, PAL_CREAM_LIGHT, false,
      24, g_pat_bull, 6, 2000 },
    // 凯门鳄王: snapping dashes, wide water volleys, punishes hugging
    { 205, 2, 5, 2, 4, 3, PAL_SEA_DEEP, PAL_CYAN_DARK, PAL_CYAN_LIGHT, true,
      16, g_pat_caiman, 7, 2400 },
    // 蛋糕魔王: everything, faster -- the final exam
    { 230, 2, 6, 2, 4, 4, PAL_CHOCO_DARK, PAL_CHERRY_MID, PAL_CREAM_LIGHT, true,
      18, g_pat_cakelord, 8, 2900 },
};

static struct {
    int      phase;         // DUEL_*
    uint16_t timer;         // phase countdown
    // Boss sim. The boss monster mirrors bx/by so every existing damage path
    // (projectiles, cards, ultimates) keeps working unmodified.
    int32_t  bx, by;        // boss centre; by sits on the ground line at rest
    int16_t  bvy;           // vertical speed during the leap, in half-pixels
    int      fsm;           // DB_* state
    uint16_t fsm_t;         // state countdown
    uint8_t  last_atk;      // last attack picked, so it never repeats
    uint8_t  atk_count;     // attacks since the last walk
    bool     enrage;        // phase 2, below half health
    int32_t  lock_x;        // dash / slam target
    int16_t  air;           // pixels above the ground during a leap
    uint16_t contact_cd;    // touch-damage gate

    // --- the pattern walk --------------------------------------------------
    // The boss reads its written rotation instead of rolling dice, so the same
    // input always produces the same fight and a patient player can learn it.
    uint8_t  step;          // index into the boss's pattern
    uint8_t  chain;         // MV_* queued to run after the current one
    uint8_t  move;          // MV_* currently running
    uint8_t  boss_phase;    // 0/1/2, gated by health
    uint16_t intro_timer;   // name card, in frames

    // --- stagger -----------------------------------------------------------
    uint16_t stagger_timer; // >0 while the boss is on its knees

    // --- move scratch ------------------------------------------------------
    // Whatever the running move needs to remember between its wind-up and its
    // execution, so the two are not forced to recompute the same geometry.
    int32_t  tgt_x, tgt_y;  // where the move will land
    uint8_t  shot_n;        // shots still owed this move
    uint16_t shot_t;        // frames until the next shot
    uint8_t  dig_surf;      // how many more surfaces the dig move owes
    bool     burrowed;      // underground: untargetable, tracking the hero

    // --- the death beat ----------------------------------------------------
    // Counts down after the final blow lands so the arena can hold on the
    // moment: the boss slumps, the loot fountains, and only then does the
    // clear screen take over. Without it a kill is just a state change.
    uint16_t dying;         // >0 while the death beat plays
    bool     dying_final;   // whether this run is being won or merely survived
} g_duel;

// Accessors for the HUD and for net.c, which must not depend on the FSM's
// private layout.
int duel_boss_phase(void) { return (int)g_duel.boss_phase; }
bool duel_boss_is_staggered(void) { return g_duel.stagger_timer > 0; }
// True while the boss is in its phase-change roar, during which it is immune.
static bool duel_boss_roaring(void) { return g_duel.fsm == DB_ROAR; }

static Monster *duel_boss(void)
{
    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        Monster *m = &g_monsters[i];
        if (m->alive && m->is_boss)
            return m;
    }
    return NULL;
}

// Defined with the rest of the duel brain further down; declared here because
// the damage path has to know when the boss is untouchable.
static bool duel_boss_immune(void);
static bool duel_boss_roaring(void);

// Is this the region's FINAL boss (the duelist), as opposed to the field
// mid-boss? The two get different victory flows.
static bool duel_is_final_boss(const Monster *m)
{
    return m->is_boss && m->boss_id == g_regions[g_region_id].boss_id;
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
    // Default ownership is the capybaras' side; monster callers that leave a
    // hazard behind say so through spawn_effect_owned().
    spawn_effect_owned(kind, x, y, life, radius, 0);
}

void spawn_effect_owned(uint8_t kind, int32_t x, int32_t y, uint8_t life,
                        uint8_t radius, uint8_t owner)
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
        e->owner = owner;
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

    // Pool full: recycle an experience mote rather than dropping the newest
    // thing on the floor. A wave's worst moment used to silently eat exactly
    // the drops the player was walking toward; seeds, hearts and tokens are
    // never the ones recycled.
    for (int i = 0; i < MAX_GEMS; i++)
    {
        Gem *g = &g_gems[i];
        if (g->kind == 0)
        {
            g->kind = kind;
            g->x = x;
            g->y = y;
            g->value = value;
            g->bob = (uint8_t)(my_rand() & 15);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Telegraphs (预警)
//
// The whole duelling design rests on these: a move that is not announced is a
// coin flip, and a coin flip in a fight the player is meant to learn is just a
// cheap hit. Every shape therefore draws as a ring or a lane that fills while
// it drains, so its remaining life is legible without a HUD.
// ---------------------------------------------------------------------------

Telegraph *spawn_telegraph(uint8_t kind, int32_t x, int32_t y, uint8_t life,
                           uint8_t radius)
{
    for (int i = 0; i < MAX_TELEGRAPHS; i++)
    {
        Telegraph *t = &g_telegraphs[i];
        if (t->alive)
            continue;
        t->alive = true;
        t->kind = kind;
        t->x = x;
        t->y = y;
        t->x2 = 0;
        t->y2 = 0;
        t->radius = radius;
        t->life = life;
        t->life_max = life;
        t->delay = 0;
        t->owner = 0;
        return t;
    }
    return NULL;
}

// A combo's worth of rings, lighting one after another. Each ring beyond the
// first is held back by `stagger` frames, so the sequence reads left to right
// the way the attack will actually land.
void spawn_telegraph_chain(const int32_t *points, int count,
                           uint8_t life, uint8_t stagger)
{
    for (int i = 0; i < count; i++)
    {
        Telegraph *t = spawn_telegraph(TG_CIRCLE, points[i * 3 + 0],
                                       points[i * 3 + 1], life, 0);
        if (!t)
            continue;
        t->radius = (uint8_t)points[i * 3 + 2];
        t->delay = (uint8_t)(i * stagger);
        // A ring that has not started yet still occupies the slot, so the pool
        // cannot be eaten by a half-issued combo.
        t->life = (uint8_t)(life + t->delay);
        t->life_max = t->life;
    }
}

void telegraph_update(void)
{
    for (int i = 0; i < MAX_TELEGRAPHS; i++)
        if (g_telegraphs[i].alive && g_telegraphs[i].life > 0)
            g_telegraphs[i].life--;
    // Sweep the dead. Done here rather than inline so a shape's whole life is
    // visible to the drawing pass even on the frame it expires.
    for (int i = 0; i < MAX_TELEGRAPHS; i++)
        if (g_telegraphs[i].alive && g_telegraphs[i].life == 0)
            g_telegraphs[i].alive = false;
}

// ---------------------------------------------------------------------------
// Card fusion
// ---------------------------------------------------------------------------

int fusion_for_pair(int a, int b)
{
    for (int i = 0; i < FUSION_COUNT; i++)
    {
        const FusionRecipe *r = &g_fusion_recipes[i];
        if ((r->a == a && r->b == b) || (r->a == b && r->b == a))
            return i;
    }
    return -1;
}

int fusion_available(const uint8_t *cards)
{
    for (int i = 0; i < FUSION_COUNT; i++)
    {
        const FusionRecipe *r = &g_fusion_recipes[i];
        // Both parents have to be *unlocked*, not merely owned: fusing two
        // level-2 cards would throw away the mechanic the fusion is meant to
        // build on, so level 3 is the gate in both directions.
        if (cards[r->a] < CARD_UNLOCK_LEVEL) continue;
        if (cards[r->b] < CARD_UNLOCK_LEVEL) continue;
        // A card already fused is not on offer again -- the result would be a
        // duplicate carrying nothing new.
        if (cards[r->result] > 0) continue;
        return i;
    }
    return -1;
}

void hero_fuse_cards(int hero_index, int recipe_index)
{
    if (hero_index < 0 || hero_index >= 2)
        return;
    if (recipe_index < 0 || recipe_index >= FUSION_COUNT)
        return;
    Hero *h = &g_heroes[hero_index];
    if (!h->active)
        return;

    const FusionRecipe *r = &g_fusion_recipes[recipe_index];
    if (h->cards[r->a] < CARD_UNLOCK_LEVEL || h->cards[r->b] < CARD_UNLOCK_LEVEL)
        return;
    if (h->cards[r->result] > 0)
        return;

    // The result carries the higher of the two parents forward, so a fusion is
    // never a downgrade -- it costs a slot and two level-3 cards, and pays back
    // one card with both mechanics plus the level the player already earned.
    int lvl = h->cards[r->a] > h->cards[r->b] ? h->cards[r->a] : h->cards[r->b];
    h->cards[r->result] = (uint8_t)lvl;
    h->cards[r->a] = 0;
    h->cards[r->b] = 0;

    // Both parents are gone, so any slot holding one is now holding a card the
    // hero does not own. Move the fused card into the first freed slot so the
    // player does not have to re-assign what just happened.
    for (int s = 0; s < SKILL_SLOT_COUNT; s++)
    {
        if (h->skill_slots[s] == r->a || h->skill_slots[s] == r->b)
        {
            h->skill_slots[s] = r->result;
            break;
        }
    }

    h->cooldown[r->a] = 0;
    h->cooldown[r->b] = 0;
    hero_recompute_stats(hero_index);

    if (hero_index == g_local_hero)
    {
        int32_t cx, cy;
        hero_center(h, &cx, &cy);
        spawn_effect(FX_LEVELUP, cx, cy, 50, 30);
        audio_play_sfx(SND_LEVELUP);
        show_toast("卡牌融合！", PAL_UI_GOLD_LIGHT, 160);
    }
}


static Projectile *spawn_projectile(uint8_t kind, int32_t x, int32_t y,
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
        p->hostile = false;
        return p;
    }
    return NULL;
}

// A monster's shot. Ownership is declared here rather than inferred from the
// kind, because the kinds overlap: a field boss's water blast and the
// capybaras' water jet are both PROJ_WATER, and the old kind-based test made
// the boss's own volley harmless to the player while it chipped whatever
// hostile creature it crossed. A recruited monster's shot fights for us.
static Projectile *spawn_mob_projectile(const Monster *owner, uint8_t kind,
                                        int32_t x, int32_t y,
                                        int16_t vx, int16_t vy, uint8_t life,
                                        uint16_t power, uint8_t radius)
{
    Projectile *p = spawn_projectile(kind, x, y, vx, vy, life, power, radius);
    if (!p)
        return NULL;
    p->hostile = !owner || !owner->recruited;
    return p;
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

// Does the hero have this card's *mechanic* yet? Level 3 is the gate for every
// mechanic a boss fight cares about (铁尾's guard, 水柱's pierce, 冲撞's
// i-frames, 利爪's stagger feed). One helper, read by both the field and the
// duel paths, so the two can never disagree about whether it is unlocked.
static bool card_unlocked(const Hero *h, int id)
{
    return h->cards[id] >= CARD_UNLOCK_LEVEL;
}

// A fused card stands in for both of its parents. Everything that asks "do you
// have X" goes through here, so 潮汐双炮 counts as 水柱 AND 灵水 without a
// single call site having to know about fusions -- which is what lets the deck
// view and the fusion picker show the same build consistently.
static bool card_owned(const Hero *h, int id)
{
    if (h->cards[id] > 0)
        return true;
    for (int i = 0; i < FUSION_COUNT; i++)
    {
        const FusionRecipe *r = &g_fusion_recipes[i];
        if ((r->a == id || r->b == id) && h->cards[r->result] > 0)
            return true;
    }
    return false;
}

// The effective level of a card, counting a fused card as its parents' level.
// Damage and radii read this, so a fused card hits with the strength of what
// the player actually invested rather than starting over at level 3.
static int card_eff_level(const Hero *h, int id)
{
    if (h->cards[id] > 0)
        return h->cards[id];
    for (int i = 0; i < FUSION_COUNT; i++)
    {
        const FusionRecipe *r = &g_fusion_recipes[i];
        if (r->a != id && r->b != id)
            continue;
        if (h->cards[r->result] > 0)
            return h->cards[r->result];
    }
    return 0;
}

static uint16_t card_cd(const Hero *h, int id)
{
    int lvl = card_eff_level(h, id);
    if (lvl <= 0)
        lvl = 1;
    uint16_t base = g_card_defs[id].cooldown;
    if (base == 0)
        base = g_card_defs[id].duel_cd ? g_card_defs[id].duel_cd : 60;
    if (g_skill_tree.agi_lv >= 3)
        base = (uint16_t)(base - (base * 20) / 100); // 敏捷之树Lv3: 冷却-20%
    // Shorter as it levels, but never below a quarter of the base.
    uint16_t cut = (uint16_t)((base * (lvl - 1)) / 8);
    return (uint16_t)(base - cut);
}

// The duel's own cooldown: the arena is a fighting game, so its abilities come
// back on a faster clock than the field's automatic volleys, and 快腿 shaves it
// further -- which is how the movement card stays useful after the field phase.
static uint16_t card_duel_cd(const Hero *h, int id)
{
    const CardDef *d = &g_card_defs[id];
    uint16_t base = d->duel_cd ? d->duel_cd : (d->cooldown ? d->cooldown : 60);
    int lvl = card_eff_level(h, id);
    if (lvl <= 0)
        lvl = 1;
    base = (uint16_t)(base - (base * (lvl - 1)) / 10);
    int swift = card_eff_level(h, CARD_SWIFT);
    if (swift > 0)
        base = (uint16_t)(base - base * swift / 25);   // 快腿: -4% per level
    if (g_skill_tree.agi_lv >= 3)
        base = (uint16_t)(base - (base * 20) / 100);
    return base < 12 ? 12 : base;
}

static int card_power(const Hero *h, int id)
{
    int lvl = card_eff_level(h, id);
    if (lvl <= 0)
        lvl = 1;
    int p = g_card_defs[id].power;
    p += (p * (lvl - 1)) / 2;      // +50% of base per level
    p += h->attack;
    int fang = card_eff_level(h, CARD_FANG);
    if (fang > 0)
        p += (p * fang * 8) / 100; // 利爪: +8% per level to everything
    if (g_skill_tree.power_lv >= 3)
        p += (p * 30) / 100;       // 力量之树Lv3: 全技能威力+30%
    return p;
}

static int card_radius(const Hero *h, int id)
{
    int lvl = card_eff_level(h, id);
    if (lvl <= 0)
        lvl = 1;
    int r = g_card_defs[id].radius;
    if (r == 0)
        r = 24;
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

    int hide = card_eff_level(h, CARD_HIDE);
    int swift = card_eff_level(h, CARD_SWIFT);
    int magnet = card_eff_level(h, CARD_MAGNET);

    // 厚皮: +1 max heart per level. 坚韧之树Lv2: +2颗心
    int hm = HERO_BASE_HEARTS + hide + (g_skill_tree.tough_lv >= 2 ? 2 : 0);
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
    h->attack = (uint16_t)(3 + card_eff_level(h, CARD_FANG) * 2 +
                           (g_skill_tree.power_lv >= 1 ? 2 : 0));

    // 水球数量绑定灵水卡牌等级 (Lv1: 1球, Lv2: 2球 ... Lv5: 5球)
    int orbs = card_eff_level(h, CARD_MUD);
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

    // Reaching the unlock level is a real event: the card stops being a body
    // and becomes an answer to something the bosses do. Say so, because a
    // mechanic the player does not know they own is a mechanic they will never
    // use.
    if (h->cards[card_id] == CARD_UNLOCK_LEVEL && hero_index == g_local_hero)
    {
        show_toast("卡牌觉醒！", PAL_UI_GOLD_LIGHT, 150);
    }

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

    // Every level costs about a third more than the one before it, plus a flat
    // step. Tuned so an hour-long campaign -- all five regions, both boss
    // phases each -- lands around level 18-22, which is roughly twenty card
    // picks: enough to master a few cards and dabble in the rest.
    while (h->exp >= h->exp_need)
    {
        h->exp -= h->exp_need;
        h->level++;

        // exp_need is 16-bit, so the ramp has to stop somewhere. The cap is
        // far past any level a run reaches; it only exists so the growth
        // cannot wrap around and hand out cheap levels again.
        //
        // +18% +10, down from +30% +12. The old ramp was tuned against an
        // assumed income rather than the measured one: a full five-region run
        // pays out around 14-15k experience before combo and elite multipliers,
        // which on the old curve arrived at level 15 -- about fifteen picks, not
        // the twenty the comment above claims, and not enough to bring even
        // three cards to level 5. This ramp puts that same run at level 21-22,
        // which is where "master a few cards and dabble in the rest" actually
        // becomes true.
        uint32_t next = (uint32_t)h->exp_need + h->exp_need * 18 / 100 + 10;
        h->exp_need = (next > EXP_NEED_MAX) ? EXP_NEED_MAX : (uint16_t)next;

        // A level-up is a full heal, which is the only sustain besides 感化's
        // ultimate and the heart drops.
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

// Kill-chain experience. Kills inside COMBO_WINDOW frames of each other build
// a combo, and every chained kill pays up to +40% more experience. The combo
// rewards pushing into the crowd instead of kiting it at zero risk -- and it
// is shared by both capybaras, because it measures the fight.
static int combo_exp_mult(void)
{
    int c = g_combo;
    if (c > COMBO_BONUS_CAP)
        c = COMBO_BONUS_CAP;
    return 100 + c * 2;
}

// ---------------------------------------------------------------------------
// Hitstop
// ---------------------------------------------------------------------------

// Requests stack by keeping the longest: two cards landing on the same frame
// should not add up to a stutter, and a small hit must never cut a big one
// short. See the block in game.h for why this is a frame skip rather than a
// timer every system has to consult.
void hitstop_request(uint8_t frames)
{
    if (frames > HITSTOP_MAX)
        frames = HITSTOP_MAX;
    if (frames > g_hitstop)
        g_hitstop = frames;
}

// ---------------------------------------------------------------------------
// Monster death and damage
// ---------------------------------------------------------------------------

static void ult_credit(int index, int gain);
static int region_battle_music(void);

static void monster_die(Monster *m)
{
    bool was_boss = m->is_boss;
    bool was_ally = m->recruited;
    bool was_elite = m->elite != 0;
    int32_t dcx, dcy;
    monster_center(m, &dcx, &dcy);

    m->alive = false;

    // What the kill looks like. Sized off the creature so a slime and a wasp do
    // not leave the same mark, and longer for an elite or a boss so the biggest
    // things in the game are also the ones that take a moment to come apart.
    {
        int r = m->is_boss ? 40 : monster_draw_radius(m);
        int life = m->is_boss ? 34 : (was_elite ? 24 : 16);
        spawn_effect(FX_KILL, dcx, dcy, (uint8_t)life, (uint8_t)r);
    }

    // Recruited allies drop nothing: 感化 converts a kill into a permanent
    // ally, and paying exp for it too would make the card a farming tool.
    if (was_ally)
        return;

    // Splitter mobs burst into two smaller copies -- killing one is the start
    // of the fight, not the end. Elites split into ordinary children, so an
    // elite slime pays out once, not three times.
    if (!was_boss && m->elite == 0)
        spawn_splitter_children(m);

    // A thief killed mid-flight drops what it stole, plus interest.
    if (!was_boss && monster_def(m->def_id)->ai == AI_STEALER && m->state == 1)
    {
        for (int k = 0; k < 2; k++)
            spawn_gem(GEM_SEED, dcx + (int)(my_rand() % 17) - 8,
                      dcy + (int)(my_rand() % 9) - 4,
                      (uint16_t)(g_region_id + 1));
        show_toast("夺回了被盗的种子！", PAL_UI_GOLD_LIGHT, 150);
    }

    // The kill chain. Bosses keep the combo alive but elites spike it, which
    // is what makes a wave full of elites the best experience moment in a
    // region.
    g_combo++;
    g_combo_timer = COMBO_WINDOW;

    if (!was_boss)
        g_region_kills++;

    // Experience is per type, so the brute that soaked ten hits is worth
    // materially more than the swarm that died to one. That is what makes
    // picking fights a decision instead of clearing whatever is nearest.
    // BOTH capybaras are credited: in a pair each player levels at
    // single-player pace, and on the host this is also how a client's
    // reported damage turns into levels back on its own screen (the totals
    // ride the world packet home).
    int exp = was_boss ? (int)(300 + g_region_id * 120)
                       : (int)monster_def(m->def_id)->exp_reward;
    if (was_elite)
        exp *= 5;
    exp = exp * combo_exp_mult() / 100;
    for (int i = 0; i < 2; i++)
        if (g_heroes[i].active)
            hero_gain_exp(i, exp);

    // Ultimates charge off kills, and every active capybara is credited -- a
    // pair charges together, so the combined release is reachable on purpose
    // rather than by luck. Elites are worth two kills, bosses a burst.
    ult_credit(0, was_boss ? 8 : (was_elite ? 2 : 1));
    ult_credit(1, was_boss ? 8 : (was_elite ? 2 : 1));

    if (was_boss && g_duel_active)
    {
        // The duel is a closed arena with no time to chase drops: the rewards
        // are credited the moment the boss falls, with the fanfare doing the
        // "here is what you won" work the gems would otherwise do.
        bool final = duel_is_final_boss(m);
        g_boss_tokens += (uint16_t)(final ? 2 : 1);
        Hero *lh = &g_heroes[g_local_hero];
        if (lh->active)
        {
            // Victory lap: refill some hearts, half the meter as a bonus.
            lh->hearts = lh->hearts_max;
        }

        // The death beat. Rather than snapping the state on the frame the last
        // hit lands, the arena holds for a moment: the boss slumps, the screen
        // shakes, gems fountain out of it, and only then does the clear screen
        // arrive. The pause is what makes a kill feel earned instead of
        // instant.
        g_duel.dying = 60;
        g_duel.dying_final = final;
        // Its pending warnings die with it. A ring left on the floor after the
        // boss is gone would be a warning for an attack that will never come.
        for (int i = 0; i < MAX_TELEGRAPHS; i++)
            g_telegraphs[i].alive = false;
        spawn_effect(FX_LEVELUP, dcx, dcy, 60, 44);
        for (int k = 0; k < 18; k++)
        {
            int ang = (k * 256) / 18;
            int dx2, dy2;
            gfx_dir_256(ang, &dx2, &dy2);
            spawn_gem(0, dcx + (dx2 * 30) / 127, dcy + (dy2 * 24) / 127,
                      (uint16_t)(1 + g_region_id));
        }
        for (int k = 0; k < 3; k++)
            spawn_gem(3, dcx + (int)(my_rand() % 41) - 20, dcy, 1);
        g_screen_shake = 8;
        audio_play_sfx(SND_EXPLOSION);
        return;
    }

    // An experience mote, then the seed rolls. The drop chance is per mob
    // (MonsterDef.drop_pct): brutes are the seed battery, swarm are nearly
    // barren, so "which crowd do I wade into" decides how fast the field can
    // be planted. Elites always pay out, plus a heart most of the time.
    //
    // Seeds replace the old ingredient drop, and the region id doubles as the
    // crop id: monsters in region r drop seed r, seed r grows crop r, crop r
    // yields region r's ingredient. One number carries the chain, which is why
    // no lookup table is needed anywhere.
    spawn_gem(0, dcx, dcy,
              (uint16_t)(1 + monster_def(m->def_id)->exp_reward / 12));
    if (was_elite)
    {
        spawn_gem(GEM_SEED, dcx + (int)(my_rand() % 13) - 6, dcy,
                  (uint16_t)(g_region_id + 1));
        if ((my_rand() % 100) < 40)
            spawn_gem(3, dcx - 10, dcy, 1);
        spawn_gem(0, dcx + 8, dcy, 5);
        spawn_effect(FX_LEVELUP, dcx, dcy, 30, 18);
    }
    else if (!was_boss)
    {
        // Same chance as the old ingredient drop, on purpose: the region's bar
        // is what paces the expedition, and swapping what the bar counts must
        // not change how long a region takes. The difference is what the bar
        // *means* -- these are seeds to plant, not ingredients for the cake.
        if ((int)(my_rand() % 100) < (int)monster_def(m->def_id)->drop_pct)
            spawn_gem(GEM_SEED, dcx + (int)(my_rand() % 9) - 4, dcy,
                      (uint16_t)(g_region_id + 1));
        // Brutes occasionally bleed a heart: the slow tank is the sustain
        // option as well as the damage threat.
        if (monster_def(m->def_id)->damage >= 2 && (my_rand() % 100) < 12)
            spawn_gem(3, dcx + (int)(my_rand() % 13) - 6, dcy, 1);
    }

    if (was_boss)
    {
        const RegionDef *rg = &g_regions[g_region_id];
        if (m->boss_id == rg->boss_id)
        {
            // Final Boss defeated! Drops 2 Boss Tokens (霸主印记)
            spawn_gem(2, dcx - 8, dcy, 1);
            spawn_gem(2, dcx + 8, dcy, 1);
            spawn_gem(3, dcx, dcy - 12, 1);
            if (g_skill_tree.harvest_lv >= 3)
                vacuum_all_gems(dcx, dcy);

            g_ingredients_done |= (uint8_t)(1 << g_region_id);
            audio_play_music(MUS_VICTORY);
            audio_play_sfx(SND_EXPLOSION);
            g_state = ST_LEVEL_CLEAR;
        }
        else
        {
            // Mid Boss defeated! Drops 1 Boss Token, a burst of ingredients
            // and a heart: the mid-fight is the region's half-time reward,
            // and it visibly moves the bar toward the duel.
            spawn_gem(2, dcx, dcy, 1);
            spawn_gem(3, dcx + 12, dcy, 1);
            for (int k = 0; k < 3; k++)
                spawn_gem(GEM_SEED, dcx + (int)(my_rand() % 25) - 12,
                          dcy + (int)(my_rand() % 25) - 12,
                          (uint16_t)(g_region_id + 1));
            for (int k = 0; k < 5; k++)
                spawn_gem(0, dcx + (int)(my_rand() % 21) - 10,
                             dcy + (int)(my_rand() % 21) - 10, 3);
            spawn_effect(FX_LEVELUP, dcx, dcy, 40, 26);
            show_toast("先锋倒下了！掉落了材料", PAL_UI_GOLD_LIGHT, 180);
            audio_play_sfx(SND_EXPLOSION);
            if (!g_boss_out)
                audio_play_music(region_battle_music());
        }
    }
    else
    {
        audio_play_sfx(SND_ENEMY_DIE);
    }
}

static void damage_monster(Monster *m, int damage, int knock_from_x,
                           int knock_from_y, bool knockback, bool show_num)
{
    if (!m->alive)
        return;

    // A boss mid-roar or underground is untouchable. Both states are drawn, so
    // the immunity is information rather than an invisible wall -- and it is
    // what makes the phase-change beat read as a moment rather than as a bug.
    if (m->is_boss && g_duel_active && duel_boss_immune())
        return;

    if (damage <= 0)
        damage = 1;

    // Linked duels: the client never simulates the boss, so its hits are
    // reported upstream instead of applied. The floater still plays locally,
    // which is all the feedback the person swinging needs.
    if (g_duel_active && g_multiplayer && !g_is_host && m->is_boss)
    {
        g_duel_hit_pending += (uint16_t)damage;
        spawn_floater(m->x, m->y, (int16_t)damage, FLOATER_HIT);
        m->hurt_timer = 8;
        return;
    }

    // The field twin of the branch above: monsters are the host's to
    // simulate, so a client's swings in the field are reported, not applied.
    // The client's mirrors are placed at the host's exact positions, so the
    // last hit position tells the host which creature was meant. Without
    // this, a co-op field run was one player fighting and one player
    // watching an invincible pet.
    if (g_multiplayer && !g_is_host && !m->recruited)
    {
        if (g_field_hit_pending <= 999 - damage)
            g_field_hit_pending = (uint16_t)(g_field_hit_pending + damage);
        else
            g_field_hit_pending = 999;
        g_field_hit_x = m->x;
        g_field_hit_y = m->y;
        spawn_floater(m->x, m->y, (int16_t)damage, FLOATER_HIT);
        m->hurt_timer = 8;
        return;
    }

    // 利爪's ultimate: a flat crit chance on everything. 力量之树Lv2: +15%暴击
    // 利爪 counts through a fusion too, so 百兽盟主 keeps the crit line alive.
    bool crit = false;
    if (card_eff_level(&g_heroes[g_local_hero], CARD_FANG) >= CARD_MAX_LEVEL &&
        (my_rand() % 100) < 25)
    {
        damage *= 2;
        crit = true;
    }
    else if (g_skill_tree.power_lv >= 2 && (my_rand() % 100) < 15)
    {
        damage *= 2;
        crit = true;
    }

    // 踉跄处决 (stagger). Bosses keep poise rather than knockback: every hit
    // inside the window chips it, and enough chips drop the boss to its knees.
    // 利爪 feeds this faster (it is the aggressive line), which is what makes
    // the damage card also a control card.
    if (m->is_boss && m->stagger_timer == 0 && m->stagger_immune == 0 &&
        !duel_boss_roaring())
    {
        if (m->stagger_window == 0)
        {
            m->stagger_window = STAGGER_WINDOW;
            m->stagger_hits = 0;
        }
        int feed = 1;
        if (card_unlocked(&g_heroes[g_local_hero], CARD_FANG))
            feed = 2;
        if (card_eff_level(&g_heroes[g_local_hero], CARD_FUSE_KING) > 0)
            feed = 3;
        m->stagger_hits = (uint8_t)(m->stagger_hits + feed);
        if (m->stagger_hits >= STAGGER_HITS)
        {
            m->stagger_window = 0;
            m->stagger_hits = 0;
            m->stagger_timer = STAGGER_FRAMES;
            spawn_effect(FX_LEVELUP, m->x, m->y, 40, 34);
            g_screen_shake = 5;
            // The biggest pause in the game, reserved for the moment the whole
            // aggressive line is built around: poise breaking is worthless if
            // the player does not notice it happened.
            hitstop_request(HITSTOP_BREAK);
            audio_play_sfx(SND_EXPLOSION);
            show_toast("霸主踉跄了！快打！", PAL_UI_GOLD_LIGHT, 150);
        }
    }

    // A staggered boss takes bonus damage: the reward for the aggressive line,
    // and the reason the window is worth spending cards on rather than saving.
    if (m->is_boss && m->stagger_timer > 0)
        damage = damage * STAGGER_DMG_NUM / STAGGER_DMG_DEN;

    if (show_num)
        spawn_floater(m->x, m->y, (int16_t)damage,
                      crit ? FLOATER_CRIT : FLOATER_HIT);

    if (m->hp <= damage)
    {
        m->hp = 0;
        // A kill gets a longer hold than a graze, an elite or a boss longer
        // still: the pause is proportional to what just happened.
        hitstop_request((m->is_boss || m->elite) ? HITSTOP_HEAVY : HITSTOP_KILL);
        monster_die(m);
        return;
    }

    m->hp -= (uint16_t)damage;
    m->hurt_timer = 8;
    hitstop_request(HITSTOP_HIT);

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
                         bool knockback, bool show_num)
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
            damage_monster(m, power, cx, cy, knockback, show_num);
            hits++;
        }
    }
    return hits;
}

// ---------------------------------------------------------------------------
// Capybara damage
// ---------------------------------------------------------------------------

// Bank progress toward the next ultimate. The field credits kills, the duel
// credits a clock and the blows the boss lands; both pay through here so the
// meter fills the same way no matter what earned it.
static void ult_credit(int index, int gain);

void hero_take_damage(int index, int damage, int from_x, int from_y)
{
    Hero *h = &g_heroes[index];
    if (!h->active || h->invuln > 0)
        return;

    // In a linked session the partner's capybara is simulated on its own
    // console: report the hit instead of writing into a mirror its owner
    // would overwrite on the next packet. This used to be duel-only, which
    // is why a co-op FIELD run had an invulnerable second player -- the
    // client now applies this amount through its own guard, invuln and
    // hearts, exactly as if the blow had landed at home.
    if (g_multiplayer && g_is_host && index != g_local_hero)
    {
        g_duel_hit_amt = (uint8_t)damage;
        return;
    }

    // 招架 (铁尾 Lv3+). A guard raised in time eats the hit outright -- the
    // whole point of the mechanic is that reading a telegraph and answering it
    // is strictly better than eating the blow, and it works against every
    // melee source in the game so the lesson transfers between phases.
    if (h->guard_timer > 0)
    {
        // A perfect parry is the first few frames: that is the reward for
        // answering the wind-up rather than pre-holding the button, and it is
        // the only way the card gives health back.
        bool perfect = h->guard_timer > GUARD_PERFECT_FRAMES;
        h->guard_success = true;
        h->guard_timer = 0;

        int32_t gx, gy;
        hero_center(h, &gx, &gy);
        spawn_effect(FX_TAIL, gx, gy, 14, 34);
        audio_play_sfx(SND_ATTACK);

        // 荆棘铁壁 (铁尾+厚皮): the riposte lands automatically and stuns, so a
        // defensive build still converts a read into damage.
        int bastion = card_eff_level(h, CARD_FUSE_BASTION);
        if (bastion > 0)
        {
            damage_around(from_x, from_y, 40, 8 + card_power(h, CARD_TAIL),
                          true, true);
            for (int i = 0; i < MAX_MONSTERS; i++)
            {
                Monster *m = &g_monsters[i];
                if (!m->alive || m->recruited || m->is_boss)
                    continue;
                int32_t mx, my;
                monster_center(m, &mx, &my);
                if (in_radius(mx, my, from_x, from_y, 40))
                {
                    m->state = 1;
                    m->state_timer = 60;
                }
            }
        }

        // A perfect parry on a normal 铁尾 buys a heart back, once the card is
        // mastered -- the sustain half of the guard, and what makes the
        // defensive line viable against the later bosses' damage.
        if (perfect && card_eff_level(h, CARD_TAIL) >= CARD_MAX_LEVEL &&
            h->hearts < h->hearts_max)
        {
            h->hearts++;
            spawn_floater(gx, gy - 12, 1, FLOATER_HEAL);
        }

        if (index == g_local_hero)
        {
            g_screen_shake = 3;
            show_toast("招架！", PAL_UI_GOLD_LIGHT, 60);
        }
        return;
    }

    // 快腿's ultimate: brief immunity while charging.
    if (card_eff_level(h, CARD_SWIFT) >= CARD_MAX_LEVEL && h->ram_timer > 0)
        return;
    // 冲撞's own guard (Lv3+): the charge body-checks through light fire.
    if (card_unlocked(h, CARD_RAM) && h->ram_timer > 0)
        return;

    int32_t cx, cy;
    hero_center(h, &cx, &cy);

    // 坚韧之树Lv1: 25%几率厚脂减伤，完全抵消此次伤害！
    if (g_skill_tree.tough_lv >= 1 && (my_rand() % 100) < 25)
    {
        spawn_effect(FX_RAM_DUST, cx, cy, 12, 16);
        return;
    }

    // The blow costs what it declared. Contact damage carries the monster's own
    // weight -- a brute's bite is two hearts, a swarm's is one -- so the
    // difference between the heavy, slow monsters and the light, fast ones is
    // something the player feels rather than something only the data table
    // knows. Callers that fire ranged chip damage pin it to one heart; see
    // HERO_MAX_DAMAGE_PER_HIT for why.
    int cost = damage;
    if (cost < 1)
        cost = 1;
    if (cost > HERO_MAX_DAMAGE_PER_HIT)
        cost = HERO_MAX_DAMAGE_PER_HIT;

    if (h->hearts > (uint8_t)cost)
        h->hearts = (uint8_t)(h->hearts - cost);
    else
        h->hearts = 0;

    // 敏捷之树Lv2: 受击无敌时间延长至90帧(+50%)
    h->invuln = (g_skill_tree.agi_lv >= 2) ? 90 : 60;
    // Duel only: the meter also feeds on getting hit, so an ultimate is a
    // once-per-duel card earned by surviving rather than a button that can
    // never fill because the arena holds no crowd to farm.
    if (g_duel_active)
        ult_credit(index, 2);
    // 厚皮 Lv3: the capybara is too thick to be staggered. It still takes the
    // heart, but it keeps its feet -- which in a duel is the difference between
    // losing one heart and losing the exchange.
    h->hurt_stun = card_unlocked(h, CARD_HIDE) ? 0 : 10;
    audio_play_sfx(SND_HURT);

    // 受击反馈：屏幕轻微震动 + 头顶惊慌受击汗滴特效
    if (index == g_local_hero)
    {
        // First pain is the moment "walk away from the crowd" becomes real;
        // the heart line answers the question the red bar just raised.
        if (!tutorial_done(TUT_B_HURT) && !g_duel_active)
        {
            tutorial_mark(TUT_B_HURT);
            show_toast("往怪少的地方走；红心能补回来",
                       PAL_UI_GOLD_LIGHT, 200);
        }
        g_screen_shake = 5;
        // Only the local capybara's own hit stops the clock. The partner's is
        // shown, not felt -- freezing a shared screen for someone else's
        // mistake would cost the player who did not make it.
        hitstop_request(HITSTOP_PLAYER);
    }
    spawn_effect(FX_HIT, cx, cy - 8, 14, 10);

    // 击退受力反馈: a decaying shove applied over the next few frames (see
    // hero_update), so the hit pushes instead of teleporting.
    int dx = (cx > from_x) ? 4 : -4;
    int dy = (cy > from_y) ? 4 : -4;
    if (iabs(cx - from_x) < 8) dx = 0;
    if (iabs(cy - from_y) < 8) dy = 0;
    h->kb_vx = (int8_t)dx;
    h->kb_vy = (int8_t)dy;
    h->kb_timer = 6;

    // In a linked duel the partner's capybara is simulated over there; flag
    // the hit so the client can play its own hurt reaction (net.c carries it).
    // (Handled at the top of this function since the field gained the same
    // channel.)

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
                damage_monster(m, 4, cx, cy, true, true);
        }
    }

    if (h->hearts == 0 && index == g_local_hero && g_hero_death_beat == 0)
    {
        // Soft failure, kept from the original design: going down ends the run
        // but never the game. Ingredients already secured stay secured, so a
        // birthday playthrough cannot be bricked by one bad region.
        // (This used to play the victory fanfare -- the one sound in the bank
        // that says the opposite of what just happened.)
        // Losing the duel is remembered: the boss stays summoned for this
        // region, so the rematch starts at the arena door, not ten minutes
        // of re-farming the meter it already paid for.
        if (g_duel_active)
            g_duel_pending = (int8_t)g_region_id;
        // The fall is an event, not a screen swap: the world holds for a beat
        // while the defeat burst plays out under the frozen hero, and only
        // then does the game-over plate arrive (the countdown lives with the
        // field and duel updates).
        g_hero_death_beat = 55;
        h->hurt_stun = 55;
        h->invuln = 70;
        spawn_effect(FX_KILL, cx, cy, 30, 16);
        g_screen_shake = 8;
        audio_stop_music();
        audio_play_sfx(SND_EXPLOSION);
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

    int level = card_eff_level(h, CARD_TEETH);
    int range = BITE_RANGE + level * 4;
    // The duel boss is a tower of a creature; the bite reaches up into its
    // lower body so hugging the boss is a real, if risky, tactic.
    if (g_duel_active)
        range += 14;
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
                damage_monster(m, power, cx, cy, true, true);
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

// ---------------------------------------------------------------------------
// The duel form of the cards
//
// A card keeps its identity between the two phases; what changes is who does
// the timing. In the field the capybara fights on its own and the card fires on
// a timer; in the arena the player has the controller, so the same card becomes
// a move. The mechanic does not change -- 铁尾 interrupts a wind-up in the
// field and catches a swing in the arena, and both are "guard" -- because a
// build the player learned during the swarm phase has to still mean something
// when the boss walks in.
//
// Level 3 is where a card stops being a body and becomes an answer (see
// CARD_UNLOCK_LEVEL / card_unlocked). Below it a card still does its damage and
// still scales; it simply has no mechanic yet, which is what makes the third
// level of a card a real decision at the picker.
// ---------------------------------------------------------------------------

// A short forward lunge, used by 尖牙 and by 掠食突袭. Returns true if the hero
// actually moved, so the caller can decide whether to spend the cooldown.
static bool duel_lunge(Hero *h, int frames, int speed)
{
    int dir = (h->facing == DIR_LEFT) ? -1 : 1;
    bool moved = false;
    for (int k = 0; k < frames; k++)
    {
        int32_t nx = h->x + dir * speed;
        int32_t cx = nx + HERO_SPRITE_W / 2;
        if (cx < DUEL_X_MIN || cx > DUEL_X_MAX)
            break;
        h->x = nx;
        moved = true;
    }
    if (moved)
        spawn_effect(FX_RAM_DUST, h->x + 16, h->y + 28, 12, 10);
    return moved;
}

// Everything the capybara can hit this frame, as a single pass. The duel has
// exactly one boss, so "hit what is in front of me" is a rectangle test against
// its body rather than a search through a crowd.
static int duel_hit_in_front(Hero *h, int reach, int height, int power,
                             bool heavy)
{
    Monster *b = duel_boss();
    if (!b || !b->alive)
        return 0;
    if (g_duel.burrowed)
        return 0;   // it is underground; swinging at the floor costs the turn

    int32_t cx, cy;
    hero_center(h, &cx, &cy);
    int dir = (h->facing == DIR_LEFT) ? -1 : 1;

    // The boss is a tall body: the box reaches from its feet up through its
    // torso, so a ground attack connects the way it looks like it should.
    int32_t bx = b->x, by = b->y;
    int32_t dx = iabs(bx - cx);
    int32_t dy = iabs(by - cy);
    if (dx > reach + 30 || dy > height)
        return 0;
    // And it has to be in front, not behind.
    if ((bx - cx) * dir < -20)
        return 0;

    damage_monster(b, power, cx, cy, false, true);
    if (heavy)
    {
        // A heavy blow is a distinct thing from a fast one: it buys the boss a
        // bigger poise hit, which is how the aggressive cards pay into the
        // stagger rather than merely into the health bar.
        b->stagger_hits = (uint8_t)(b->stagger_hits + 2);
    }
    return 1;
}

// 尖牙's duel form: a pounce. Level 1-2 is a single lunging bite; from level 3
// the pounce leaves the capybara primed, and from level 5 the bite chains up to
// three times if the player keeps pressing.
static void duel_card_teeth(int index)
{
    Hero *h = &g_heroes[index];
    int lvl = card_eff_level(h, CARD_TEETH);
    int power = card_power(h, CARD_TEETH);
    bool local = (index == g_local_hero);

    duel_lunge(h, 5, 3);
    h->bite_timer = 12;

    // The three-hit chain (Lv5). Each press inside the window advances the
    // chain; the third bite is the heavy one that feeds the stagger.
    if (lvl >= CARD_MAX_LEVEL && h->combo_window > 0)
    {
        h->combo_step = (uint8_t)((h->combo_step + 1) % 3);
    }
    else
    {
        h->combo_step = 0;
    }
    bool finisher = (h->combo_step == 2);
    if (lvl >= CARD_MAX_LEVEL)
        h->combo_window = 30;

    duel_hit_in_front(h, 34, 40, finisher ? power * 2 : power, finisher);

    // Lv3+: the pounce sets up the next blow instead of ending the exchange.
    if (lvl >= CARD_UNLOCK_LEVEL)
    {
        spawn_effect(FX_BITE, h->x + 16 + (h->facing == DIR_LEFT ? -22 : 22),
                     h->y + 16, 10, 12);
    }
    if (local) audio_play_sfx(SND_ATTACK);
}

// 铁尾's duel form: the guard. This is the card's level-3 mechanic and the
// reason a defensive build works at all -- it converts a read into immunity
// plus a counter, and it is deliberately the same mechanic the field form uses
// to interrupt a wind-up.
static void duel_card_tail(int index)
{
    Hero *h = &g_heroes[index];
    int lvl = card_eff_level(h, CARD_TAIL);

    if (lvl < CARD_UNLOCK_LEVEL)
    {
        // Below the unlock it is only a sweep: damage and knockback, no guard.
        int power = card_power(h, CARD_TAIL);
        duel_hit_in_front(h, 44, 34, power, false);
        spawn_effect(FX_TAIL, h->x + 16, h->y + 16, 12, 44);
        if (index == g_local_hero) audio_play_sfx(SND_ATTACK);
        return;
    }

    // Level 3+: raise the guard. The window is short and the cooldown is real,
    // so it has to be an answer rather than a stance.
    h->guard_timer = GUARD_FRAMES;
    h->guard_cd = GUARD_COOLDOWN;
    spawn_effect(FX_TAIL, h->x + 16, h->y + 16, GUARD_FRAMES, 30);
    if (index == g_local_hero) audio_play_sfx(SND_UI_MOVE);
}

// 冲撞's duel form: the charge. Level 3 adds i-frames to the dash itself; level
// 5 makes the contact a body-check that staggers whatever it meets, which is
// the answer to a boss that is mid-charge.
static void duel_card_ram(int index)
{
    Hero *h = &g_heroes[index];
    int lvl = card_eff_level(h, CARD_RAM);
    int power = card_power(h, CARD_RAM);
    bool local = (index == g_local_hero);

    h->ram_timer = (uint16_t)(10 + lvl * 2);
    h->ram_dx = (h->facing == DIR_LEFT) ? -1 : 1;
    if (lvl >= CARD_UNLOCK_LEVEL && h->invuln < 12)
        h->invuln = 12;

    // The contact test happens here and again during the dash (hero_update),
    // so a charge that connects on frame 1 still pays.
    Monster *b = duel_boss();
    if (b && b->alive && !g_duel.burrowed)
    {
        int32_t cx, cy;
        hero_center(h, &cx, &cy);
        if (iabs(b->x - cx) < 40 && iabs(b->y - cy) < 44)
        {
            damage_monster(b, power, cx, cy, false, true);
            if (lvl >= CARD_MAX_LEVEL)
            {
                // The body-check: a big poise hit, the counter to a charging
                // boss, and the reason the aggressive line can break poise.
                b->stagger_hits = (uint8_t)(b->stagger_hits + 3);
                g_screen_shake = 4;
                show_toast("对撞！", PAL_UI_GOLD_LIGHT, 60);
            }
        }
    }
    spawn_effect(FX_RAM_DUST, h->x + 16, h->y + 28, 16, 20);
    if (local) audio_play_sfx(SND_ATTACK);
}

// 水柱's duel form: a chargeable piercing shot. Holding the button winds it up;
// releasing fires. Level 3 pierces the whole arena; level 5 cancels enemy
// projectiles it passes through, which is the answer to a bullet-heavy boss.
static void duel_card_water(int index)
{
    Hero *h = &g_heroes[index];
    int lvl = card_eff_level(h, CARD_WATER);
    int power = card_power(h, CARD_WATER);

    // Fire along the facing, at the boss's body height rather than the
    // capybara's: the arena is a side view and a horizontal shot should hit.
    int32_t cx, cy;
    hero_center(h, &cx, &cy);
    int dir = (h->facing == DIR_LEFT) ? -1 : 1;
    int16_t vx = (int16_t)(dir * 8);

    if (lvl >= CARD_UNLOCK_LEVEL)
    {
        // A charged shot pierces: it travels the arena and hits whatever it
        // passes, and at Lv5 it also eats the shots it crosses.
        spawn_projectile(PROJ_WATER, cx, cy, vx, 0, 90, power, 9);
    }
    else
    {
        spawn_projectile(PROJ_WATER, cx, cy, vx, 0, 50, power, 6);
    }
    if (index == g_local_hero)
        audio_play_sfx(SND_MAGIC_FIRE);
}

// 灵水's duel form: the orbiting orbs stay up and, from level 3, they intercept.
// The interception itself lives in the projectile update (an orb eats a shot),
// so this only re-arms; the card is passive in pace but very much present in
// the fight.
static void duel_card_mud(int index)
{
    Hero *h = &g_heroes[index];
    int lvl = card_eff_level(h, CARD_MUD);
    if (lvl <= 0)
        return;

    // The burst: spend the orbs for a nova around the capybara. A full ring
    // does real damage, which is what makes saving the orbs a decision.
    int orbs = h->water_orbs;
    if (orbs <= 0)
        return;
    int power = card_power(h, CARD_MUD);
    int32_t cx, cy;
    hero_center(h, &cx, &cy);
    spawn_effect(FX_PACIFY, cx, cy, 16, (uint8_t)(28 + orbs * 6));
    damage_around(cx, cy, 28 + orbs * 6, power, false, true);
    h->water_orbs = 0;
    h->orb_regen_timer = 60;
    if (index == g_local_hero) audio_play_sfx(SND_MAGIC_ICE);
}

// 便便's duel form: a pool at the capybara's feet. Level 3 makes it poisonous,
// level 5 lets a second press detonate everything it has left. The pool is the
// answer to a boss that walks the arena, because it claims ground.
static void duel_card_dung(int index)
{
    Hero *h = &g_heroes[index];
    int32_t cx, cy;
    hero_center(h, &cx, &cy);

    // Second press with an armed pool: blow it. Everything standing in the
    // pool takes the stored damage.
    if (h->dung_armed > 0)
    {
        spawn_effect(FX_STINK, cx, cy, 20, 40);
        damage_around(cx, cy, 44, card_power(h, CARD_DUNG) * 2, false, true);
        g_screen_shake = 4;
        h->dung_armed = 0;
        if (index == g_local_hero) audio_play_sfx(SND_EXPLOSION);
        return;
    }

    spawn_effect(FX_DUNG_TRAP, cx, DUEL_GROUND_Y - 6, 180, 16);
    h->dung_armed = 1;
    if (index == g_local_hero) audio_play_sfx(SND_ATTACK);
}

// 安抚's duel form: the song. Level 3 shortens the boss's next wind-up; from
// level 4 it lengthens it instead -- the card's whole identity is buying the
// player time to read, so in a duel it does exactly that.
static void duel_card_pacify(int index)
{
    Hero *h = &g_heroes[index];
    int lvl = card_eff_level(h, CARD_PACIFY);
    int32_t cx, cy;
    hero_center(h, &cx, &cy);
    spawn_effect(FX_PACIFY, cx, cy, 40, 90);

    Monster *b = duel_boss();
    if (b && b->alive)
    {
        // Lengthen whatever the boss is winding up right now. This is the
        // single most direct "help me read this" button in the game.
        if (g_duel.fsm == DB_WIND && lvl >= 4)
            g_duel.fsm_t = (uint16_t)(g_duel.fsm_t + 12);
        else if (lvl >= CARD_UNLOCK_LEVEL && b->stagger_window == 0)
        {
            // Below Lv4 it instead opens a small poise window, so the card is
            // never a dead press.
            b->stagger_window = 90;
            b->stagger_hits = (uint8_t)(b->stagger_hits + 1);
        }
    }

    // Its field role carries over: calms the boss's summoned adds outright.
    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        Monster *m = &g_monsters[i];
        if (!m->alive || m->is_boss || m->recruited)
            continue;
        m->pacify_timer = 300;
    }
    if (index == g_local_hero) audio_play_sfx(SND_HEAL);
}

// 感化's duel form: charm. In the arena there is nothing to convert except what
// the boss itself calls in, so the card turns those -- and when there is nothing
// to turn it spends the charge on a poise window instead.
static void duel_card_recruit(int index)
{
    Hero *h = &g_heroes[index];
    int lvl = card_eff_level(h, CARD_RECRUIT);
    bool local = (index == g_local_hero);

    // Level 5 turns the add instantly; below it, the charge still has to build.
    int need = (lvl >= CARD_MAX_LEVEL) ? 1 : 2;
    if (h->recruit_charge < need)
    {
        h->recruit_charge++;
        if (local) audio_play_sfx(SND_UI_MOVE);
        return;
    }

    h->recruit_charge = 0;
    bool converted = false;
    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        Monster *m = &g_monsters[i];
        if (!m->alive || m->is_boss || m->recruited)
            continue;
        m->recruited = true;
        m->hp = m->hp_max;
        m->pacify_timer = 0;
        spawn_effect(FX_PACIFY, m->x, m->y, 30, 16);
        converted = true;
        // 百兽盟主: a charmed add fights twice as hard.
        if (card_eff_level(h, CARD_FUSE_KING) > 0)
            m->hp_max = (uint16_t)(m->hp_max * 2);
        break;
    }

    // Nothing to charm is not a wasted press: the song still lands on the boss
    // as an opening, so the card always does something in the arena.
    if (!converted)
    {
        Monster *b = duel_boss();
        if (b && b->alive)
        {
            b->stagger_window = STAGGER_WINDOW;
            b->stagger_hits = (uint8_t)(b->stagger_hits + 2);
            show_toast("魅惑！", PAL_HEAL_OUTER, 60);
        }
    }
    if (lvl >= CARD_MAX_LEVEL && h->hearts < h->hearts_max)
        h->hearts++;
    if (local) audio_play_sfx(SND_HEAL);
}

// ---------------------------------------------------------------------------
// The fused cards' duel forms
//
// A fusion is not a fifth card bolted on: it is both parents at once, so each
// one below performs its two halves in a single action. That is what makes the
// fusion read as a combination rather than as a replacement.
// ---------------------------------------------------------------------------

// 掠食突袭 (尖牙+冲撞): a long invulnerable dive that lands behind the boss.
static void duel_fuse_predator(int index)
{
    Hero *h = &g_heroes[index];
    int power = card_power(h, CARD_FUSE_PREDATOR);

    if (h->invuln < 16)
        h->invuln = 16;
    duel_lunge(h, 12, 3);
    h->bite_timer = 14;
    duel_hit_in_front(h, 44, 46, power, true);

    // It ends up facing back the way it came, which is the "behind you" of the
    // description and a real repositioning tool.
    h->facing = (h->facing == DIR_LEFT) ? DIR_RIGHT : DIR_LEFT;
    spawn_effect(FX_BITE, h->x + 16, h->y + 16, 12, 18);
    spawn_effect(FX_RAM_DUST, h->x + 16, h->y + 28, 16, 22);
    if (index == g_local_hero) audio_play_sfx(SND_ATTACK);
}

// 荆棘铁壁 (铁尾+厚皮): the guard, plus an automatic riposte when it catches.
// The riposte itself lives in hero_take_damage; the press just raises it.
static void duel_fuse_bastion(int index)
{
    Hero *h = &g_heroes[index];
    h->guard_timer = GUARD_FRAMES + 6;
    h->guard_cd = GUARD_COOLDOWN - 20;
    spawn_effect(FX_TAIL, h->x + 16, h->y + 16, GUARD_FRAMES, 36);
    if (index == g_local_hero) audio_play_sfx(SND_UI_MOVE);
}

// 潮汐双炮 (水柱+灵水): a piercing shot that also spends an orb, so the two
// halves fire as one. A full ring makes it a double volley.
static void duel_fuse_tide(int index)
{
    Hero *h = &g_heroes[index];
    int power = card_power(h, CARD_FUSE_TIDE);
    int32_t cx, cy;
    hero_center(h, &cx, &cy);
    int dir = (h->facing == DIR_LEFT) ? -1 : 1;

    spawn_projectile(PROJ_WATER, cx, cy, (int16_t)(dir * 8), 0, 90, power, 9);
    if (h->water_orbs > 0)
    {
        h->water_orbs--;
        spawn_projectile(PROJ_WATER, cx, cy - 10, (int16_t)(dir * 7), -1,
                         80, power, 7);
        spawn_projectile(PROJ_WATER, cx, cy + 10, (int16_t)(dir * 7), 1,
                         80, power, 7);
    }
    if (index == g_local_hero) audio_play_sfx(SND_MAGIC_FIRE);
}

// 恶臭香氛 (粪球+安抚): the pool plus the song, in one press.
static void duel_fuse_miasma(int index)
{
    Hero *h = &g_heroes[index];
    int32_t cx, cy;
    hero_center(h, &cx, &cy);

    spawn_effect(FX_DUNG_TRAP, cx, DUEL_GROUND_Y - 6, 200, 18);
    spawn_effect(FX_STINK, cx, DUEL_GROUND_Y - 10, 40, 46);
    damage_around(cx, DUEL_GROUND_Y - 10, 46, card_power(h, CARD_FUSE_MIASMA),
                  false, true);

    // The 安抚 half: the boss's next wind-up is stretched while it stands in
    // the reek, which is the fusion's whole idea -- control, not damage.
    Monster *b = duel_boss();
    if (b && b->alive && iabs(b->x - cx) < 60)
    {
        if (g_duel.fsm == DB_WIND)
            g_duel.fsm_t = (uint16_t)(g_duel.fsm_t + 16);
        b->stagger_window = STAGGER_WINDOW;
    }
    for (int i = 0; i < MAX_MONSTERS; i++)
        if (g_monsters[i].alive && !g_monsters[i].is_boss)
            g_monsters[i].pacify_timer = 300;

    if (index == g_local_hero) audio_play_sfx(SND_ATTACK);
}

// 百兽盟主 (感化+利爪): charm every add at once, and feed the stagger hard.
static void duel_fuse_king(int index)
{
    Hero *h = &g_heroes[index];
    bool any = false;
    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        Monster *m = &g_monsters[i];
        if (!m->alive || m->is_boss || m->recruited)
            continue;
        m->recruited = true;
        m->hp = m->hp_max;
        m->hp_max = (uint16_t)(m->hp_max * 2);
        m->pacify_timer = 0;
        spawn_effect(FX_PACIFY, m->x, m->y, 30, 16);
        any = true;
    }
    Monster *b = duel_boss();
    if (b && b->alive)
    {
        b->stagger_window = STAGGER_WINDOW;
        b->stagger_hits = (uint8_t)(b->stagger_hits + 4);
    }
    (void)any;
    if (h->hearts < h->hearts_max)
        h->hearts++;
    if (index == g_local_hero) audio_play_sfx(SND_HEAL);
}

// 疾风吸取 (快腿+磁吸): a roll with a longer i-frame window that also inhales
// whatever is on the floor. Mobility and sustain in one button.
static void duel_fuse_gale(int index)
{
    Hero *h = &g_heroes[index];
    // The roll itself is started by the caller (the roll is the movement verb);
    // what the fusion adds is the shove and the vacuum.
    if (h->invuln < DUEL_ROLL_INVULN + 4)
        h->invuln = DUEL_ROLL_INVULN + 4;
    h->roll_timer = DUEL_ROLL_FRAMES;
    h->roll_cd = DUEL_ROLL_CD / 2;
    h->roll_dir = (h->facing == DIR_LEFT) ? -1 : 1;

    int32_t cx, cy;
    hero_center(h, &cx, &cy);
    spawn_effect(FX_PACIFY, cx, cy, 20, 60);
    damage_around(cx, cy, 60, card_power(h, CARD_FUSE_GALE), false, true);
    for (int i = 0; i < MAX_GEMS; i++)
        if (g_gems[i].alive)
        {
            g_gems[i].x = cx;
            g_gems[i].y = cy;
        }
    if (index == g_local_hero) audio_play_sfx(SND_MAGIC_ICE);
}

// 沸泉地雷 (水柱+粪球): a shot that detonates the pool it lands in.
static void duel_fuse_geyser(int index)
{
    Hero *h = &g_heroes[index];
    int power = card_power(h, CARD_FUSE_GEYSER);
    int32_t cx, cy;
    hero_center(h, &cx, &cy);
    int dir = (h->facing == DIR_LEFT) ? -1 : 1;

    spawn_projectile(PROJ_WATER, cx, cy, (int16_t)(dir * 8), 0, 70, power, 8);

    // The 粪球 half: leave a pool behind, and blow it immediately if one was
    // already armed. The two cards take turns rather than stacking, which
    // keeps the button's meaning stable.
    if (h->dung_armed > 0)
    {
        damage_around(cx, DUEL_GROUND_Y - 10, 50, power * 2, false, true);
        spawn_effect(FX_STINK, cx, DUEL_GROUND_Y - 10, 24, 44);
        g_screen_shake = 4;
        h->dung_armed = 0;
    }
    else
    {
        spawn_effect(FX_DUNG_TRAP, cx, DUEL_GROUND_Y - 6, 160, 16);
        h->dung_armed = 1;
    }
    if (index == g_local_hero) audio_play_sfx(SND_MAGIC_FIRE);
}

// 回旋冲角 (铁尾+冲撞): the charge, and the guard waiting at the end of it.
static void duel_fuse_whirl(int index)
{
    Hero *h = &g_heroes[index];
    int power = card_power(h, CARD_FUSE_WHIRL);

    h->ram_timer = 14;
    h->ram_dx = (h->facing == DIR_LEFT) ? -1 : 1;
    if (h->invuln < 10)
        h->invuln = 10;

    Monster *b = duel_boss();
    if (b && b->alive && !g_duel.burrowed)
    {
        int32_t cx, cy;
        hero_center(h, &cx, &cy);
        if (iabs(b->x - cx) < 46 && iabs(b->y - cy) < 44)
        {
            damage_monster(b, power, cx, cy, false, true);
            b->stagger_hits = (uint8_t)(b->stagger_hits + 2);
        }
    }

    // The 铁尾 half: the guard opens at the end of the charge, so the move
    // cannot simply be punished by whatever the boss was already doing.
    h->guard_timer = GUARD_FRAMES;
    h->guard_cd = GUARD_COOLDOWN;
    spawn_effect(FX_RAM_DUST, h->x + 16, h->y + 28, 14, 20);
    spawn_effect(FX_TAIL, h->x + 16, h->y + 16, 12, 30);
    if (index == g_local_hero) audio_play_sfx(SND_ATTACK);
}

// The duel dispatcher. One entry per card that can be used by hand; a card at
// level 0 is simply not there, and a passive has no branch at all.
static void duel_fire_card(int index, int card_id)
{
    Hero *h = &g_heroes[index];
    if (card_eff_level(h, card_id) <= 0)
        return;

    switch (card_id)
    {
    case CARD_TEETH:   duel_card_teeth(index);   break;
    case CARD_TAIL:    duel_card_tail(index);    break;
    case CARD_RAM:     duel_card_ram(index);     break;
    case CARD_WATER:   duel_card_water(index);   break;
    case CARD_MUD:     duel_card_mud(index);     break;
    case CARD_DUNG:    duel_card_dung(index);    break;
    case CARD_PACIFY:  duel_card_pacify(index);  break;
    case CARD_RECRUIT: duel_card_recruit(index); break;

    case CARD_FUSE_PREDATOR: duel_fuse_predator(index); break;
    case CARD_FUSE_BASTION:  duel_fuse_bastion(index);  break;
    case CARD_FUSE_TIDE:     duel_fuse_tide(index);     break;
    case CARD_FUSE_MIASMA:   duel_fuse_miasma(index);   break;
    case CARD_FUSE_KING:     duel_fuse_king(index);     break;
    case CARD_FUSE_GALE:     duel_fuse_gale(index);     break;
    case CARD_FUSE_GEYSER:   duel_fuse_geyser(index);   break;
    case CARD_FUSE_WHIRL:    duel_fuse_whirl(index);    break;

    default:
        return;   // passives have no active form
    }

    h->cooldown[card_id] = card_duel_cd(h, card_id);
}


static void fire_card(int index, int card_id);

static void hero_manual_bite(int index)


{
    Hero *h = &g_heroes[index];
    if (!h->active || h->hurt_stun > 0)
        return;

    int32_t cx, cy;
    hero_center(h, &cx, &cy);

    int level = card_eff_level(h, CARD_TEETH);
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
    damage_around(tx, ty, 20, power, true, true);

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
    if (card_eff_level(h, cid) <= 0)
        return;

    // In the duel a card is a fighting-game move, so it answers to the
    // arena's clock (card_duel_cd) rather than the field's; the field keeps the
    // automatic cadence. Both read the same card, the same level and the same
    // unlock gate -- only the pacing and the performance differ.
    if (g_duel_active)
    {
        if (h->cooldown[cid] > 0)
            return;
        duel_fire_card(index, cid);
        return;
    }

    if (h->cooldown[cid] > 0)
        return;

    if (cid == CARD_TEETH)
    {
        hero_manual_bite(index);
    }
    else
    {
        fire_card(index, cid);
        h->cooldown[cid] = card_cd(h, cid);
        if (index == g_local_hero)
            audio_play_sfx(SND_ATTACK);
    }
}

static int s_touch_drag_from = -1;

// ---------------------------------------------------------------------------
// Ultimates (大招)
//
// Charged by kills, released with R or by tapping the button on the bottom
// screen. The two capybaras do different things with it, because the two cards
// being identical would waste the fact that they are different characters:
//
//   公水豚  怒涛冲击   a shockwave that tears through everything on screen
//   母水豚  温柔庇护   heals to full, calms the field, gathers every drop
//
// If both capybaras release inside ULT_COMBO_WINDOW frames of each other the
// second one lands as 水豚合击 instead: both effects plus a stun, which is what
// makes playing as a pair worth more than playing as two solos.
// ---------------------------------------------------------------------------

// Everything hostile within roughly one screen of the capybara. The ultimate is
// a way out of a crowd you are already in, not a way to clear the map from a
// corner, so it deliberately does not reach further than the camera does.
static bool ult_in_reach(int32_t cx, int32_t cy, int32_t mx, int32_t my)
{
    return iabs(mx - cx) <= AREA_W / 2 + 32 &&
           iabs(my - cy) <= AREA_H / 2 + 32;
}

static void ult_shockwave(int32_t cx, int32_t cy, int damage)
{
    spawn_effect(FX_TAIL, cx, cy, 60, 128);

    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        Monster *m = &g_monsters[i];
        if (!m->alive || !monster_is_hostile(m))
            continue;
        int32_t mx, my;
        monster_center(m, &mx, &my);
        if (!ult_in_reach(cx, cy, mx, my))
            continue;
        damage_monster(m, m->is_boss ? damage * 2 : damage, cx, cy, true,
                       true);
    }
}

static void ult_shelter(int index, int32_t cx, int32_t cy, int calm_frames)
{
    Hero *h = &g_heroes[index];
    h->hearts = h->hearts_max;
    h->invuln = 90;
    spawn_effect(FX_PACIFY, cx, cy, 60, 128);

    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        Monster *m = &g_monsters[i];
        if (!m->alive || !monster_is_hostile(m) || m->is_boss)
            continue;
        int32_t mx, my;
        monster_center(m, &mx, &my);
        if (!ult_in_reach(cx, cy, mx, my))
            continue;
        // 安抚's own effect, at a strength the card cannot reach: the ultimate
        // is meant to buy real breathing room.
        m->pacify_timer = (uint16_t)calm_frames;
    }
    vacuum_all_gems(cx, cy);
}

void hero_release_ultimate(int index)
{
    Hero *h = &g_heroes[index];
    if (!h->active || !h->ult_ready)
        return;

    h->ult_ready = false;
    h->ult_charge = 0;
    h->ult_flash = 30;

    int32_t cx, cy;
    hero_center(h, &cx, &cy);

    // A combo is the partner having released a moment ago -- the window is
    // short enough that it has to be deliberate, not accidental.
    int partner = index ^ 1;
    bool combo = g_heroes[partner].active && g_heroes[partner].ult_window > 0;
    h->ult_window = ULT_COMBO_WINDOW;

    if (combo)
    {
        audio_play_sfx(SND_EXPLOSION);
        // Both halves of the pair's kit, plus the stun neither one has alone.
        ult_shockwave(cx, cy, 90);
        ult_shelter(index, cx, cy, 420);
        for (int i = 0; i < MAX_MONSTERS; i++)
        {
            Monster *m = &g_monsters[i];
            if (!m->alive || !monster_is_hostile(m) || m->is_boss)
                continue;
            int32_t mx, my;
            monster_center(m, &mx, &my);
            if (!ult_in_reach(cx, cy, mx, my))
                continue;
            m->state = 1;
            m->state_timer = 120;
        }
        return;
    }

    audio_play_sfx(SND_MAGIC_ICE);
    if (h->variant & 1)
        ult_shelter(index, cx, cy, 300);
    else
        ult_shockwave(cx, cy, 60);

    // In a linked game the host owns monster simulation, so a client release
    // has to be applied over there. Harmless outside a session: net.c ignores
    // it unless this console is the client half of a live link.
    net_notify_ultimate();
}

// Bank progress toward the next ultimate. The field credits kills, the duel
// credits a clock and the blows the boss lands; both pay through here so the
// meter fills the same way no matter what earned it.
static void ult_credit(int index, int gain)
{
    Hero *h = &g_heroes[index];
    if (!h->active || h->ult_ready)
        return;
    h->ult_charge = (uint16_t)(h->ult_charge + gain);
    if (h->ult_charge < ULT_CHARGE_MAX)
        return;
    h->ult_charge = ULT_CHARGE_MAX;
    h->ult_ready = true;
    if (index == g_local_hero)
    {
        audio_play_sfx(SND_LEVELUP);
        int32_t hx, hy;
        hero_center(h, &hx, &hy);
        spawn_effect(FX_LEVELUP, hx, hy, 40, 22);
        // The meter is easy to watch and easier to ignore; the first time it
        // fills, the game says what the R button has been for. In a linked
        // session both bars fill together, so that same beat teaches the
        // combo instead -- the one move two players should know to aim for.
        if (!tutorial_done(TUT_B_ULT))
        {
            tutorial_mark(TUT_B_ULT);
            if (g_multiplayer && g_heroes[1].active &&
                !tutorial_done(TUT_B_COMBO) && g_heroes[1 ^ g_local_hero].ult_ready)
            {
                tutorial_mark(TUT_B_COMBO);
                show_toast("两人接连按 R，就是水豚合击！",
                           PAL_UI_GOLD_LIGHT, 240);
            }
            else
            {
                show_toast("大招满了！按 R 释放", PAL_UI_GOLD_LIGHT, 240);
            }
        }
        else if (!tutorial_done(TUT_B_COMBO) && g_multiplayer &&
                 g_heroes[1].active && g_heroes[1 ^ g_local_hero].ult_ready)
        {
            tutorial_mark(TUT_B_COMBO);
            show_toast("两人接连按 R，就是水豚合击！",
                       PAL_UI_GOLD_LIGHT, 240);
        }
    }
}

// Charge accrual and the combo window both tick on the game clock.
static void hero_update_ultimate(int index)
{
    Hero *h = &g_heroes[index];
    if (h->ult_flash > 0)
        h->ult_flash--;
    if (h->ult_window > 0)
        h->ult_window--;

    // Duel trickle: the arena holds three minions at most, so kills alone can
    // never fill the meter there. One kill's worth every second and a half,
    // plus two per hit taken (hero_take_damage), lands a release roughly once
    // per fight -- a deliberate spend, not a dead button.
    if (g_duel_active && (g_frame % 90) == 0)
        ult_credit(index, 1);
}

static void hero_handle_active_skills(int index)
{
    Hero *h = &g_heroes[index];
    if (!h->active || h->hurt_stun > 0 || h->remote)
        return;

    uint16_t down = input_down();
    uint16_t held = input_held();

    if (g_duel_active)
    {
        // Duel controls: A and B belong to jump and roll (see hero_update),
        // so the skill buttons are X, Y and L -- plus every slot stays
        // tappable on the bottom screen. R is still the ultimate.
        if (down & KEY_X)
            hero_cast_slot(index, SKILL_SLOT_A);
        if (down & KEY_Y)
            hero_cast_slot(index, SKILL_SLOT_B);
        if (down & KEY_L)
            hero_cast_slot(index, SKILL_SLOT_X);
        if ((down & KEY_Y) == 0 && (held & KEY_L) && (g_frame % 20 == 0))
            hero_cast_slot(index, SKILL_SLOT_X);
        if (down & KEY_R)
            hero_release_ultimate(index);

        if (input_touch_pressed())
        {
            if (input_touch_in(ULT_BTN_X, ULT_BTN_Y, ULT_BTN_W, ULT_BTN_H))
            {
                hero_release_ultimate(index);
                return;
            }
            for (int s = 0; s < 4; s++)
            {
                if (input_touch_in(SLOT_X(s), SLOT_Y, SLOT_W, SLOT_H))
                {
                    hero_cast_slot(index, s);
                    break;
                }
            }
        }
        return;
    }

    // 1. Hardware Buttons: A, B, X, Y (plus L for B). R belongs to the
    //    ultimate, which is a different kind of decision from the four slots --
    //    it is the one button that is always worth saving.
    if (down & KEY_A)
        hero_cast_slot(index, SKILL_SLOT_A);
    if (down & (KEY_B | KEY_L))
        hero_cast_slot(index, SKILL_SLOT_B);
    if (down & KEY_X)
        hero_cast_slot(index, SKILL_SLOT_X);
    if ((down & KEY_Y) || ((held & KEY_Y) && (g_frame % 20 == 0)))
        hero_cast_slot(index, SKILL_SLOT_Y);

    if (down & KEY_R)
        hero_release_ultimate(index);

    // 2. Touch Screen: tap slot box to cast, drag to another slot to swap!
    //    The ultimate has its own button above the bar (see hud_draw).
    if (input_touch_pressed())
    {
        if (input_touch_in(ULT_BTN_X, ULT_BTN_Y, ULT_BTN_W, ULT_BTN_H))
        {
            hero_release_ultimate(index);
            return;
        }

        // The slot rectangles come from game.h, which is also what hud_draw
        // lays them out from -- a drawn box and a tappable box that disagree
        // is a control that silently does nothing.
        for (int s = 0; s < 4; s++)
        {
            if (input_touch_in(SLOT_X(s), SLOT_Y, SLOT_W, SLOT_H))
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
        if (ty >= SLOT_Y - 3)
        {
            int drop_slot = -1;
            for (int s = 0; s < 4; s++)
            {
                int sx = SLOT_X(s);
                if (tx >= sx && tx < sx + SLOT_W)
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

// The 水柱 volley, in every tier from single jet to the Lv5 super cannon.
// Shared by the base card and the fusions built on it (潮汐双炮, 沸泉地雷),
// so the three always fire the same family of shots.
static void fire_water_volley(Hero *h, int32_t cx, int32_t cy,
                              int lvl, int power, bool local)
{
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
}

static void fire_card(int index, int card_id)
{
    Hero *h = &g_heroes[index];
    int lvl = card_eff_level(h, card_id);
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
        damage_around(cx, cy, radius, power, true, true);

        // 铁尾 Lv3 unlocks the guard, and the field form of the guard is an
        // interrupt: whatever was winding up inside the sweep loses its turn.
        // This is the same mechanic the duel form performs by catching a blow;
        // here it is performed by hitting first, which is what a top-down view
        // can express.
        if (lvl >= CARD_UNLOCK_LEVEL)
        {
            for (int i = 0; i < MAX_MONSTERS; i++)
            {
                Monster *m = &g_monsters[i];
                if (!m->alive || m->recruited)
                    continue;
                int32_t mx, my;
                monster_center(m, &mx, &my);
                if (!in_radius(mx, my, cx, cy, radius))
                    continue;
                // An enemy mid-wind-up or mid-charge is knocked out of it.
                if (m->is_boss)
                {
                    if (m->boss_skill_timer > 20 || m->state == 1)
                    {
                        m->boss_skill_timer = 0;
                        m->boss_skill = 0;
                        m->state = 0;
                        m->state_timer = 0;
                        spawn_effect(FX_PACIFY, mx, my, 20, 22);
                    }
                }
                else if (m->attack_timer > 0 || m->state == 1)
                {
                    m->attack_timer = 0;
                    m->state = 0;
                    m->state_timer = 0;
                }
            }
        }

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
        fire_water_volley(h, cx, cy, lvl, power, local);
        break;
    }


    case CARD_MUD: // 灵水 (Orbiting Water Orbs burst pulse)
    {
        int orbs = h->water_orbs;
        if (orbs <= 0) orbs = 1;
        spawn_effect(FX_PACIFY, cx, cy, 16, (uint8_t)(radius + orbs * 6));
        damage_around(cx, cy, radius + orbs * 6, power, true, true);

        // 灵水 Lv3 turns the orbiting orbs into a screen: each one eats an
        // enemy shot it runs into (see projectile_update). That is the field
        // form of the duel's intercept, so the card reads the same in both.
        // Lv5 re-arms them immediately on a catch, which is the difference
        // between a screen and a wall.
        if (lvl >= CARD_UNLOCK_LEVEL)
        {
            for (int i = 0; i < MAX_PROJECTILES; i++)
            {
                Projectile *p = &g_projectiles[i];
                if (!p->alive)
                    continue;
                if (!p->hostile)
                    continue;
                if (dist2(p->x, p->y, cx, cy) < 40 * 40)
                {
                    p->alive = false;
                    spawn_effect(FX_BITE, p->x, p->y, 10, 10);
                }
            }
        }
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

            // 镇魂歌 (Lv3+): on a boss the song does not calm it -- bosses are
            // not calmed -- it slows its next move down, which is the field
            // form of the duel's stretched wind-up. One mechanic, two views.
            if (m->is_boss && lvl >= CARD_UNLOCK_LEVEL)
            {
                if (m->boss_skill_timer > 0)
                    m->boss_skill_timer = (uint16_t)(m->boss_skill_timer + 14);
                else if (m->boss_skill_cd > 0)
                    m->boss_skill_cd = (uint16_t)(m->boss_skill_cd + 30);
                spawn_effect(FX_PACIFY, mx, my, 30, 30);
            }

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
                        damage_monster(o, power + 6, mx, my, false, true);
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

        // Lv3 halves the charge the card needs, which is the same "it takes
        // fewer presses now" idea the duel form uses.
        int need = (lvl >= CARD_UNLOCK_LEVEL) ? 3 : 5;
        if (h->recruit_charge < need)
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

    case CARD_FUSE_PREDATOR:  // 掠食突袭 (尖牙+冲撞)
    {
        // The charge is the body, the bite is the point: it bursts on the
        // target the moment the dash starts, and the dash itself is
        // invulnerable -- a kill dive, not a gap closer.
        int32_t tx, ty;
        int dx = 0, dy = 0;
        bool found = nearest_monster(cx, cy, 140, &tx, &ty) >= 0;
        if (found)
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
        h->ram_timer = (uint16_t)(18 + lvl * 2);
        if (h->invuln < 16)
            h->invuln = 16;
        spawn_effect(FX_RAM_DUST, cx, cy, 18, (uint8_t)(radius + 6));
        if (found)
        {
            spawn_effect(FX_BITE, tx, ty, 12, 20);
            damage_around(tx, ty, 24 + lvl * 2, power, true, true);
        }
        if (local) audio_play_sfx(SND_ATTACK);
        break;
    }

    case CARD_FUSE_BASTION:  // 荆棘铁壁 (铁尾+厚皮)
    {
        // The sweep is still there, but the reason to press it is the guard
        // it raises: whatever answers the sweep is caught, and the catch
        // ripostes by itself (hero_take_damage owns the riposte).
        if (local) audio_play_sfx(SND_ATTACK);
        spawn_effect(FX_TAIL, cx, cy, 12, (uint8_t)radius);
        damage_around(cx, cy, radius, power, true, true);
        h->guard_timer = GUARD_FRAMES + 6;
        h->guard_cd = GUARD_COOLDOWN - 20;
        break;
    }

    case CARD_FUSE_TIDE:  // 潮汐双炮 (水柱+灵水)
    {
        // The mastered water volley, plus one spent orb splitting into two
        // side shots -- the ring and the cannon firing as one press.
        fire_water_volley(h, cx, cy, lvl, power, local);
        if (h->water_orbs > 0)
        {
            h->water_orbs--;
            int16_t vx = 0, vy = 0;
            if (h->facing == DIR_LEFT) vx = -6;
            else if (h->facing == DIR_RIGHT) vx = 6;
            else if (h->facing == DIR_UP) vy = -6;
            else if (h->facing == DIR_DOWN) vy = 6;
            spawn_projectile(PROJ_WATER, cx, cy - 10, vx, (int16_t)(vy - 1),
                             70, power, 6);
            spawn_projectile(PROJ_WATER, cx, cy + 10, vx, (int16_t)(vy + 1),
                             70, power, 6);
        }
        break;
    }

    case CARD_FUSE_MIASMA:  // 恶臭香氛 (粪球+安抚)
    {
        // The trap and the song in one press: a stinking pool that poisons
        // and slows what walks in, a crowd that loses the will to fight, and
        // a boss whose next wind-up drags.
        spawn_effect_owned(FX_STINK, cx, cy, (uint8_t)(120 + lvl * 30),
                           (uint8_t)(radius + 8), 0);
        spawn_effect(FX_DUNG_TRAP, cx, cy, (uint8_t)(200 + lvl * 50),
                     (uint8_t)(radius + lvl * 2));
        for (int i = 0; i < MAX_MONSTERS; i++)
        {
            Monster *m = &g_monsters[i];
            if (!m->alive || m->recruited)
                continue;
            int32_t mx, my;
            monster_center(m, &mx, &my);
            if (!in_radius(mx, my, cx, cy, radius + 16))
                continue;
            m->pacify_timer = 300;
            if (m->is_boss)
            {
                if (m->boss_skill_timer > 0)
                    m->boss_skill_timer = (uint16_t)(m->boss_skill_timer + 14);
                else if (m->boss_skill_cd > 0)
                    m->boss_skill_cd = (uint16_t)(m->boss_skill_cd + 30);
            }
        }
        if (local) audio_play_sfx(SND_HEAL);
        break;
    }

    case CARD_FUSE_KING:  // 百兽盟主 (感化+利爪)
    {
        // No charging, one convert: every rank-and-file beast in reach turns
        // at once and comes back tougher, and the moment pays one heart.
        bool any = false;
        for (int i = 0; i < MAX_MONSTERS; i++)
        {
            Monster *m = &g_monsters[i];
            if (!m->alive || m->recruited || m->is_boss)
                continue;
            int32_t mx, my;
            monster_center(m, &mx, &my);
            if (!in_radius(mx, my, cx, cy, radius))
                continue;
            m->recruited = true;
            m->hp = m->hp_max;
            m->hp_max = (uint16_t)(m->hp_max * 2);
            m->pacify_timer = 0;
            m->state = 0;
            m->state_timer = 0;
            m->attack_timer = 0;
            spawn_effect(FX_PACIFY, mx, my, 30, 16);
            any = true;
        }
        if (any && h->hearts < h->hearts_max)
            h->hearts++;
        if (local && any)
            audio_play_sfx(SND_HEAL);
        break;
    }

    case CARD_FUSE_GALE:  // 疾风吸取 (快腿+磁吸)
    {
        // The field has no roll to carry it, so the fusion is the vacuum
        // itself: a burst that inhales every drop on the floor and shoves
        // whatever is standing in the wind.
        spawn_effect(FX_PACIFY, cx, cy, 20, 60);
        damage_around(cx, cy, 60, power / 2 + 4, false, true);
        vacuum_all_gems(cx, cy);
        if (h->invuln < 10)
            h->invuln = 10;
        if (local) audio_play_sfx(SND_MAGIC_ICE);
        break;
    }

    case CARD_FUSE_GEYSER:  // 沸泉地雷 (水柱+粪球)
    {
        // The shot, and then the pay-off: an existing trap in range explodes
        // under the jet; with none down, the press lays one instead -- the
        // same arm/detonate rhythm the duel form uses.
        fire_water_volley(h, cx, cy, lvl, power, local);
        int best = -1;
        int32_t best_d = 120 * 120;
        for (int i = 0; i < MAX_EFFECTS; i++)
        {
            Effect *e = &g_effects[i];
            if (!e->alive || e->kind != FX_DUNG_TRAP)
                continue;
            int32_t d = dist2(e->x, e->y, cx, cy);
            if (d < best_d)
            {
                best_d = d;
                best = i;
            }
        }
        if (best >= 0)
        {
            Effect *e = &g_effects[best];
            damage_around(e->x, e->y, e->radius + 22, power * 2, true, true);
            spawn_effect_owned(FX_STINK, e->x, e->y, 40,
                               (uint8_t)(e->radius + 10), 0);
            e->alive = false;
            if (local) g_screen_shake = 3;
        }
        else
        {
            spawn_effect(FX_DUNG_TRAP, cx, cy, (uint8_t)(200 + lvl * 50),
                         (uint8_t)(radius + lvl * 2));
        }
        break;
    }

    case CARD_FUSE_WHIRL:  // 回旋冲角 (铁尾+冲撞)
    {
        // The charge wrapped in a sweep, with the guard open through it: the
        // move that dashes in cannot simply be punished by whatever was
        // already winding up.
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
        h->guard_timer = GUARD_FRAMES + 14;
        h->guard_cd = GUARD_COOLDOWN;
        spawn_effect(FX_TAIL, cx, cy, 12, (uint8_t)radius);
        damage_around(cx, cy, radius, power, true, true);
        spawn_effect(FX_RAM_DUST, cx, cy, 18, (uint8_t)(radius + 6));
        if (local) audio_play_sfx(SND_ATTACK);
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

// Start a roll if one is possible right now: on the ground, or once per jump
// in the air, with the cooldown clear. The direction follows the held keys
// first and the facing second, so kiting away from a boss rolls backwards
// without a wasted turn. Returns true when the roll started.
static bool duel_start_roll(Hero *h, uint16_t keys)
{
    if (h->roll_cd > 0)
        return false;
    if (h->jump_vy != 0 && h->roll_air)
        return false;

    int8_t dir = 0;
    if (keys & KEY_LEFT)  dir = -1;
    if (keys & KEY_RIGHT) dir = 1;
    if (dir == 0)
        dir = (h->facing == DIR_LEFT) ? -1 : 1;

    h->roll_timer = DUEL_ROLL_FRAMES;
    h->roll_cd = DUEL_ROLL_CD;
    h->roll_dir = dir;
    if (h->jump_vy != 0)
        h->roll_air = true;
    if (h->invuln < DUEL_ROLL_INVULN)
        h->invuln = DUEL_ROLL_INVULN;
    audio_play_sfx(SND_UI_MOVE);
    spawn_effect(FX_RAM_DUST, h->x + 16, h->y + 28, 10, 8);
    return true;
}

static void hero_update(int index)
{
    Hero *h = &g_heroes[index];
    if (!h->active)
        return;

    if (h->invuln > 0) h->invuln--;
    if (h->bite_timer > 0) h->bite_timer--;
    if (h->hurt_stun > 0) h->hurt_stun--;
    if (h->pacify_timer > 0) h->pacify_timer--;
    // 铁尾's guard window and its cooldown. `guard_success` is cleared here so
    // it is a one-frame flag for the draw pass rather than a latch.
    if (h->guard_timer > 0) h->guard_timer--;
    if (h->guard_cd > 0) h->guard_cd--;
    if (h->combo_window > 0) h->combo_window--;
    h->guard_success = false;
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
            int32_t ocx, ocy;
            hero_center(h, &ocx, &ocy);
            bool orb_hit = false;
            for (int k = 0; k < h->water_orbs; k++)
            {
                int sx = sin8(h->orb_angle + k * (256 / h->water_orbs));
                int sy = sin8(h->orb_angle + k * (256 / h->water_orbs) + 64);
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
                        damage_monster(mo, card_power(h, CARD_MUD), ocx, ocy, true, true);
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

                // 灵水 Lv3: the orbs are a screen, not just a flail. A shot
                // that runs into one is eaten -- which is what makes the card
                // the answer to a bullet-heavy boss, and it is the same
                // intercept the duel form performs in the arena.
                if (card_unlocked(h, CARD_MUD))
                {
                    for (int p = 0; p < MAX_PROJECTILES; p++)
                    {
                        Projectile *pr = &g_projectiles[p];
                        if (!pr->alive)
                            continue;
                        if (pr->kind != PROJ_ENEMY && pr->kind != PROJ_LOB &&
                            pr->kind != PROJ_MUD)
                            continue;
                        if (dist2(ox, oy, pr->x, pr->y) < 12 * 12)
                        {
                            pr->alive = false;
                            spawn_effect(FX_BITE, ox, oy, 8, 8);
                            h->water_orbs--;
                            // Lv5 re-arms instantly: a screen that has to be
                            // rebuilt between shots is a toy, one that keeps
                            // coming back is a wall.
                            h->orb_regen_timer =
                                (orbs_lvl >= CARD_MAX_LEVEL) ? 1 : 60;
                            orb_hit = true;
                            break;
                        }
                    }
                }
                if (orb_hit)
                    break;
            }
        }
    }

    // In the duel the same body runs, jumps and rolls instead of roaming a
    // tile world: horizontal input on the ground, a fixed arc in the air, and
    // a short i-framed roll once per landing. Everything else on this struct
    // -- orbs, cooldowns, bite, ultimates -- keeps working unchanged.
    if (g_duel_active && !h->remote)
    {
        uint16_t keys = input_held();
        uint16_t down = input_down();
        int dx = 0;
        if (keys & KEY_LEFT)  dx -= 1;
        if (keys & KEY_RIGHT) dx += 1;

        if (h->hurt_stun > 0)
        {
            dx = 0;   // no control while stunned
        }
        else if (h->roll_timer > 0)
        {
            // Rolling: locked direction and speed, dust kicking behind.
            h->roll_timer--;
            h->x += h->roll_dir * DUEL_ROLL_SPEED;
            if ((h->roll_timer & 3) == 0)
                spawn_effect(FX_RAM_DUST, h->x + 16, h->y + 28, 10, 8);
        }
        else
        {
            if (dx != 0)
            {
                h->x += dx * h->move_speed;
                h->facing = (dx < 0) ? DIR_LEFT : DIR_RIGHT;
            }

            // Jump. Edge-triggered (button or tap) so holding it doesn't
            // turn the capybara into a pogo stick. A press while airborne is
            // banked for a few frames: a jump that lands inside that window
            // chains straight into the next hop instead of eating the press.
            bool jump_pressed = (down & KEY_A) != 0 ||
                                (input_touch_pressed() &&
                                 input_touch_in(DUEL_BTN_JUMP_X, DUEL_BTN_JUMP_Y,
                                                DUEL_BTN_JUMP_W, DUEL_BTN_JUMP_H));
            if (jump_pressed)
            {
                if (h->jump_vy == 0)
                {
                    h->jump_vy = DUEL_JUMP_VY;
                    audio_play_sfx(SND_ATTACK);
                }
                else
                    h->jump_buffer = 7;
            }
            // Variable jump height: releasing early cuts the arc.
            bool jump_held = (keys & KEY_A) != 0 ||
                             input_touch_in(DUEL_BTN_JUMP_X, DUEL_BTN_JUMP_Y,
                                            DUEL_BTN_JUMP_W, DUEL_BTN_JUMP_H);
            if (!jump_held && h->jump_vy < -4)
                h->jump_vy = -4;

            // Roll. Ground roll always; air roll once per jump. Edge and
            // tap triggered, like the jump; a press that cannot start one
            // (cooldown, air roll spent) is banked briefly so a roll fired
            // the frame the cooldown ends is still the player's intent.
            bool roll_pressed = (down & KEY_B) != 0 ||
                                (input_touch_pressed() &&
                                 input_touch_in(DUEL_BTN_ROLL_X, DUEL_BTN_ROLL_Y,
                                                DUEL_BTN_ROLL_W, DUEL_BTN_ROLL_H));
            if (roll_pressed)
            {
                if (!duel_start_roll(h, keys))
                    h->roll_buffer = 8;
            }
        }

        if (h->jump_buffer > 0)
            h->jump_buffer--;
        if (h->roll_buffer > 0)
            h->roll_buffer--;
        if (h->roll_cd > 0)
        {
            h->roll_cd--;
            if (h->roll_cd == 0 && h->roll_buffer > 0)
            {
                h->roll_buffer = 0;
                duel_start_roll(h, input_held());
            }
        }

        // Knockback rides the same arena: a horizontal shove that decays,
        // clamped by the walls like every other x step.
        if (h->kb_timer > 0)
        {
            h->kb_timer--;
            if (h->kb_vx != 0)
            {
                h->x += h->kb_vx;
                h->kb_vx += (h->kb_vx < 0) - (h->kb_vx > 0);
            }
        }

        // Vertical arc. jump_vy counts in half-pixels so the arc is smooth at
        // DS frame rates without fixed-point books everywhere.
        //
        // The whole arc used to be gated on jump_vy != 0 -- but gravity steps
        // by exactly 1, so the velocity landed on 0 exactly at the apex and
        // this block was skipped from then on: a capybara frozen at the top of
        // its own jump, which is why the duel jump never came back down.
        bool airborne = (h->y + HERO_SPRITE_H) < DUEL_GROUND_Y ||
                        h->jump_vy != 0;
        if (airborne)
        {
            h->y += h->jump_vy / 2;
            h->jump_vy += DUEL_GRAVITY;
            int32_t feet = h->y + HERO_SPRITE_H;
            if (feet >= DUEL_GROUND_Y)
            {
                h->y = DUEL_GROUND_Y - HERO_SPRITE_H;
                h->jump_vy = 0;
                h->roll_air = false;
                spawn_effect(FX_RAM_DUST, h->x + 16, h->y + 28, 8, 6);
                // A jump pressed just before touchdown fires on the landing
                // frame: the buffer exists so the arc reads as continuous.
                if (h->jump_buffer > 0)
                {
                    h->jump_buffer = 0;
                    h->jump_vy = DUEL_JUMP_VY;
                    audio_play_sfx(SND_ATTACK);
                }
            }
        }

        // Arena bounds.
        int32_t cx2 = h->x + HERO_SPRITE_W / 2;
        if (cx2 < DUEL_X_MIN) h->x = DUEL_X_MIN - HERO_SPRITE_W / 2;
        if (cx2 > DUEL_X_MAX) h->x = DUEL_X_MAX - HERO_SPRITE_W / 2;

        h->moving = (dx != 0) || h->jump_vy != 0 || h->roll_timer > 0;
        if (h->moving)
            h->anim++;

        if (h->ram_timer > 0)
        {
            // 冲撞 in the duel: a horizontal dash attack along the ground.
            h->ram_timer--;
            h->x += h->ram_dx * 3;
            int32_t cx3 = h->x + HERO_SPRITE_W / 2;
            if (cx3 < DUEL_X_MIN) h->x = DUEL_X_MIN - HERO_SPRITE_W / 2;
            if (cx3 > DUEL_X_MAX) h->x = DUEL_X_MAX - HERO_SPRITE_W / 2;
        }
        return;
    }

    // Knockback as a decaying velocity rather than a one-frame teleport: the
    // shove plays out over a few frames while the player is already steering
    // again, which is what makes a hit read as a push instead of a skip.
    if (h->kb_timer > 0 && !h->remote)
    {
        h->kb_timer--;
        if (h->kb_vx != 0)
        {
            int nx = h->x + h->kb_vx;
            if (hero_hitbox_ok(h, nx, h->y))
                h->x = nx;
            h->kb_vx += (h->kb_vx < 0) - (h->kb_vx > 0);
        }
        if (h->kb_vy != 0)
        {
            int ny = h->y + h->kb_vy;
            if (hero_hitbox_ok(h, h->x, ny))
                h->y = ny;
            h->kb_vy += (h->kb_vy < 0) - (h->kb_vy > 0);
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
                      card_power(h, CARD_RAM), true, true);
        // Ultimate: a shockwave every few frames of the charge.
        if (lvl >= CARD_MAX_LEVEL && (h->ram_timer % 6) == 0)
        {
            spawn_effect(FX_RAM_DUST, cx, cy, 14, 26);
            damage_around(cx, cy, 34, card_power(h, CARD_RAM) / 2, true, true);
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

        // Subpixel movement. A diagonal step is scaled by 1/sqrt(2), held in
        // 1/256-pixel remainders so the truncation never shows as a stutter:
        // over frames the capybara covers exactly 70.7% of the cardinal
        // distance, where the old integer path covered a full 141%.
        int step_x = dx * (int)h->move_speed * 256;
        int step_y = dy * (int)h->move_speed * 256;
        if (dx != 0 && dy != 0)
        {
            step_x = step_x * 181 / 256;
            step_y = step_y * 181 / 256;
        }
        h->sub_x = (int16_t)(h->sub_x + step_x);
        h->sub_y = (int16_t)(h->sub_y + step_y);
        int mx = h->sub_x >> 8;
        int my = h->sub_y >> 8;
        h->sub_x = (int16_t)(h->sub_x & 0xFF);
        h->sub_y = (int16_t)(h->sub_y & 0xFF);

        // Axis-separated movement so sliding along walls feels right.
        if (mx != 0)
        {
            int nx = h->x + mx;
            if (hero_hitbox_ok(h, nx, h->y))
                h->x = nx;
            else
                h->sub_x = 0;   // pressed against the wall: spend the remainder
        }
        if (my != 0)
        {
            int ny = h->y + my;
            if (hero_hitbox_ok(h, h->x, ny))
                h->y = ny;
            else
                h->sub_y = 0;
        }

        h->moving = (dx != 0 || dy != 0);
        if (h->moving)
            h->anim++;

    }

    if (h->x < 0) h->x = 0;
    if (h->y < 0) h->y = 0;
    if (h->x > G_WORLD_W - HERO_SPRITE_W) h->x = G_WORLD_W - HERO_SPRITE_W;
    if (h->y > G_WORLD_H - HERO_SPRITE_H) h->y = G_WORLD_H - HERO_SPRITE_H;
}

// Fire every card whose cooldown has elapsed. The bite is deliberately NOT
// here: it is the always-on auto-attack and lives in hero_auto_bite, because it
// has to work before any card is taken.
//
// In the duel this does nothing at all. The arena hands the player the
// controller, so a card is a move to perform rather than a volley to send --
// letting the field's autofire run there would play the fight on the player's
// behalf and make the manual buttons decorative.
static void hero_fire_cards(int index)
{
    if (g_duel_active)
        return;

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
        damage_monster(t, 6, cx, cy, true, true);
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

// Runs one beat of a field boss's rotation. The beat number comes from the
// shared FieldMove table, so every boss telegraphs in the same vocabulary while
// doing something that is recognisably its own:
//
//   1  the area slam, marked where the player is standing
//   2  the immediate follow-up (a lunge, a rake, a dive)
//   3  the committed attack, marked as a lane
//   4  phase 2: the ranged answer
//   5  phase 2: the nova that closes the cycle
//
// Beats 4 and 5 are the escalation the fight was missing: they only exist once
// the boss is below BOSS_PHASE2_PCT, so the second half of every field boss is
// materially different from the first rather than merely faster.
static void execute_boss_skill(Monster *m, int32_t cx, int32_t cy)
{
    int b = m->boss_id;
    int32_t tx = m->boss_tx, ty = m->boss_ty;
    int dx = tx - cx, dy = ty - cy;
    int len = (iabs(dx) > iabs(dy)) ? iabs(dx) : iabs(dy);
    len = len ? len : 1;
    int dmg = (m->phase >= 1) ? 2 : 1;   // phase 2 hits for two hearts

    switch (b)
    {
    case 0: // 雨林巨猿 (Rainforest Ape)
        if (m->boss_skill == 1) // 撼地重砸
        {
            spawn_effect(FX_RAM_DUST, tx, ty, 20, 65);
            damage_around_heroes(tx, ty, 65, 1);
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
        else if (m->boss_skill == 3) // 蛮横直冲
        {
            m->x += (dx * 60) / len;
            m->y += (dy * 60) / len;
            spawn_effect(FX_RAM_DUST, cx, cy, 20, 26);
            damage_around_heroes(m->x + 16, m->y + 16, 40, 1);
            g_screen_shake = 3;
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 4) // 野果投掷
        {
            spawn_mob_projectile(m, PROJ_ENEMY, cx, cy, (int16_t)((dx * 4) / len), (int16_t)((dy * 4) / len), 90, 1, 5);
            spawn_mob_projectile(m, PROJ_ENEMY, cx, cy, (int16_t)(((dx + dy/3) * 4) / len), (int16_t)(((dy - dx/3) * 4) / len), 90, 1, 5);
            spawn_mob_projectile(m, PROJ_ENEMY, cx, cy, (int16_t)(((dx - dy/3) * 4) / len), (int16_t)(((dy + dx/3) * 4) / len), 90, 1, 5);
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        else // 猿啸震地
        {
            spawn_effect(FX_TAIL, cx, cy, 22, 62);
            damage_around_heroes(cx, cy, 62, dmg);
            g_screen_shake = 5;
            audio_play_sfx(SND_EXPLOSION);
        }
        break;

    case 1: // 树精长老 (Treant Elder)
        if (m->boss_skill == 1) // 爆发树根
        {
            spawn_effect(FX_BITE, tx, ty, 18, 62);
            damage_around_heroes(tx, ty, 62, 1);
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 2) // 地刺树根
        {
            spawn_effect(FX_BITE, tx, ty, 16, 20);
            damage_around_heroes(tx, ty, 28, 1);
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 3) // 盘根推移
        {
            m->x += (dx * 50) / len;
            m->y += (dy * 50) / len;
            spawn_effect(FX_BITE, m->x + 16, m->y + 16, 14, 34);
            damage_around_heroes(m->x + 16, m->y + 16, 40, 1);
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 4) // 飞叶旋风
        {
            int sp = 3;
            int dirs[8][2] = {{sp,0},{-sp,0},{0,sp},{0,-sp},{sp*2/3,sp*2/3},{-sp*2/3,sp*2/3},{sp*2/3,-sp*2/3},{-sp*2/3,-sp*2/3}};
            for (int k = 0; k < 8; k++)
                spawn_mob_projectile(m, PROJ_ENEMY, cx, cy, dirs[k][0], dirs[k][1], 80, 1, 4);
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        else // 爆裂毒果
        {
            spawn_effect_owned(FX_STINK, cx, cy, 90, 60, 1);
            damage_around_heroes(cx, cy, 60, dmg);
            audio_play_sfx(SND_EXPLOSION);
        }
        break;

    case 2: // 狂暴野猪 (Raging Boar)
        if (m->boss_skill == 1) // 踏地扬尘
        {
            spawn_effect(FX_RAM_DUST, tx, ty, 20, 62);
            damage_around_heroes(tx, ty, 62, 1);
            g_screen_shake = 4;
            audio_play_sfx(SND_EXPLOSION);
        }
        else if (m->boss_skill == 2) // 獠牙挑飞
        {
            spawn_effect(FX_TAIL, cx, cy, 14, 30);
            damage_around_heroes(cx, cy, 32, 1);
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 3) // 麦浪狂冲
        {
            m->x += (dx * 50) / len;
            m->y += (dy * 50) / len;
            spawn_effect(FX_RAM_DUST, cx, cy, 20, 25);
            damage_around_heroes(m->x + 16, m->y + 16, 40, 1);
            g_screen_shake = 3;
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 4) // 掘地飞泥
        {
            spawn_mob_projectile(m, PROJ_MUD, cx, cy, (int16_t)((-dx * 3) / len), (int16_t)((-dy * 3) / len), 70, 1, 6);
            spawn_mob_projectile(m, PROJ_MUD, cx, cy, (int16_t)((dy * 3) / len), (int16_t)((-dx * 3) / len), 70, 1, 6);
            spawn_mob_projectile(m, PROJ_MUD, cx, cy, (int16_t)((-dy * 3) / len), (int16_t)((dx * 3) / len), 70, 1, 6);
            audio_play_sfx(SND_HIT);
        }
        else // 狂暴旋风
        {
            spawn_effect(FX_TAIL, cx, cy, 20, 58);
            damage_around_heroes(cx, cy, 58, dmg);
            g_screen_shake = 4;
            audio_play_sfx(SND_EXPLOSION);
        }
        break;

    case 3: // 硕鼠大王 (Rat King)
        if (m->boss_skill == 1) // 巨尾重扫
        {
            spawn_effect(FX_TAIL, tx, ty, 18, 62);
            damage_around_heroes(tx, ty, 62, 1);
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 2) // 鼠群召集
        {
            spawn_monster(3, cx + 18, cy, false, 0);
            spawn_monster(3, cx - 18, cy, false, 0);
            audio_play_sfx(SND_PICKUP);
        }
        else if (m->boss_skill == 3) // 钻地突袭
        {
            m->x = tx - 16;
            m->y = ty - 16;
            spawn_effect(FX_MUD, tx, ty, 50, 34);
            damage_around_heroes(tx, ty, 40, 1);
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 4) // 金麦圣光
        {
            for (int k = 1; k <= 3; k++)
                spawn_mob_projectile(m, PROJ_ENEMY, cx + (dx * k * 8) / len, cy + (dy * k * 8) / len, (int16_t)((dx * 5) / len), (int16_t)((dy * 5) / len), 80, 1, 4);
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        else // 鼠王践踏
        {
            spawn_effect(FX_RAM_DUST, cx, cy, 22, 60);
            damage_around_heroes(cx, cy, 60, dmg);
            g_screen_shake = 5;
            audio_play_sfx(SND_EXPLOSION);
        }
        break;

    case 4: // 夜狼领袖 (Night Wolf Leader)
        if (m->boss_skill == 1) // 扑咬压地
        {
            spawn_effect(FX_RAM_DUST, tx, ty, 20, 62);
            damage_around_heroes(tx, ty, 62, 1);
            g_screen_shake = 4;
            audio_play_sfx(SND_EXPLOSION);
        }
        else if (m->boss_skill == 2) // 残影双连突
        {
            m->x += (dx * 45) / len;
            m->y += (dy * 45) / len;
            spawn_effect(FX_RAM_DUST, cx, cy, 14, 20);
            damage_around_heroes(m->x + 16, m->y + 16, 32, 1);
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 3) // 影袭长驱
        {
            m->x += (dx * 70) / len;
            m->y += (dy * 70) / len;
            spawn_effect(FX_RAM_DUST, cx, cy, 20, 28);
            damage_around_heroes(m->x + 16, m->y + 16, 40, 1);
            g_screen_shake = 3;
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 4) // 霜月爪刃
        {
            spawn_mob_projectile(m, PROJ_ENEMY, cx, cy, (int16_t)((dx * 4) / len), (int16_t)((dy * 4) / len), 85, 1, 5);
            spawn_mob_projectile(m, PROJ_ENEMY, cx, cy, (int16_t)(((dx + dy/2) * 4) / len), (int16_t)(((dy - dx/2) * 4) / len), 85, 1, 4);
            spawn_mob_projectile(m, PROJ_ENEMY, cx, cy, (int16_t)(((dx - dy/2) * 4) / len), (int16_t)(((dy + dx/2) * 4) / len), 85, 1, 4);
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        else // 苍狼长啸
        {
            spawn_effect(FX_PACIFY, cx, cy, 20, 64);
            damage_around_heroes(cx, cy, 64, dmg);
            g_screen_shake = 4;
            audio_play_sfx(SND_HURT);
        }
        break;

    case 5: // 狂暴黑牛 (Raging Bull)
        if (m->boss_skill == 1) // 巨蹄震荡
        {
            spawn_effect(FX_RAM_DUST, tx, ty, 20, 70);
            damage_around_heroes(tx, ty, 70, 1);
            g_screen_shake = 3;
            audio_play_sfx(SND_EXPLOSION);
        }
        else if (m->boss_skill == 2) // 巨角旋风
        {
            spawn_effect(FX_TAIL, cx, cy, 16, 42);
            damage_around_heroes(cx, cy, 42, 1);
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 3) // 狂牛冲锋
        {
            m->x += (dx * 60) / len;
            m->y += (dy * 60) / len;
            spawn_effect(FX_RAM_DUST, cx, cy, 24, 30);
            damage_around_heroes(m->x + 16, m->y + 16, 42, 1);
            g_screen_shake = 4;
            audio_play_sfx(SND_EXPLOSION);
        }
        else if (m->boss_skill == 4) // 抛沙迷眼
        {
            spawn_mob_projectile(m, PROJ_MUD, cx, cy, (int16_t)((dx * 4) / len), (int16_t)((dy * 4) / len), 75, 1, 6);
            spawn_mob_projectile(m, PROJ_MUD, cx, cy, (int16_t)(((dx + dy/3) * 4) / len), (int16_t)(((dy - dx/3) * 4) / len), 75, 1, 6);
            spawn_mob_projectile(m, PROJ_MUD, cx, cy, (int16_t)(((dx - dy/3) * 4) / len), (int16_t)(((dy + dx/3) * 4) / len), 75, 1, 6);
            audio_play_sfx(SND_HIT);
        }
        else // 怒牛践踏
        {
            spawn_effect(FX_RAM_DUST, cx, cy, 24, 64);
            damage_around_heroes(cx, cy, 64, dmg);
            g_screen_shake = 6;
            audio_play_sfx(SND_EXPLOSION);
        }
        break;

    case 6: // 泥沼巨蟾 (Swamp Giant Toad)
        if (m->boss_skill == 1) // 巨蟾压顶
        {
            m->x = tx - 16;
            m->y = ty - 16;
            spawn_effect(FX_MUD, tx, ty, 70, 62);
            damage_around_heroes(tx, ty, 62, 1);
            g_screen_shake = 4;
            audio_play_sfx(SND_EXPLOSION);
        }
        else if (m->boss_skill == 2) // 舌击
        {
            spawn_effect(FX_BITE, tx, ty, 12, 22);
            damage_around_heroes(tx, ty, 30, 1);
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 3) // 蟾跃冲撞
        {
            m->x += (dx * 55) / len;
            m->y += (dy * 55) / len;
            spawn_effect(FX_MUD, cx, cy, 20, 26);
            damage_around_heroes(m->x + 16, m->y + 16, 40, 1);
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 4) // 毒泥喷射
        {
            spawn_mob_projectile(m, PROJ_ENEMY, cx, cy, (int16_t)((dx * 3) / len), (int16_t)((dy * 3) / len), 90, 1, 6);
            spawn_mob_projectile(m, PROJ_ENEMY, cx, cy, (int16_t)(((dx + dy/3) * 3) / len), (int16_t)(((dy - dx/3) * 3) / len), 90, 1, 5);
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        else // 毒瘴弥漫
        {
            spawn_effect_owned(FX_STINK, cx, cy, 100, 62, 1);
            damage_around_heroes(cx, cy, 62, dmg);
            audio_play_sfx(SND_HIT);
        }
        break;

    case 7: // 凯门鳄王 (Caiman King)
        if (m->boss_skill == 1) // 掀泥重压
        {
            spawn_effect(FX_MUD, tx, ty, 20, 62);
            damage_around_heroes(tx, ty, 62, 1);
            g_screen_shake = 4;
            audio_play_sfx(SND_EXPLOSION);
        }
        else if (m->boss_skill == 2) // 泥潭潜伏
        {
            m->x = tx + (dx < 0 ? 30 : -30);
            m->y = ty;
            spawn_effect(FX_MUD, m->x + 16, m->y + 16, 50, 24);
            damage_around_heroes(m->x + 16, m->y + 16, 34, 1);
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 3) // 死亡翻滚
        {
            m->x += (dx * 50) / len;
            m->y += (dy * 50) / len;
            spawn_effect(FX_BITE, m->x + 16, m->y + 16, 14, 28);
            damage_around_heroes(m->x + 16, m->y + 16, 40, 1);
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 4) // 鳄尾掀浪
        {
            spawn_projectile(PROJ_WATER, cx, cy, (int16_t)((dx * 4) / len), (int16_t)((dy * 4) / len), 70, 1, 8);
            spawn_projectile(PROJ_WATER, cx, cy, (int16_t)(((dx + dy/3) * 4) / len), (int16_t)(((dy - dx/3) * 4) / len), 70, 1, 7);
            spawn_projectile(PROJ_WATER, cx, cy, (int16_t)(((dx - dy/3) * 4) / len), (int16_t)(((dy + dx/3) * 4) / len), 70, 1, 7);
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        else // 巨鳄翻身
        {
            spawn_effect(FX_TAIL, cx, cy, 22, 60);
            damage_around_heroes(cx, cy, 60, dmg);
            g_screen_shake = 5;
            audio_play_sfx(SND_EXPLOSION);
        }
        break;

    case 8: // 焦糖熔岩巨兽 (Caramel Lava Golem)
        if (m->boss_skill == 1) // 熔岩重拳
        {
            spawn_effect(FX_RAM_DUST, tx, ty, 20, 62);
            damage_around_heroes(tx, ty, 62, 1);
            g_screen_shake = 4;
            audio_play_sfx(SND_EXPLOSION);
        }
        else if (m->boss_skill == 2) // 炽热拍击
        {
            spawn_effect(FX_TAIL, cx, cy, 14, 32);
            damage_around_heroes(cx, cy, 34, 1);
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 3) // 地裂火波
        {
            for (int k = 1; k <= 3; k++)
            {
                int fx = cx + (dx * k * 14) / len;
                int fy = cy + (dy * k * 14) / len;
                spawn_effect(FX_BITE, fx, fy, 16, 14);
                damage_around_heroes(fx, fy, 24, 1);
            }
            audio_play_sfx(SND_EXPLOSION);
        }
        else if (m->boss_skill == 4) // 天降火雨
        {
            for (int k = -1; k <= 1; k++)
                spawn_mob_projectile(m, PROJ_ENEMY, tx + k * 20, ty - 40, 0, 4, 40, 1, 6);
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        else // 熔核爆发
        {
            spawn_effect(FX_RAM_DUST, cx, cy, 24, 62);
            damage_around_heroes(cx, cy, 62, dmg);
            g_screen_shake = 6;
            audio_play_sfx(SND_EXPLOSION);
        }
        break;

    case 9: // 蛋糕魔王 (Cake Lord)
        if (m->boss_skill == 1) // 恶魔蛋糕下砸
        {
            spawn_effect(FX_RAM_DUST, tx, ty, 26, 62);
            damage_around_heroes(tx, ty, 62, 1);
            g_screen_shake = 6;
            audio_play_sfx(SND_EXPLOSION);
        }
        else if (m->boss_skill == 2) // 尖角顶撞
        {
            spawn_effect(FX_TAIL, cx, cy, 14, 34);
            damage_around_heroes(cx, cy, 36, 1);
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 3) // 奶油冲阵
        {
            m->x += (dx * 60) / len;
            m->y += (dy * 60) / len;
            spawn_effect(FX_RAM_DUST, cx, cy, 22, 28);
            damage_around_heroes(m->x + 16, m->y + 16, 42, 1);
            g_screen_shake = 4;
            audio_play_sfx(SND_ATTACK);
        }
        else if (m->boss_skill == 4) // 八方樱桃炸弹
        {
            int sp = 3;
            int dirs[8][2] = {{sp,0},{-sp,0},{0,sp},{0,-sp},{sp*2/3,sp*2/3},{-sp*2/3,sp*2/3},{sp*2/3,-sp*2/3},{-sp*2/3,-sp*2/3}};
            for (int k = 0; k < 8; k++)
                spawn_mob_projectile(m, PROJ_ENEMY, cx, cy, dirs[k][0], dirs[k][1], 75, 1, 5);
            audio_play_sfx(SND_EXPLOSION);
        }
        else // 高压奶油光束
        {
            for (int k = 0; k < 4; k++)
                spawn_mob_projectile(m, PROJ_ENEMY, cx, cy, (int16_t)(((dx + k * 4) * 4) / len), (int16_t)(((dy + k * 4) * 4) / len), 80, 1, 5);
            spawn_effect(FX_PACIFY, cx, cy, 22, 62);
            damage_around_heroes(cx, cy, 62, dmg);
            g_screen_shake = 5;
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Field bosses (先锋)
//
// The field boss used to pick one of three skills at random every couple of
// seconds, which made it a slot machine: the player could not learn it, only
// out-stat it. It now walks a written cycle instead -- the same idea as the
// duel patterns, expressed in a top-down arena.
//
// Each field boss has a five-beat rotation. The first three beats are always
// available; the last two only unlock once the fight passes BOSS_PHASE2_PCT,
// so the second half of the fight genuinely has moves the first half did not.
// Every beat is telegraphed on the ground before it lands, using the same
// shapes the duel uses.
// ---------------------------------------------------------------------------

// One beat of a field boss's rotation, and the arena-level plan for it.
//
// `telegraph` chooses the shape drawn during the wind-up: 0 none, 1 a ring at
// the locked target, 2 a ring at the boss itself (a nova), 3 a line toward the
// target (a charge). `shape` in the skill executor decides what actually
// happens when it fires.
typedef struct {
    uint8_t skill;      // 1..5, which executor branch to run
    uint8_t phase;      // 0 = from the start, 1 = phase 2 only
    uint8_t telegraph;  // 0 none / 1 target ring / 2 self ring / 3 charge line
    uint16_t radius;    // telegraph and hit radius, in pixels
} FieldMove;

// Every field boss shares the same five-beat shape -- wind-up, area, rush,
// ranged, and a phase-2 finisher -- because the *pattern of a fight* is what
// the player learns first. What differs per boss is what each beat does, which
// stays exactly what it was.
static const FieldMove g_field_moves[5] = {
    { 1, 0, 1, 65 },   // beat 1: the area slam, marked where you stand
    { 2, 0, 0, 36 },   // beat 2: the immediate follow-up (a lunge or a rake)
    { 3, 0, 3, 34 },   // beat 3: the committed attack, marked as a line
    { 4, 1, 1, 40 },   // beat 4: phase 2, the ranged answer
    { 5, 1, 2, 62 },   // beat 5: phase 2, the nova that ends the cycle
};

static void boss_update(Monster *m, int32_t cx, int32_t cy, int32_t hx,
                        int32_t hy, int speed, int hi, int touch)
{
    if (m->boss_skill_cd > 0)
        m->boss_skill_cd--;

    // --- phase 2 ------------------------------------------------------------
    // Health gates the rotation just like the duel's phases gate its pattern.
    if (m->phase == 0 && m->hp_max > 0 &&
        (int)(m->hp * 100 / m->hp_max) <= BOSS_PHASE2_PCT)
    {
        m->phase = 1;
        m->pattern_step = 0;      // the new beats are met in order
        show_toast("先锋狂暴了！", PAL_UI2_RED, 180);
        audio_play_sfx(SND_EXPLOSION);
        spawn_effect(FX_LEVELUP, cx, cy, 40, 30);
        g_screen_shake = 5;
    }

    if (m->boss_skill > 0)
    {
        if (m->boss_skill_timer > 0)
            m->boss_skill_timer--;

        // The old executor fires at a fixed frame; the beat's own telegraph is
        // what the player reads, so the boundary stays where it was.
        if (m->boss_skill_timer == 20)
            execute_boss_skill(m, cx, cy);

        if (m->boss_skill_timer == 0)
        {
            m->boss_skill = 0;
            m->boss_skill_cd = (uint16_t)(60 + (my_rand() % 40));
        }
        return;
    }

    // A staggered boss does not act. Poise breaking works in the field too:
    // the same hits that would break it in a duel break it here.
    if (m->stagger_timer > 0)
    {
        m->stagger_timer--;
        if (m->stagger_timer == 0)
        {
            m->boss_skill_cd = 40;
            // Poise does not reset instantly: standing back up buys a span in
            // which it cannot be dropped again, so the fight reads as
            // "break it, take the burst, survive its answer" instead of a
            // permanent knockdown for fast-enough builds.
            m->stagger_immune = STAGGER_IMMUNE;
        }
        return;
    }
    if (m->stagger_window > 0)
    {
        m->stagger_window--;
        if (m->stagger_window == 0)
            m->stagger_hits = (uint8_t)(m->stagger_hits / 2);
    }

    monster_move_toward(m, hx, hy, speed);

    if (m->boss_skill_cd == 0)
    {
        // Walk the rotation to the next beat this phase may use.
        const FieldMove *fm = &g_field_moves[0];
        for (int tries = 0; tries < 8; tries++)
        {
            const FieldMove *cand = &g_field_moves[m->pattern_step % 5];
            m->pattern_step = (uint8_t)((m->pattern_step + 1) % 5);
            if (cand->phase <= m->phase)
            {
                fm = cand;
                break;
            }
        }

        m->boss_skill = fm->skill;
        m->boss_skill_timer = 40;
        m->boss_tx = hx;
        m->boss_ty = hy;

        // Lay the shape down. This is the change that turns a field boss from
        // "something that hurts you sometimes" into something you can dodge:
        // the ground says where the blow lands before it lands.
        if (fm->telegraph == 1)
            spawn_telegraph(TG_CIRCLE, hx, hy, 40, (uint8_t)fm->radius);
        else if (fm->telegraph == 2)
            spawn_telegraph(TG_ARC, cx, cy, 40, (uint8_t)fm->radius);
        else if (fm->telegraph == 3)
        {
            // A charge line from here through the target and out the far side.
            int dx = hx - cx, dy = hy - cy;
            int len = (iabs(dx) > iabs(dy)) ? iabs(dx) : iabs(dy);
            len = len ? len : 1;
            Telegraph *t = spawn_telegraph(TG_LINE, cx, cy, 40, 20);
            if (t)
            {
                t->x2 = cx + (dx * 90) / len;
                t->y2 = cy + (dy * 90) / len;
            }
        }
        audio_play_sfx(SND_UI_MOVE);
    }

    if (dist2(cx, cy, hx, hy) < touch * touch && m->attack_timer == 0)
    {
        // Once the vanguard turns over it stops being a thing you can stand
        // next to: phase two's body costs two hearts, which is the field-boss
        // half of the same rule the brutes follow.
        hero_take_damage(hi, m->phase > 0 ? 2 : 1, cx, cy);
        m->attack_timer = 45;
    }
}

static void try_touch_damage(Monster *m, int32_t cx, int32_t cy,
                             int32_t hx, int32_t hy, int hi, int touch)
{
    if (dist2(cx, cy, hx, hy) < touch * touch && m->attack_timer == 0)
    {
        hero_take_damage(hi, monster_def(m->def_id)->damage, cx, cy);
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
    if (m->stagger_immune > 0) m->stagger_immune--;
    if (m->pacify_timer > 0) m->pacify_timer--;
    if (m->haste_timer > 0) m->haste_timer--;
    if (m->boss_skill_cd > 0) m->boss_skill_cd--;   // spare for mob use
    if (m->rot_timer > 0)
    {
        m->rot_timer--;
        // Rotting damage, applied on a slow tick so it reads as a status.
        if ((m->rot_timer % 24) == 0)
            damage_monster(m, 3, m->x, m->y, false, false);
    }
    m->anim++;

    if (m->recruited)
    {
        ally_update(m);
        return;
    }

    // Standing in mud stops movement for as long as the pool is underfoot.
    // Speed is per type, which is what separates the pack that runs you down
    // from the brute that only ever walks. A support-buffed mob runs one step
    // faster -- visible by its sparks, so the hornet becomes a priority target.
    const MonsterDef *mdef = monster_def(m->def_id);
    int speed = (m->slow_timer > 0) ? 0 : mdef->speed;
    if (speed > 0 && m->haste_timer > 0)
        speed++;

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

    // Stunned by 铁尾's ultimate. The mobs that run their own little state
    // machines key off `state` too, so the stun only applies to the ones that
    // treat state 1 as "idle".
    if (m->state == 1 && m->state_timer > 0 &&
        (mdef->ai == AI_CHASER || mdef->ai == AI_SPLITTER ||
         mdef->ai == AI_PACK || mdef->ai == AI_LOBBER ||
         mdef->ai == AI_ZIGZAG || mdef->ai == AI_SHOOTER))
        return;

    int hi = nearest_hero(cx, cy, NULL);
    if (hi < 0)
        return;
    int32_t hx, hy;
    hero_center(&g_heroes[hi], &hx, &hy);

    int size = monster_size(m);
    int touch = (size / 2) + 8;

    // In the duel the boss is driven by the duel FSM, not by field AI. On the
    // client nobody runs monster_update at all; this guard covers the host.
    if (m->is_boss)
    {
        if (!g_duel_active)
            boss_update(m, cx, cy, hx, hy, speed, hi, touch);
        return;
    }

    switch (mdef->ai)
    {
    case AI_CHASER:
    {
        if (dist2(cx, cy, hx, hy) < 200 * 200)
            monster_move_toward(m, hx, hy, speed);
        try_touch_damage(m, cx, cy, hx, hy, hi, touch);
        break;
    }
    case AI_ZIGZAG:
    {
        // Serpentine approach: aim at a point swinging side to side across
        // the hero. Aimed cards are harder to land, standing still is still
        // fatal.
        int32_t d2v = dist2(cx, cy, hx, hy);
        if (d2v < 220 * 220)
        {
            int32_t dx = hx - cx, dy = hy - cy;
            int len = (iabs(dx) > iabs(dy)) ? iabs(dx) : iabs(dy);
            len = len ? len : 1;
            int wob = sin8(m->anim * 5) * 34 / 127;
            monster_move_toward(m, hx + (-dy * wob) / len,
                                hy + (dx * wob) / len, speed);
        }
        try_touch_damage(m, cx, cy, hx, hy, hi, touch);
        break;
    }
    case AI_LOBBER:
    {
        int32_t d2v = dist2(cx, cy, hx, hy);
        if (d2v < 80 * 80)
            monster_move_toward(m, cx * 2 - hx, cy * 2 - hy, speed);
        else if (d2v > 150 * 150)
            monster_move_toward(m, hx, hy, speed);

        if (m->attack_timer == 0 && d2v < 190 * 190)
        {
            // The lob is aimed at where the hero stands; the real threat is
            // the scalding puddle it leaves, so camping one tile is what
            // gets punished.
            int32_t dx = hx - cx, dy = hy - cy;
            int len = (iabs(dx) > iabs(dy)) ? iabs(dx) : iabs(dy);
            len = len ? len : 1;
            spawn_mob_projectile(m, PROJ_LOB, cx, cy,
                             (int16_t)((dx * 2) / len),
                             (int16_t)((dy * 2) / len),
                             (uint8_t)((len / 2 < 20) ? 20 : len / 2),
                             RANGED_CHIP_DAMAGE, 5);
            int cd = 140 - g_difficulty * 4;
            m->attack_timer = (uint16_t)(cd < 60 ? 60 : cd);
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        break;
    }
    case AI_SPLITTER:
    {
        // A slow, fat chaser whose death is only half the story -- the split
        // itself happens where it dies.
        if (dist2(cx, cy, hx, hy) < 200 * 200)
            monster_move_toward(m, hx, hy, speed);
        try_touch_damage(m, cx, cy, hx, hy, hi, touch);
        break;
    }
    case AI_STEALER:
    {
        if (m->state == 1)
        {
            // Caught red-handed: run for the map edge. Kill the thief before
            // the timer lets it slip away and it drops the goods -- plus one
            // extra, as interest.
            monster_move_toward(m, m->boss_tx, m->boss_ty, speed);
            if (m->state_timer == 0)
            {
                if (dist2(cx, cy, hx, hy) > 260 * 260)
                    m->alive = false;
                else
                    m->state_timer = 150;
            }
            break;
        }
        // Dart-pause rhythm: fast for the first third of each cycle, then a
        // beat of stillness. Step in on the beat and the thief cannot touch
        // you; be greedy and it robs the region gate.
        if (m->state_timer == 0)
            m->state_timer = 40;
        if (m->state_timer > 26)
            monster_move_toward(m, hx, hy, speed);
        if (dist2(cx, cy, hx, hy) < touch * touch && m->attack_timer == 0)
        {
            m->attack_timer = 90;
            if (g_seed_have > 0)
            {
                g_seed_have--;
                // The theft doubles as the one lesson about the thief: it
                // robbed the SUMMON METER, chasing it pays double, and both
                // facts arrive the first time it robs you.
                if (!tutorial_done(TUT_B_RAT))
                {
                    tutorial_mark(TUT_B_RAT);
                    show_toast("田鼠偷走了召唤进度！追上它拿回双倍",
                               PAL_UI2_RED, 240);
                }
                else
                    show_toast("种子被田鼠偷走了！", PAL_UI2_RED, 200);
                audio_play_sfx(SND_HURT);
            }
            m->state = 1;
            m->state_timer = 420;
            m->boss_tx = (cx < G_WORLD_W / 2) ? 40 : G_WORLD_W - 40;
            m->boss_ty = (cy < G_WORLD_H / 2) ? 40 : G_WORLD_H - 40;
            spawn_effect(FX_HIT, cx, cy, 12, 10);
        }
        break;
    }
    case AI_DIVER:
    {
        if (m->state == 0)
        {
            int32_t d2v = dist2(cx, cy, hx, hy);
            if (d2v < 90 * 90)
                monster_move_toward(m, cx * 2 - hx, cy * 2 - hy, speed);
            else if (d2v > 150 * 150)
                monster_move_toward(m, hx, hy, speed);
            if (m->attack_timer == 0)
            {
                m->state = 1;
                m->state_timer = 35;
                // The dive line is locked at windup: sidestep, don't outrun.
                m->boss_tx = hx;
                m->boss_ty = hy;
            }
        }
        else if (m->state == 1)
        {
            if (m->state_timer == 0)
            {
                m->state = 2;
                m->state_timer = 60;
                audio_play_sfx(SND_ATTACK);
            }
        }
        else
        {
            monster_move_toward(m, m->boss_tx, m->boss_ty, 5);
            if (m->state_timer == 0 ||
                dist2(cx, cy, m->boss_tx, m->boss_ty) < 14 * 14)
            {
                m->state = 0;
                int cd = 170 - g_difficulty * 5;
                m->attack_timer = (uint16_t)(cd < 70 ? 70 : cd);
            }
            try_touch_damage(m, cx, cy, hx, hy, hi, touch);
        }
        break;
    }
    case AI_NOVA:
    {
        if (m->state == 0)
        {
            monster_move_toward(m, hx, hy, speed);
            if (m->attack_timer == 0 && dist2(cx, cy, hx, hy) < 58 * 58)
            {
                m->state = 1;   // swell up -- the cue to back off
                m->state_timer = 36;
            }
        }
        else
        {
            if (m->state_timer == 0)
            {
                m->state = 0;
                m->attack_timer = 150;
                spawn_effect(FX_TAIL, cx, cy, 14, 40);
                damage_around_heroes(cx, cy, 40, mdef->damage);
                g_screen_shake = 2;
                audio_play_sfx(SND_EXPLOSION);
            }
        }
        try_touch_damage(m, cx, cy, hx, hy, hi, touch);
        break;
    }
    case AI_PACK:
    {
        // Each dog runs faster with packmates beside it: scattered strays are
        // a nuisance, a converged pack is a genuine chase.
        int pack = 0;
        for (int i = 0; i < MAX_MONSTERS && pack < 4; i++)
        {
            Monster *o = &g_monsters[i];
            if (o == m || !o->alive || o->recruited || o->def_id != m->def_id)
                continue;
            int32_t ox, oy;
            monster_center(o, &ox, &oy);
            if (dist2(cx, cy, ox, oy) < 80 * 80)
                pack++;
        }
        int spd = speed + (pack >= 1 ? 1 : 0) + (pack >= 3 ? 1 : 0);
        if (dist2(cx, cy, hx, hy) < 220 * 220)
            monster_move_toward(m, hx, hy, spd);
        try_touch_damage(m, cx, cy, hx, hy, hi, touch);
        break;
    }
    case AI_CHARGER:
    {
        if (m->state == 0)
        {
            monster_move_toward(m, hx, hy, speed);
            if (m->attack_timer == 0 && dist2(cx, cy, hx, hy) < 120 * 120)
            {
                m->state = 1;
                m->state_timer = 45;
                m->boss_tx = hx;
                m->boss_ty = hy;
            }
        }
        else if (m->state == 1)
        {
            // Pawing the ground, aim locked: the ring is your cue to be
            // somewhere else.
            if (m->state_timer == 0)
            {
                m->state = 2;
                m->state_timer = 50;
                audio_play_sfx(SND_ATTACK);
            }
        }
        else if (m->state == 2)
        {
            monster_move_toward(m, m->boss_tx, m->boss_ty, 5);
            if (m->state_timer == 0 ||
                dist2(cx, cy, m->boss_tx, m->boss_ty) < 16 * 16)
            {
                m->state = 3;
                m->state_timer = 80;
            }
        }
        else
        {
            // Winded after the charge: the punish window.
            if (m->state_timer == 0)
            {
                m->state = 0;
                m->attack_timer = 90;
            }
        }
        try_touch_damage(m, cx, cy, hx, hy, hi, touch);
        break;
    }
    case AI_SUPPORT:
    {
        int32_t d2v = dist2(cx, cy, hx, hy);
        if (d2v < 80 * 80)
            monster_move_toward(m, cx * 2 - hx, cy * 2 - hy, speed);
        else if (d2v > 130 * 130)
            monster_move_toward(m, hx, hy, speed);

        if (m->attack_timer == 0 && d2v < 170 * 170)
        {
            int32_t dx = hx - cx, dy = hy - cy;
            int len = (iabs(dx) > iabs(dy)) ? iabs(dx) : iabs(dy);
            len = len ? len : 1;
            spawn_mob_projectile(m, PROJ_ENEMY, cx, cy,
                             (int16_t)((dx * 3) / len),
                             (int16_t)((dy * 3) / len),
                             100, RANGED_CHIP_DAMAGE, 4);
            m->attack_timer = 110;
            audio_play_sfx(SND_MAGIC_FIRE);
        }

        // The war-drum pulse: up to four allies near the hornet get hasted.
        // Killing the support first strips the pack of its speed -- the
        // classic "shoot the medic" decision, in capybara form.
        if (m->boss_skill_cd == 0)
        {
            m->boss_skill_cd = 300;
            int buffed = 0;
            for (int i = 0; i < MAX_MONSTERS && buffed < 4; i++)
            {
                Monster *o = &g_monsters[i];
                if (o == m || !o->alive || o->recruited || o->is_boss)
                    continue;
                int32_t ox, oy;
                monster_center(o, &ox, &oy);
                if (dist2(cx, cy, ox, oy) < 90 * 90)
                {
                    o->haste_timer = 240;
                    buffed++;
                }
            }
            if (buffed)
            {
                spawn_effect(FX_PACIFY, cx, cy, 22, 90);
                audio_play_sfx(SND_UI_MOVE);
            }
        }
        try_touch_damage(m, cx, cy, hx, hy, hi, touch);
        break;
    }
    case AI_SPITTER:
    {
        int32_t d2v = dist2(cx, cy, hx, hy);
        if (m->state == 0)
        {
            if (d2v > 90 * 90)
                monster_move_toward(m, hx, hy, speed);
            if (d2v < 110 * 110 && m->attack_timer == 0)
            {
                m->state = 1;
                m->attack_timer = 3;     // shots left in the burst
                m->state_timer = 6;
            }
        }
        else
        {
            // Stationary three-shot burst: the window to rush it is while it
            // spits.
            if (m->state_timer == 0)
            {
                int32_t dx = hx - cx, dy = hy - cy;
                int len = (iabs(dx) > iabs(dy)) ? iabs(dx) : iabs(dy);
                len = len ? len : 1;
                spawn_mob_projectile(m, PROJ_ENEMY, cx, cy,
                                 (int16_t)((dx * 4) / len),
                                 (int16_t)((dy * 4) / len), 80,
                                 RANGED_CHIP_DAMAGE, 4);
                audio_play_sfx(SND_MAGIC_FIRE);
                if (m->attack_timer <= 1)
                    m->state = 0;
                else
                {
                    m->attack_timer--;
                    m->state_timer = 12;
                }
            }
        }
        try_touch_damage(m, cx, cy, hx, hy, hi, touch);
        break;
    }
    case AI_BOMBER:
    {
        if (m->state == 0)
        {
            monster_move_toward(m, hx, hy, speed);
            if (dist2(cx, cy, hx, hy) < 40 * 40)
            {
                m->state = 1;            // fuse lit: blink means back off
                m->state_timer = 45;
            }
        }
        else
        {
            if (m->state_timer == 0)
            {
                // Detonates on the spot. No drops and no experience: shooting
                // it down before it arrives is the entire game against it.
                spawn_effect(FX_RAM_DUST, cx, cy, 20, 30);
                damage_around_heroes(cx, cy, 34, mdef->damage);
                g_screen_shake = 3;
                audio_play_sfx(SND_EXPLOSION);
                m->alive = false;
                return;
            }
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

            try_touch_damage(m, cx, cy, hx, hy, hi, touch);
        }
        break;
    }
    case AI_SHOOTER:
    {
        int32_t d2v = dist2(cx, cy, hx, hy);
        // Keep a comfortable distance: retreat when too close, close in when
        // too far, fire on a timer that shortens as difficulty rises.
        if (d2v < 60 * 60)
            monster_move_toward(m, cx * 2 - hx, cy * 2 - hy, speed);
        else if (d2v > 100 * 100)
            monster_move_toward(m, hx, hy, speed);

        if (m->attack_timer == 0 && d2v < 140 * 140)
        {
            int32_t dx = hx - cx, dy = hy - cy;
            int len = (iabs(dx) > iabs(dy)) ? iabs(dx) : iabs(dy);
            len = len ? len : 1;
            spawn_mob_projectile(m, PROJ_ENEMY, cx, cy,
                             (int16_t)((dx * 3) / len),
                             (int16_t)((dy * 3) / len), 110,
                             RANGED_CHIP_DAMAGE, 4);
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

        // 水柱 Lv5: a mastered jet eats what it crosses. The card's whole
        // identity is a piercing line, so at the top level that line clears the
        // air in front of it -- a counter to a bullet-heavy boss rather than
        // merely more damage.
        if (p->kind == PROJ_WATER &&
            card_eff_level(&g_heroes[g_local_hero], CARD_WATER) >= CARD_MAX_LEVEL)
        {
            for (int q = 0; q < MAX_PROJECTILES; q++)
            {
                Projectile *o = &g_projectiles[q];
                if (!o->alive || o == p || !o->hostile)
                    continue;
                if (dist2(o->x, o->y, p->x, p->y) <
                    (int32_t)(o->radius + p->radius) * (o->radius + p->radius))
                {
                    o->alive = false;
                    spawn_effect(FX_BITE, p->x, p->y, 8, 10);
                }
            }
        }

        // A lobber shot lands where its flight ends -- on the hero, on a wall,
        // or in mid-air when the arc runs out -- and scalds the ground there.
        // The puddle belongs to whoever fired the shot: an enemy's puddle is
        // something to stand out of, ours is crowd control.
        if (p->life == 0 && p->kind == PROJ_LOB)
        {
            spawn_effect_owned(FX_STINK, p->x, p->y, 55, 14,
                               p->hostile ? 1 : 0);
            if (p->hostile)
                damage_around_heroes(p->x, p->y, 14, p->power);
            else
                damage_around(p->x, p->y, 14, p->power, false, true);
            audio_play_sfx(SND_HIT);
        }
        if (p->life == 0)
        {
            p->alive = false;
            continue;
        }

        // Terrain collision. The duel arena has no tile map under it, so the
        // test is skipped while it runs.
        if (!g_duel_active && collision_at(p->x, p->y))
        {
            if (p->kind == PROJ_DUNG)
            {
                spawn_effect(FX_STINK, p->x, p->y, 80, (uint8_t)p->radius);
                damage_around(p->x, p->y, p->radius, p->power, true, true);
                audio_play_sfx(SND_HIT);
            }
            else if (p->kind == PROJ_WATER)
            {
                spawn_effect(FX_MUD, p->x, p->y, 40, 14);
            }
            else if (p->kind == PROJ_LOB)
            {
                spawn_effect_owned(FX_STINK, p->x, p->y, 55, 14,
                                   p->hostile ? 1 : 0);
                if (p->hostile)
                    damage_around_heroes(p->x, p->y, 14, p->power);
                else
                    damage_around(p->x, p->y, 14, p->power, false, true);
                audio_play_sfx(SND_HIT);
            }
            p->alive = false;
            continue;
        }

        if (p->kind == PROJ_SHOCK)
        {
            // The duel ground wave. It hugs the floor, so the answer to it is
            // the jump, not the roll -- rolling is horizontal, jumping is up.
            for (int h = 0; h < 2; h++)
            {
                Hero *hero = &g_heroes[h];
                if (!hero->active)
                    continue;
                bool grounded = hero->jump_vy == 0;
                if (!grounded)
                    continue;
                int32_t cx, cy;
                hero_center(hero, &cx, &cy);
                if (iabs(p->x - cx) < 10 + p->radius)
                {
                    hero_take_damage(h, p->power, p->x, p->y);
                }
            }
            continue;
        }

        if (p->hostile)
        {
            // Anything a monster fired answers to this side, whatever kind it
            // carries -- the fix that makes a field boss's water blast and mud
            // volley real threats instead of shots that only ever chipped
            // their own team.
            for (int h = 0; h < 2; h++)
            {
                Hero *hero = &g_heroes[h];
                if (!hero->active)
                    continue;
                int32_t cx, cy;
                hero_center(hero, &cx, &cy);
                if (dist2(p->x, p->y, cx, cy) < 10 * 10)
                {
                    hero_take_damage(h, p->power, p->x, p->y);
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
                damage_around(p->x, p->y, p->radius, p->power, true, true);
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
                damage_monster(mo, p->power, p->x, p->y, true, true);
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
            // The cloud keeps damaging while it lasts -- whichever side owns
            // it. An enemy's cloud is a zone to leave; ours is crowd control.
            if ((e->life % 20) == 0)
            {
                if (e->owner)
                    damage_around_heroes(e->x, e->y, e->radius,
                                         RANGED_CHIP_DAMAGE);
                else
                    damage_around(e->x, e->y, e->radius, 4, false, false);
            }
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
                    damage_monster(mo, dmg, e->x, e->y, true, true);
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
                    // Shared spoils: on the host, a mote the host's capybara
                    // grabbed credits the partner too, so both players level
                    // at the same pace (the client has no mote stream of its
                    // own -- drops live on the authoritative side).
                    if (g_multiplayer && g_is_host)
                        hero_gain_exp(1, g->value);
                    if (h == g_local_hero)
                        audio_play_sfx(SND_PICKUP);
                }
                else if (g->kind == GEM_INGREDIENT)
                {
                    // An ingredient: this is what summons the region boss.
                    if (g_seed_have < 255)
                        g_seed_have++;
                    if (h == g_local_hero)
                        audio_play_sfx(SND_HEART);
                }
                else if (g->kind == GEM_SEED)
                {
                    // A seed does two things at once, and both matter. It goes
                    // into the satchel (`g_seeds[crop]`, crop+1 stored in
                    // `value`), and it advances the region's collection bar --
                    // which is what still gates the 先锋 and the boss. Keeping
                    // the bar on the same count as before is deliberate:
                    // swapping what the bar counts must not change how long a
                    // region takes.
                    int crop = (int)g->value - 1;
                    if (crop >= 0 && crop < FARM_CROP_COUNT &&
                        g_seeds[crop] < 99)
                        g_seeds[crop]++;
                    if (g_seed_have < 255)
                        g_seed_have++;
                    // First seed in hand: the loop's next beat is the field.
                    // The one field hint the chain gives is this toast, so
                    // the "why am I collecting these" question is answered
                    // the moment it comes up.
                    bool first_seed = (g_tut_flags & (1u << TUT_SEED)) == 0;
                    tutorial_notify(TUT_EV_SEED_PICKED);
                    if (h == g_local_hero)
                    {
                        int32_t hx2, hy2;
                        hero_center(hero, &hx2, &hy2);
                        spawn_floater(hx2, hy2, 1, FLOATER_SEED);
                        audio_play_sfx(SND_PICKUP);
                        if (first_seed)
                            show_toast("种子带回去种进菜地，才能长成材料",
                                       PAL_UI_GOLD_LIGHT, 180);
                    }
                }
                else if (g->kind == 2)
                {
                    // 霸主印记：用于解锁被动技能树
                    g_boss_tokens += g->value;
                    if (h == g_local_hero)
                        audio_play_sfx(SND_LEVELUP);
                }
                else if (g->kind == 3)
                {
                    // A heart: the only mid-run heal between level-ups. At
                    // full health it is worth a little experience instead, so
                    // a lucky drop is never wasted.
                    if (hero->hearts < hero->hearts_max)
                    {
                        hero->hearts++;
                        // Co-op: the client sees no drop stream, so a heart
                        // the host picks up heals the partner as well; the
                        // amount rides the world packet (g_partner_heal).
                        if (g_multiplayer && g_is_host)
                            g_partner_heal = 1;
                        if (h == g_local_hero)
                        {
                            int32_t hx2, hy2;
                            hero_center(hero, &hx2, &hy2);
                            spawn_floater(hx2, hy2, 1, FLOATER_HEAL);
                            audio_play_sfx(SND_HEAL);
                        }
                    }
                    else
                    {
                        hero_gain_exp(h, 8);
                        if (h == g_local_hero)
                            audio_play_sfx(SND_PICKUP);
                    }
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

// Spawns a monster and returns it, so callers can customise the spawn (elite
// rolls, splitter offspring). Returns NULL when the arena pool is full.
static Monster *spawn_monster(uint8_t def_id, int32_t x, int32_t y, bool boss,
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
        m->scale_pct = 100;
        // Spread the idle bob across the crowd. Derived from the pool slot
        // rather than drawn from my_rand() so that it is well distributed even
        // when the RNG is being consumed by the spawn roll at the same time.
        m->bob_phase = (uint8_t)((i * 5) & 7);

        // Monsters scale with elapsed difficulty, not with the character's
        // level: this is an endless run, so the clock is what eventually wins.
        if (boss)
        {
            // 先锋 (field bosses). Budgeted the same way the duel bosses are:
            // the player arrives here with a couple of cards built up and the
            // tree half-bought, and the fight is meant to run half a minute --
            // long enough to read the rotation twice, short enough that it
            // never crowds out the farming it interrupts.
            m->hp_max = (uint16_t)(180 + g_region_id * 70 +
                                   g_difficulty * 40);
            m->phase = 0;
            m->pattern_step = 0;
            m->stagger_hits = 0;
            m->stagger_window = 0;
            m->stagger_timer = 0;
        }
        else
        {
            // Each type brings its own bulk, and the difficulty multiplies it
            // rather than replacing it: a brute keeps its lead over a swarm for
            // the whole run instead of the two converging.
            const MonsterDef *d = monster_def(def_id);
            m->hp_max = (uint16_t)(d->hp_base + (d->hp_base * g_difficulty) / 2 +
                                   g_region_id * 8);
        }
        m->radius = monster_draw_radius(m);
        m->hp = m->hp_max;
        return m;
    }
    return NULL;
}

// Elite roll. Elites are the region's slot machine: tougher than the crowd, a
// guaranteed ingredient, a big experience mote, often a heart. A golden crown
// and pulsing ring announce them the moment they step on screen.
//
// The multiplier came down from 3x to 2.5x when the duel-form cards started
// feeding the stagger: a 3x-health elite is now a wall the capybara can stand
// in front of and break, but it also made an elite wave a fight that outlasted
// the wave itself.
static void apply_elite_roll(Monster *m, int pct)
{
    if (!m || (int)(my_rand() % 100) >= pct)
        return;
    m->elite = 1;
    m->scale_pct = 118;
    m->hp_max = (uint16_t)(m->hp_max * 5 / 2);
    m->hp = m->hp_max;
    m->radius = monster_draw_radius(m);
}

// Splitter offspring: same species, one generation down -- smaller, softer,
// and they die like anything else. One generation only, so a slime pit is a
// burst of work, not an endless chain.
static void spawn_splitter_children(const Monster *parent)
{
    const MonsterDef *d = monster_def(parent->def_id);
    if (d->ai != AI_SPLITTER || parent->gen != 0)
        return;

    for (int k = 0; k < 2; k++)
    {
        int32_t ox = parent->x + (k ? 10 : -10);
        int32_t oy = parent->y + (int)(my_rand() % 9) - 4;
        Monster *c = spawn_monster(parent->def_id, ox, oy, false, 0);
        if (!c)
            return;
        c->gen = 1;
        c->scale_pct = 60;
        c->hp_max = (uint16_t)(c->hp_max * 45 / 100 + 2);
        c->hp = c->hp_max;
        c->radius = monster_draw_radius(c);
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
    int32_t px = G_WORLD_W / 2, py = G_WORLD_H / 2;
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
        if (x < 40 || y < 40 || x > G_WORLD_W - 40 || y > G_WORLD_H - 40)
            continue;

        if (spot_free(x, y, 28))
        {
            // Elite roll: rare once difficulty is rolling, common during a
            // raid wave. Wave elites are the whole point of the wave -- a
            // golden crown in the crowd is a loot beacon the player will
            // wade in to chase.
            int elite_pct = (g_wave_active > 0) ? WAVE_ELITE_PCT
                                                : (g_difficulty >= 2 ? 7 : 0);
            Monster *nm = spawn_monster(def_id, x, y, false, 0);
            apply_elite_roll(nm, elite_pct);
            if (nm && nm->elite && elite_pct && g_wave_active == 0)
            {
                // The first crown carries its own explanation; after that the
                // gold outline is news enough on its own.
                if (!tutorial_done(TUT_B_ELITE))
                {
                    tutorial_mark(TUT_B_ELITE);
                    show_toast("金冠精英出现了：必掉种子，值得一博",
                               PAL_UI_GOLD_LIGHT, 220);
                }
                else
                    show_toast("金冠精英出现了！", PAL_UI_GOLD_LIGHT, 150);
            }
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

// Each region marches to its own battle theme (gen_audio.py emits one per
// region under the shared battle chassis: five keys, five tempos). audio.c
// degrades a missing bank entry to silence per entry, but the shared track
// stays in the bank, so falling back to it keeps every region audible.
static int region_battle_music(void)
{
    static const int tracks[INGREDIENT_COUNT] = {
        MUS_BATTLE_RAIN, MUS_BATTLE_FARM, MUS_BATTLE_PAST,
        MUS_BATTLE_MARSH, MUS_BATTLE_VOLCANO,
    };
    if (g_region_id < INGREDIENT_COUNT)
        return tracks[g_region_id];
    return MUS_BATTLE;
}

void game_start_region(int region_index)
{
    if (region_index < 0 || region_index >= INGREDIENT_COUNT)
        return;

    const RegionDef *rg = &g_regions[region_index];

    g_region_id = (uint8_t)region_index;
    g_seed_have = 0;
    g_mid_boss_out = false;
    g_boss_out = false;
    g_run_time = 0;
    // A duel lost here left the boss standing at the arena door. The meter
    // was already paid once, so the rematch re-enters with it full -- the
    // vanguard stays down too, since beating it is what unlocked the duel.
    // The leap-out beat plays the moment the field opens.
    if (g_duel_pending == (int8_t)region_index)
    {
        g_seed_have = rg->target;
        g_mid_boss_out = true;
        g_duel_pending = -1;
    }
    // Walking out of the gate is the first thing the guidance chain teaches;
    // having actually done it closes that step.
    tutorial_notify(TUT_EV_REGION_ENTER);
    // Later regions open partway up the difficulty ladder: the fifth region
    // is never as gentle as the first, and the mob roster unlocks faster.
    g_difficulty = (uint8_t)region_index;
    g_spawn_timer = 30;   // a moment of calm before the first wave
    g_combo = 0;
    g_combo_timer = 0;
    g_region_kills = 0;
    g_wave_cd = WAVE_FIRST;
    g_wave_active = 0;
    g_toast.life = 0;
    g_duel.phase = DUEL_OFF;
    g_duel.enrage = false;
    g_duel_active = false;

    memset(g_monsters, 0, sizeof(g_monsters));
    memset(g_projectiles, 0, sizeof(g_projectiles));
    memset(g_gems, 0, sizeof(g_gems));
    memset(g_effects, 0, sizeof(g_effects));
    // Warnings belong to a fight, not to a scene: a ring left over from
    // the last one would be a shape that threatens nothing.
    memset(g_telegraphs, 0, sizeof(g_telegraphs));
    memset(g_floaters, 0, sizeof(g_floaters));

    // Bind the region's world, then put everyone at the middle of it. The
    // spawn clearing is kept walkable by the generator, so no collision test
    // is needed here -- but the camera is snapped, not lerped, so the first
    // frame of a region never shows the previous region's corner.
    const uint8_t *atlas = area_atlas(rg->area_id);
    const uint8_t *map = area_map(rg->area_id);
    const uint8_t *prop = area_prop(rg->area_id);
    int cols, rows;
    area_size(rg->area_id, &cols, &rows);
    world_set_size(cols, rows);
    render_set_world(atlas, map, prop);

    int32_t cx = G_WORLD_W / 2;
    int32_t cy = G_WORLD_H / 2;

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
        h->jump_vy = 0;
        h->roll_timer = 0;
        h->roll_cd = 0;
        h->roll_dir = 0;
        h->roll_air = false;
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

    audio_play_music(region_battle_music());
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
    if (g_duel_active || g_duel.phase != DUEL_OFF)
        return;   // the duel has its own pacing; the clock stops mattering
    uint8_t d = (uint8_t)(g_run_time / DIFFICULTY_STEP) + g_region_id;
    if (d > DIFFICULTY_MAX)
        d = DIFFICULTY_MAX;
    if (d != g_difficulty)
    {
        bool up = (d > g_difficulty);
        g_difficulty = d;
        if (!up)
            return;
        // Every step is announced: escalation the player can hear and read,
        // not just feel.
        show_toast("危险度提升！", PAL_UI2_RED, 120);
        audio_play_sfx(SND_UI_MOVE);
        if (d == DIFFICULTY_MAX)
            audio_play_sfx(SND_EXPLOSION);   // the run has peaked; hold on
    }
}

static void spawn_update(void)
{
    const RegionDef *rg = &g_regions[g_region_id];

    // Phase 1: Mid-boss appears when half of the ingredient target is gathered
    if (!g_mid_boss_out && g_seed_have >= (rg->target / 2))
    {
        g_mid_boss_out = true;
        int32_t px = G_WORLD_W / 2, py = G_WORLD_H / 2;
        if (g_heroes[g_local_hero].active)
            hero_center(&g_heroes[g_local_hero], &px, &py);

        int32_t bx = px + (AREA_W / 2 + 40), by = py;
        if (bx > G_WORLD_W - 60) bx = px - (AREA_W / 2 + 40);
        if (bx < 60) bx = 60;
        if (by < 60) by = 60;
        if (by > G_WORLD_H - 60) by = G_WORLD_H - 60;

        spawn_monster(0, bx, by, true, rg->mid_boss_id);
        // The name card, in the field. The duel gets a full title card; the
        // 先锋 gets a toast with its epithet, so the two boss encounters in a
        // region are introduced the same way rather than one being anonymous.
        {
            static char mbuf[64];
            snprintf(mbuf, sizeof(mbuf), "%s · %s", rg->mid_boss_name,
                     rg->mid_boss_title);
            show_toast(mbuf, PAL_UI2_RED, 200);
        }
        audio_play_music(MUS_BOSS);
    }

    // Phase 2: the ingredient target is full -- the region climax begins.
    // The final boss never fights in the field: it LEAPS out (a short beat
    // played by game_update), every small monster is cleared, and the duel
    // starts on its own screen.
    if (!g_boss_out && g_seed_have >= rg->target)
    {
        g_boss_out = true;
        duel_begin_leap_out();
        return;
    }

    // --- raid waves (兽潮) ---
    // The region's rhythm: calm stretches of farming punctuated by a short,
    // loud spike. The payout on the far side is what makes standing your
    // ground during the spike the smart play, not just the brave one.
    if (g_wave_active > 0)
    {
        g_wave_active--;
        if (g_wave_active == 0)
        {
            g_wave_cd = WAVE_INTERVAL;
            int32_t px = G_WORLD_W / 2, py = G_WORLD_H / 2;
            if (g_heroes[g_local_hero].active)
                hero_center(&g_heroes[g_local_hero], &px, &py);
            for (int k = 0; k < 6; k++)
                spawn_gem(0, px + (int)(my_rand() % 61) - 30,
                             py + (int)(my_rand() % 61) - 30, 3);
            spawn_gem(3, px + 14, py, 1);
            if ((int)(my_rand() % 100) < 60)
                spawn_gem(GEM_SEED, px - 14, py,
                          (uint16_t)(g_region_id + 1));
            show_toast("兽潮退去，掉落了战利品！", PAL_UI_GOLD_LIGHT, 200);
            audio_play_sfx(SND_LEVELUP);
        }
    }
    else if (!g_boss_out)
    {
        if (g_wave_cd > 0)
            g_wave_cd--;
        // Five seconds of warning before the first wave ever lands: a spike
        // you did not see coming teaches "run", one you see coming teaches
        // "choose your ground". Only the first wave needs the headstart --
        // after that the player knows the rhythm exists.
        if (g_wave_cd == 300 && !tutorial_done(TUT_B_WAVE))
        {
            tutorial_mark(TUT_B_WAVE);
            show_toast("兽潮要来了：往开阔地走，金冠怪必掉种子",
                       PAL_UI_GOLD_LIGHT, 240);
        }
        if (g_wave_cd == 0)
        {
            g_wave_active = WAVE_DURATION;
            show_toast("兽潮来袭！", PAL_UI2_RED, 220);
            audio_play_sfx(SND_EXPLOSION);
            g_screen_shake = 3;
        }
    }

    if (g_spawn_timer > 0)
    {
        g_spawn_timer--;
        return;
    }
    g_spawn_timer = spawn_interval();

    // Progressive mob variety. Each region lists its three roles as
    // {swarm, ranged, brute} (see g_regions), so the order here is also the
    // order the player meets them: a fast melee pack first, then something that
    // shoots back, and only once the run is deep does the heavy one show up.
    int max_choices = 1;
    if (g_difficulty >= 3) max_choices = 2;
    if (g_difficulty >= 6) max_choices = rg->mob_count;
    if (max_choices > rg->mob_count) max_choices = rg->mob_count;

    // A wave doubles the batch: the spike is felt, not just read on a banner.
    int group = 1 + g_difficulty / 4;
    if (g_wave_active > 0)
        group += 2;
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
// The boss duel: trigger, boss brain, and the side-on arena.
//
// The field phase ends the moment the ingredient target fills. The boss leaps
// at the camera (DUEL_LEAP_OUT, drawn over the field), every small monster is
// poofed away, and the fight moves to a single-screen side-view arena: run,
// jump, roll with i-frames, and spend the cards the field phase built. The
// boss itself stays a plain Monster, so projectiles, cards, orbs and
// ultimates all keep their exact field behaviour -- only its brain and the
// drawing change.
// ---------------------------------------------------------------------------

static void duel_draw(Monster *b);
static void duel_leap_out_draw(void);
static void hud_draw_slots(void);
static void hud_draw_ultimate(void);
// Defined with the field's other drawing code, but the arena needs it too.
static void weather_draw(void);
static void draw_heart(int x, int y, uint8_t fill);

void duel_begin_leap_out(void)
{
    g_duel.phase = DUEL_LEAP_OUT;
    g_duel.timer = 50;

    // Clear the stage: everything small pops, every drop flies to the
    // capybaras. The vacuum is the reward for filling the bar, and it also
    // means nothing of value is left behind when the scene changes.
    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        Monster *m = &g_monsters[i];
        if (!m->alive || m->is_boss)
            continue;
        int32_t cx, cy;
        monster_center(m, &cx, &cy);
        spawn_effect(FX_RAM_DUST, cx, cy, 18, 12);
        m->alive = false;
    }
    int32_t px = G_WORLD_W / 2, py = G_WORLD_H / 2;
    if (g_heroes[g_local_hero].active)
        hero_center(&g_heroes[g_local_hero], &px, &py);
    vacuum_all_gems(px, py);

    g_combo = 0;
    g_combo_timer = 0;
    show_toast("霸主出现了！", PAL_UI2_RED, 240);
    audio_play_music(MUS_BOSS);
    audio_play_sfx(SND_EXPLOSION);
    g_screen_shake = 5;
}

// The in-field beat: the boss sprite rushes at the camera, growing, while the
// screen shakes. Drawn over the field by game_update for the countdown's
// duration, then the state flips to ST_DUEL and the arena takes over.
static bool duel_leap_out_update(void)
{
    if (g_duel.phase != DUEL_LEAP_OUT)
        return false;
    if (g_duel.timer > 0)
        g_duel.timer--;

    if (g_duel.timer == 0)
    {
        // Switch to the arena. The boss is spawned here, mid-sky, and the
        // intro drops it on the player.
        const RegionDef *rg = &g_regions[g_region_id];
        const DuelDef *dd = &g_duel_defs[g_region_id];
        Monster *b = spawn_monster(0, AREA_W / 2, 0, true, rg->boss_id);
        if (b)
        {
            // 霸主 (duel bosses). The health is the duel definition's own --
            // it is budgeted against what a built-up capybara actually lands
            // per second in the arena (cards at Lv3-5 plus a couple of tree
            // passives), because this fight is meant to run the better part of
            // two minutes with three phases in it. Difficulty still nudges it
            // for a linked pair playing the same region late.
            b->hp_max = (uint16_t)(dd->hp + g_difficulty * 35);
            b->hp = b->hp_max;
            b->radius = 46;
        }
        // The collision centre rides the boss's lower body -- a hero on the
        // ground has to be able to actually reach the thing it is fighting.
        g_duel.phase = DUEL_INTRO;
        g_duel.timer = 150;
        g_duel.bx = AREA_W / 2;
        g_duel.by = DUEL_GROUND_Y - DUEL_BOSS_CTR_H;
        g_duel.air = 220;
        g_duel.bvy = 0;
        g_duel.fsm = DB_IDLE;
        g_duel.fsm_t = 30;
        g_duel.last_atk = 0xFF;
        g_duel.atk_count = 0;
        g_duel.enrage = false;
        g_duel.contact_cd = 0;
        // The pattern walk restarts for every fight, so the boss always opens
        // with the move that was written to be learned first.
        g_duel.step = 0;
        g_duel.chain = MV_NONE;
        g_duel.move = MV_NONE;
        g_duel.boss_phase = 0;
        g_duel.stagger_timer = 0;
        g_duel.burrowed = false;
        g_duel.dying = 0;
        g_duel.dying_final = false;
        g_duel.intro_timer = 110;
        g_duel.shot_n = 0;
        g_duel.shot_t = 0;
        g_duel.dig_surf = 0;
        g_duel_active = true;

        for (int i = 0; i < 2; i++)
        {
            Hero *h = &g_heroes[i];
            if (!h->active)
                continue;
            h->x = (i == 0 ? 48 : AREA_W - 80);
            h->y = DUEL_GROUND_Y - HERO_SPRITE_H;
            h->facing = (i == 0) ? DIR_RIGHT : DIR_LEFT;
            h->jump_vy = 0;
            h->roll_timer = 0;
            h->roll_cd = 0;
            h->roll_air = false;
            h->invuln = 60;
            h->ram_timer = 0;
            h->hurt_stun = 0;
            for (int c = 0; c < CARD_COUNT; c++)
                h->cooldown[c] = 0;
        }
        g_state = ST_DUEL;
        render_camera(0, 0);
    }
    return true;
}

// ---------------------------------------------------------------------------
// The duel boss brain
//
// Three ideas hold this fight together, and all three are borrowed from the
// games this rework is modelled on:
//
//   1. The boss walks a WRITTEN pattern, not a random wheel. It reads its
//      rotation in order, skipping moves whose phase has not arrived. A player
//      who survives one loop has genuinely learned something.
//   2. Every move TELEGRAPHS, and the telegraph is the same shape in every
//      region: a ring means "it lands here", a lane means "it crosses here".
//      Learning to read one boss teaches you to read all of them.
//   3. Every move ends in a RECOVERY. That recovery is the player's turn, and
//      its length is written into the pattern -- so the fight has a rhythm of
//      threat and opportunity rather than a constant stream of both.
//
// Phase changes and the stagger are the two things layered on top: health gates
// add new moves to the rotation, and breaking the boss's poise buys a free
// damage window.
// ---------------------------------------------------------------------------

// The pattern entry for a move id, or NULL. Patterns never repeat a move id
// within one boss, so a linear scan is both correct and cheap.
static const DuelMove *duel_move_for(const DuelDef *dd, uint8_t move)
{
    for (int i = 0; i < dd->pattern_n; i++)
        if (dd->pattern[i].move == move)
            return &dd->pattern[i];
    return NULL;
}

// The boss's own "am I untouchable" test. Roaring during a phase change and
// burrowed underground are the two states where hitting it does nothing; both
// are states the player can see, so neither is a lie.
static bool duel_boss_immune(void)
{
    return g_duel.fsm == DB_ROAR || g_duel.burrowed;
}

// Aim at the local hero. In a linked game the host simulates the boss, and the
// client's capybara is the one whose position the host actually has, so every
// aimed move is aimed at whoever this console drives.
static void duel_boss_aim(int32_t *hx, int32_t *hy)
{
    Hero *h = &g_heroes[g_local_hero];
    if (h->active)
        hero_center(h, hx, hy);
    else
    {
        *hx = AREA_W / 2;
        *hy = DUEL_GROUND_Y - HERO_SPRITE_H / 2;
    }
}

// A lane of ground, telegraphed as a filling rectangle. Used by the dash and
// the beam, which are the same shape to the player: "do not be standing here".
static void telegraph_lane(int32_t x0, int32_t x1, uint8_t life, uint8_t h)
{
    if (x1 < x0)
    {
        int32_t t = x0;
        x0 = x1;
        x1 = t;
    }
    Telegraph *t = spawn_telegraph(TG_LINE, x0, DUEL_GROUND_Y, life, 0);
    if (!t)
        return;
    t->x2 = x1;
    t->y2 = DUEL_GROUND_Y - h;
    t->radius = h;
}

// The phase a boss's health puts it in. Written once so the roar trigger, the
// move filter and the HUD all agree.
static int duel_phase_from_hp(const Monster *b)
{
    if (b->hp_max == 0)
        return 0;
    int pct = (int)(b->hp * 100 / b->hp_max);
    if (pct <= BOSS_PHASE3_PCT) return 2;
    if (pct <= BOSS_PHASE2_PCT) return 1;
    return 0;
}

// Begin a move: set the state, the timer, and lay down whatever the player has
// to read. Every branch here is a shape on the floor before it is a hit.
static void duel_begin_move(Monster *b, uint8_t move, const DuelMove *dm)
{
    const DuelDef *dd = &g_duel_defs[g_region_id];
    int32_t hx, hy;
    duel_boss_aim(&hx, &hy);

    g_duel.move = move;
    g_duel.fsm = DB_WIND;
    g_duel.fsm_t = dm->wind;
    g_duel.tgt_x = hx;
    g_duel.tgt_y = hy;
    g_duel.shot_n = 0;
    g_duel.shot_t = 0;
    g_duel.dig_surf = 0;

    switch (move)
    {
    case MV_DASH:
    {
        // A lane from wherever the boss stands to the far wall, in the
        // direction of the hero. A dash chosen with no runway would be a
        // shrug, so it always commits to a full crossing.
        int dir = (hx > g_duel.bx) ? 1 : -1;
        b->dx = (int8_t)dir;
        int32_t from = g_duel.bx;
        int32_t to = (dir > 0) ? DUEL_X_MAX : DUEL_X_MIN;
        telegraph_lane(from, to, (uint8_t)dm->wind, 22);
        break;
    }
    case MV_LEAP:
        spawn_telegraph(TG_CIRCLE, hx, DUEL_GROUND_Y - 4,
                        (uint8_t)dm->wind, 30);
        break;
    case MV_SWEEP:
    {
        // A half-ring in front of the boss: the arc that the sweep will cover.
        int dir = (hx > g_duel.bx) ? 1 : -1;
        Telegraph *t = spawn_telegraph(TG_ARC, g_duel.bx, g_duel.by,
                                       (uint8_t)dm->wind, 58);
        if (t)
        {
            t->x2 = g_duel.bx + dir * 58;
            t->y2 = DUEL_GROUND_Y;
        }
        break;
    }
    case MV_SPIKES:
    {
        // A chain: three spots on the ground, lighting in order, so the player
        // reads a sequence rather than a single slam.
        int32_t pts[3 * 3];
        for (int k = 0; k < 3; k++)
        {
            int32_t sx = hx + (k - 1) * 44;
            if (sx < 20) sx = 20;
            if (sx > AREA_W - 20) sx = AREA_W - 20;
            pts[k * 3 + 0] = sx;
            pts[k * 3 + 1] = DUEL_GROUND_Y - 6;
            pts[k * 3 + 2] = 22;
        }
        spawn_telegraph_chain(pts, 3, 16, 8);
        g_duel.shot_n = 3;
        break;
    }
    case MV_LOB:
        for (int k = 0; k < dd->volley_n; k++)
        {
            int32_t lx = hx + (k - (dd->volley_n - 1) / 2) * 30;
            if (lx < 20) lx = 20;
            if (lx > AREA_W - 20) lx = AREA_W - 20;
            spawn_telegraph(TG_CIRCLE, lx, DUEL_GROUND_Y - 4,
                            (uint8_t)(dm->wind + k * 6), 24);
        }
        break;
    case MV_DIG:
        // No shape yet. The boss is not attacking a place, it is diving; the
        // ring appears under the hero once it is actually underground, which
        // is the tell that it is hunting rather than aiming.
        break;
    case MV_LASER:
    {
        int32_t from = (hx > g_duel.bx) ? g_duel.bx : 20;
        int32_t to = (hx > g_duel.bx) ? AREA_W - 20 : g_duel.bx;
        telegraph_lane(from, to, (uint8_t)dm->wind, 16);
        break;
    }
    case MV_RAIN:
        for (int k = 0; k < 5; k++)
        {
            int32_t rx = 30 + (int32_t)(my_rand() % (AREA_W - 60));
            spawn_telegraph(TG_CIRCLE, rx, DUEL_GROUND_Y - 4,
                            (uint8_t)(dm->wind + k * 5), 20);
        }
        break;
    case MV_VOLLEY:
    case MV_BURST:
    case MV_SUMMON:
    default:
        // These announce themselves with the boss's own pose and a growing
        // charge, which the draw pass handles: a mouth glow for the volley,
        // an inflating body for the nova.
        break;
    }

    audio_play_sfx(SND_UI_MOVE);
}

// Fire whatever the current move owes at this instant. Called every frame while
// the move runs, with the timer counting down.
static void duel_run_move(Monster *b, const DuelMove *dm)
{
    const DuelDef *dd = &g_duel_defs[g_region_id];
    int dash_spd = dd->dash_speed + (g_duel.boss_phase > 0 ? 1 : 0);
    int volley_n = dd->volley_n + (g_duel.boss_phase > 0 ? 1 : 0);

    switch (g_duel.move)
    {
    case MV_DASH:
    {
        // The crossing itself. Contact damage on the body does the work; the
        // lane told the player where not to be.
        g_duel.bx += b->dx * dash_spd;
        if ((g_duel.fsm_t & 3) == 0)
            spawn_effect(FX_RAM_DUST, g_duel.bx, g_duel.by + 20, 10, 10);
        break;
    }
    case MV_LEAP:
        // Rising then falling; handled by the shared vertical arc below.
        break;
    case MV_SWEEP:
    {
        // One arc, once, at the moment the wind-up ends. The draw pass shows
        // the arc sweeping; the damage is a single radial test in front.
        int dir = (b->dx < 0) ? -1 : 1;
        int32_t ax = g_duel.bx + dir * 30;
        spawn_effect(FX_TAIL, ax, g_duel.by, 14, 52);
        damage_around_heroes(ax, g_duel.by, 52, dd->contact_dmg);
        g_screen_shake = 3;
        audio_play_sfx(SND_ATTACK);
        break;
    }
    case MV_SPIKES:
    {
        // Eruptions in the same order the rings lit.
        if (g_duel.shot_n > 0 && (g_duel.fsm_t % 8) == 0)
        {
            int k = 3 - g_duel.shot_n;
            int32_t sx = g_duel.tgt_x + (k - 1) * 44;
            if (sx < 20) sx = 20;
            if (sx > AREA_W - 20) sx = AREA_W - 20;
            spawn_effect(FX_BITE, sx, DUEL_GROUND_Y - 10, 16, 22);
            damage_around_heroes(sx, DUEL_GROUND_Y - 10, 24, dd->contact_dmg);
            g_screen_shake = 2;
            audio_play_sfx(SND_ATTACK);
            g_duel.shot_n--;
        }
        break;
    }
    case MV_LOB:
    {
        // One arc per marked spot, landing on it.
        if (g_duel.shot_n == 0)
            g_duel.shot_n = dd->volley_n;
        if (g_duel.shot_t == 0)
        {
            int k = dd->volley_n - g_duel.shot_n;
            int32_t lx = g_duel.tgt_x + (k - (dd->volley_n - 1) / 2) * 30;
            if (lx < 20) lx = 20;
            if (lx > AREA_W - 20) lx = AREA_W - 20;
            spawn_mob_projectile(b, PROJ_LOB, lx, DUEL_GROUND_Y - 4, 0, -4, 30,
                             RANGED_CHIP_DAMAGE, 7);
            g_duel.shot_t = 10;
            g_duel.shot_n--;
            audio_play_sfx(SND_MAGIC_FIRE);
        }
        else
        {
            g_duel.shot_t--;
        }
        break;
    }
    case MV_DIG:
        // Handled by the burrowed branch in the caller: the whole move is the
        // hunt, so there is nothing to do here on the frames between surfaces.
        break;
    case MV_LASER:
    {
        // The beam owns the ground for as long as it lasts: jump it. Damage is
        // tested against the hero's feet, so a jump genuinely clears it.
        spawn_effect(FX_RAM_DUST, g_duel.bx, DUEL_GROUND_Y - 8, 8, 14);
        for (int i = 0; i < 2; i++)
        {
            Hero *oh = &g_heroes[i];
            if (!oh->active)
                continue;
            int32_t cx, cy;
            hero_center(oh, &cx, &cy);
            int feet = oh->y + HERO_SPRITE_H;
            // Feet near the floor means standing in the beam.
            if (feet > DUEL_GROUND_Y - 14 && oh->roll_timer == 0)
                hero_take_damage(i, dd->contact_dmg, g_duel.bx, DUEL_GROUND_Y);
        }
        g_screen_shake = 2;
        break;
    }
    case MV_RAIN:
    {
        // Impacts, one per frame slot, each on its own marked spot.
        if ((g_duel.fsm_t % 6) == 0)
        {
            int idx = (int)(g_duel.fsm_t / 6);
            int32_t rx = 30 + (int32_t)((idx * 53 + 17) % (AREA_W - 60));
            spawn_effect(FX_RAM_DUST, rx, DUEL_GROUND_Y - 6, 14, 20);
            damage_around_heroes(rx, DUEL_GROUND_Y - 6, 22, dd->contact_dmg);
            audio_play_sfx(SND_EXPLOSION);
        }
        break;
    }
    case MV_BURST:
    {
        // One nova, at the halfway point of the execution.
        if (g_duel.fsm_t == dm->act / 2)
        {
            spawn_effect(FX_TAIL, g_duel.bx, g_duel.by, 20, 52);
            damage_around_heroes(g_duel.bx, g_duel.by, 54, dd->contact_dmg);
            g_screen_shake = 3;
            audio_play_sfx(SND_EXPLOSION);
        }
        break;
    }
    case MV_VOLLEY:
    {
        // A spread fired in one beat, three beats in a row at higher phases.
        int beats = (g_duel.boss_phase >= 2) ? 3 : 2;
        if (g_duel.shot_n == 0)
            g_duel.shot_n = beats;
        if (g_duel.shot_t == 0)
        {
            int32_t hx, hy;
            duel_boss_aim(&hx, &hy);
            int32_t sx = g_duel.bx;
            int32_t sy = g_duel.by - 10;
            int32_t dx = hx - sx, dy = hy - sy;
            int len = (iabs(dx) > iabs(dy)) ? iabs(dx) : iabs(dy);
            len = len ? len : 1;
            for (int k = 0; k < volley_n; k++)
            {
                int spread = (k - (volley_n - 1) / 2) * 14;
                spawn_mob_projectile(b, PROJ_ENEMY, sx, sy,
                                 (int16_t)((dx * dd->proj_speed) / len),
                                 (int16_t)((dy * dd->proj_speed) / len + spread),
                                 130, RANGED_CHIP_DAMAGE, 5);
            }
            audio_play_sfx(SND_MAGIC_FIRE);
            g_duel.shot_t = 14;
            g_duel.shot_n--;
        }
        else
        {
            g_duel.shot_t--;
        }
        break;
    }
    case MV_SUMMON:
    {
        // One add, once, partway through. Capped so a long fight cannot turn
        // into a crowd: the duel is a duel.
        if (g_duel.fsm_t == dm->act - 6)
        {
            int live = 0;
            for (int i = 0; i < MAX_MONSTERS; i++)
                if (g_monsters[i].alive && !g_monsters[i].is_boss)
                    live++;
            if (live < 3)
            {
                // Region-flavoured: the roster's swarm mob, which is the fast
                // one and therefore the one that matters in a small arena.
                const RegionDef *rg = &g_regions[g_region_id];
                spawn_monster(rg->mob_ids[0], g_duel.bx, DUEL_GROUND_Y - 40,
                              false, 0);
            }
            spawn_effect(FX_PACIFY, g_duel.bx, g_duel.by, 24, 30);
            audio_play_sfx(SND_PICKUP);
        }
        break;
    }
    default:
        break;
    }
}

// Boss brain. Walks the region's written pattern, with phase gates, a
// telegraphed wind-up before every move, and a stagger when its poise breaks.
static void duel_boss_ai(Monster *b)
{
    const DuelDef *dd = &g_duel_defs[g_region_id];
    int32_t hx, hy;
    duel_boss_aim(&hx, &hy);

    // --- stagger -----------------------------------------------------------
    // Poise breaking drops the boss to its knees. It is the player's biggest
    // opening, so it interrupts whatever the boss was doing -- including a
    // wind-up, which means a well-timed stagger can cancel a lethal move.
    if (b->stagger_immune > 0)
        b->stagger_immune--;
    if (b->stagger_timer > 0)
    {
        b->stagger_timer--;
        g_duel.fsm = DB_STAGGER;
        g_duel.move = MV_NONE;
        g_duel.chain = MV_NONE;
        if (b->stagger_timer == 0)
        {
            g_duel.fsm = DB_RECOVER;
            g_duel.fsm_t = 40;
            // Poise recovery, same as the field bosses: getting up buys a span
            // of immunity so the fight keeps its rhythm instead of collapsing
            // into a permanent knockdown.
            b->stagger_immune = STAGGER_IMMUNE;
            // The boss stands up angry: a short phase-appropriate haste so the
            // punish window is not simply "free hits, then nothing".
            g_duel.shot_t = 0;
        }
        b->x = g_duel.bx;
        b->y = g_duel.by;
        return;
    }

    // Poise window bookkeeping. The window is what makes the stagger reachable
    // without a dedicated meter on the HUD: land STAGGER_HITS hits inside it
    // and it breaks, so sustained pressure is its own reward.
    if (b->stagger_window > 0)
    {
        b->stagger_window--;
        if (b->stagger_window == 0)
            b->stagger_hits = (uint8_t)(b->stagger_hits / 2);
    }

    // --- phase change ------------------------------------------------------
    // Health gates the escalation. The roar is invulnerable and immobile: it is
    // a beat, not a move, and the player is meant to use it to breathe.
    int hp_phase = duel_phase_from_hp(b);
    if (hp_phase > g_duel.boss_phase && g_duel.fsm != DB_ROAR)
    {
        g_duel.boss_phase = (uint8_t)hp_phase;
        g_duel.fsm = DB_ROAR;
        g_duel.fsm_t = BOSS_PHASE_ROAR;
        g_duel.move = MV_NONE;
        g_duel.chain = MV_NONE;
        g_screen_shake = 6;
        audio_play_sfx(SND_EXPLOSION);
        spawn_effect(FX_LEVELUP, g_duel.bx, g_duel.by, BOSS_PHASE_ROAR, 40);
        if (hp_phase == 1)
            show_toast("霸主狂暴了！", PAL_UI2_RED, 200);
        else
            show_toast("霸主拼命了！", PAL_UI2_RED, 200);
    }

    if (g_duel.contact_cd > 0)
        g_duel.contact_cd--;

    int idle_frames = (g_duel.boss_phase >= 2) ? 14
                     : (g_duel.boss_phase == 1 ? 22 : 34);

    switch (g_duel.fsm)
    {
    case DB_ROAR:
    {
        g_duel.fsm_t--;
        if ((g_duel.fsm_t & 3) == 0)
            spawn_effect(FX_RAM_DUST, g_duel.bx, g_duel.by + 16, 8, 16);
        if (g_duel.fsm_t == 0)
        {
            g_duel.fsm = DB_IDLE;
            g_duel.fsm_t = idle_frames;
            // A phase turn resets the pattern to the top, so the new moves are
            // met in the order they were written.
            g_duel.step = 0;
        }
        break;
    }
    case DB_IDLE:
    {
        g_duel.fsm_t--;
        if (g_duel.fsm_t == 0)
        {
            g_duel.fsm = DB_WALK;
            g_duel.fsm_t = 34 + (int)(my_rand() % 26);
        }
        break;
    }
    case DB_WALK:
    {
        // Close the gap, then begin the next written move.
        g_duel.fsm_t--;
        int dir = (hx > g_duel.bx) ? 1 : -1;
        b->dx = (int8_t)dir;
        g_duel.bx += dir * dd->walk_speed;

        if (g_duel.fsm_t == 0)
        {
            // Walk the pattern until a move this phase is allowed to use. The
            // scan always terminates because every pattern has at least one
            // phase-0 entry.
            uint8_t move = MV_DASH;
            const DuelMove *dm = &dd->pattern[0];
            for (int tries = 0; tries < 16; tries++)
            {
                const DuelMove *cand = &dd->pattern[g_duel.step % dd->pattern_n];
                g_duel.step = (uint8_t)((g_duel.step + 1) % dd->pattern_n);
                if (cand->phase <= g_duel.boss_phase)
                {
                    dm = cand;
                    move = cand->move;
                    break;
                }
            }
            g_duel.atk_count++;
            duel_begin_move(b, move, dm);
        }
        break;
    }
    case DB_WIND:
    {
        g_duel.fsm_t--;
        const DuelMove *dm = duel_move_for(dd, g_duel.move);

        // A little strain shake, so a wind-up reads as effort and not as lag.
        if ((g_frame & 1) == 0 && g_duel.move != MV_DIG)
            b->dx = (int8_t)((hx > g_duel.bx) ? 1 : -1);

        // Dig is the one move that starts working during its own wind-up: the
        // boss submerges first, and only then does the ring appear under the
        // hero. Sinking is drawn, not simulated -- `air` goes negative so the
        // sprite slides below the floor line.
        if (g_duel.move == MV_DIG && dm)
        {
            int sink = (int)dm->wind - (int)g_duel.fsm_t;
            g_duel.air = -(int16_t)(sink * 3 > 64 ? 64 : sink * 3);
        }

        if (g_duel.fsm_t == 0)
        {
            g_duel.fsm = DB_ACT;
            g_duel.fsm_t = dm ? dm->act : 30;
            g_duel.shot_t = 0;
            g_duel.shot_n = 0;

            if (g_duel.move == MV_DIG && dm)
            {
                // Underground and untouchable, tracking the hero. The ring
                // follows the hero for the first half, then locks: long enough
                // to walk out of, short enough that standing still is fatal.
                g_duel.burrowed = true;
                g_duel.dig_surf = dm->chain == MV_DIG ? 3 : 1;
                g_duel.air = -64;
                spawn_telegraph(TG_CIRCLE, hx, DUEL_GROUND_Y - 4,
                                (uint8_t)(dm->act / 2), 28);
            }
            if (g_duel.move == MV_LEAP)
            {
                g_duel.bvy = -13;
                audio_play_sfx(SND_ATTACK);
            }
        }
        break;
    }
    case DB_ACT:
    {
        g_duel.fsm_t--;
        const DuelMove *dm = duel_move_for(dd, g_duel.move);

        // --- burrowed: surface under the hero -----------------------------
        if (g_duel.burrowed)
        {
            // Track for the first half of the move, then hold the locked spot.
            if (g_duel.fsm_t > (uint16_t)(dm->act / 2))
            {
                g_duel.bx += (hx > g_duel.bx) ? 2 : -2;
                g_duel.tgt_x = hx;
            }
            else
            {
                g_duel.bx += (g_duel.tgt_x > g_duel.bx) ? 2 : -2;
            }
            if (g_duel.bx < 30) g_duel.bx = 30;
            if (g_duel.bx > AREA_W - 30) g_duel.bx = AREA_W - 30;

            if (g_duel.fsm_t == 0)
            {
                // Burst out. The bite reaches wider than the ring that warned
                // of it, so stepping out is required rather than merely wise.
                g_duel.burrowed = false;
                g_duel.air = 0;
                g_screen_shake = 4;
                audio_play_sfx(SND_EXPLOSION);
                spawn_effect(FX_MUD, g_duel.bx, DUEL_GROUND_Y - 6, 24, 30);
                damage_around_heroes(g_duel.bx, DUEL_GROUND_Y - 20, 34,
                                     dd->contact_dmg);

                if (g_duel.dig_surf > 1)
                {
                    // Chain: back under, hunt again. The chain length came
                    // from the pattern, so the player was told how many times.
                    g_duel.dig_surf--;
                    g_duel.burrowed = true;
                    g_duel.air = -64;
                    g_duel.fsm = DB_ACT;
                    g_duel.fsm_t = (uint16_t)(dm->act / 2);
                    duel_boss_aim(&g_duel.tgt_x, &g_duel.tgt_y);
                    spawn_telegraph(TG_CIRCLE, g_duel.tgt_x,
                                    DUEL_GROUND_Y - 4,
                                    (uint8_t)(dm->act / 2), 28);
                    break;
                }
                g_duel.fsm = DB_RECOVER;
                g_duel.fsm_t = dm->rec;
                g_duel.move = MV_NONE;
            }
            break;
        }

        // --- the leap owns its own vertical arc ---------------------------
        if (g_duel.move == MV_LEAP)
        {
            if (g_duel.bvy < 0)
            {
                g_duel.air = (int16_t)(g_duel.air - g_duel.bvy);
                g_duel.bvy += 1;
                g_duel.bx += (hx > g_duel.bx) ? 2 : -2;
                if (g_duel.bvy >= 0)
                {
                    g_duel.bvy = 2;
                    g_duel.lock_x = hx;
                }
            }
            else
            {
                g_duel.air = (int16_t)(g_duel.air - g_duel.bvy);
                g_duel.bvy += 2;
                g_duel.bx += (g_duel.lock_x > g_duel.bx) ? 1 : -1;
                if (g_duel.air <= 0)
                {
                    g_duel.air = 0;
                    g_duel.bvy = 0;
                    g_screen_shake = 6;
                    audio_play_sfx(SND_EXPLOSION);
                    spawn_effect(FX_RAM_DUST, g_duel.bx, DUEL_GROUND_Y - 6,
                                 22, 34);
                    // Two ground waves race out. Jump, and they pass under.
                    for (int k = -1; k <= 1; k += 2)
                        spawn_mob_projectile(b, PROJ_SHOCK, g_duel.bx + k * 14,
                                         DUEL_GROUND_Y - 5, (int16_t)(k * 2), 0,
                                         90, RANGED_CHIP_DAMAGE, 8);
                }
            }
        }
        else
        {
            duel_run_move(b, dm);
        }

        if (g_duel.fsm_t == 0)
        {
            // A chained move runs straight on with no walk between, which is
            // how the patterns escalate without a second state machine.
            if (dm && dm->chain != MV_NONE && dm->chain != MV_DIG)
            {
                const DuelMove *next = duel_move_for(dd, dm->chain);
                if (next)
                {
                    duel_begin_move(b, dm->chain, next);
                    break;
                }
            }
            g_duel.fsm = DB_RECOVER;
            g_duel.fsm_t = (uint16_t)(dm ? dm->rec : 40);
            g_duel.move = MV_NONE;
        }
        break;
    }
    case DB_RECOVER:
    default:
    {
        // The player's turn. The boss stands up, breathes, and starts again.
        g_duel.fsm_t--;
        if (g_duel.fsm_t == 0)
        {
            g_duel.fsm = DB_IDLE;
            g_duel.fsm_t = (uint16_t)idle_frames;
        }
        break;
    }
    }

    // Arena clamp.
    if (g_duel.bx < 30) g_duel.bx = 30;
    if (g_duel.bx > AREA_W - 30) g_duel.bx = AREA_W - 30;

    // Mirror the sim onto the monster so every existing damage path and the
    // network sync see exactly one source of truth.
    b->x = g_duel.bx;
    b->y = g_duel.by - g_duel.air;

    // Touch damage: the body hurts on contact, gated like every melee mob. A
    // burrowed or roaring boss has no body to walk into, so neither one does.
    if (duel_boss_immune())
        return;

    int half = 32 * dd->scale_pct / 100;
    int reach = half * 3 / 4 + 8;
    if (g_duel.contact_cd == 0 &&
        dist2(b->x, b->y, hx, hy) < reach * reach)
    {
        for (int i = 0; i < 2; i++)
        {
            Hero *oh = &g_heroes[i];
            if (!oh->active)
                continue;
            int32_t ox, oy;
            hero_center(oh, &ox, &oy);
            if (dist2(b->x, b->y, ox, oy) < reach * reach)
                hero_take_damage(i, dd->contact_dmg, b->x, b->y);
        }
        g_duel.contact_cd = 40;
    }
}

void duel_apply_client_damage(int dmg)
{
    Monster *b = duel_boss();
    if (b && dmg > 0)
        damage_monster(b, dmg, b->x, b->y, false, true);
}

// net.c calls this on the host with damage a client's capybara dealt in the
// field. The client's mirrors sit at the host's exact positions, so the
// nearest hostile creature to the reported spot is the intended target; the
// kill (and its exp, drops and token credit) then happens wholly on the
// authoritative side.
void field_apply_client_damage(int dmg, int32_t x, int32_t y)
{
    if (dmg <= 0 || g_duel_active)
        return;

    Monster *best = NULL;
    int32_t best_d = 28 * 28;
    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        Monster *m = &g_monsters[i];
        if (!m->alive || m->recruited)
            continue;
        int32_t cx, cy;
        monster_center(m, &cx, &cy);
        int32_t d = dist2(cx, cy, x, y);
        if (d < best_d)
        {
            best_d = d;
            best = m;
        }
    }
    if (best)
        damage_monster(best, dmg, x, y, false, true);
}

// The duel frame. Runs on both consoles: the host owns the boss brain, the
// client drives its own capybara and renders the mirrored state.
void duel_update(void)
{
    g_frame++;
    g_run_time++;
    // The defeat beat runs here too: a duel loss holds the arena for its
    // moment before the game-over plate arrives.
    if (g_hero_death_beat > 0 && --g_hero_death_beat == 0)
        g_state = ST_GAME_OVER;
    if (g_toast.life > 0)
        g_toast.life--;
    render_camera(0, 0);

    Monster *b = duel_boss();
    bool authority = (!g_multiplayer || g_is_host);

    if (!authority && g_duel.phase == DUEL_OFF)
    {
        // Client entry: the packet flipped g_state to ST_DUEL, so park this
        // console's capybara in the arena. The boss itself is never
        // simulated here -- its packets carry position and health.
        Hero *me = &g_heroes[g_local_hero];
        if (me->active)
        {
            me->x = 48;
            me->y = DUEL_GROUND_Y - HERO_SPRITE_H;
            me->facing = DIR_RIGHT;
            me->jump_vy = 0;
            me->roll_timer = 0;
            me->roll_cd = 0;
            me->roll_air = false;
            me->invuln = 60;
            me->hurt_stun = 0;
            for (int c = 0; c < CARD_COUNT; c++)
                me->cooldown[c] = 0;
        }
        g_duel.phase = DUEL_FIGHT;
        g_duel.bx = AREA_W / 2;
        g_duel.by = DUEL_GROUND_Y - DUEL_BOSS_CTR_H;
        g_duel.enrage = false;
        g_duel.fsm = DB_IDLE;
        g_duel_active = true;
    }

    // Capybaras first, exactly like the field: movement, timers, orbs.
    hero_update(0);
    if (g_multiplayer && g_heroes[1].active)
        hero_update(1);
    hero_update_ultimate(0);
    hero_update_ultimate(1);

    if (authority && g_duel.phase == DUEL_INTRO)
    {
        // The boss hangs over the arena while its name banner shows, then
        // crashes down; the landing is the starting gun.
        g_duel.timer--;
        if (g_duel.timer > 100)
        {
            g_duel.bvy = 0;   // hovering: hold the drop for the announcement
        }
        else
        {
            g_duel.bvy += 1;
            g_duel.air = (int16_t)(g_duel.air - g_duel.bvy);
        }
        if (g_duel.air <= 0)
        {
            g_duel.air = 0;
            g_duel.phase = DUEL_FIGHT;
            g_duel.fsm = DB_IDLE;
            g_duel.fsm_t = 40;
            g_screen_shake = 7;
            audio_play_sfx(SND_EXPLOSION);
            spawn_effect(FX_RAM_DUST, g_duel.bx, DUEL_GROUND_Y - 6, 24, 36);
        }
        if (b)
        {
            b->x = g_duel.bx;
            b->y = g_duel.by - g_duel.air;
        }
    }
    else if (g_duel.phase == DUEL_FIGHT)
    {
        if (authority && b)
            duel_boss_ai(b);
    }

    // Cards fire on their own timers and on the duel buttons; the auto-bite
    // gives the capybara a melee answer when the boss is in its face.
    hero_handle_active_skills(g_local_hero);
    hero_fire_cards(g_local_hero);
    if (!g_multiplayer && g_heroes[g_local_hero ^ 1].active)
        hero_fire_cards(g_local_hero ^ 1);

    if (g_heroes[g_local_hero].cooldown[CARD_TEETH] == 0)
        hero_auto_bite(g_local_hero);
    if (!g_multiplayer && g_heroes[g_local_hero ^ 1].active &&
        g_heroes[g_local_hero ^ 1].cooldown[CARD_TEETH] == 0)
        hero_auto_bite(g_local_hero ^ 1);

    // Effects and projectiles are gameplay in the duel too (mud, stink, dung
    // traps and the boss's shockwaves all live in these pools).
    if (authority)
    {
        projectile_update();
        effect_update();
        pickup_update();
        telegraph_update();
    }

    // The death beat: the arena holds while what the player just earned rains
    // out of the boss. Everything else keeps running so the loot actually
    // flies to them -- only the boss's brain is stopped, because it is dead.
    if (g_duel.dying > 0)
    {
        g_duel.dying--;
        if ((g_duel.dying & 3) == 0)
            g_screen_shake = 3;
        if (g_duel.dying == 0)
        {
            if (g_duel.dying_final)
            {
                g_ingredients_done |= (uint8_t)(1 << g_region_id);
                // The wait is over: nothing is pending at this gate any more.
                g_duel_pending = -1;
                audio_play_music(MUS_VICTORY);
                g_state = ST_LEVEL_CLEAR;
            }
            else if (!g_boss_out)
            {
                audio_play_music(region_battle_music());
            }
        }
        duel_draw(NULL);
        return;
    }

    // A fallen or victorious boss ends the state from inside the damage
    // paths; just stand down cleanly here.
    if (g_state != ST_DUEL)
    {
        g_duel_active = false;
        g_duel.phase = DUEL_OFF;
        return;
    }

    duel_draw(b);
}

// ---------------------------------------------------------------------------
// Duel drawing
// ---------------------------------------------------------------------------

// Region-tinted arena backdrop. Flat bands and simple silhouettes: the arena
// has to read at a glance in the middle of a fight, and the boss art carries
// the spectacle.
static void duel_draw_arena(void)
{
    switch (g_region_id)
    {
    case 0:   // 雨林: deep green canopy, hanging vines
        gfx_fill_rect(0, 0, AREA_W, AREA_H, PAL_PINE_DARK);
        gfx_fill_rect(0, 60, AREA_W, 40, PAL_GRN_DARK);
        for (int x = 0; x < AREA_W; x += 34)
        {
            gfx_fill_rect(x + 6, 96, 5, 26 + (x % 3) * 5, PAL_GRN_MID);
            gfx_fill_rect(x + 22, 100, 4, 20 + (x % 4) * 4, PAL_LEAF_DARK);
        }
        gfx_fill_rect(0, 118, AREA_W, 3, PAL_LEAF_MID);
        break;
    case 1:   // 农场: golden dusk, wheat and a windmill silhouette
        gfx_fill_rect(0, 0, AREA_W, AREA_H, PAL_SAND_DARK);
        gfx_fill_rect(0, 66, AREA_W, 34, PAL_WHEAT_DARK);
        gfx_fill_rect(60, 52, 4, 46, PAL_TRUNK_DARK);
        gfx_fill_rect(52, 58, 20, 3, PAL_TRUNK_MID);
        gfx_fill_rect(52, 66, 20, 3, PAL_TRUNK_MID);
        for (int x = 0; x < AREA_W; x += 6)
            gfx_fill_rect(x, 112 + ((x / 6) % 2), 3, 6, PAL_WHEAT_MID);
        break;
    case 2:   // 牧场: pale sky, mountains, a fence line
        gfx_fill_rect(0, 0, AREA_W, AREA_H, PAL_BLUE_DARK);
        gfx_fill_rect(0, 74, AREA_W, 26, PAL_SNOW_DARK);
        for (int x = 0; x < AREA_W; x += 48)
        {
            for (int k = 0; k < 14; k++)
                gfx_fill_rect(x + 24 - k, 92 - k, k * 2, 1, PAL_GREY_DARK);
        }
        for (int x = 8; x < AREA_W; x += 26)
            gfx_fill_rect(x, 104, 3, 14, PAL_TRUNK_MID);
        gfx_fill_rect(0, 108, AREA_W, 2, PAL_TRUNK_DARK);
        break;
    case 3:   // 沼泽: murky murk, cane stalks, rising bubbles
        gfx_fill_rect(0, 0, AREA_W, AREA_H, PAL_MURK_DARK);
        gfx_fill_rect(0, 70, AREA_W, 30, PAL_PINE_DARK);
        for (int x = 10; x < AREA_W; x += 22)
        {
            gfx_fill_rect(x, 96 - (x % 3) * 6, 2, 26 + (x % 3) * 6,
                          PAL_GRN_DARK);
            gfx_fill_rect(x - 2, 94 - (x % 3) * 6, 6, 2, PAL_GRN_MID);
        }
        for (int k = 0; k < 5; k++)
        {
            int by2 = 140 - (int)((g_frame / 3 + k * 37) % 60);
            gfx_circle_outline(30 + k * 48, by2, 2, PAL_SEA_MID);
        }
        break;
    default:  // 火山: ember sky over a volcano cone
        gfx_fill_rect(0, 0, AREA_W, AREA_H, PAL_ASH_DARK);
        gfx_fill_rect(0, 60, AREA_W, 36, PAL_LAVA_DARK);
        gfx_fill_rect(0, 92, AREA_W, 2, PAL_EMBER_ORANGE);
        for (int k = -20; k <= 20; k += 4)
        {
            int h = 20 - iabs(k);
            gfx_fill_rect(128 + k - 2, 94 - h, 4, h, PAL_ASH_MID);
        }
        gfx_fill_rect(126, 74, 4, 3, PAL_EMBER_YELLOW);
        for (int k = 0; k < 7; k++)
        {
            int ey = 150 - (int)((g_frame / 2 + k * 29) % 90);
            int ex = (k * 41 + 17) % AREA_W;
            gfx_fill_rect(ex, ey, 2, 2,
                          (k & 1) ? PAL_EMBER_ORANGE : PAL_EMBER_YELLOW);
        }
        break;
    }

    // The ground: one solid slab with a lit lip, so "this is the floor you
    // fight on" is unambiguous for a game that is usually seen from above.
    gfx_fill_rect(0, DUEL_GROUND_Y, AREA_W, AREA_H - DUEL_GROUND_Y,
                  PAL_BRN_DARK);
    gfx_fill_rect(0, DUEL_GROUND_Y, AREA_W, 3, PAL_BRN_MID);
    gfx_fill_rect(0, DUEL_GROUND_Y + 3, AREA_W, 1, PAL_BRN_LIGHT);
    for (int x = 4; x < AREA_W; x += 18)
        gfx_fill_rect(x, DUEL_GROUND_Y + 12 + (x % 3) * 4, 8, 2, PAL_BRN_MID2);
}

// One shadow under a fighter; it shrinks as the fighter rises, which is how a
// side-view without lighting still sells height.
static void duel_draw_shadow(int32_t cx, int feet, int half_w, int height)
{
    (void)cx;
    int shrink = height / 8;
    if (shrink > half_w / 2) shrink = half_w / 2;
    if (shrink < 0) shrink = 0;
    gfx_fill_rect(cx - half_w + shrink, DUEL_GROUND_Y + 1,
                  (half_w - shrink) * 2, 3, PAL_OUTLINE_SOFT);
}

static void duel_draw_boss(const Monster *b)
{
    const DuelDef *dd = &g_duel_defs[g_region_id];
    int half = 32 * dd->scale_pct / 100;
    int32_t bx = b->x, by = b->y;
    int feet = by + DUEL_BOSS_CTR_H;

    duel_draw_shadow(bx, feet, half - 6, DUEL_GROUND_Y - feet);

    // A burrowed boss is under the floor: only the mound of moving earth
    // shows, which is the tell that it is tracking rather than aiming.
    if (g_duel.burrowed)
    {
        bool blink = ((g_frame >> 2) & 1);
        gfx_fill_rect(bx - 20, DUEL_GROUND_Y - 3, 40, 6, PAL_BRN_DARK);
        gfx_fill_rect(bx - 16, DUEL_GROUND_Y - 5, 32, 4,
                      blink ? PAL_BRN_MID : PAL_BRN_MID2);
        gfx_circle_outline(bx, DUEL_GROUND_Y - 2, 24,
                           blink ? PAL_WHITE_SPARK : PAL_RED_LIGHT);
        return;
    }

    // --- pose: squash and stretch ------------------------------------------
    // The sheet's walk frames differ by a few hundred pixels, which is nothing
    // at duel scale -- mid-fight the boss read as a sticker sliding around.
    // Body language does the talking instead: every state owns a silhouette,
    // Hollow-Knight style, so an attack is visible before it is dangerous.
    //
    //   wind-up  = crouch wide and low, shaking with effort
    //   dash     = stretched along the run
    //   leap     = stretched tall in the air, pancaked on landing
    //   volley   = knocked back a step per shot
    //   burst    = inflated while charging, popped on release
    int pct_x = 100, pct_y = 100;
    int ox = 0;   // pose lean / recoil, in pixels
    switch (g_duel.fsm)
    {
    case DB_IDLE:
        pct_y = ((g_frame >> 4) & 1) ? 98 : 100;      // breathing
        break;
    case DB_WALK:
        pct_y = ((g_frame >> 3) & 1) ? 96 : 100;      // step bob
        break;
    case DB_DASH_WIND:
    case DB_LEAP_WIND:
        pct_x = 118;
        pct_y = 68;
        ox = ((g_frame >> 1) & 1) ? 1 : -1;           // strain shake
        break;
    case DB_DASH:
        pct_x = 122;
        pct_y = 84;
        break;
    case DB_LEAP_RISE:
        pct_x = 82;
        pct_y = 120;
        break;
    case DB_LEAP_FALL:
        pct_x = 92;
        pct_y = 92;
        break;
    case DB_LAND:
        pct_x = 136;
        pct_y = 60;                                    // the pancake
        break;
    case DB_VOLLEY:
        ox = -(b->dx != 0 ? b->dx : 1) * 3;            // shot recoil
        pct_y = 92;
        break;
    case DB_BURST_WIND:
        pct_x = 112;
        pct_y = 108;                                   // inflating
        ox = ((g_frame >> 1) & 1) ? 2 : -2;
        break;
    case DB_BURST:
        pct_x = 128;
        pct_y = 88;                                    // the pop
        break;
    case DB_RECOVER:
    default:
        pct_x = 108;
        pct_y = 94;                                    // easing back up
        break;
    }

    // Frame: the pose is the read. Broken first, then wind-up, then the
    // signature pose of whatever move is running, then the ordinary cycle.
    int f;
    if (g_duel.stagger_timer > 0)                 f = BF_STAGGER;
    else if (b->hurt_timer > 0)                   f = BF_HURT;
    else if (g_duel.fsm == DB_WIND)               f = BF_WINDUP;
    else if (g_duel.fsm == DB_ACT ||
             g_duel.fsm == DB_DASH || g_duel.fsm == DB_LAND) f = BF_ATTACK;
    else if (g_duel.burrowed ||
             g_duel.fsm == DB_LEAP_RISE ||
             g_duel.fsm == DB_LEAP_FALL)          f = BF_SPECIAL_A;
    else if (g_duel.boss_phase >= 1)              f = BF_SPECIAL_B;
    else if ((g_frame >> 3) & 1)                  f = BF_BOB;

    int dw = half * 2 * pct_x / 100;
    int dh = half * 2 * pct_y / 100;
    // The 64x64 canvas carries the creature in a 48x48 inset with 8px of
    // padding all round (tools/gen_monsters.py, to_boss_canvas). The box bottom
    // therefore sits 1/8 below the creature's real feet, so the blit is shifted
    // down by exactly that much -- anchored to the box, the boss visibly floats
    // above the floor it is supposed to be standing on.
    int art_pad = dh / 8;
    gfx_blit_8bpp_scaled_xy(boss_tiles_bin, 64, 64,
                            b->boss_id * BOSS_FRAMES + f,
                            bx - dw / 2 + ox, feet - dh + art_pad,
                            b->dx < 0, true, pct_x, pct_y);

    // Telegraphs are drawn from the shared pool before the boss (see
    // duel_draw), so what is left here is the boss's own body language: the
    // mouth glow before a volley, the inflating body before a nova, the dust
    // under a charge, and the shiver before it commits.
    uint8_t tc = ((g_frame >> 2) & 1) ? PAL_RED_LIGHT : PAL_WHITE_SPARK;
    if (g_duel.fsm == DB_WIND && g_duel.move == MV_VOLLEY)
    {
        // A charging glow at the mouth: the tell that shots are coming, and
        // the reason the region-tinted shots themselves get outlined below.
        int r = 3 + (int)(g_duel.fsm_t / 6);
        int mx = bx + (b->dx < 0 ? -10 : 10);
        gfx_circle(mx, by - 10, r, PAL_RED_DARK);
        gfx_circle(mx, by - 10, r - 2 > 1 ? r - 2 : 1, PAL_RED_LIGHT);
    }
    else if (g_duel.fsm == DB_WIND && g_duel.move == MV_BURST)
    {
        // The blast radius grows in from nothing: when the ring reaches you,
        // it detonates.
        gfx_circle_outline(bx, by, 54, tc);
        gfx_circle_outline(bx, by, 50, tc);
    }
    else if (g_duel.fsm == DB_WIND && g_duel.move == MV_LASER)
    {
        // A lens charging above the head, so the beam has a visible source.
        int r = 2 + (int)(g_duel.fsm_t / 8);
        gfx_circle(bx, by - 26, r, PAL_EMBER_YELLOW);
        gfx_circle(bx, by - 26, r - 2 > 1 ? r - 2 : 1, PAL_WHITE_SPARK);
    }
    else if (g_duel.fsm == DB_ACT && g_duel.move == MV_LASER)
    {
        // The beam itself: a bright horizontal bar at chest height, with a hot
        // core. Jumping clears it; the ground does not.
        gfx_fill_rect(0, DUEL_GROUND_Y - 18, AREA_W, 14, tc);
        gfx_fill_rect(0, DUEL_GROUND_Y - 14, AREA_W, 6, PAL_EMBER_YELLOW);
        gfx_fill_rect(0, DUEL_GROUND_Y - 12, AREA_W, 2, PAL_WHITE_SPARK);
    }
    else if (g_duel.fsm == DB_ACT && g_duel.move == MV_DASH && (g_frame & 3) == 0)
    {
        int back = (b->dx < 0) ? half / 2 : -half / 2;
        spawn_effect(FX_RAM_DUST, bx + back, by + 14, 8, 8);
    }

    // The stagger: the reward window, drawn so there is no doubt it is open.
    // A bright ring on the floor plus a hot core under the slumped body.
    if (g_duel.stagger_timer > 0)
    {
        bool blink = ((g_frame >> 2) & 1);
        uint8_t c = blink ? PAL_UI_GOLD_LIGHT : PAL_WHITE_SPARK;
        gfx_circle_outline(bx, feet - 2, half + 2, c);
        gfx_circle_outline(bx, feet - 2, half - 4, c);
        // The window drains visibly, so "punish now" has a deadline attached.
        int pct = (int)g_duel.stagger_timer * 100 / STAGGER_FRAMES;
        gfx_fill_rect(bx - 24, feet + 8, 48 * pct / 100, 3, PAL_UI_GOLD);
    }

    // Phase state, as a shimmer under the boss. Derived from health so a linked
    // client shows it without needing the host's private FSM.
    if (g_duel.boss_phase >= 1 && ((g_frame >> 3) & 1))
        gfx_circle_outline(bx, feet - 2, half - 8,
                           g_duel.boss_phase >= 2 ? PAL_RED_LIGHT
                                                  : PAL_EMBER_ORANGE);
}

// The capybara in the arena: the usual sprite blit, plus roll ghosts. The
// ghost is what makes the roll readable as "untouchable" rather than as a
// small hop.
static void duel_draw_hero(int index)
{
    Hero *h = &g_heroes[index];
    if (!h->active)
        return;
    if (h->roll_timer > 0 && (h->roll_timer & 2))
    {
        const uint8_t *sheet = (h->variant & 1) ? hero_f_tiles_bin
                                                : hero_m_tiles_bin;
        gfx_blit_8bpp(sheet, HERO_W, HERO_H,
                      h->facing * HERO_FRAMES + HF_WALK_A,
                      h->x - h->roll_dir * 6, h->y + 2, false, false);
    }
    draw_hero(index);
}

static void duel_hud_top(void)
{
    Hero *h = &g_heroes[g_local_hero];
    const RegionDef *rg = &g_regions[g_region_id];

    // The same vitals band the field uses, minus the region name: the second
    // line belongs to the boss now.
    top_fill_rect(0, 0, AREA_W, VITALS_H, PAL_UI2_SCREEN);
    top_fill_rect(0, 0, AREA_W, 1, PAL_UI2_BLUE_DARK);
    top_fill_rect(0, VITALS_H, AREA_W, 2, PAL_UI2_LINE);

    ui_plate_sel(UI_MAIN, 4, 4, 40, 22, UI_R_TAB);
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "Lv%d", h->level);
        ui_label(UI_MAIN, 4, 4, 40, 22, buf, PAL_WHITE);
    }
    for (int i = 0; i < h->hearts_max && i < HERO_MAX_HEARTS; i++)
    {
        uint8_t fill = (i < h->hearts) ? PAL_HUD_HEART : PAL_HUD_HEART_EMPTY;
        draw_heart(50 + i * 8, 9, fill);
    }
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d:%02d",
                 g_run_time / 3600, (g_run_time / 60) % 60);
        ui_text_right(UI_MAIN, 250, 22, buf, PAL_UI2_PLATE_HI);
    }

    // The boss's health bar. The phase ticks are drawn on the bar itself, so
    // the player can see how far the next escalation is -- that is what turns
    // "it got faster" into "it is about to get faster".
    Monster *b2 = duel_boss();
    ui_plate_sel(UI_MAIN, 4, 34, 248, 26, UI_R_PLATE);
    ui_text(UI_MAIN, 12, 38, "霸主", PAL_UI2_RED);
    ui_text(UI_MAIN, 40, 38, rg->boss_name, PAL_WHITE);
    if (b2 && b2->hp_max > 0)
    {
        int pct = (b2->hp * 100) / b2->hp_max;
        if (pct > 100) pct = 100;
        uint8_t fill_c = PAL_PURPLE_MID;
        uint8_t back_c = PAL_PURPLE_DARK;
        if (g_duel.boss_phase == 1) { fill_c = PAL_EMBER_ORANGE; back_c = PAL_BRN_DEEP; }
        if (g_duel.boss_phase >= 2) { fill_c = PAL_RED_LIGHT; back_c = PAL_RED_DARK; }
        ui_bar(UI_MAIN, 96, 37, 148, 10, pct, back_c, fill_c);

        // Phase ticks: where the boss will change, drawn over the bar so the
        // next escalation has a position on the screen rather than being a
        // surprise.
        top_fill_rect(96 + 148 * BOSS_PHASE2_PCT / 100 - 1, 36, 2, 11,
                      PAL_UI2_PLATE_HI);
        top_fill_rect(96 + 148 * BOSS_PHASE3_PCT / 100 - 1, 36, 2, 11,
                      PAL_UI2_PLATE_HI);
    }

    // The stagger call: the one moment the player must not miss, so it is
    // shouted rather than implied by the pose alone.
    if (g_duel.stagger_timer > 0)
    {
        bool blink = ((g_frame >> 2) & 1);
        ui_label(UI_MAIN, 60, 62, 136, 22, "踉跄！全力输出！",
                 blink ? PAL_UI_GOLD_LIGHT : PAL_WHITE);
    }
    else if (g_duel.fsm == DB_ROAR)
    {
        ui_label(UI_MAIN, 60, 62, 136, 22, "阶段转换！", PAL_UI2_RED);
    }

    // The intro name card: the boss announces itself once, with its epithet,
    // then gets out of the way of its own fight. A named boss is a character,
    // and this is the moment the fight says so.
    if (g_duel.phase == DUEL_INTRO)
    {
        bool blink = ((g_frame >> 3) & 1);
        int t = 150 - (int)g_duel.timer;      // 0..150
        // A short slide-in, then hold: the card arrives rather than appearing.
        int sy = 76 + (t < 12 ? (12 - t) * 2 : 0);
        ui_plate(UI_MAIN, 24, sy, 208, 46, UI_R_PLATE);
        top_fill_rect(24, sy + 1, 208, 1,
                      blink ? PAL_UI_GOLD_LIGHT : PAL_UI_GOLD);

        ui_label(UI_MAIN, 24, sy + 6, 208, 20, rg->boss_name,
                 blink ? PAL_WHITE : PAL_UI2_AMBER);
        ui_label(UI_MAIN, 24, sy + 26, 208, 16, rg->boss_title,
                 PAL_UI2_PLATE_HI);
    }
}

// Bottom screen of the duel: the two big touch actions, the ultimate, the
// four skill slots and a one-line control legend.
static void duel_hud_bottom(void)
{
    ui_field(UI_SUB);
    sub_fill_rect(0, 0, AREA_W, 30, PAL_UI2_LINE);
    sub_fill_rect(0, 0, AREA_W, 28, PAL_UI2_SCREEN);
    sub_fill_rect(0, 0, AREA_W, 1, PAL_UI2_BLUE_DARK);
    ui_plate_sel(UI_SUB, 4, 4, 128, 22, UI_R_TAB);
    ui_label(UI_SUB, 4, 4, 128, 22, "霸主决战", PAL_WHITE);
    ui_text_right(UI_SUB, 250, 14, "←→移动 A跳 B滚", PAL_UI2_PLATE_HI);

    // Jump and roll, as plates. The roll shows its cooldown as a filling
    // bar, so a mistimed roll teaches its price without a word.
    Hero *h = &g_heroes[g_local_hero];
    bool jump_hot = h->jump_vy == 0;
    ui_btn(UI_SUB, DUEL_BTN_JUMP_X, DUEL_BTN_JUMP_Y, DUEL_BTN_JUMP_W,
           DUEL_BTN_JUMP_H, jump_hot ? UI_SEL : UI_OFF);
    ui_label(UI_SUB, DUEL_BTN_JUMP_X, DUEL_BTN_JUMP_Y, DUEL_BTN_JUMP_W,
             DUEL_BTN_JUMP_H, "跳跃(A)", jump_hot ? PAL_WHITE : PAL_UI2_INK_DIM);

    bool roll_ready = h->roll_cd == 0;
    ui_btn(UI_SUB, DUEL_BTN_ROLL_X, DUEL_BTN_ROLL_Y, DUEL_BTN_ROLL_W,
           DUEL_BTN_ROLL_H, roll_ready ? UI_SEL : UI_OFF);
    ui_label(UI_SUB, DUEL_BTN_ROLL_X, DUEL_BTN_ROLL_Y, DUEL_BTN_ROLL_W,
             DUEL_BTN_ROLL_H, "翻滚(B)",
             roll_ready ? PAL_WHITE : PAL_UI2_INK_DIM);
    if (!roll_ready)
    {
        int pct = 100 - (h->roll_cd * 100) / DUEL_ROLL_CD;
        ui_bar(UI_SUB, DUEL_BTN_ROLL_X + 8,
               DUEL_BTN_ROLL_Y + DUEL_BTN_ROLL_H - 12,
               DUEL_BTN_ROLL_W - 16, 7, pct, PAL_CYAN_DARK, PAL_CYAN_MID);
    }

    hud_draw_ultimate();
    hud_draw_slots();

    // The duel status line, where the field's card list sits. It reports the
    // two things the boss itself does not: which phase it is in, and whether
    // its poise is close to breaking -- both of which the player acts on.
    //
    // The plate is taller than a text line because it has to hold the poise
    // meter too; drawing the meter on top of the label was the first version,
    // and it erased the label the moment the window opened.
    {
        int px = 148, py = 34, pw = 100, ph = 34;
        ui_plate(UI_SUB, px, py, pw, ph, UI_R_PLATE);

        Monster *b = duel_boss();
        int hits = b ? b->stagger_hits : 0;
        bool window_open = (b && b->stagger_window > 0);

        if (g_duel.boss_phase >= 2)
            ui_text(UI_SUB, px + 8, py + 4, "霸主·拼命", PAL_RED_LIGHT);
        else if (g_duel.boss_phase == 1)
            ui_text(UI_SUB, px + 8, py + 4, "霸主·狂暴", PAL_EMBER_ORANGE);
        else
            ui_text(UI_SUB, px + 8, py + 4, "霸主·从容", PAL_UI2_INK_DIM);

        // A poise meter on its own line: the closer the bar is to full, the
        // sooner the boss goes down. It only appears once the window is open,
        // so the player learns what sustained pressure buys them.
        if (window_open && hits > 0)
        {
            int pct = hits * 100 / STAGGER_HITS;
            if (pct > 100) pct = 100;
            ui_text(UI_SUB, px + 8, py + 20, "踉跄", PAL_UI_GOLD_LIGHT);
            ui_bar(UI_SUB, px + 40, py + 19, pw - 48, 9, pct,
                   PAL_OUTLINE_SOFT, PAL_UI_GOLD_LIGHT);
        }
        else
        {
            ui_text(UI_SUB, px + 8, py + 20, "连击可致踉跄", PAL_UI2_INK_DIM);
        }
    }
}

static void duel_draw(Monster *b)
{
    duel_draw_arena();

    // The arena's own air, in the region's own weather -- the duel used to be
    // the one scene with none, which made it feel like a different game's
    // backdrop rather than the far end of the region the player walked in from.
    weather_draw();

    // Ground FX under everybody (dung traps the boss steps on).
    for (int i = 0; i < MAX_EFFECTS; i++)
    {
        Effect *e = &g_effects[i];
        if (!e->alive || e->kind != FX_DUNG_TRAP)
            continue;
        gfx_circle(e->x, e->y + 1, 5, PAL_CHOCO_DARK);
        gfx_circle(e->x, e->y, 4, PAL_CHOCO_MID);
    }

    // Telegraphs sit UNDER the actors: they are the floor talking, and a boss
    // standing over its own warning must not be hidden by it.
    for (int i = 0; i < MAX_TELEGRAPHS; i++)
        draw_telegraph(i);

    if (b)
        duel_draw_boss(b);

    duel_draw_hero(0);
    if (g_multiplayer && g_heroes[1].active)
        duel_draw_hero(1);

    // Projectiles over the actors: boss shots in the region's palette, the
    // ground waves as low bright ridges.
    const DuelDef *dd = &g_duel_defs[g_region_id];
    for (int i = 0; i < MAX_PROJECTILES; i++)
    {
        Projectile *p = &g_projectiles[i];
        if (!p->alive)
            continue;
        if (p->kind == PROJ_ENEMY || p->kind == PROJ_LOB)
        {
            // A dark rim and a hot core, always: several regions pick shot
            // tints from their own ground palette, and a green bolt over a
            // green arena is an attack nobody was warned about.
            gfx_circle(p->x, p->y, p->radius + 2, PAL_UI2_SHADOW);
            gfx_circle(p->x, p->y, p->radius + 1, dd->proj_a);
            gfx_circle(p->x, p->y, p->radius, dd->proj_b);
            gfx_circle(p->x - 1, p->y - 1,
                       p->radius - 2 > 1 ? p->radius - 2 : 1, dd->proj_c);
            gfx_fill_rect(p->x - 1, p->y - 1, 2, 2, PAL_WHITE_SPARK);
        }
        else if (p->kind == PROJ_SHOCK)
        {
            gfx_fill_rect(p->x - p->radius, DUEL_GROUND_Y - 8,
                          p->radius * 2, 8, dd->proj_a);
            gfx_fill_rect(p->x - p->radius + 2, DUEL_GROUND_Y - 6,
                          p->radius * 2 - 4, 4, dd->proj_b);
            gfx_fill_rect(p->x - 2, DUEL_GROUND_Y - 11, 4, 4, dd->proj_c);
            // A lit leading edge, so the wave reads as travelling and the
            // dodge direction is obvious at a glance.
            int lead = (p->vx < 0) ? p->x - p->radius : p->x + p->radius - 2;
            gfx_fill_rect(lead, DUEL_GROUND_Y - 8, 2, 8, PAL_WHITE_SPARK);
        }
    }

    // Damage numbers over everything.
    floaters_draw();

    duel_hud_top();
    duel_hud_bottom();

    // The death beat tints the whole arena: a white flash on the beat the blow
    // lands, then a gold wash that fades as the loot flies. It sits over both
    // HUDs, because for these 60 frames the fight is the only thing that
    // matters.
    if (g_duel.dying > 0)
    {
        if (g_duel.dying > 48)
        {
            // The impact itself: two frames of white.
            gfx_fill_rect(0, 0, AREA_W, AREA_H, PAL_WHITE_SPARK);
        }
        else if ((g_frame >> 1) & 1)
        {
            // Then a slow pulse while the gems rise.
            gfx_rect_outline(0, 0, AREA_W, AREA_H, PAL_UI_GOLD_LIGHT);
            gfx_rect_outline(2, 2, AREA_W - 4, AREA_H - 4, PAL_UI_GOLD);
        }
        ui_label(UI_MAIN, 48, 92, 160, 24, g_duel.dying_final
                 ? "霸主倒下了！" : "先锋被击溃！", PAL_UI_GOLD_LIGHT);
    }

    // Low-health edge: the whole screen frame pulses from a quarter health
    // down, hotter inside the last heart. Cheap, unmissable, and it sits over
    // both HUDs.
    Hero *h = &g_heroes[g_local_hero];
    if (h->active && h->hearts_max > 0 && g_duel.dying == 0)
    {
        bool quarter = h->hearts * 4 <= h->hearts_max;
        bool last = h->hearts <= 1;
        if (quarter && ((g_frame >> (last ? 2 : 3)) & 1))
        {
            uint8_t c = last ? PAL_RED_LIGHT : PAL_RED_DARK;
            gfx_fill_rect(0, 0, AREA_W, 2, c);
            gfx_fill_rect(0, AREA_H - 2, AREA_W, 2, c);
            gfx_fill_rect(0, 0, 2, AREA_H, c);
            gfx_fill_rect(AREA_W - 2, 0, 2, AREA_H, c);
        }
    }
}

// The in-field leap-at-the-camera overlay, drawn over the world while the
// leap-out countdown runs.
static void duel_leap_out_draw(void)
{
    int32_t px = G_WORLD_W / 2, py = G_WORLD_H / 2;
    if (g_heroes[g_local_hero].active)
        hero_center(&g_heroes[g_local_hero], &px, &py);

    int t = 50 - g_duel.timer;          // 0..50
    int pct = 60 + t * 4;               // 60% -> 260%
    int half = 32 * pct / 100;

    // The boss's shadow rushing in under the capybara, then the boss itself
    // falling toward the camera, then a white flash as the scene switches.
    gfx_fill_rect(px - 18, py + 14, 36, 4, PAL_OUTLINE_SOFT);
    int by = py - 130 + t;
    gfx_blit_8bpp_scaled(boss_tiles_bin, 64, 64,
                         g_regions[g_region_id].boss_id * BOSS_FRAMES + BF_ATTACK,
                         px - half, by - half, false, true, pct);
    if (t > 44)
        gfx_fill_rect(0, 0, AREA_W, AREA_H, PAL_WHITE_SPARK);
}

// ---------------------------------------------------------------------------
// Top-screen vitals
//
// Hearts, level and the experience bar live on the top screen, over the world.
// They are the numbers a player glances at mid-fight, and the bottom screen is
// already spoken for by the map, the cards and the touch slots -- putting the
// vitals there too meant they competed with the things drawn around them.
//
// Everything here is SCREEN space: the top screen scrolls with the camera, so a
// HUD element must not. Hence top_* rather than gfx_*.
// ---------------------------------------------------------------------------

// A 7x7 heart. The notch between the lobes and the single-pixel point at the
// bottom are each one pixel, so both are placed explicitly: a rounded blob with
// a triangle under it reads as a balloon, not as a heart.
static void draw_heart(int x, int y, uint8_t fill)
{
    top_fill_rect(x + 1, y, 2, 1, PAL_OUTLINE);
    top_fill_rect(x + 4, y, 2, 1, PAL_OUTLINE);
    top_fill_rect(x, y + 1, 7, 4, PAL_OUTLINE);
    top_fill_rect(x + 1, y + 5, 5, 1, PAL_OUTLINE);
    top_fill_rect(x + 2, y + 6, 3, 1, PAL_OUTLINE);

    if (fill != PAL_HUD_HEART_EMPTY)
    {
        top_fill_rect(x + 1, y + 1, 2, 1, fill);
        top_fill_rect(x + 4, y + 1, 2, 1, fill);
        top_fill_rect(x + 1, y + 2, 5, 2, fill);
        top_fill_rect(x + 2, y + 4, 3, 1, fill);
        top_fill_rect(x + 3, y + 5, 1, 1, fill);
        // A lit lobe and a shaded lobe: the two pixels that make a flat heart
        // look like a rounded object.
        top_fill_rect(x + 1, y + 2, 1, 1, PAL_PINK_LIGHT);
        top_fill_rect(x + 4, y + 3, 1, 1, PAL_HUD_HEART_DARK);
    }
    else
    {
        top_fill_rect(x + 1, y + 1, 5, 3, fill);
        top_fill_rect(x + 2, y + 4, 3, 1, fill);
    }
}

// The vitals band on the top screen: the level badge, the hearts, the region
// and the experience meter.
//
// This is drawn over a scrolling world, so every element here is a plate or a
// well rather than bare text: white glyphs over a snowfield are invisible, and
// the outline pass is only affordable for the one or two strings that cannot
// live inside a box. The band itself is opaque for the same reason -- a
// translucent strip would let a crowd of monsters read through the numbers the
// player is looking at mid-fight.
static void hud_draw_top(void)
{
    Hero *h = &g_heroes[g_local_hero];
    const RegionDef *rg = &g_regions[g_region_id];

    // Backdrop: three navy bands with a lit top edge, so the strip reads as a
    // solid object pinned over the world rather than as a tint.
    top_fill_rect(0, 0, AREA_W, VITALS_H, PAL_UI2_SCREEN);
    top_fill_rect(0, 0, AREA_W, 1, PAL_UI2_BLUE_DARK);
    top_fill_rect(0, VITALS_H, AREA_W, 2, PAL_UI2_LINE);

    // The level badge, as a blue tab: the one number that changes rarely, so it
    // gets the loudest treatment on the strip.
    ui_plate_sel(UI_MAIN, 4, 4, 40, 22, UI_R_TAB);
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "Lv%d", h->level);
        ui_label(UI_MAIN, 4, 4, 40, 22, buf, PAL_WHITE);
    }

    // Hearts, on the strip's upper line. Twelve at an 8px pitch is 96px, so
    // they run from just after the level badge to a little past mid-screen,
    // which leaves the right third for the region name.
    for (int i = 0; i < h->hearts_max && i < HERO_MAX_HEARTS; i++)
    {
        uint8_t fill = (i < h->hearts) ? PAL_HUD_HEART : PAL_HUD_HEART_EMPTY;
        draw_heart(50 + i * 8, 9, fill);
    }

    // Region name on the upper line, right-aligned and outlined: it is the one
    // string with no room for a plate behind it, and the outline is what keeps
    // it legible over whatever terrain is below.
    {
        int w = text_utf8_width(rg->name);
        ui_text_lit(UI_MAIN, AREA_W - 5 - w + 3, 3, rg->name,
                    PAL_WHITE_SPARK);
    }

    // Experience meter along the lower line, starting after the level badge.
    // The clock is right-aligned on the SAME line, and the bar is sized to stop
    // short of it -- the two are on one row on purpose, since putting the clock
    // on the row above puts it under the region name and the two overlap.
    int pct = 0;
    if (h->exp_need > 0)
    {
        pct = (h->exp * 100) / h->exp_need;
        if (pct > 100)
            pct = 100;
    }
    ui_bar(UI_MAIN, 48, 16, 140, 12, pct, PAL_GRN_DARK, PAL_GREEN_SPARK);

    {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d:%02d",
                 g_run_time / 3600, (g_run_time / 60) % 60);
        ui_text_right(UI_MAIN, 250, 22, buf, PAL_UI2_PLATE_HI);
    }
}

// ---------------------------------------------------------------------------
// Bottom-screen HUD
// ---------------------------------------------------------------------------

// A minimap of the whole region on the bottom screen, with the player on it.
//
// With a world up to 8x8 screens in size, the player has no way to know where
// they are or where they have been without one -- and a survivors-like is
// played by kiting, which means deliberately running a long way. The map is
// drawn by sampling the tile map on a grid coarse enough to redraw every frame
// and fine enough to show the shape of the terrain.
//
// The sample rate is derived from the bound world rather than fixed: the
// kingdom is 48x40 tiles and a region is 128x96, so a fixed rate would draw
// the kingdom as a postage stamp in the corner of the frame. Scaling so the
// whole map fills the box keeps the *frame* constant, and a frame that never
// moves is what makes the thing readable at a glance.
// The map's inner drawing area: the frame plus its 1px rounded border, then a
// 2px inset so the terrain never touches the outline.
#define MM_IN_X   (MINIMAP_X + 3)
#define MM_IN_Y   (MINIMAP_Y + 3)
#define MM_IN_W   (MINIMAP_W - 6)
#define MM_IN_H   (MINIMAP_H - 6)

static void draw_minimap(void)
{
    // A recessed well rather than a plate: the map is something you read, not
    // something you press, and the inverted lighting says so.
    ui_well(UI_SUB, MINIMAP_X, MINIMAP_Y, MINIMAP_W, MINIMAP_H, UI_R_PLATE,
            PAL_UI2_PLATE_DEEP);

    const uint8_t *map = area_map(g_regions[g_region_id].area_id);
    const uint8_t *prop = area_prop(g_regions[g_region_id].area_id);
    if (!map || !prop)
        return;

    int sx = (g_world_cols + MM_IN_W - 1) / MM_IN_W;
    int sy = (g_world_rows + MM_IN_H - 1) / MM_IN_H;
    if (sx < 1) sx = 1;
    if (sy < 1) sy = 1;

    int mw = g_world_cols / sx;
    int mh = g_world_rows / sy;
    if (mw > MM_IN_W) mw = MM_IN_W;
    if (mh > MM_IN_H) mh = MM_IN_H;
    int x0 = MM_IN_X + (MM_IN_W - mw) / 2;
    int y0 = MM_IN_Y + (MM_IN_H - mh) / 2;

    for (int cy = 0; cy < mh; cy++)
    {
        for (int cx = 0; cx < mw; cx++)
        {
            // Sample the top-left tile of each cell; a finer average would cost
            // more than the extra detail is worth at this size.
            int tx = cx * sx;
            int ty = cy * sy;
            unsigned id = map[ty * g_world_cols + tx];
            uint8_t col;
            if (id >= ATLAS_TILES)
                col = PAL_UI2_PLATE_DEEP;
            else if (id >= 14 && id <= 17)
                col = PAL_SEA_MID;              // water
            else if (id >= 12 && id <= 13)
                col = PAL_PLANK_MID;            // boardwalk
            else if (prop[id])
                col = PAL_PINE_DARK;            // walls and props
            else
                col = PAL_GRASS_MID;            // open ground
            sub_fill_rect(x0 + cx, y0 + cy, 1, 1, col);
        }
    }

    // Clamp helper for both dots below, so neither can leave the drawn map.
    #define MM_CLAMP_X(v) do { if ((v) < x0) (v) = x0; \
        if ((v) > x0 + mw - 2) (v) = x0 + mw - 2; } while (0)
    #define MM_CLAMP_Y(v) do { if ((v) < y0) (v) = y0; \
        if ((v) > y0 + mh - 2) (v) = y0 + mh - 2; } while (0)

    // The player, as a bright dot. White on purpose: the boss dot below is
    // red, and two red dots on one map answered "something is here" without
    // ever saying which one was you.
    if (g_heroes[g_local_hero].active)
    {
        int32_t hx, hy;
        hero_center(&g_heroes[g_local_hero], &hx, &hy);
        int mx = x0 + (int)(hx / TILE_PX) / sx;
        int my = y0 + (int)(hy / TILE_PX) / sy;
        MM_CLAMP_X(mx);
        MM_CLAMP_Y(my);
        sub_fill_rect(mx - 1, my - 1, 3, 3, PAL_UI2_LINE);
        sub_fill_rect(mx, my, 1, 1, PAL_WHITE_SPARK);
    }

    // The boss, when there is one, so the player can choose to engage or run.
    // Red, the HUD's danger colour; the player's own dot is white.
    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        Monster *m = &g_monsters[i];
        if (!m->alive || !m->is_boss)
            continue;
        int mx = x0 + (int)(m->x / TILE_PX) / sx;
        int my = y0 + (int)(m->y / TILE_PX) / sy;
        MM_CLAMP_X(mx);
        MM_CLAMP_Y(my);
        sub_fill_rect(mx - 1, my - 1, 3, 3, PAL_UI2_LINE);
        sub_fill_rect(mx, my, 1, 1, PAL_HUD_HEART);
        break;
    }

    #undef MM_CLAMP_X
    #undef MM_CLAMP_Y
}

// Arrows at the screen edge pointing at anything important that is off screen.
//
// One aimed wedge pinned to the screen border, pointing along (ux, uy) from a
// tip already clipped to the border rectangle. Shared by the boss marker and
// the hub's guidance arrow, so "an arrow means that way" stays one silhouette
// wherever it appears.
static void draw_edge_wedge(int tipx, int tipy, int ux, int uy,
                            uint8_t col_main)
{
    // A tapered wedge stepped back from the tip: the same silhouette as
    // the menu cursor, rotated to aim. A dark rim pass first, so it holds
    // over any terrain the way the outlined HUD text does.
    int px = -uy, py = ux;   // perpendicular, for the wedge's width
    for (int pass = 0; pass < 2; pass++)
    {
        uint8_t col = pass ? col_main : PAL_UI2_SHADOW;
        int grow = pass ? 0 : 1;
        for (int d = -1; d <= 7; d++)
        {
            // d measures BACK from the tip along the aim line, so the
            // wedge narrows to its point at d = -1..0 and widens inward.
            int half = (d < 0 ? 0 : d) / 2 + grow;
            for (int j = -half; j <= half; j++)
            {
                top_fill_rect(tipx - ux * d + px * j,
                              tipy - uy * d + py * j, 1, 1, col);
            }
        }
    }
}

// Clip a direction from screen centre to (sx, sy) against the border rectangle
// the arrow tip is allowed to sit on, then draw the wedge. Inset so the whole
// arrow stays on screen; the top inset also clears the vitals band. Returns
// false when the point sits at the exact screen centre.
//
// The border fraction is kept in 8.8 fixed point: in int truncation a target
// even one pixel beyond the border distance floors t to zero and pins the
// arrow to the screen centre -- exactly where the player is standing.
static bool edge_arrow_toward(int sx, int sy, uint8_t col)
{
    const int bx0 = 10, by0 = VITALS_BAND + 10;
    const int bx1 = AREA_W - 10, by1 = AREA_H - 10;
    int dx = sx - AREA_W / 2;
    int dy = sy - AREA_H / 2;

    int t = 1 << 30;
    if (dx > 0) t = ((bx1 - AREA_W / 2) << 8) / dx;
    else if (dx < 0) t = ((bx0 - AREA_W / 2) << 8) / dx;
    if (dy > 0)
    {
        int ty = ((by1 - AREA_H / 2) << 8) / dy;
        if (ty < t) t = ty;
    }
    else if (dy < 0)
    {
        int ty = ((by0 - AREA_H / 2) << 8) / dy;
        if (ty < t) t = ty;
    }
    int tipx = AREA_W / 2 + (dx * t) / 256;
    int tipy = AREA_H / 2 + (dy * t) / 256;

    // Unit step along the aim line, scaled so the larger component is 1.
    int adx = (dx < 0) ? -dx : dx;
    int ady = (dy < 0) ? -dy : dy;
    int md = (adx > ady) ? adx : ady;
    if (md == 0)
        return false;
    draw_edge_wedge(tipx, tipy, dx / md, dy / md, col);
    return true;
}

// In a world this large, "where is the boss" is a question the player cannot
// answer from what is drawn, and the boss appearing is the event the whole
// region builds up to. The marker is a real ARROW pinned to the border and
// aimed along the true direction to the boss: the old version clamped a red
// square to the boss's screen position, which read as "the boss is here" and
// sent the player running the wrong way.
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

        edge_arrow_toward(sx, sy, PAL_HUD_HEART);
        return;
    }
}

// The hub's guidance arrow: while the chain has somewhere for a first-timer
// to go, point at it with the same aimed wedge the field uses for the boss,
// in gold and on a gentle pulse so it reads as "this way", never as danger.
// A newcomer does not yet know the kingdom's compass points; where the hint
// sentence runs out of pixels, the arrow finishes the sentence.
static void tutorial_arrow_draw(void)
{
    int lm = tutorial_arrow_landmark();
    if (!lm)
        return;
    // Three beats on, one off: visible long enough to read at a glance,
    // breathing so it never reads as a fixed part of the world.
    if ((g_frame & 31) >= 24)
        return;

    int wx, wy;
    switch (lm)
    {
    case HUB_TUT_FARM:
        wx = FARM_ORIGIN_X + FARM_COLS * 16 / 2;
        wy = FARM_ORIGIN_Y + FARM_ROWS * 16 / 2;
        break;
    case HUB_LM_SPRING_ID: wx = HUB_LM_SPRING_X; wy = HUB_LM_SPRING_Y; break;
    case HUB_LM_OVEN_ID:   wx = HUB_LM_OVEN_X;   wy = HUB_LM_OVEN_Y;   break;
    default:               wx = HUB_LM_GATE_X;   wy = HUB_LM_GATE_Y;   break;
    }

    int cam_x, cam_y;
    render_camera_get(&cam_x, &cam_y);
    int sx = wx - cam_x;
    int sy = wy - cam_y;
    if (sx >= -8 && sx < AREA_W + 8 && sy >= -8 && sy < AREA_H + 8)
        return;   // it is on screen; the landmark can speak for itself

    edge_arrow_toward(sx, sy, PAL_YELLOW_LIGHT);
}

static uint8_t card_kind_color(int kind)
{
    switch (kind)
    {
    case KIND_PHYS:    return PAL_ORANGE_DARK;
    case KIND_MAGIC:   return PAL_CYAN_DARK;
    case KIND_SUPPORT: return PAL_GRN_DARK;
    default:           return PAL_UI_GOLD;   // passive: same gold as the cards
    }
}

// The header across the top of the play screen: the region and what is being
// collected from it, plus the ingredient counter that decides when the boss
// appears.
static void hud_draw_header(void)
{
    const RegionDef *rg = &g_regions[g_region_id];

    // A square-cornered band across the full width: this is the screen's edge
    // rather than a button, so it must not have rounded corners of its own.
    // 30px tall, which is exactly the gap above the card column at y 32.
    sub_fill_rect(0, 0, AREA_W, 30, PAL_UI2_LINE);
    sub_fill_rect(0, 0, AREA_W, 28, PAL_UI2_SCREEN);
    sub_fill_rect(0, 0, AREA_W, 1, PAL_UI2_BLUE_DARK);

    // Region name in a blue tab, so it reads as a title rather than as another
    // value in the row.
    ui_plate_sel(UI_SUB, 4, 4, 88, 22, UI_R_TAB);
    ui_label(UI_SUB, 4, 4, 88, 22, rg->name, PAL_WHITE);

    // Ingredient progress: label, then a bar and the count. Targets run up
    // to 45, where pips stopped fitting; a bar carries the same two facts --
    // how far, how many -- at the same glance.
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "收集%s种子", rg->ingredient);
        ui_text(UI_SUB, 100, 4, buf, PAL_UI2_PLATE_HI);

        int pct = (g_seed_have * 100) / rg->target;
        if (pct > 100)
            pct = 100;
        bool peaked = (g_seed_have >= rg->target);
        ui_bar(UI_SUB, 100, 15, 112, 11, pct,
               peaked ? PAL_UI_GOLD_LIGHT : PAL_UI_GOLD,
               peaked ? PAL_WHITE : PAL_UI_GOLD_LIGHT);
        snprintf(buf, sizeof(buf), "%d/%d", (int)g_seed_have,
                 (int)rg->target);
        ui_text_right(UI_SUB, 250, 16, buf, PAL_UI2_PLATE_HI);
    }
}

// The held-card list in the tall left column. Each entry is a row with a
// kind-coloured chip, the name and the level, so the column reads as a list of
// what the capybara can do rather than as a wall of text.
static void hud_draw_cards(void)
{
    Hero *h = &g_heroes[g_local_hero];

    ui_plate(UI_SUB, CARDS_X, CARDS_Y, CARDS_W, CARDS_H, UI_R_PLATE);
    ui_header_tab(UI_SUB, CARDS_X + 5, CARDS_Y + 4, 66, "卡牌");

    int shown = 0;
    for (int i = 0; i < CARD_COUNT; i++)
    {
        if (h->cards[i] <= 0)
            continue;
        if (shown >= 12)
            break;

        int rx = CARDS_X + 6;
        int ry = CARDS_Y + 30 + shown * 6;
        uint8_t c = card_kind_color(g_card_defs[i].kind);

        // A kind chip, then the name, then the level at the right edge.
        ui_round(UI_SUB, rx, ry - 1, 4, 6, 1, c);

        char buf[32];
        snprintf(buf, sizeof(buf), "%s", g_card_defs[i].name);
        ui_text(UI_SUB, rx + 8, ry - 6, buf, PAL_UI2_INK);

        snprintf(buf, sizeof(buf), "%d", h->cards[i]);
        ui_text_right(UI_SUB, CARDS_X + CARDS_W - 8, ry - 4, buf,
                      PAL_UI2_INK_DIM);

        // A fused card is listed under its own name, but its parents' names
        // are gone from the list -- so the card is marked as carrying them,
        // which is what stops "where did my 水柱 go" from being a mystery.
        if (i >= CARD_BASE_COUNT && card_owned(h, i))
        {
            ui_round(UI_SUB, CARDS_X + CARDS_W - 18, ry - 3, 5, 5, 1,
                     PAL_UI_GOLD_LIGHT);
        }
        shown++;
    }

    if (shown == 0)
        ui_text(UI_SUB, CARDS_X + 8, CARDS_Y + 34, "升级就能获得卡牌",
                PAL_UI2_INK_DIM);
}

// The ultimate: the one control the player reaches for without looking. It is
// the widest plate on the screen and the only element whose *whole* surface
// changes colour, because "is it up yet" has to be answerable from the corner
// of the eye while a crowd is closing in.
static void hud_draw_ultimate(void)
{
    Hero *h = &g_heroes[g_local_hero];
    bool ready = h->ult_ready;

    // Ready: a solid blue plate, pulsing lighter so a full charge is noticed
    // peripherally. Charging: a neutral plate with the meter doing the talking.
    UiState st = ready ? UI_SEL : UI_ON;
    if (ready && ((g_frame >> 3) & 1))
        ui_plate(UI_SUB, ULT_BTN_X, ULT_BTN_Y, ULT_BTN_W, ULT_BTN_H,
                 UI_R_BTN);
    else
        ui_btn(UI_SUB, ULT_BTN_X, ULT_BTN_Y, ULT_BTN_W, ULT_BTN_H, st);

    int pct = (h->ult_charge * 100) / ULT_CHARGE_MAX;
    if (ready)
        pct = 100;
    ui_bar(UI_SUB, ULT_BTN_X + 6, ULT_BTN_Y + 19, ULT_BTN_W - 12, 11, pct,
           ready ? PAL_UI_GOLD : PAL_PURPLE_DARK,
           ready ? PAL_UI_GOLD_LIGHT : PAL_PURPLE_MID);

    ui_label(UI_SUB, ULT_BTN_X, ULT_BTN_Y + 1, ULT_BTN_W, 18,
             ready ? "按 R 释放" : "大招充能中",
             ready ? PAL_WHITE : PAL_UI2_INK);
}

// The four A/B/X/Y skill slots. Each is a plate carrying a key cap, the skill's
// name, its level and a cooldown meter; the plate's state is the same "ready"
// signal the ultimate uses, so a glance at the bar tells the player what is
// available.
static void hud_draw_slots(void)
{
    Hero *h = &g_heroes[g_local_hero];
    static const char *slot_key[4] = { "A", "B", "X", "Y" };

    for (int s = 0; s < 4; s++)
    {
        int sx = SLOT_X(s);
        uint8_t cid = h->skill_slots[s];
        bool has_sk = (cid != SKILL_SLOT_NONE && cid < CARD_COUNT);
        // A fused card carries its parents' levels, so the effective level is
        // what belongs on the plate -- showing "Lv3" for a card the player
        // raised to 5 under another name would be a lie.
        int eff = has_sk ? card_eff_level(h, cid) : 0;
        bool ready = has_sk && (h->cooldown[cid] == 0) &&
                     (eff > 0 || cid == CARD_TEETH);

        ui_btn(UI_SUB, sx, SLOT_Y, SLOT_W, SLOT_H,
               ready ? UI_ON : UI_OFF);

        // Key cap top-left, name to its right, and on the lower line either
        // the level or the cooldown meter -- never both. The two used to be
        // drawn on top of each other, leaving "Lv1" half erased under the bar.
        ui_keycap(UI_SUB, sx + 4, SLOT_Y + 4, 15, 13, slot_key[s], ready);

        if (has_sk)
        {
            ui_text(UI_SUB, sx + 22, SLOT_Y + 4, g_card_defs[cid].name,
                    ready ? card_kind_color(g_card_defs[cid].kind)
                          : PAL_UI2_INK_DIM);

            if (has_sk && h->cooldown[cid] > 0)
            {
                // Cooling down: the meter owns the lower line, filling left
                // to right as the skill comes back. In the duel the card
                // answers to the arena's clock, so the meter measures that one.
                int max_cd = g_duel_active ? card_duel_cd(h, cid)
                                           : card_cd(h, cid);
                if (max_cd > 0)
                {
                    int cd_w = ((SLOT_W - 8) * (max_cd - h->cooldown[cid]))
                               / max_cd;
                    ui_bar_px(UI_SUB, sx + 4, SLOT_Y + SLOT_H - 14,
                              SLOT_W - 8, 10, cd_w,
                              PAL_CYAN_DARK, PAL_CYAN_MID);
                }
            }
            else
            {
                char buf[16];
                snprintf(buf, sizeof(buf), "Lv%d", eff);
                ui_text(UI_SUB, sx + 22, SLOT_Y + 18, buf, PAL_UI2_INK_DIM);
            }
        }
        else
        {
            ui_text(UI_SUB, sx + 22, SLOT_Y + 12, "空", PAL_UI2_INK_DIM);
        }
    }
}

// The bottom screen during play: what you are collecting, what you are
// carrying, and where you are. The vitals live on the top screen, over the
// world, because they are what the eye goes to mid-fight.
static void hud_draw(void)
{
    ui_field(UI_SUB);
    hud_draw_header();

    // The news banner. It replaces the header strip while it lives: the one
    // line of "what just changed" outranks the strip it sits on, and its
    // blink is what makes a peripheral glance notice a new event.
    if (g_toast.life > 0 && g_toast.text)
    {
        bool blink = ((g_frame >> 3) & 1);
        ui_well(UI_SUB, 6, 3, 244, 24, UI_R_TAB,
                blink ? g_toast.color : PAL_UI2_PLATE_DEEP);
        ui_label(UI_SUB, 6, 3, 244, 24, g_toast.text, PAL_WHITE);
    }

    // The "boss is here" banner, across the top of the card column. It is the
    // one event the whole region builds to, so it gets the loudest colour on
    // the screen and a full-width plate of its own.
    if (g_boss_out)
    {
        bool blink = ((g_frame >> 4) & 1);
        ui_plate(UI_SUB, CARDS_X + 74, CARDS_Y + 4, CARDS_W - 74, 22,
                 UI_R_TAB);
        ui_well(UI_SUB, CARDS_X + 76, CARDS_Y + 6, CARDS_W - 78, 18, UI_R_TAB,
                blink ? PAL_HUD_HEART : PAL_HUD_HEART_DARK);
        ui_label(UI_SUB, CARDS_X + 76, CARDS_Y + 6, CARDS_W - 78, 18,
                 "首领出现了！", PAL_WHITE);
    }

    hud_draw_cards();
    draw_minimap();
    hud_draw_ultimate();
    hud_draw_slots();
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
    // Warnings belong to a fight, not to a scene: a ring left over from
    // the last one would be a shape that threatens nothing.
    memset(g_telegraphs, 0, sizeof(g_telegraphs));
    // A fresh file teaches the loop from the first step again.
    g_tut_flags = 0;
    g_duel_pending = -1;
    g_field_hit_pending = 0;
    g_partner_heal = 0;

    for (int i = 0; i < 2; i++)
    {
        Hero *h = &g_heroes[i];
        h->level = 1;
        h->exp = 0;
        // Two or three swarm kills to the first card. See hero_gain_exp()
        // for how this grows from here.
        h->exp_need = EXP_BASE;
        // Six rather than five: contact damage now costs what the monster
        // weighs, so a brute takes two hearts off in one bite. Six keeps that
        // readable as "three mistakes", and a bad region still cannot brick a
        // birthday playthrough.
        h->hearts_max = HERO_BASE_HEARTS;
        h->hearts = HERO_BASE_HEARTS;
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
    g_seed_have = 0;
    g_mid_boss_out = false;
    g_boss_out = false;
    g_run_time = 0;
    g_difficulty = 0;

    // A new game starts on a bare field with an empty satchel. The farm is
    // deliberately NOT reset by game_start_hub(): it is progress, like the
    // skill tree, and wiping it every time the player walks home would make
    // planting pointless.
    farm_init();
}

// ---------------------------------------------------------------------------
// Weather
//
// Six regions, and until now they differed only in palette: the same terrain
// generator, the same prop density, the same still air. The one thing that
// separates a rainforest from a volcano at a glance is what is falling through
// it, and it costs almost nothing -- a few dozen one-pixel specks, positioned
// from a hash rather than from state, so there is nothing to update, allocate or
// save.
//
// Positions come from the speck's own index and the frame, so travel is smooth,
// the two consoles agree without synchronising anything, and a speck that
// leaves the top of the screen is replaced by the same arithmetic rather than by
// a respawn.
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t palette_a;   // most specks wear this
    uint8_t palette_b;   // every fourth wears this, for variety
    uint8_t count;       // how many specks
    uint8_t rise;        // 0 = falling, 1 = rising
    uint8_t sway;        // horizontal drift amplitude, in pixels
    uint8_t speed;       // vertical pixels per two seconds
} WeatherStyle;

// Indexed by region, with the hub at 0. The hub gets a handful of drifting
// petals and nothing else: the kingdom is where the pair rest and read menus,
// and weather there is only worth having if it is quiet enough to ignore.
static const WeatherStyle g_weather[INGREDIENT_COUNT + 1] = {
    { PAL_PINK_LIGHT,  PAL_WHITE_SPARK, 10, 0, 2,  6 },  // 王城: petals
    { PAL_LEAF_LIGHT,  PAL_GRN_MID,     34, 0, 4, 11 },  // 雨林: leaves
    { PAL_WHEAT_MID,   PAL_SAND_LIGHT,  30, 0, 5,  8 },  // 农场: chaff
    { PAL_CREAM_LIGHT, PAL_WHEAT_MID,   16, 1, 6,  5 },  // 牧场: seed down
    { PAL_CYAN_LIGHT,  PAL_HEAL_OUTER,  26, 1, 3,  6 },  // 沼泽: marsh gas
    { PAL_ORANGE_MID,  PAL_YELLOW_LIGHT, 40, 1, 3, 13 }, // 火山: embers
};

// A stable per-speck number: speck 7 is in the same column all session, so
// nothing shimmers as the pool is walked.
#define WEATHER_HASH(i)  ((((uint32_t)(i) + 1u) * 73856093u) ^ 0x9E3779B9u)

// Specks actually emitted by the last weather_draw(). Exists so the perf
// readout can report the layer is live: a dozen one-pixel specks drifting
// through a busy scene cannot be confirmed by looking at a screenshot, and
// "the code ran and drew N of them" is a fact rather than an impression.
uint16_t g_weather_specks;

static void weather_draw(void)
{
    int style_id = (g_state == ST_HUB) ? 0 : (int)g_region_id + 1;
    if (style_id < 0 || style_id > INGREDIENT_COUNT)
        style_id = 0;
    const WeatherStyle *w = &g_weather[style_id];
    g_weather_specks = 0;
    if (w->count == 0)
        return;

    const int span = AREA_H + 24;
    uint32_t tick = g_frame / 30;   // one step per two seconds at 60 fps

    // gfx_fill_rect() takes WORLD coordinates and subtracts the camera itself,
    // so a screen-space position has to be handed over with the camera added
    // back. The first version of this passed the screen position straight
    // through, which put every speck at (screen - camera): off the edge of the
    // viewport the moment the player walked away from the origin, and invisible
    // everywhere except the top-left corner of the map.
    int cam_x, cam_y;
    render_camera_get(&cam_x, &cam_y);

    for (int i = 0; i < (int)w->count; i++)
    {
        uint32_t h = WEATHER_HASH(i);

        int x = (int)(h % (uint32_t)AREA_W);
        int phase = (int)((h >> 9) % (uint32_t)span);
        int travel = (int)((tick * (uint32_t)w->speed) / 2u);

        // Falling specks start above the top and move down; rising ones start
        // below the bottom. Both wrap inside the viewport, so the air never
        // empties and the weather follows the camera instead of the map.
        int y = w->rise ? (span - ((phase + travel) % span))
                        : (((phase + travel) % span) - 12);
        if (y < 0)       y += span;
        if (y >= AREA_H) y -= span;

        if (w->sway)
        {
            int a = (int)((h >> 17) & 0xFFu) + (int)tick * 3;
            x += (sin8(a & 0xFF) * (int)w->sway) / 127;
        }
        if (x < 0)        x += AREA_W;
        if (x >= AREA_W)  x -= AREA_W;

        // A two-pixel smear on every other speck: falling things have a
        // direction, and this is the cheapest way to say which.
        gfx_fill_rect(x + cam_x, y + cam_y, 1, (i & 1) ? 2 : 1,
                      (i & 3) ? w->palette_a : w->palette_b);
        g_weather_specks++;
    }
}

void game_update(void)
{
    g_frame++;
    g_run_time++;

    // The defeat beat: the world keeps breathing while the fallen capybara's
    // burst plays, and the plate arrives only when it is done.
    if (g_hero_death_beat > 0 && --g_hero_death_beat == 0)
        g_state = ST_GAME_OVER;

    // A minute and a half in is when the surprise peaks for someone who has
    // never played a survivors game: things keep coming and nothing they
    // press makes the capybara bite harder. One sentence, once, at that
    // moment -- not on a menu they will never open.
    if (g_run_time == 90 && !tutorial_done(TUT_B_MOVE))
    {
        tutorial_mark(TUT_B_MOVE);
        show_toast("水豚自己会战斗——你只管走到怪物边上",
                   PAL_UI_GOLD_LIGHT, 240);
    }

    difficulty_update();

    // The kill chain and the news banner tick on the same clock as the run.
    if (g_combo_timer > 0)
    {
        g_combo_timer--;
        if (g_combo_timer == 0)
            g_combo = 0;
    }
    if (g_toast.life > 0)
        g_toast.life--;

#ifdef AUTOPLAY
    // Debug keys for capture runs (self-driving ROMs only, never shipped):
    // L forces a raid wave, R spawns an elite beside the capybara.
    {
        uint16_t dbg = input_down();
        if (dbg & KEY_L)
            g_wave_cd = 1;
        if (dbg & KEY_R)
        {
            int32_t dpx, dpy;
            hero_center(&g_heroes[g_local_hero], &dpx, &dpy);
            Monster *dnm = spawn_monster(g_regions[g_region_id].mob_ids[0],
                                         dpx + 40, dpy, false, 0);
            if (dnm)
            {
                dnm->elite = 1;
                dnm->scale_pct = 118;
                dnm->hp_max = (uint16_t)(dnm->hp_max * 3);
                dnm->hp = dnm->hp_max;
                show_toast("精英出现了！", PAL_UI_GOLD_LIGHT, 200);
            }
        }
    }
#endif

    // The leap-out beat: the region fight is over -- the boss is jumping at
    // the camera. The capybaras keep moving underneath; everything else
    // holds its breath until the arena takes over.
    bool leap_beat = (g_duel.phase == DUEL_LEAP_OUT);
    if (leap_beat)
        duel_leap_out_update();

    // Capybaras first: their new positions matter for monster targeting.
    hero_update(0);
    hero_update(1);
    hero_update_ultimate(0);
    hero_update_ultimate(1);

    // The host owns monster simulation; in single player the local console
    // always is the authority.
    if (!leap_beat && (!g_multiplayer || g_is_host))
    {
        spawn_update();
        for (int i = 0; i < MAX_MONSTERS; i++)
            monster_update(i);
        projectile_update();
        effect_update();
        pickup_update();
        // Telegraphs drain on the host's clock, like everything else the host
        // is authoritative for: a warning drawn on the client but expired on
        // the host would be a warning that lies.
        telegraph_update();
    }

    // Cards fire on their own timers, then the always-on bite. Both run for
    // every capybara this console simulates.
    if (!leap_beat)
    {
        hero_handle_active_skills(g_local_hero);
        hero_fire_cards(g_local_hero);
        if (!g_multiplayer && g_heroes[g_local_hero ^ 1].active)
            hero_fire_cards(g_local_hero ^ 1);

        if (g_heroes[g_local_hero].cooldown[CARD_TEETH] == 0)
            hero_auto_bite(g_local_hero);
        if (!g_multiplayer && g_heroes[g_local_hero ^ 1].active &&
            g_heroes[g_local_hero ^ 1].cooldown[CARD_TEETH] == 0)
            hero_auto_bite(g_local_hero ^ 1);
    }

    // --- camera ---
    // Follow the local character, softened so the view does not jitter on a
    // single-frame twitch, and lead slightly toward where it faces so the
    // space it is walking into is the space on screen.
    if (g_heroes[g_local_hero].active)
    {
        static const int8_t lead_dx[DIRS_COUNT] = { 0, 0, -1, 1, -1, 1, -1, 1 };
        static const int8_t lead_dy[DIRS_COUNT] = { 1, -1, 0, 0, 1, 1, -1, -1 };
        Hero *ch = &g_heroes[g_local_hero];
        int32_t hx, hy;
        hero_center(ch, &hx, &hy);

        int lead = ch->moving ? 14 : 0;
        int cam_x, cam_y;
        render_camera_get(&cam_x, &cam_y);
        int want_x = hx - AREA_W / 2 + lead_dx[ch->facing & 7] * lead;
        int want_y = hy - AREA_H / 2 + lead_dy[ch->facing & 7] * lead;
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

    // Weather goes on with the ground: behind the crowd, so it can never hide a
    // monster or the player, but in front of the terrain, so it reads as air
    // rather than as a texture on the floor.
    weather_draw();

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
            // Ground FX expand as they fade: the radius grows with the effect's
            // age, which is what makes a pool of mud or a stink cloud read as
            // something spreading rather than as a static disc.
            case FX_MUD:
            {
                int r = e->radius - (e->life * e->radius) / (e->life_max * 2);
                gfx_circle(e->x, e->y + 1, r + 1, PAL_CLAY_DARK);
                gfx_circle(e->x, e->y, r, PAL_CLAY_MID);
                // A dithered wet sheen on the up-left: the light that pools
                // on fresh mud. Dither, not a solid disc, or it reads as a
                // second puddle sitting on the first.
                for (int a = 128; a < 256; a += 8)
                {
                    int dx, dy;
                    gfx_dir_256(a, &dx, &dy);
                    gfx_fill_rect(e->x + (dx * r) / 170,
                                  e->y + (dy * r) / 170, 1, 1, PAL_BRN_MID2);
                }
                // Bubbles that wander as the pool ages.
                if (r >= 3)
                {
                    for (int k = 0; k < 3; k++)
                    {
                        int bx = e->x + ((g_frame * (3 + k) + k * 21)
                                         % (2 * r)) - r;
                        int by = e->y + ((g_frame * (5 + 2 * k) + k * 13)
                                         % (r > 0 ? r : 1)) - r / 2;
                        gfx_circle(bx, by, 1 + (k & 1), PAL_CLAY_DARK);
                        gfx_fill_rect(bx - 1, by - 1, 1, 1, PAL_BRN_MID2);
                    }
                }
                break;
            }
            case FX_STINK:
            {
                int r = e->radius + (e->life_max - e->life) / 3;
                gfx_circle(e->x, e->y, r, PAL_PINE_DARK);
                gfx_circle(e->x, e->y, r - 2, PAL_CACT_DARK);
                gfx_circle(e->x - 1, e->y - 3, r / 2, PAL_GRN_MID);
                // Drifting puffs orbiting the cloud: the cloud breathes
                // instead of sitting there.
                for (int k = 0; k < 5; k++)
                {
                    int a = (int)(g_frame * 2 + k * 51) & 255;
                    int dx, dy;
                    gfx_dir_256(a, &dx, &dy);
                    int px = e->x + (dx * r) / 127;
                    int py = e->y + (dy * r) / 150 - ((g_frame + k * 7) & 3);
                    int pr = 2 + ((g_frame + k * 9) & 1);
                    gfx_circle(px, py, pr, PAL_CACT_DARK);
                    gfx_circle(px - 1, py - 1, pr / 2, PAL_GRN_MID);
                }
                gfx_fill_rect(e->x - 4, e->y - r - 2, 2, 2, PAL_GREEN_SPARK);
                gfx_fill_rect(e->x + 3, e->y - r - 4, 2, 2, PAL_GREEN_SPARK);
                break;
            }
            case FX_RAM_DUST:
            {
                int t = e->life_max - e->life;
                for (int a = 0; a < 256; a += 32)
                {
                    int dx, dy;
                    gfx_dir_256(a, &dx, &dy);
                    int d = e->radius + t * 2;
                    int sz = 3 - (t & 1);
                    gfx_fill_rect(e->x + (dx * d) / 127 - 1,
                                  e->y + (dy * d) / 127 - 2,
                                  sz, sz, PAL_SAND_LIGHT);
                    gfx_fill_rect(e->x + (dx * d) / 127,
                                  e->y + (dy * d) / 127 - 2,
                                  1, 1, PAL_WHITE_SPARK);
                }
                break;
            }
            case FX_PACIFY:
            {
                int r = e->radius + ((g_frame >> 2) & 3);
                gfx_circle_outline(e->x, e->y, r, PAL_HEAL_OUTER);
                gfx_circle_outline(e->x, e->y, r - 1, PAL_HEAL_INNER);
                gfx_circle_outline(e->x, e->y, r - 3, PAL_HEAL_CORE);
                for (int a = 0; a < 256; a += 64)
                {
                    int dx, dy;
                    gfx_dir_256(a + g_frame, &dx, &dy);
                    gfx_fill_rect(e->x + (dx * r) / 127,
                                  e->y + (dy * r) / 127,
                                  2, 2, PAL_HEAL_CORE);
                }
                // The little "z" drifting up: pacified means sleepy.
                int zx = e->x + 6 + ((g_frame >> 4) & 1);
                int zy = e->y - r - 3 - ((g_frame >> 3) & 3);
                gfx_fill_rect(zx, zy, 3, 1, PAL_WHITE_SPARK);
                gfx_fill_rect(zx + 1, zy + 1, 1, 1, PAL_WHITE_SPARK);
                gfx_fill_rect(zx + 2, zy + 2, 3, 1, PAL_WHITE_SPARK);
                break;
            }
            case FX_DUNG_TRAP:
            {
                gfx_circle(e->x, e->y + 1, 5, PAL_CHOCO_DARK);
                gfx_circle(e->x, e->y, 4, PAL_CHOCO_MID);
                gfx_circle(e->x - 1, e->y - 1, 2, PAL_CHOCO_LIGHT);
                gfx_fill_rect(e->x + 1, e->y - 2, 2, 2, PAL_GREEN_SPARK);
                // Two flies circling the pile: the trap's own idle hum.
                for (int k = 0; k < 2; k++)
                {
                    int a = (int)(g_frame * 6 + k * 128) & 255;
                    int dx, dy;
                    gfx_dir_256(a, &dx, &dy);
                    gfx_fill_rect(e->x + (dx * 8) / 127,
                                  e->y - 2 + (dy * 4) / 127,
                                  1, 1, PAL_OUTLINE);
                }
                break;
            }
            case FX_LEVELUP:
            {
                // Three rings at different radii, chasing each other outwards,
                // plus a ring of sparkles. A single ring is easy to miss in a
                // crowd, and a level-up is the beat the whole loop is built on.
                int t = e->life_max - e->life;
                for (int k = 0; k < 3; k++)
                {
                    int r = e->radius + t * 2 - k * 7;
                    if (r <= 2)
                        continue;
                    uint8_t c = (k == 0) ? PAL_UI_GOLD_LIGHT
                                         : (k == 1 ? PAL_UI_GOLD : PAL_ORANGE_MID);
                    gfx_circle_outline(e->x, e->y, r, c);
                }
                for (int a = 0; a < 256; a += 32)
                {
                    int dx, dy;
                    gfx_dir_256(a + t * 3, &dx, &dy);
                    gfx_fill_rect(e->x + (dx * (e->radius + t * 2)) / 127,
                                  e->y + (dy * (e->radius + t * 2)) / 127,
                                  2, 2, PAL_WHITE_SPARK);
                }
                // A brief pillar of light under the burst's first half.
                if (t < e->life_max / 2)
                {
                    for (int py = 0; py < 26; py += 2)
                    {
                        int wob = ((g_frame + py) >> 1) & 1;
                        gfx_fill_rect(e->x - 1 + wob, e->y - py - 6, 2, 2,
                                      PAL_UI_GOLD_LIGHT);
                    }
                }
                break;
            }
            default:
                break;
            }
        }

        // Telegraphs sit under the crowd: they are the ground talking, and a
        // boss standing over its own warning must not hide it.
        for (int i = 0; i < MAX_TELEGRAPHS; i++)
            draw_telegraph(i);

        for (int i = 0; i < MAX_GEMS; i++)
            draw_gem(i);

        // The crowd, drawn back to front so overlapping creatures stack by
        // depth rather than by pool slot: without the sort, a slime standing
        // "in front of" a brute could flicker behind it as slots churn.
        {
            uint8_t yorder[MAX_MONSTERS];
            int n = 0;
            for (int i = 0; i < MAX_MONSTERS; i++)
                if (g_monsters[i].alive)
                    yorder[n++] = (uint8_t)i;
            for (int a = 1; a < n; a++)
            {
                uint8_t v = yorder[a];
                int32_t vx2, vy;
                monster_center(&g_monsters[v], &vx2, &vy);
                int b = a - 1;
                while (b >= 0)
                {
                    int32_t bx2, by2;
                    monster_center(&g_monsters[yorder[b]], &bx2, &by2);
                    if (by2 <= vy)
                        break;
                    yorder[b + 1] = yorder[b];
                    b--;
                }
                yorder[b + 1] = v;
            }
            for (int a = 0; a < n; a++)
                draw_monster(yorder[a]);
        }

        for (int i = 0; i < MAX_PROJECTILES; i++)
        {
            Projectile *p = &g_projectiles[i];
            if (!p->alive)
                continue;
            switch (p->kind)
            {
            case PROJ_WATER:
            {
                // A pressurised jet: bright head, a tapering trail along its
                // own velocity, and droplets spraying sideways off the nozzle.
                int len = (iabs(p->vx) > iabs(p->vy)) ? iabs(p->vx)
                                                      : iabs(p->vy);
                if (len < 1)
                    len = 1;
                int tx = p->vx / len, ty = p->vy / len;
                for (int k = 2; k >= 0; k--)
                {
                    int tr = p->radius - k * 2;
                    if (tr < 1)
                        tr = 1;
                    gfx_circle(p->x - tx * k * 4, p->y - ty * k * 4, tr,
                               (k == 0) ? PAL_CYAN_MID : PAL_CYAN_DARK);
                }
                gfx_circle(p->x, p->y,
                           p->radius > 3 ? p->radius - 3 : 1, PAL_CYAN_LIGHT);
                gfx_circle(p->x - tx * 2, p->y - ty * 2, 1, PAL_WHITE_SPARK);
                gfx_fill_rect(p->x - ty * 4 - 1, p->y + tx * 4, 1, 1,
                              PAL_CYAN_LIGHT);
                gfx_fill_rect(p->x + ty * 4, p->y - tx * 4 - 1, 1, 1,
                              PAL_CYAN_LIGHT);
                break;
            }
            case PROJ_DUNG:
            {
                // The lobbed ball, tumbling: its highlight rolls with the
                // flight and a crumb drops off behind it.
                int pr = p->radius > 4 ? 4 : p->radius;
                gfx_circle(p->x, p->y, pr, PAL_CHOCO_DARK);
                gfx_circle(p->x, p->y, pr - 1, PAL_CHOCO_MID);
                int a = (int)(g_frame * 10) & 255;
                int dx, dy;
                gfx_dir_256(a, &dx, &dy);
                gfx_fill_rect(p->x + (dx * pr) / 170 - 1,
                              p->y + (dy * pr) / 170 - 1,
                              2, 2, PAL_CHOCO_LIGHT);
                int len = (iabs(p->vx) > iabs(p->vy)) ? iabs(p->vx)
                                                      : iabs(p->vy);
                if (len >= 1)
                    gfx_fill_rect(p->x - (p->vx / len) * 5,
                                  p->y - (p->vy / len) * 5,
                                  1, 1, PAL_CHOCO_DARK);
                break;
            }
            case PROJ_ENEMY:
            {
                // Region-flavoured shots: the same colours the duel arena
                // uses, so a swamp bolt and a volcano bolt do not look alike.
                const DuelDef *dd = &g_duel_defs[g_region_id];
                gfx_circle(p->x, p->y, 4, dd->proj_a);
                gfx_circle(p->x, p->y, 3, dd->proj_b);
                gfx_fill_rect(p->x - 1, p->y - 1, 2, 2, dd->proj_c);
                int len = (iabs(p->vx) > iabs(p->vy)) ? iabs(p->vx)
                                                      : iabs(p->vy);
                if (len >= 1)
                    gfx_fill_rect(p->x - (p->vx / len) * 5 - 1,
                                  p->y - (p->vy / len) * 5 - 1,
                                  2, 2, dd->proj_a);
                break;
            }
            case PROJ_LOB:
            {
                // A scalding lob: hot body, brighter core, a smoke trail --
                // and (this used to be missing entirely) an actual sprite,
                // so the treefrog's shot is no longer an invisible tax.
                gfx_circle(p->x + 2, p->y + 6, 3, PAL_SHADOW);
                gfx_circle(p->x, p->y, 4, PAL_ORANGE_MID);
                gfx_circle(p->x - 1, p->y - 1, 2, PAL_YELLOW_MID);
                gfx_fill_rect(p->x - 2, p->y - 2, 1, 1, PAL_WHITE_SPARK);
                int len = (iabs(p->vx) > iabs(p->vy)) ? iabs(p->vx)
                                                      : iabs(p->vy);
                if (len >= 1)
                {
                    gfx_fill_rect(p->x - (p->vx / len) * 6,
                                  p->y - (p->vy / len) * 6,
                                  2, 2, PAL_GREY_MID);
                    gfx_fill_rect(p->x - (p->vx / len) * 4,
                                  p->y - (p->vy / len) * 4,
                                  1, 1, PAL_ORANGE_MID);
                }
                break;
            }
            case PROJ_MUD:
            {
                // The ultimate's flung mud, which used to draw as nothing.
                gfx_circle(p->x, p->y, 4, PAL_CLAY_DARK);
                gfx_circle(p->x, p->y, 3, PAL_CLAY_MID);
                gfx_fill_rect(p->x - 1, p->y - 2, 1, 1, PAL_BRN_MID2);
                break;
            }
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
                // A bite is a snap: one full-white flash frame, then the
                // ring of teeth marks with a bright core. The flash is what
                // separates "landed" from "grazed" in a crowd of forty.
                int t = e->life_max - e->life;
                if (t < 2)
                    gfx_circle(e->x, e->y, 6, PAL_WHITE_SPARK);
                gfx_circle(e->x, e->y, 5, PAL_ARC_CORE);
                gfx_circle(e->x, e->y, 3, PAL_WHITE_SPARK);
                gfx_circle_outline(e->x, e->y, 7, PAL_OUTLINE);
                for (int a = 0; a < 256; a += 32)
                {
                    int dx, dy;
                    gfx_dir_256(a, &dx, &dy);
                    gfx_fill_rect(e->x + (dx * 8) / 127, e->y + (dy * 8) / 127,
                                  2, 2, PAL_YELLOW_LIGHT);
                }
            }
            else if (e->kind == FX_TAIL)
            {
                // The sweep arc: a fan of sparks trailing the ring, which is
                // what says "this is moving round you" rather than "you are
                // standing in a circle". The first two frames flash the whole
                // disc, so a perfect-timing sweep lands with a clap of light.
                int t = e->life_max - e->life;
                int r = e->radius;
                if (t < 2)
                    gfx_circle(e->x, e->y, r, PAL_ARC_INNER);
                gfx_circle_outline(e->x, e->y, r, PAL_ARC_CORE);
                gfx_circle_outline(e->x, e->y, r - 3, PAL_ARC_INNER);
                gfx_circle_outline(e->x, e->y, r - 6, PAL_ARC_OUTER);
                for (int a = 0; a < 256; a += 12)
                {
                    int dx, dy;
                    gfx_dir_256((int)(a + g_frame * 6), &dx, &dy);
                    gfx_fill_rect(e->x + (dx * (r + 2)) / 127,
                                  e->y + (dy * (r + 2)) / 127,
                                  2, 2, PAL_WHITE_SPARK);
                }
            }
            else if (e->kind == FX_KILL)
            {
                // What is left where something died: a ring blown out to the
                // creature's own width, ash flying off it, and a couple of heavy
                // chunks that lag and fall. There is no alpha to fade with at
                // 8bpp, so the shape carries the dissolve -- it thins as it
                // grows and has nothing left to draw by its final radius.
                int t = e->life_max - e->life;
                int r = 3 + (t * e->radius) / e->life_max;
                int late = (t * 3) / e->life_max;   // 0..3

                gfx_circle_outline(e->x, e->y, r, PAL_CLAY_DARK);
                if (late < 3 && r > 4)
                    gfx_circle_outline(e->x, e->y, r - 2,
                                       late ? PAL_SAND_DARK : PAL_SAND_LIGHT);

                for (int a = 0; a < 256; a += 42)
                {
                    int dx, dy;
                    gfx_dir_256(a, &dx, &dy);
                    gfx_fill_rect(e->x + (dx * r) / 127,
                                  e->y + (dy * r) / 127, 2, 2,
                                  (late & 1) ? PAL_SAND_LIGHT : PAL_WHITE_SPARK);
                }

                if (late < 2)
                {
                    int cr = (r * 2) / 3;
                    for (int a = 32; a < 256; a += 85)
                    {
                        int dx, dy;
                        gfx_dir_256(a, &dx, &dy);
                        gfx_fill_rect(e->x + (dx * cr) / 127,
                                      e->y + (dy * cr) / 127 + t / 2,
                                      3, 3, PAL_CLAY_DARK);
                    }
                }
            }
            else if (e->kind == FX_HIT)
            {
                // Impact sparks, drawn as a four-point star: two crossing
                // bars and diagonal ticks that stretch as the effect ages.
                // Spawned on every landed blow, so it stays cheap -- but the
                // star silhouette is what makes a hit read in a crowd where
                // eight radial dots would read as noise.
                int t = e->life_max - e->life;
                uint8_t c = (t < 3) ? PAL_WHITE_SPARK
                                    : (t < 8 ? PAL_YELLOW_LIGHT : PAL_ORANGE_MID);
                int r = 3 + t;
                if (r > 8)
                    r = 8;
                gfx_hline(e->x - r, e->x + r, e->y, c);
                gfx_fill_rect(e->x, e->y - r, 1, r * 2 + 1, c);
                for (int k = 2; k < r; k++)
                {
                    gfx_fill_rect(e->x + k, e->y + k, 1, 1, c);
                    gfx_fill_rect(e->x - k, e->y + k, 1, 1, c);
                    gfx_fill_rect(e->x + k, e->y - k, 1, 1, c);
                    gfx_fill_rect(e->x - k, e->y - k, 1, 1, c);
                }
                if (t < 3)
                    gfx_fill_rect(e->x - 2, e->y - 2, 4, 4, PAL_WHITE_SPARK);
            }
        }

        draw_hero(0);
        draw_hero(1);

        // Floating damage numbers, over the crowd they describe.
        floaters_draw();

        // Off-screen indicators: a boss that has appeared but is out of view
        // is otherwise invisible, and "the boss is somewhere over there" is
        // information the player cannot play without.
        draw_offscreen_markers();

        // The boss leaping at the camera is drawn last over the world, so it
        // visibly sits on top of everything it is about to replace.
        if (leap_beat)
            duel_leap_out_draw();

        // Low-health edge: the frame of the whole screen pulses once the
        // capybara is a quarter down -- early enough to matter while there is
        // still something to do about it -- and pulses faster and hotter
        // inside the last heart.
        Hero *lh2 = &g_heroes[g_local_hero];
        if (lh2->active && lh2->hearts_max > 0)
        {
            bool quarter = lh2->hearts * 4 <= lh2->hearts_max;
            bool last = lh2->hearts <= 1;
            if (quarter && ((g_frame >> (last ? 2 : 3)) & 1))
            {
                uint8_t c = last ? PAL_RED_LIGHT : PAL_RED_DARK;
                gfx_fill_rect(0, 0, AREA_W, 2, c);
                gfx_fill_rect(0, AREA_H - 2, AREA_W, 2, c);
                gfx_fill_rect(0, 0, 2, AREA_H, c);
                gfx_fill_rect(AREA_W - 2, 0, 2, AREA_H, c);
            }
        }
    }

    hud_draw();

    // Last, so the vitals band sits over the world and over anything the world
    // pass drew near the top of the screen.
    hud_draw_top();

    // The combo chip rides under the vitals band while a chain is alive:
    // pushing into the crowd is supposed to feel rewarded, not just riskier.
    if (g_combo >= 3)
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "连击 x%d", (int)g_combo);
        ui_plate_sel(UI_MAIN, 168, 34, 84, 20, UI_R_TAB);
        ui_label(UI_MAIN, 168, 34, 84, 20, buf,
                 g_combo >= 10 ? PAL_UI_GOLD_LIGHT : PAL_WHITE);
    }
    // A raid wave keeps its warning up for the whole spike, not just the
    // first second of it.
    if (g_wave_active > 0 && ((g_frame >> 4) & 1))
    {
        ui_plate_sel(UI_MAIN, 4, 34, 84, 20, UI_R_TAB);
        ui_label(UI_MAIN, 4, 34, 84, 20, "兽潮来袭中",
                 PAL_UI2_RED);
    }
}


// ---------------------------------------------------------------------------
// The kingdom (水豚王国 · 湿地公园)
//
// A small park, not a second arena: 48x40 tiles (see hub_layout.h), dense with
// buildings, and every service is a few seconds' walk from the plaza. The
// landmark anchors come from the generator rather than being spelled out here,
// because a hand-copied pixel pair is what puts an interaction point on a tile
// that has since moved.
// ---------------------------------------------------------------------------

// Every landmark, in the order the generator emits them. The id doubles as the
// return value of hub_near_landmark(), which main.c switches on.
typedef struct {
    uint8_t  id;
    const char *name;       // what the bottom screen calls it
    const char *verb;       // what pressing A does
    int16_t  x, y;          // world pixels
} HubLandmark;

static const HubLandmark g_hub_landmarks[] = {
    { HUB_LM_GATE_ID,   "出征之门", "按A挑选远征关卡",
      HUB_LM_GATE_X,   HUB_LM_GATE_Y },
    { HUB_LM_POST_ID,   "信鸽塔",   "按A无线联机",
      HUB_LM_POST_X,   HUB_LM_POST_Y },
    { HUB_LM_TREE_ID,   "神树",     "按A打开被动神树",
      HUB_LM_TREE_X,   HUB_LM_TREE_Y },
    { HUB_LM_OVEN_ID,   "蛋糕烤箱", "按A烤制庆典蛋糕",
      HUB_LM_OVEN_X,   HUB_LM_OVEN_Y },
    { HUB_LM_MARKET_ID, "集市",     "看一眼食材进度",
      HUB_LM_MARKET_X, HUB_LM_MARKET_Y },
    { HUB_LM_SPRING_ID, "温泉",     "按A歇一晚，作物长大",
      HUB_LM_SPRING_X, HUB_LM_SPRING_Y },
};
#define HUB_LANDMARK_COUNT (sizeof(g_hub_landmarks) / sizeof(g_hub_landmarks[0]))

// How close the capybara has to be for a landmark to wake up. Generous on
// purpose: 48 px is three tiles, which at a stroll speed of 4 px/frame is
// under a fifth of a second of walking, so aiming is never fiddly.
#define HUB_REACH 48

void game_start_hub(void)
{
    g_region_id = 0;
    g_mid_boss_out = false;
    g_boss_out = false;
    g_seed_have = 0;
    // Whether the last run ended in victory or in the dirt, the park is a
    // place without bosses: any duel left standing would keep driving the
    // capybaras with arena controls.
    g_duel_active = false;
    g_duel.phase = DUEL_OFF;
    g_duel.enrage = false;
    g_toast.life = 0;
    g_combo = 0;
    g_combo_timer = 0;

    memset(g_monsters, 0, sizeof(g_monsters));
    memset(g_projectiles, 0, sizeof(g_projectiles));
    memset(g_gems, 0, sizeof(g_gems));
    memset(g_effects, 0, sizeof(g_effects));
    // Warnings belong to a fight, not to a scene: a ring left over from
    // the last one would be a shape that threatens nothing.
    memset(g_telegraphs, 0, sizeof(g_telegraphs));

    // The kingdom has its own dimensions; binding it without telling the
    // engine would let the camera clamp and the collision test read past the
    // end of this (much smaller) map buffer.
    world_set_size(HUB_COLS, HUB_ROWS);
    render_set_world(area_atlas(0), area_map(0), area_prop(0));

    Hero *h0 = &g_heroes[0];
    h0->active = true;
    h0->remote = false;
    // h0->variant is deliberately NOT set here: the character-select screen
    // picked it, and this runs on every return to the hub.
    // Spawn on the plaza, a little south of its centre, so the first thing on
    // screen is the boardwalk running north to the gate.
    h0->x = (HUB_W - HERO_SPRITE_W) / 2;
    h0->y = (HUB_H - HERO_SPRITE_H) / 2 + 24;
    h0->facing = DIR_UP;
    h0->invuln = 0;
    h0->hurt_stun = 0;
    h0->ram_timer = 0;
    hero_recompute_stats(0);

    if (g_multiplayer)
    {
        Hero *h1 = &g_heroes[1];
        h1->active = true;
        // The partner is always the capybara the local player did not pick.
        h1->variant = g_heroes[0].variant ^ 1;
        h1->x = (HUB_W - HERO_SPRITE_W) / 2 + 30;
        h1->y = (HUB_H - HERO_SPRITE_H) / 2 + 24;
        h1->facing = DIR_UP;
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

// The id of the landmark the local capybara is standing at, or 0.
int hub_near_landmark(void)
{
    Hero *h = &g_heroes[g_local_hero];
    if (!h->active)
        return 0;

    int32_t hx, hy;
    hero_center(h, &hx, &hy);

    // Nearest wins rather than first-found: the oven and the market are close
    // enough together that a first-match scan would make one of them
    // unreachable from certain approach angles.
    int best = 0;
    int32_t best_d = HUB_REACH * HUB_REACH;
    for (unsigned i = 0; i < HUB_LANDMARK_COUNT; i++)
    {
        const HubLandmark *lm = &g_hub_landmarks[i];
        int32_t d = dist2(hx, hy, lm->x, lm->y);
        if (d < best_d)
        {
            best_d = d;
            best = lm->id;
        }
    }
    return best;
}

const char *hub_landmark_name(int id)
{
    for (unsigned i = 0; i < HUB_LANDMARK_COUNT; i++)
        if (g_hub_landmarks[i].id == id)
            return g_hub_landmarks[i].name;
    return "";
}

const char *hub_landmark_verb(int id)
{
    for (unsigned i = 0; i < HUB_LANDMARK_COUNT; i++)
        if (g_hub_landmarks[i].id == id)
            return g_hub_landmarks[i].verb;
    return "";
}

// The landmark overlay: a breathing ring of light under each service, drawn in
// world space over the tile art.
//
// The buildings themselves are tiles now (tools/gen_world.py draws the gate,
// the tower, the tree, the oven), so this must NOT paint anything solid over
// them -- a placeholder rectangle on top of finished art is worse than no
// marker at all. A ground ring reads through the building's own silhouette and
// says "this one responds to you" without covering it.
static void hub_draw_landmarks(void)
{
    int near = hub_near_landmark();

    for (unsigned i = 0; i < HUB_LANDMARK_COUNT; i++)
    {
        const HubLandmark *lm = &g_hub_landmarks[i];
        if (!gfx_onscreen(lm->x - 24, lm->y - 24, 48, 48))
            continue;

        bool here = (lm->id == near);
        // The pulse is a two-step blink on a slow clock: fast enough to read as
        // alive, slow enough not to strobe on a small panel.
        bool pulse = ((g_frame >> 4) & 1) != 0;
        uint8_t ring = here ? (pulse ? PAL_WHITE_SPARK : PAL_UI_GOLD_LIGHT)
                            : (pulse ? PAL_UI_GOLD : PAL_UI_FRAME);
        int r = here ? 18 : 14;

        // Outlines only. A filled disc would blot out the tile art underneath,
        // and the buildings these mark are the best-looking things on the map.
        gfx_circle_outline(lm->x, lm->y + 6, r, PAL_OUTLINE);
        gfx_circle_outline(lm->x, lm->y + 6, r - 1, ring);
        if (here)
            gfx_circle_outline(lm->x, lm->y + 6, r - 4, ring);

        // A floating diamond above the building, so the marker is visible even
        // when the capybara is standing behind it.
        int fy = lm->y - 26 + ((g_frame >> 3) & 1);
        gfx_fill_rect(lm->x - 2, fy, 4, 4, PAL_OUTLINE);
        gfx_fill_rect(lm->x - 1, fy + 1, 2, 2,
                      here ? PAL_WHITE_SPARK : PAL_UI_GOLD_LIGHT);
    }
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

    // Follow camera, with the same soft lead the field uses: the hub is small,
    // but a view that drifts into the walk still reads better than a rigid one.
    Hero *h = &g_heroes[g_local_hero];
    if (h->active)
    {
        static const int8_t lead_dx[DIRS_COUNT] = { 0, 0, -1, 1, -1, 1, -1, 1 };
        static const int8_t lead_dy[DIRS_COUNT] = { 1, -1, 0, 0, 1, 1, -1, -1 };
        int32_t hx, hy;
        hero_center(h, &hx, &hy);
        int lead = h->moving ? 12 : 0;
        int cam_x, cam_y;
        render_camera_get(&cam_x, &cam_y);
        int want_x = hx - AREA_W / 2 + lead_dx[h->facing & 7] * lead;
        int want_y = hy - AREA_H / 2 + lead_dy[h->facing & 7] * lead;
        render_camera(cam_x + (want_x - cam_x) / 2,
                      cam_y + (want_y - cam_y) / 2);
    }

    render_set_world(area_atlas(0), area_map(0), area_prop(0));
    render_world();

    // The kingdom's own air: a few petals, which is all the hub wants.
    weather_draw();

    // The farm sits on the lawn, so it goes down before the markers and the
    // characters: the player walks in front of the crops, not behind them.
    farm_draw();

    // Ground markers under the services, then the characters over them.
    hub_draw_landmarks();

    draw_hero(0);
    if (g_multiplayer && g_heroes[1].active)
        draw_hero(1);

    // First-run guidance points the way on the top screen, over the world.
    tutorial_arrow_draw();

    hub_draw();
}

void hub_draw(void)
{
    ui_field(UI_SUB);

    // --- header: where you are, and how many tokens you are carrying --------
    sub_fill_rect(0, 0, AREA_W, 28, PAL_UI2_LINE);
    sub_fill_rect(0, 0, AREA_W, 26, PAL_UI2_SCREEN);
    sub_fill_rect(0, 0, AREA_W, 1, PAL_UI2_BLUE_DARK);
    ui_plate_sel(UI_SUB, 4, 4, 104, 20, UI_R_TAB);
    ui_label(UI_SUB, 4, 4, 104, 20, "水豚王国", PAL_WHITE);
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "印记 %d", g_boss_tokens);
        ui_text_right(UI_SUB, 250, 14, buf, PAL_UI2_BLUE_HI);
    }

    // --- proximity prompt --------------------------------------------------
    //
    // The one line that changes as the player walks around. Two things compete
    // for it: a landmark and the field. The field wins when the player is close
    // to it, because farming is a per-step decision ("plant this one too") and
    // a landmark is not -- you have already decided where you are going.
    int near = hub_near_landmark();
    bool farm_here = false;
    if (g_heroes[g_local_hero].active)
    {
        int32_t hx, hy;
        hero_center(&g_heroes[g_local_hero], &hx, &hy);
        farm_here = farm_in_reach(hx, hy);
    }
    bool oven_ready = (near == HUB_LM_OVEN_ID) && cake_ready();
    {
        bool hot = farm_here || near;
        ui_btn(UI_SUB, 8, 32, 240, 26, hot ? UI_SEL : UI_ON);
        char buf[96];
        if (farm_here)
            snprintf(buf, sizeof(buf), "★ 菜地 · %s", farm_prompt());
        else if (oven_ready)
            snprintf(buf, sizeof(buf), "★ 材料齐备！按 A 开烤");
        else if (near == HUB_LM_OVEN_ID)
            snprintf(buf, sizeof(buf), "★ 蛋糕烤箱 · 还差材料");
        else if (near)
            snprintf(buf, sizeof(buf), "★ %s · %s",
                     hub_landmark_name(near), hub_landmark_verb(near));
        else
        {
            // First-run guidance rides the one line that already changes as
            // you walk: while the chain has something to teach, it teaches
            // here, in place of the idle prompt. Nothing new on screen, and
            // it disappears for good the moment the loop is learned.
            const char *tut = tutorial_hint();
            if (tut)
                snprintf(buf, sizeof(buf), "★ %s", tut);
            else
                snprintf(buf, sizeof(buf), "走近一处地标，按 A 进入");
        }
        ui_label(UI_SUB, 8, 32, 240, 26, buf, hot ? PAL_WHITE : PAL_UI2_INK);
    }

    // --- the field, or the ingredient progress -----------------------------
    //
    // Walking up to the field swaps the two panels over to the farm, so the
    // screen answers the question the player is actually asking at that moment.
    // The checklist is still one step away (walk out of the field's reach),
    // which is why this is a swap and not a second screen.
    if (farm_here)
    {
        farm_draw_panel(64);
        {
            ui_plate(UI_SUB, 8, 128, 240, 62, UI_R_PLATE);
            ui_text(UI_SUB, 14, 134, "庆典蛋糕食材", PAL_UI2_INK_DIM);
            for (int i = 0; i < INGREDIENT_COUNT; i++)
            {
                bool done = cake_region_done(i);
                int ry = 152 + i * 11;
                ui_bullet(UI_SUB, 14, ry, done);
                ui_text(UI_SUB, 24, ry - 5, g_regions[i].ingredient,
                        done ? PAL_UI2_INK : PAL_UI2_INK_DIM);
                char cnt[24];
                snprintf(cnt, sizeof(cnt), "%d/%d",
                         (int)g_cake_have[i], cake_need(i));
                ui_text_right(UI_SUB, 240, ry - 1, cnt,
                              done ? PAL_UI2_GREEN : PAL_UI2_INK_DIM);
            }
        }
    }
    else
    {
        // --- ingredient progress ----------------------------------------------
        //
        // Five rows of dot + name, which reads as a checklist rather than as a
        // paragraph. The dot carries the state, so the eye can scan the column
        // without reading a word. Each row carries its harvest count too, so the
        // player can see how far the field still has to go.
        ui_plate(UI_SUB, 8, 64, 96, 82, UI_R_PLATE);
        ui_text(UI_SUB, 14, 70, "庆典蛋糕食材", PAL_UI2_INK_DIM);
        for (int i = 0; i < INGREDIENT_COUNT; i++)
        {
            bool done = cake_region_done(i);
            int ry = 90 + i * 11;
            ui_bullet(UI_SUB, 14, ry, done);
            ui_text(UI_SUB, 24, ry - 5, g_regions[i].ingredient,
                    done ? PAL_UI2_INK : PAL_UI2_INK_DIM);
        }

        // The seed satchel, on the far side of the checklist: what the player
        // still has to plant. Paired with the checklist it answers both halves
        // of "what do I do next" on one screen.
        ui_plate(UI_SUB, 108, 64, 140, 82, UI_R_PLATE);
        ui_text(UI_SUB, 114, 70, "种子袋", PAL_UI2_INK_DIM);
        for (int i = 0; i < FARM_CROP_COUNT; i++)
        {
            int ry = 90 + i * 11;
            char buf[32];
            bool have = g_seeds[i] > 0;
            ui_bullet(UI_SUB, 114, ry, have);
            snprintf(buf, sizeof(buf), "%s %d", g_regions[i].name,
                     (int)g_seeds[i]);
            ui_text(UI_SUB, 124, ry - 5, buf,
                    have ? PAL_UI2_INK : PAL_UI2_INK_DIM);
        }
    }

    // --- landmark guide ----------------------------------------------------
    //
    // Six entries as a two-column list of direction + name, so the park's
    // layout is something the player can memorise rather than read. The columns
    // are NOT filled in reading order: the four-character names (出征之门,
    // 蛋糕烤箱) all sit in the left one and the shorter ones in the right, and
    // each name starts after its own direction's measured width -- a fixed
    // offset would run "西北 信鸽塔" into the right column.
    //
    // Skipped while the player is at the field, because the farm panel occupies
    // the same slot. Drawing both would stack two plates on the same pixels and
    // the guide would paint over the satchel -- which is what a first cut of
    // this did, and it looked like the satchel had failed to render.
    if (!farm_here)
    {
        ui_plate(UI_SUB, 108, 64, 140, 82, UI_R_PLATE);
        ui_text(UI_SUB, 114, 70, "地标指南", PAL_UI2_INK_DIM);
        static const char *const guide[7][2] = {
            { "北",   "出征之门" }, { "东北", "神树"   },
            { "西北", "信鸽塔"   }, { "西南", "集市"   },
            { "南",   "蛋糕烤箱" }, { "东南", "温泉"   },
            { "西",   "菜地"     },
        };
        // Seven entries: the six services in a 2x3 grid, then the field on its
        // own row underneath. The field is not a landmark -- it is worked where
        // it stands, without a menu -- so putting it in the same grid would
        // promise an interaction it does not have.
        //
        // The index arithmetic has to say that, and for a long time it did not:
        // `col = i / 3` divides entry 6 out to column 2, and the column lookup
        // below only knows two of them, so 西 菜地 was painted on top of
        // 西南 集市 at the same coordinates. Both came out as overlapping
        // glyphs, which read as a font problem rather than as a layout one.
        // Rows are 14 apart so the fourth one still lands inside the plate.
        for (int i = 0; i < 7; i++)
        {
            bool own_row = (i >= 6);
            int col = own_row ? 0 : (i / 3);
            int row = own_row ? 3 : (i % 3);
            int gx = (col == 0) ? 114 : 182;
            int gy = 90 + row * 14;
            ui_text(UI_SUB, gx, gy, guide[i][0], PAL_UI2_INK_DIM);
            ui_text(UI_SUB, gx + text_utf8_width(guide[i][0]) + 4, gy,
                    guide[i][1], PAL_UI2_INK);
        }
    }

    // --- bottom touch bar: the four things worth a shortcut ---------------
    int near_id = hub_near_landmark();
    static const struct { const char *label; int lm; } shortcuts[4] = {
        { "世界地图", HUB_LM_GATE_ID },
        { "无线联机", HUB_LM_POST_ID },
        { "被动神树", HUB_LM_TREE_ID },
        { "烤箱庆典", HUB_LM_OVEN_ID },
    };
    for (int i = 0; i < 4; i++)
    {
        bool hot = (near_id == shortcuts[i].lm);
        int bx = 4 + i * 63;
        ui_btn(UI_SUB, bx, 150, 59, 38, hot ? UI_SEL : UI_ON);
        // The label takes an ink COLOUR, not a state: passing the state here
        // handed the text layer palette index 0, which is the transparent
        // marker -- hence labels that rendered in magenta.
        ui_label(UI_SUB, bx, 150, 59, 38, shortcuts[i].label,
                 hot ? PAL_WHITE : PAL_UI2_INK);
    }
}
