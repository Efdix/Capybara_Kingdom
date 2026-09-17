// Capybara Survival -- shared types, constants and globals.
//
// Two capybaras (one male, one female -- they have no names) survive endless
// waves in a series of regions. Each region is themed around the cake
// ingredient that grows there, enemies drop that ingredient as they die, and
// the region boss appears once enough has been gathered. Collect all five
// ingredients and the cake can be baked before the party.
//
// Screen layout: the top screen is the arena, composited in software into a
// single 256x192 8bpp bitmap. The bottom screen is a 4bpp text/UI bitmap drawn
// from our own CJK glyph atlas.
//
// Multiplayer model: each console fully simulates its own capybara (zero input
// latency) and reports it to the other console ~30x/second. The host is
// authoritative for monsters, pickups and progression, and broadcasts that
// state; the client renders the host's monsters with light interpolation.

#pragma once

#include <nds.h>
#include <stdbool.h>
#include <stdint.h>

// Palette index constants (PAL_*) generated from the same source of truth as
// the palette data itself.
#include "palette_ids.h"

// ---------------------------------------------------------------------------
// Screen and world geometry
//
// AREA_* describes the VIEWPORT (one screen). WORLD_* describes the actual
// playfield, which is 8x8 screens in size, and is what every entity coordinate
// in this game refers to. Keeping the two names distinct is deliberate: mixing
// them up is the mistake that made the old version unable to scroll.
// ---------------------------------------------------------------------------

#define TILE_PX       16
#define AREA_COLS     16
#define AREA_ROWS     12
#define AREA_W        (AREA_COLS * TILE_PX)   // 256, viewport width
#define AREA_H        (AREA_ROWS * TILE_PX)   // 192, viewport height

#define WORLD_COLS    128                     // tiles -> 2048 px
#define WORLD_ROWS     96                     // tiles -> 1536 px
#define WORLD_W       (WORLD_COLS * TILE_PX)
#define WORLD_H       (WORLD_ROWS * TILE_PX)

// Sprites are stored as a contiguous blob of 16x16 tiles, so the byte offset of
// tile `id` is a shift rather than a multiply.
#define TILE_BYTES    (TILE_PX * TILE_PX)     // 256 bytes per 16x16 tile
#define ATLAS_TILES   48                      // tiles per region atlas

// ---------------------------------------------------------------------------
// Game states
// ---------------------------------------------------------------------------

typedef enum {
    ST_BOOT = 0,
    ST_TITLE,
    ST_MODE_SELECT,     // single capybara / create party / join party
    ST_LOBBY,           // waiting for the other console
    ST_HUB,             // the kingdom: choose a region, then bake
    ST_LEVEL_INTRO,     // what this region is and what grows there
    ST_PLAY,            // surviving inside a region
    ST_CARD_PICK,       // leveled up: choose one of three cards
    ST_LEVEL_CLEAR,     // ingredient secured
    ST_GAME_OVER,       // run ended; the region is lost but the game is not
    ST_ENDING,          // the cake is baked
    ST_CREDITS,
    ST_SKILL_TREE,      // passive skill tree menu
} GameState;

// ---------------------------------------------------------------------------
// Cards
//
// The pool is organised by the five things a capybara actually does: bite with
// its teeth, sweep its stubby tail, and ram with its bulk; and -- being a
// semi-aquatic animal that lives in mud and does not much care what it stands
// in -- spray water, fling mud and drop dung. Two support cards play on its
// real-world reputation as the animal every other species tolerates: calming
// enemies, and at the top level recruiting them to your side.
//
// Every card goes to level 5 and the fifth is a signature ultimate that changes
// the card qualitatively rather than just adding damage. Passive cards are
// always in effect; active cards fire on their own timers, because this is a
// survivors-like: the player steers, the capybara fights.
// ---------------------------------------------------------------------------

#define CARD_TEETH      0   // 尖牙  bite the nearest enemy
#define CARD_TAIL       1   // 铁尾  sweep everything around us
#define CARD_RAM        2   // 冲撞  charge through the crowd
#define CARD_WATER      3   // 水柱  pressurised water, pierces
#define CARD_MUD        4   // 泥沼  ground that slows and rots
#define CARD_DUNG       5   // 粪球  lobbed, bursts into a stink cloud
#define CARD_PACIFY     6   // 安抚  nearby enemies lose the will to attack
#define CARD_RECRUIT    7   // 感化  a dying enemy switches sides
#define CARD_HIDE       8   // 厚皮  passive: more heart
#define CARD_SWIFT      9   // 快腿  passive: faster
#define CARD_MAGNET    10   // 磁吸  passive: wider pickup radius
#define CARD_FANG      11   // 利爪  passive: more damage

#define CARD_COUNT      12
#define CARD_MAX_LEVEL   5
#define CARD_CHOICES     3   // options offered on each level-up

// Card categories. The kind drives colour in the UI and which effect path the
// card uses at runtime.
#define KIND_PHYS        0   // 物理  teeth, tail, ram
#define KIND_MAGIC       1   // 魔法  water, mud, dung
#define KIND_SUPPORT     2   // 辅助  pacify, recruit
#define KIND_PASSIVE     3   // 被动  hide, swift, magnet, fang

typedef struct {
    const char *name;
    uint8_t  kind;
    uint16_t cooldown;      // frames between automatic uses (0 = passive)
    uint16_t power;         // base damage at level 1 (0 = no direct damage)
    uint8_t  radius;        // reach or area of effect, in pixels
    const char *desc;       // one line for the card picker
    const char *ultra;      // what the level-5 form adds
} CardDef;

extern const CardDef g_card_defs[CARD_COUNT];

// Card art geometry. Each card is a drawn picture (tools/gen_cards.py), not a
// text label: the picker blits this image and then draws only the name, the
// level pips and the description over the bands the generator left clear.
#define CARD_W        72
#define CARD_H        96
#define CARD_BYTES    (CARD_W * CARD_H)

// The name band and the footer band are laid out by gen_cards.py and read back
// here, so the engine knows where it is safe to draw text without covering the
// art. Keeping them in the header means a card redraw only has to change the
// generator.
#define CARD_BAND_TITLE_Y   3     // top of the name band, in card pixels
#define CARD_BAND_FOOT_Y   79     // top of the level-pip band

// ---------------------------------------------------------------------------
// Capybaras
// ---------------------------------------------------------------------------

#define HERO_MAX_HEARTS   12
#define HERO_BASE_SPEED   3

#define DIR_DOWN        0
#define DIR_UP          1
#define DIR_LEFT        2
#define DIR_RIGHT       3
#define DIR_DOWN_LEFT   4
#define DIR_DOWN_RIGHT  5
#define DIR_UP_LEFT     6
#define DIR_UP_RIGHT    7
#define DIRS_COUNT      8

#define SKILL_SLOT_A      0
#define SKILL_SLOT_B      1
#define SKILL_SLOT_X      2
#define SKILL_SLOT_Y      3
#define SKILL_SLOT_COUNT  4
#define SKILL_SLOT_NONE   0xFF


typedef struct {
    bool     active;
    bool     remote;        // driven by network input, not the pad
    uint8_t  variant;       // 0 = male sheet, 1 = female sheet

    // --- growth within the current run ---
    uint8_t  level;
    uint16_t exp;
    uint16_t exp_need;
    uint8_t  hearts;
    uint8_t  hearts_max;
    uint16_t attack;
    uint8_t  cards[CARD_COUNT];          // level of each card, 0 = not taken
    uint8_t  next_choice[CARD_CHOICES];  // cards offered on level-up
    uint8_t  skill_slots[SKILL_SLOT_COUNT]; // card_id mapped to [A, B, X, Y]

    // --- per-frame state ---
    int32_t  x, y;          // top-left pixel position of the sprite
    int8_t   facing;
    bool     moving;
    uint16_t anim;
    uint16_t bite_timer;    // >0 while the bite pose shows
    uint16_t hurt_stun;     // >0 while stunned and flashing
    uint16_t invuln;        // i-frames after taking damage
    uint16_t cooldown[CARD_COUNT];
    uint16_t ram_timer;     // >0 while charging
    int8_t   ram_dx, ram_dy;
    uint16_t pacify_timer;  // >0 while nearby enemies are cowed
    uint8_t  recruit_charge; // 感化 progress, spent when it converts one

    // --- orbiting water orbs (灵水环绕) ---
    uint8_t  water_orbs;       // current active orbiting water orbs (0..5)
    uint8_t  water_orbs_max;   // max orbiting orbs based on card level
    uint16_t orb_regen_timer;  // frames until 1 orb restored
    uint8_t  orb_angle;        // rotation angle (0..255)

    // --- derived stats, recomputed whenever a card is taken ---
    uint16_t move_speed;
    uint16_t pickup_r;      // pickup radius for exp and ingredients

    // --- network mirror for the remote capybara ---
    int32_t  net_x, net_y;
    uint8_t  net_facing;
    uint8_t  net_flags;
} Hero;

// ---------------------------------------------------------------------------
// Monsters
// ---------------------------------------------------------------------------

typedef enum {
    AI_CHASER = 0,   // walks at the nearest capybara
    AI_DASHER,       // periodically lunges
    AI_SHOOTER,      // keeps distance, fires projectiles
    AI_WANDER,       // no longer interested in fighting (pacified)
    AI_ALLY,         // recruited: fights for the capybaras
} AiType;

// One enemy archetype. Everything here is live: the spawner reads hp_base, the
// movement code reads speed, the draw code reads scale, and the damage and exp
// paths read their own fields.
//
// A region always fields three of these, one per role -- a small fast one, a
// big slow one and a ranged one -- so no region can be beaten by standing still
// and no two regions play the same way. See g_monster_defs.
typedef struct {
    AiType   ai;
    uint8_t  radius;        // collision and reach radius, in pixels
    uint8_t  scale;         // drawn size, as a percentage of the 32x32 art
    uint8_t  speed;         // pixels per frame, before slow effects
    uint8_t  damage;        // per hit, contact or projectile
    uint16_t hp_base;       // hit points at difficulty 0 in the first region
    uint16_t exp_reward;    // experience granted for a kill
} MonsterDef;

extern const MonsterDef g_monster_defs[];
const MonsterDef *monster_def(int id);

// 48 is what the arena holds before it stops being readable at 256x192: past
// that the crowd hides the player rather than threatening them.
#define MAX_MONSTERS     48
#define MAX_PROJECTILES  32
#define MAX_GEMS         96
#define MAX_EFFECTS      32

typedef struct {
    bool     alive;
    uint8_t  def_id;
    int32_t  x, y;
    int8_t   dx, dy;
    uint16_t hp, hp_max;
    uint16_t hurt_timer;
    uint16_t attack_timer;
    uint16_t anim;
    bool     is_boss;
    uint8_t  boss_id;
    uint8_t  state;
    uint16_t state_timer;
    uint8_t  radius;
    bool     recruited;     // 感化 turned this one: it fights for us now
    uint16_t slow_timer;    // >0 while standing in mud
    uint16_t pacify_timer;  // >0 while cowed by 安抚
    uint16_t rot_timer;     // >0 while rotting in high-level mud

    // --- Boss 2-3 unique skill tracking ---
    uint8_t  boss_skill;       // which skill currently executing (0 = pursuit/idle, 1..3)
    uint16_t boss_skill_timer; // execution/telegraph timer
    uint16_t boss_skill_cd;    // cooldown until next skill choice
    int32_t  boss_tx, boss_ty; // target coordinates for dash/leap/slam
} Monster;

typedef struct {
    bool     alive;
    uint8_t  kind;
    int32_t  x, y;
    int16_t  vx, vy;
    uint8_t  life;
    uint16_t power;
    uint8_t  radius;
} Projectile;

#define PROJ_WATER     0   // 水柱 -- piercing jet
#define PROJ_DUNG      1   // 粪球 -- arcs, bursts on impact
#define PROJ_ENEMY     2   // monster shot
#define PROJ_MUD       3   // flung mud from the ultimate

// Short-lived visual effects. Kept in their own pool rather than as part of the
// cards so a card's visual can outlive the frame that spawned it.
typedef struct {
    bool     alive;
    uint8_t  kind;
    int32_t  x, y;
    uint8_t  life, life_max;
    uint8_t  radius;
    uint16_t power;
} Effect;

#define FX_STINK       0   // 粪球 stink cloud
#define FX_MUD         1   // 泥沼 pool
#define FX_RAM_DUST    2   // dust behind a charge
#define FX_BITE        3   // bite snap
#define FX_TAIL        4   // tail sweep arc
#define FX_PACIFY      5   // calm ring
#define FX_LEVELUP     6   // ring under a level-up
#define FX_HIT         7
#define FX_DUNG_TRAP   8   // ground dung poison trap

// A dropped experience mote. Ingredients use the same pool with a different
// kind, because they behave identically: lie there until a capybara comes near.
typedef struct {
    bool     alive;
    uint8_t  kind;          // 0 = exp mote, 1 = this region's ingredient
    int32_t  x, y;
    uint16_t value;
    uint8_t  bob;
} Gem;

// ---------------------------------------------------------------------------
// Regions
//
// Each region is themed around the ingredient that grows there, and that is the
// reason to go: the enemies drop it, and the boss only shows up once enough has
// been gathered. The link is meant to be obvious -- fruit from a rainforest,
// flour from wheat fields, milk from pasture, sugar from a cane marsh,
// chocolate from a cacao grove.
// ---------------------------------------------------------------------------

#define INGREDIENT_COUNT 5

typedef struct {
    const char *name;         // region name
    const char *ingredient;   // cake ingredient found here
    uint8_t  area_id;
    uint8_t  mid_boss_id;     // phase 1 mid-boss
    uint8_t  boss_id;         // phase 2 final boss
    // The region's spawning roster, ordered {swarm, ranged, brute}. The spawner
    // walks it front to back as the difficulty climbs, so this order is the
    // order the player meets them in -- keep the fast melee one first.
    uint8_t  mob_ids[3];      // monster def ids that spawn; 0xFF = unused
    uint8_t  mob_count;
    uint16_t target;          // ingredient drops needed to summon the boss
    const char *intro;        // flavour line before the region
    const char *mid_boss_name;
    const char *boss_name;
} RegionDef;

extern const RegionDef g_regions[INGREDIENT_COUNT];

// ---------------------------------------------------------------------------
// Globals (owned by globals.c)
// ---------------------------------------------------------------------------

extern GameState g_state;
extern Hero      g_heroes[2];
extern Monster   g_monsters[MAX_MONSTERS];
extern Projectile g_projectiles[MAX_PROJECTILES];
extern Gem       g_gems[MAX_GEMS];
extern Effect    g_effects[MAX_EFFECTS];

extern uint8_t   g_region_id;
extern uint8_t   g_ingredient_have;    // drops gathered in the current region
extern uint8_t   g_ingredients_done;   // bitmask of secured ingredients
extern bool      g_mid_boss_out;       // mid-boss has appeared
extern bool      g_boss_out;           // the region boss is on the field
extern uint16_t  g_run_time;           // frames survived this run
extern uint8_t   g_difficulty;         // 0..DIFFICULTY_MAX, rises with time
extern uint16_t  g_frame;
extern uint8_t   g_local_hero;         // 0 or 1: which capybara this console drives
extern bool      g_multiplayer;
extern bool      g_is_host;
extern uint8_t   g_menu_cursor;
extern uint8_t   g_fade;
extern int8_t    g_screen_shake;

#include "skill_tree.h"

// Gameplay tuning shared between modules.
#define HERO_SPRITE_W     32
#define HERO_SPRITE_H     32
#define HERO_HITBOX_W     18
#define HERO_HITBOX_H     12
#define HERO_HITBOX_OX     7     // hitbox offset inside the sprite
#define HERO_HITBOX_OY    16
#define BITE_RANGE        26     // the base auto-attack's reach
#define BITE_COOLDOWN     34

// Difficulty ramp. The run is endless, so difficulty, not level length, is what
// ends it: the spawn interval shrinks and monster stats climb on a timer.
#define DIFFICULTY_STEP   1500   // 25 seconds per step
#define DIFFICULTY_MAX     8
#define SPAWN_BASE          70   // frames between spawns at difficulty 0
#define SPAWN_MIN           11   // floor on that interval

// ---------------------------------------------------------------------------
// render.c
//
// Every gfx_* helper takes WORLD coordinates. The camera translation happens
// inside render.c, so game code never subtracts the viewport itself.
// ---------------------------------------------------------------------------

void render_init(void);
void render_set_world(const uint8_t *atlas, const uint8_t *map,
                      const uint8_t *prop);
void render_world(void);
void render_flush(void);
void render_camera(int x, int y);
void render_camera_center_on(int32_t wx, int32_t wy);
void render_camera_get(int *x, int *y);
void gfx_fill_rect(int x, int y, int w, int h, uint8_t color);
void gfx_rect_outline(int x, int y, int w, int h, uint8_t color);
void gfx_blit_8bpp(const uint8_t *tiles, int tw, int th, int frame,
                   int x, int y, bool hflip, bool shadow);
// Same, resampled to `pct` percent so one sheet can draw creatures of different
// bulk. Grows down and right from (x, y) like the unscaled version.
void gfx_blit_8bpp_scaled(const uint8_t *tiles, int tw, int th, int frame,
                          int x, int y, bool hflip, bool shadow, int pct);
void gfx_hline(int x, int y, int w, uint8_t color);
void gfx_circle(int cx, int cy, int r, uint8_t color);
void gfx_circle_outline(int cx, int cy, int r, uint8_t color);
// Direction of one of 256 angles, each component scaled to about 127. Shared by
// the circle outlines and anything else that needs to place something on a ring.
void gfx_dir_256(int angle, int *dx, int *dy);
void gfx_line(int x0, int y0, int x1, int y1, uint8_t color);
bool collision_at(int px, int py);
bool collision_rect(int px, int py, int w, int h);
bool collision_open_near(int px, int py, int radius);
uint8_t *text_sub_buffer(void);

// A copy of one card's art, straight out of the atlas. The picker needs to draw
// a card at an arbitrary place on the sub screen, so it gets the source pixels
// rather than being forced through the world blitter.
const uint8_t *card_art(int card_id);

// Blit one card onto the SUB screen at (x, y), scaled to fit `scale` (1 = 72x96
// pixels). Screen coordinates: the picker is UI, not world.
void sub_blit_card(int card_id, int x, int y, int w, int h);

// text.c
void text_init(void);
void text_clear(void);
void text_draw(int col, int row, const char *utf8, uint16_t color);
void text_printf(int col, int row, uint16_t color, const char *fmt, ...);
void text_panel(int col, int row, int w, int h);
void text_flush(void);
// Sub-screen shape drawing: the HUD lives on the bottom screen, so it must not
// use the main-screen gfx_* helpers.
void sub_fill_rect(int x, int y, int w, int h, uint8_t color);
void sub_rect_outline(int x, int y, int w, int h, uint8_t color);

// input.c -- single entry point for key AND touch state.
//
// Everything reads input through these functions instead of libnds'
// keysDown()/keysHeld()/touchRead() so that the autoplay harness (see main.c)
// can swap the input source without touching gameplay code.
uint16_t input_held(void);
uint16_t input_down(void);
void input_poll(void);

// Touch. `pressed` is the down edge; `in` tests the current position against a
// rectangle on the BOTTOM screen.
bool input_touch_pressed(void);
uint16_t input_touch_x(void);
uint16_t input_touch_y(void);
bool input_touch_in(int x, int y, int w, int h);

// sprites.c
void draw_hero(int index);
void draw_monster(int index);
void draw_gem(int index);
int  monster_draw_radius(const Monster *m);

// audio.c
#define MUS_TITLE     0
#define MUS_FIELD     1
#define MUS_BATTLE    2
#define MUS_BOSS      3
#define MUS_VICTORY   4
#define MUS_BIRTHDAY  5
#define MUS_COUNT     6

void audio_init(void);
void audio_play_music(int module_id);
void audio_stop_music(void);
void audio_play_sfx(int sfx_id);

// Sound effect ids, in the order gen_audio.py emits them.
#define SND_UI_MOVE       0
#define SND_UI_CONFIRM    1
#define SND_UI_CANCEL     2
#define SND_ATTACK        3
#define SND_HIT           4
#define SND_HURT          5
#define SND_ENEMY_DIE     6
#define SND_MAGIC_FIRE    7
#define SND_MAGIC_ICE     8
#define SND_MAGIC_THUNDER 9
#define SND_HEAL         10
#define SND_PICKUP       11
#define SND_LEVELUP      12
#define SND_DOOR         13
#define SND_EXPLOSION    14
#define SND_HEART        15
#define SND_COUNT        16

// card_menu.c -- the level-up card picker.
//
// The run is paused while this is up, and the player can commit a choice with
// either the D-pad/L/R plus A, or by tapping the card on the touch screen.
void card_menu_reset(void);
void card_menu_roll(int hero_index);
void card_menu_input(void);
void card_menu_draw(void);

// game.c
void game_init(void);
void game_update(void);
void game_start_region(int region_index);
void game_start_hub(void);
void hub_update(void);
void hub_draw(void);
int  hub_near_landmark(void);
void hero_take_card(int hero_index, int card_id);
void hero_assign_slot(int hero_index, int slot, uint8_t card_id);
void hero_cast_slot(int hero_index, int slot);

void hero_recompute_stats(int hero_index);
int  heroes_alive(void);
int  nearest_hero(int px, int py, int32_t *out_dist);
int  nearest_monster(int32_t px, int32_t py, int32_t max_dist,
                     int32_t *out_x, int32_t *out_y);
void monster_center(const Monster *m, int32_t *cx, int32_t *cy);
void hero_center(const Hero *h, int32_t *cx, int32_t *cy);
void spawn_effect(uint8_t kind, int32_t x, int32_t y, uint8_t life,
                  uint8_t radius);
void spawn_gem(uint8_t kind, int32_t x, int32_t y, uint16_t value);

// net.c
bool net_start_host(void);
bool net_start_client(void);
void net_update(void);
void net_shutdown(void);
bool net_ready(void);
