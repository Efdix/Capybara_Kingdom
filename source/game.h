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

// The five battle regions are all this size: 128x96 tiles = 2048x1536 px.
// The kingdom is much smaller (see hub_layout.h) -- it is a place to cross in
// a few seconds, not a second arena -- so every piece of engine code that
// needs to know "how big is the world right now" reads g_world_cols/rows
// rather than these. Use G_WORLD_W/G_WORLD_H, never these, in game logic.
#define WORLD_COLS    128                     // tiles -> 2048 px
#define WORLD_ROWS     96                     // tiles -> 1536 px
#define WORLD_W       (WORLD_COLS * TILE_PX)
#define WORLD_H       (WORLD_ROWS * TILE_PX)

// The currently bound world, set by world_set_size() whenever a new area is
// bound. Every bounds test, camera clamp and minimap sample uses these.
extern int g_world_cols;
extern int g_world_rows;
#define G_WORLD_W     (g_world_cols * TILE_PX)
#define G_WORLD_H     (g_world_rows * TILE_PX)

void world_set_size(int cols, int rows);

// Height of the vitals band (hearts, level, experience) along the top of the
// main screen, plus the frame line under it. Anything else that wants the top
// edge of that screen -- the off-screen boss marker, for one -- has to start
// below this or it is drawn and then covered.
#define VITALS_H      30
#define VITALS_BAND   (VITALS_H + 2)

// ---------------------------------------------------------------------------
// Bottom-screen layout, during play
//
// Every interactive rectangle lives here rather than being written out at both
// the drawing and the hit-testing site. The bottom screen is driven by finger
// and by D-pad simultaneously, so a button whose drawn box and tappable box
// disagree is a control that silently fails -- and keeping the two in one place
// is the only way to be sure they cannot.
// ---------------------------------------------------------------------------

// The play screen is a 2x4 grid below the header:
//
//     +--------------+   header: region, ingredient, level      (y 0..28)
//     |  cards held  |  +-----------+
//     |   (tall)     |  |  minimap  |                           (y 32..144)
//     |              |  +-----------+
//     |              |  | ultimate  |
//     +--------------+  +-----------+
//     [ A ][ B ][ X ][ Y ]   skill slots                        (y 148..188)
//
// The left column is tall because the card list has to hold twelve entries at
// the maximum; the right column splits into the map and the one button that is
// pressed without looking. Nothing overlaps, so no element has to be drawn
// around another.
#define SLOT_X(s)     (4 + (s) * 62)
#define SLOT_Y       148
#define SLOT_W        58
#define SLOT_H        40

#define MINIMAP_X    152
#define MINIMAP_Y     32
#define MINIMAP_W     96
#define MINIMAP_H     72

// The ultimate: a wide plate under the map, with a charge meter across its
// middle. This one rectangle is used for drawing AND for hit-testing.
#define ULT_BTN_X    152
#define ULT_BTN_Y    108
#define ULT_BTN_W     96
#define ULT_BTN_H     36

// Duel touch buttons, in the bottom screen's upper-left block (where the
// minimap sits during the field phase). Jump and roll are the two actions a
// duel demands without looking, so they get the big plates.
#define DUEL_BTN_JUMP_X     8
#define DUEL_BTN_JUMP_Y    34
#define DUEL_BTN_JUMP_W    66
#define DUEL_BTN_JUMP_H    50
#define DUEL_BTN_ROLL_X    78
#define DUEL_BTN_ROLL_Y    34
#define DUEL_BTN_ROLL_W    66
#define DUEL_BTN_ROLL_H    50

// The held-card list: the tall left column.
#define CARDS_X        4
#define CARDS_Y       32
#define CARDS_W      144
#define CARDS_H      112

// Sprites are stored as a contiguous blob of 16x16 tiles, so the byte offset of
// tile `id` is a shift rather than a multiply.
#define TILE_BYTES    (TILE_PX * TILE_PX)     // 256 bytes per 16x16 tile
// Tiles per region atlas. Ids 0..47 are the original contract; 48..63 are the
// building kit and the kingdom's civic props, appended by tools/gen_world.py.
#define ATLAS_TILES   64

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
    // Appended rather than inserted: g_state is mirrored over the link as a raw
    // byte (net.c), so shifting the earlier values is safe only as long as both
    // consoles run the same build -- which is not something to rely on.
    ST_CHAR_SELECT,     // which capybara the local player controls
    ST_SETTINGS,        // music and effect volume
    // Appended for the same reason: the world map replaced the old text-only
    // region list, and it is a distinct screen with its own cursor rules.
    ST_WORLD_MAP,       // pick a region off the chart
    // The boss duel: once the region's ingredient target is reached the boss
    // leaps out of the field, every small monster is cleared, and the fight
    // moves to a side-on 1v1 arena -- run, jump and roll with i-frames, and
    // spend the cards gathered during the field phase.
    ST_DUEL,            // side-view 1v1 boss duel
    // Appended last for the same raw-byte reason as the states above: the
    // market is a small exchange screen (surplus ingredients -> boss tokens)
    // hanging off the hub's 集市 landmark.
    ST_MARKET,          // trade surplus ingredients for boss tokens
    // Appended for the same reason: the story's three opening pages, shown
    // once per save between mode select and the kingdom. Skippable, and a
    // veteran's file never reaches it.
    ST_INTRO,           // the prologue, three pages of why
} GameState;

// ---------------------------------------------------------------------------
// The boss duel (霸主决战)
//
// A fixed single-screen arena drawn side-on: ground line near the bottom, the
// capybara on the left, the boss leaping in from above. The boss itself is an
// ordinary Monster (is_boss) so every damage path, projectile and card keeps
// working unchanged -- only its AI and the arena drawing are duel-specific.
// ---------------------------------------------------------------------------

// Feet line of the arena. Every jumper is anchored to this; the hero sprite is
// drawn 32px above it.
#define DUEL_GROUND_Y    158
// Horizontal play bounds, in hero-centre coordinates.
#define DUEL_X_MIN       24
#define DUEL_X_MAX       232

// Roll (翻滚): a short dash with invulnerability frames. One ground roll or
// one air roll per jump; the cooldown keeps it from replacing walking.
#define DUEL_ROLL_FRAMES   18
#define DUEL_ROLL_INVULN   14
#define DUEL_ROLL_CD       45
#define DUEL_ROLL_SPEED     5
// Jump, in pixels-per-frame halves: launch at -5, gravity +1 every other
// frame, so the arc peaks around 27px and the whole hop fits a DS screen.
#define DUEL_JUMP_VY       -10   // in half-pixels
#define DUEL_GRAVITY         1   // half-pixels per frame, every frame
// The duel boss's collision centre rides this far above its feet, so ground
// attacks (bite, orbs, tail) can reach a creature two heroes tall.
#define DUEL_BOSS_CTR_H     40

// ---------------------------------------------------------------------------
// Telegraphs (预警)
//
// Every dangerous move announces itself first. A telegraph is a shape drawn on
// the arena that fills or pulses until the move fires, so the fight is a
// reading game: the player learns "that ring means it lands there" instead of
// memorising frame counts.
//
// The same primitives serve the field bosses, which is why they live in their
// own pool rather than inside the duel state: a field boss marks its slam with
// TG_CIRCLE exactly like a duel slam does.
// ---------------------------------------------------------------------------
#define TG_CIRCLE   0   // one ring that grows: the move lands here
#define TG_CHAIN    1   // a series of rings that light in order: a combo
#define TG_LINE     2   // a lane along the ground: a charge or a laser
#define TG_ARC      3   // a half-ring around the caster: a sweep

#define MAX_TELEGRAPHS 12

typedef struct {
    bool     alive;
    uint8_t  kind;          // TG_*
    int32_t  x, y;          // centre / anchor
    int32_t  x2, y2;        // second point, for TG_LINE
    uint8_t  radius;
    uint8_t  life;          // frames left; the shape fills as it drains
    uint8_t  life_max;
    uint8_t  delay;         // frames before the shape starts showing
    uint8_t  owner;         // 0 = hostile telegraph, 1 = the capybaras'
} Telegraph;

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

// The twelve base cards are the pool the level-up picker draws from. Fused
// cards (below) are never offered -- they only exist as the result of a
// fusion, so the picker's eligibility test skips them.
#define CARD_BASE_COUNT 12

// --- fused cards (融合卡) ---------------------------------------------------
//
// Two cards at level 3+ can be fused on the level-up screen into a single
// card that carries both parents' mechanics. This is what makes a build a
// *build* rather than a list: the fusion is the moment two investments become
// one idea, and it frees a slot for the next idea.
#define CARD_FUSE_PREDATOR  12  // 掠食突袭  尖牙 + 冲撞
#define CARD_FUSE_BASTION   13  // 荆棘铁壁  铁尾 + 厚皮
#define CARD_FUSE_TIDE      14  // 潮汐双炮  水柱 + 灵水
#define CARD_FUSE_MIASMA    15  // 恶臭香氛  粪球 + 安抚
#define CARD_FUSE_KING      16  // 百兽盟主  感化 + 利爪
#define CARD_FUSE_GALE      17  // 疾风吸取  快腿 + 磁吸
#define CARD_FUSE_GEYSER    18  // 沸泉地雷  水柱 + 粪球
#define CARD_FUSE_WHIRL     19  // 回旋冲角  铁尾 + 冲撞

#define CARD_COUNT      20
#define CARD_MAX_LEVEL   5
#define CARD_CHOICES     3   // options offered on each level-up
// The level at which a card unlocks its mechanic (guard, pierce, ...). Below
// this it is only its own body; at or above it, it becomes the thing that
// answers a boss move.
//
// Two, not three. The unlock is the moment a card stops being a bigger number
// and becomes an answer to something the bosses do -- it is the most interesting
// step in the whole progression, and every pick spent getting there is a pick
// spent on the least interesting one. With a run's worth of picks spread across
// twelve cards, gating it at three put most mechanics past the end of the run.
// This is also the fusion gate, so fusing two cards costs four picks instead of
// six, which turns fusion from a late-game curiosity into a mid-run decision.
#define CARD_UNLOCK_LEVEL 2

// A fusion recipe. `a` and `b` are base-card ids; when both sit at
// CARD_UNLOCK_LEVEL or above the picker offers the fusion as a fourth, golden
// option. The result inherits the higher of the two parent levels.
typedef struct {
    uint8_t     a, b;       // parent cards
    uint8_t     result;     // fused card id
    const char *name;       // shown on the golden offer
    const char *desc;       // what the fusion adds over its parents
} FusionRecipe;

#define FUSION_COUNT 8
extern const FusionRecipe g_fusion_recipes[FUSION_COUNT];

// Which fusion, if any, these two cards make. -1 when they make none.
int fusion_for_pair(int a, int b);
// The first recipe this hero can fuse right now, or -1. Drives the golden
// offer on the level-up screen.
int fusion_available(const uint8_t *cards);

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
    // --- the duel form -----------------------------------------------------
    // One card, two presentations. In the field the card keeps its survivors
    // behaviour; in the duel arena the same mechanic is performed by hand.
    // The mechanic identity is shared on purpose -- 铁尾 guards in both, it
    // just guards by interrupting a wind-up in the field and by catching a
    // melee in the arena -- so a build learned in the field carries over
    // without a second set of rules to learn.
    uint16_t duel_cd;       // frames between manual uses in the duel
    const char *duel_desc;  // one line for the duel legend
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

// How far a capybara walks between two poses of its 6-pose walk cycle. The
// cycle used to advance on a fixed frame counter, which meant the art was
// paced for one speed and every point of 快腿 past it made the feet slide.
// Driving the cycle from distance instead keeps the stride the same length at
// every speed, which is what makes a speed buff read as hurrying rather than
// as gliding.
#define HERO_WALK_STEP_PX 24

// Where a fresh capybara starts, before 厚皮 and the 坚韧 tree add anything.
// One number, used by both game_init() and hero_recompute_stats() -- the two
// drifted apart once already, and the symptom was a health bar that shrank the
// first time a card was taken.
#define HERO_BASE_HEARTS  6

// The heaviest single blow a capybara can take, in hearts. Contact damage is
// allowed to reach this (a brute's bite costs two, a swarm's costs one) so the
// per-monster `damage` column means something. Ranged chip damage is pinned to
// one heart by its callers: a four-shot volley at two hearts a bolt would empty
// a full health bar inside a burst the player never had time to read.
#define HERO_MAX_DAMAGE_PER_HIT 2

// What every hostile projectile costs, regardless of which monster fired it.
// Volleys arrive in threes and fours, so the bolt is a tax on position while the
// body that fired it is the real threat.
#define RANGED_CHIP_DAMAGE 1

// Ultimate charge. Every kill credits every active capybara rather than only
// whoever landed the blow, so a pair charges at the same rate and the combined
// release is something both players can actually aim for.
#define ULT_CHARGE_MAX    28     // kills banked per ultimate
// How close together two releases must be to count as a combined one.
#define ULT_COMBO_WINDOW  40

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

    // --- 铁尾's guard (招架) ------------------------------------------------
    // The card's level-3 mechanic, and the same mechanic in both phases: in the
    // field a tail sweep interrupts a wind-up, in the arena holding the tail up
    // catches a melee. `guard_timer` is the open window; the first frames count
    // as a perfect parry, which is what rewards reading a telegraph instead of
    // mashing.
    uint16_t guard_timer;      // >0 while the guard window is open
    uint16_t guard_cd;         // frames until the guard can be raised again
    bool     guard_success;    // set on the frame a guard caught something

    // --- duel-form ability state -------------------------------------------
    uint16_t charge_timer;     // 水柱's held charge, >0 while winding up
    uint8_t  charge_lvl;       // 0 = not charging, 1 = charging, 2 = full
    uint16_t dung_armed;       // 粪球's placed pool, >0 while it can be blown
    uint8_t  combo_step;       // 尖牙's three-bite chain, 0 = idle
    uint16_t combo_window;     // >0 while the next bite chains

    // --- ultimate (大招) ---
    // Charged by kills rather than by a clock, so releasing one is something
    // the player earns by fighting. The male and the female do different
    // things with it; see hero_release_ultimate().
    uint16_t ult_charge;       // kills banked toward the next ultimate
    bool     ult_ready;        // charge is full and waiting to be spent
    uint16_t ult_flash;        // >0 while the release effect plays
    uint16_t ult_window;       // >0 briefly after releasing; the partner
                               // releasing inside this window makes it a combo

    // --- derived stats, recomputed whenever a card is taken ---
    uint16_t move_speed;
    uint16_t pickup_r;      // pickup radius for exp and ingredients

    // --- game feel: subpixel movement and decaying knockback ----------------
    // Subpixel remainders (1/256 px) let a diagonal step move at 70.7% of the
    // cardinal speed without the truncation that would otherwise read as a
    // stutter. Knockback is a short decaying velocity rather than a one-frame
    // teleport, so a hit shoves instead of skipping.
    int16_t  sub_x, sub_y;
    int8_t   kb_vx, kb_vy;  // knockback velocity, pixels per frame
    uint8_t  kb_timer;      // frames of knockback left

    // --- boss duel movement (side-view arena) ---
    // In the duel x/y are arena coordinates and the d-pad drives a runner:
    // jump arcs with half-pixel vertical speed, the roll is a short dash with
    // i-frames. The top-down fields above are untouched and reused for
    // everything else (cooldowns, invuln, orbs).
    int16_t  jump_vy;       // vertical speed in half-pixels; 0 = grounded
    uint8_t  roll_timer;    // >0 while rolling
    uint8_t  roll_cd;       // frames until the next roll
    int8_t   roll_dir;      // -1 / +1 while rolling
    bool     roll_air;      // the air roll of the current jump is spent
    // Input buffers: a press that lands a few frames before it can be answered
    // (in the air, mid-roll cooldown) is held here briefly, so a jump or roll
    // fires on the first frame it becomes possible instead of being eaten.
    uint8_t  jump_buffer;
    uint8_t  roll_buffer;

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
    AI_DASHER,       // periodically lunges (legacy: kept for saves/links)
    AI_SHOOTER,      // keeps distance, fires projectiles
    AI_WANDER,       // no longer interested in fighting (pacified)
    AI_ALLY,         // recruited: fights for the capybaras
    // --- per-mob behaviours: every region's three residents move and fight
    // in a way no other mob does, so each region asks for different movement
    // from the player rather than only different numbers ---
    AI_ZIGZAG,       // weaves sideways while closing in; hard to out-aim
    AI_LOBBER,       // lobs arcing shots that leave a burning puddle on landing
    AI_SPLITTER,     // on death splits into two smaller copies (one generation)
    AI_STEALER,      // darts in, steals an ingredient, then flees -- chase it!
    AI_DIVER,        // hovers, telegraphs, then dive-bombs in a straight line
    AI_NOVA,         // armoured bruiser that erupts in a shock pulse up close
    AI_PACK,         // hunts in a pack: speeds up beside packmates
    AI_CHARGER,      // telegraphed long charge; step aside, then punish
    AI_SUPPORT,      // shoots weakly and hastes nearby allies -- kill it first
    AI_SPITTER,      // closes in, then fires a three-shot burst
    AI_BOMBER,       // drifts at you and detonates; shoot it before it arrives
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
    uint8_t  drop_pct;      // chance of an ingredient drop, in percent
} MonsterDef;

extern const MonsterDef g_monster_defs[];
const MonsterDef *monster_def(int id);

// 48 is what the arena holds before it stops being readable at 256x192: past
// that the crowd hides the player rather than threatening them.
#define MAX_MONSTERS     48
#define MAX_PROJECTILES  40
// Sized so a full raid wave's drops survive the fight: 48 monsters at the top
// drop rate plus a boss fountain used to overflow the old pool of 96, and the
// newest drops -- the ones the player was walking toward -- were the ones lost.
#define MAX_GEMS         128
#define MAX_EFFECTS      40
// Damage numbers. This was 28 while the arena can hold 48 monsters and a
// mastered 尖牙 bites three times a frame: in a dense wave the pool filled and
// the newest hits -- the ones the player is actually watching -- were the ones
// dropped, so the fight went quiet exactly when the most was happening.
#define MAX_FLOATERS     48

typedef struct {
    bool     alive;
    uint8_t  def_id;
    int32_t  x, y;
    int8_t   dx, dy;
    uint16_t hp, hp_max;
    uint16_t hurt_timer;
    uint16_t attack_timer;
    uint16_t anim;
    // Per-monster offset into the shared idle-bob cycle. Without it every
    // monster on screen reads the same global frame counter and the whole
    // crowd flips between the two idle poses on the same tick -- forty
    // creatures breathing in lockstep, which reads as one animation played
    // forty times rather than as forty animals.
    uint8_t  bob_phase;
    bool     is_boss;
    uint8_t  boss_id;
    uint8_t  state;
    uint16_t state_timer;
    uint8_t  radius;
    bool     recruited;     // 感化 turned this one: it fights for us now
    uint16_t slow_timer;    // >0 while standing in mud
    uint16_t pacify_timer;  // >0 while cowed by 安抚
    uint16_t rot_timer;     // >0 while rotting in high-level mud
    uint16_t haste_timer;   // >0 while hasted by a support mob (牛虻)

    // --- variants ---
    uint8_t  elite;         // golden elite: tougher, far more rewarding
    uint8_t  gen;           // splitter generation; gen 1 does not split again
    uint8_t  scale_pct;     // drawn/collision scale, 100 = the def's own size

    // --- Boss 2-3 unique skill tracking ---
    uint8_t  boss_skill;       // which skill currently executing (0 = pursuit/idle, 1..3)
    uint16_t boss_skill_timer; // execution/telegraph timer
    uint16_t boss_skill_cd;    // cooldown until next skill choice
    int32_t  boss_tx, boss_ty; // target coordinates for dash/leap/slam

    // --- Boss phase / stagger / pattern bookkeeping ---
    // The field bosses run a fixed looping pattern rather than a random wheel,
    // so their fights can be learned; the duel bosses run the same idea through
    // a richer state machine. Both track phase and stagger here.
    uint8_t  phase;            // 0 = P1, 1 = P2; duel bosses also use 2 = P3
    uint8_t  pattern_step;     // index into the boss's fixed move cycle
    uint8_t  stagger_hits;     // hits landed inside the current stagger window
    uint16_t stagger_window;   // >0 while the window is open
    uint16_t stagger_timer;    // >0 while the boss is down on its knees
    // Poise does not recover instantly: after getting up, the boss cannot be
    // staggered again until this runs out. Without it, a fast-enough build
    // re-filled the window while the boss was still standing up and kept it
    // down permanently -- every scripted pattern unreadable.
    uint16_t stagger_immune;   // >0 while poise cannot be chipped
    uint16_t telegraph_timer;  // >0 while a hostile telegraph is showing
} Monster;

typedef struct {
    bool     alive;
    uint8_t  kind;
    int32_t  x, y;
    int16_t  vx, vy;
    uint8_t  life;
    uint16_t power;
    uint8_t  radius;
    // Who the shot answers to. Ownership by KIND was the old rule, and it was
    // wrong in exactly the cases where it mattered: a field boss that fires a
    // PROJ_WATER or PROJ_MUD -- kinds the capybaras also use -- produced shots
    // that could not hurt the player and instead struck whatever hostile
    // creature they crossed, boss included. Every spawner now declares a side.
    bool     hostile;
} Projectile;

#define PROJ_WATER     0   // 水柱 -- piercing jet
#define PROJ_DUNG      1   // 粪球 -- arcs, bursts on impact
#define PROJ_ENEMY     2   // monster shot
#define PROJ_MUD       3   // flung mud from the ultimate
#define PROJ_LOB       4   // lobber shot -- arcs, leaves a scalding puddle
#define PROJ_SHOCK     5   // duel ground shockwave -- jump over it

// Short-lived visual effects. Kept in their own pool rather than as part of the
// cards so a card's visual can outlive the frame that spawned it.
typedef struct {
    bool     alive;
    uint8_t  kind;
    int32_t  x, y;
    uint8_t  life, life_max;
    uint8_t  radius;
    uint16_t power;
    // 0 = the capybaras' (damage lands on monsters), 1 = a monster's (damage
    // lands on the capybaras). A lobber's scalding puddle and a dung trap are
    // the same shape on the floor; whose it is decides who it burns.
    uint8_t  owner;
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
// What is left where a monster died. The kill itself used to be a pop: the
// sprite stopped being drawn on the frame its health ran out, leaving the drops
// to explain what had happened. This is the beat in between.
#define FX_KILL        9

// A dropped experience mote. Ingredients use the same pool with a different
// kind, because they behave identically: lie there until a capybara comes near.
//
// Seeds (kind 4) are what monsters drop now. The cake ingredients are grown in
// the kingdom's field instead of being looted, so an expedition brings home the
// means to farm rather than the cake itself. `value` carries which crop, as
// crop+1, so the satchel and the field agree on one number.
typedef struct {
    bool     alive;
    uint8_t  kind;          // 0 = exp mote, 1 = ingredient, 2 = boss token,
                            // 3 = heart (restores one heart), 4 = seed
    int32_t  x, y;
    uint16_t value;
    uint8_t  bob;
} Gem;

#define GEM_EXP        0
#define GEM_INGREDIENT 1
#define GEM_TOKEN      2
#define GEM_HEART      3
#define GEM_SEED       4

// Floating combat text: damage numbers that rise off the hit and pop. The
// values are drawn with a tiny built-in digit font, so a crowd of hits costs
// almost nothing and needs no glyph atlas.
//
// The pool size lives with the other pools, above MAX_MONSTERS. It used to be
// defined here as well, which meant an edit to the one above it quietly did
// nothing -- the same duplicate-define trap as FARM_YIELD in farm.h.
#define FLOATER_HIT    0   // white damage number
#define FLOATER_CRIT   1   // gold, double-size critical number
#define FLOATER_HEAL   2   // green +1
#define FLOATER_SEED   3   // amber +1: a seed went into the satchel

typedef struct {
    bool     alive;
    uint8_t  kind;
    int32_t  x, y;
    int16_t  value;
    uint8_t  life;
} Floater;

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

// ---------------------------------------------------------------------------
// Boss combat framework
//
// Two rules make these fights read as duels rather than as damage races:
//
//   1. Every move telegraphs. Wind-ups are at least TELEGRAPH_MIN frames, and
//      a combo leaves at least ROLL_CD frames between its beats, so a player
//      who reads the shape always has the roll available to answer it.
//   2. Punishing an opened boss pays. Landing STAGGER_HITS hits inside a
//      STAGGER_WINDOW drops the boss to its knees for STAGGER_FRAMES, during
//      which it takes bonus damage -- the reward loop for aggressive play.
// ---------------------------------------------------------------------------

#define TELEGRAPH_MIN      20   // shortest fair wind-up, in frames
#define STAGGER_HITS       10   // hits needed to break a boss's poise
#define STAGGER_WINDOW    240   // frames the poise window stays open
#define STAGGER_FRAMES     90   // how long the boss stays down
// Poise recovery: after standing back up, the boss is un-staggerable for this
// long. Ten seconds is enough to walk one full pattern phase, so the loop
// becomes "break it, take the burst, survive its answer, break it again".
#define STAGGER_IMMUNE    600
#define STAGGER_DMG_NUM     3   // bonus damage is dealt as x/2, so 3 = 150%
#define STAGGER_DMG_DEN     2

// 招架 (铁尾's guard). The window is short on purpose: it has to be raised in
// answer to something, not held. The first frames inside it are a perfect
// parry, which is the only version that gives health back.
#define GUARD_FRAMES        20
#define GUARD_PERFECT_FRAMES 10
#define GUARD_COOLDOWN     150
// A guard that catches something knocks the attacker's wind-up away, so the
// same window answers a field mob's charge as well as a duel boss's swing.
#define GUARD_RIPOSTE_R     46

// Phase boundaries, as a percentage of max health. P1 above the first, P2
// between, P3 below the second (duel bosses only; field bosses stop at 2).
#define BOSS_PHASE2_PCT    66
#define BOSS_PHASE3_PCT    33
// Frames of invulnerable roar when a phase turns over.
#define BOSS_PHASE_ROAR    55

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
    // The epithet under a boss's name card. A named boss is a character, and
    // the name card is where the fight tells the player it is a character --
    // the same trick Hollow Knight's title cards use.
    const char *mid_boss_title;
    const char *boss_title;
    // What the boss says when it is appeased, shown on the region-clear plate.
    // The bosses are not killed -- they are worn down until the anger breaks
    // and something softer underneath it says one honest line.
    const char *appeased;
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
extern Telegraph g_telegraphs[MAX_TELEGRAPHS];

extern uint8_t   g_region_id;
extern uint8_t   g_seed_have;    // drops gathered in the current region
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

// ---------------------------------------------------------------------------
// Hitstop
//
// A connect stops the world for a few frames. It is the cheapest weight there
// is: the picture holds, the sound has already fired, and the eye reads the
// pause as impact. Before this the game had shake, floaters and effects but
// never a pause, so a bite that took a third of a monster's health and a bite
// that grazed it looked equally instantaneous.
//
// The freeze is implemented by skipping the frame's state handler entirely --
// every screen draws into the same pair of shadow buffers that render_flush()
// then copies to VRAM, so not redrawing leaves last frame's picture on the
// glass. Nothing else in the engine has to know about it.
#define HITSTOP_HIT        2   // any landed blow
#define HITSTOP_KILL       4   // the blow that finished something
#define HITSTOP_HEAVY      6   // an elite or a boss taking a real hit
#define HITSTOP_BREAK      9   // a boss dropping to its knees
#define HITSTOP_PLAYER     5   // the player being hit
#define HITSTOP_MAX       12   // a hard ceiling, so it can never read as a hang
extern uint8_t   g_hitstop;
void hitstop_request(uint8_t frames);
// Write the bottom screen's news banner from outside the field. The hub and the
// farm both need to announce something and the screen has one banner slot, so
// they share it rather than each drawing their own overlapping strip.
void hub_toast(const char *text, uint8_t color, uint16_t life);

// Kill combo: kills landed within COMBO_WINDOW frames of each other chain up
// and each kill in the chain is worth more experience. Shared by both
// capybaras -- it measures the fight, not the fighter.
extern uint16_t  g_combo;
// How many weather specks the last frame drew. Reported by the perf readout so
// the atmosphere layer can be confirmed without trying to spot a dozen drifting
// one-pixel specks in a screenshot.
extern uint16_t  g_weather_specks;
extern uint16_t  g_combo_timer;
// Hostile kills this region, for the tallies on the clear/failure screens.
extern uint16_t  g_region_kills;
// Duel damage sync. On the client this accumulates damage its cards dealt to
// the duel boss and is drained by net.c into the hero packet; on the host it
// carries the frame flag "the boss just hit the partner's capybara".
extern uint16_t  g_duel_hit_pending;
// The defeat beat: frames left of the local capybara's fall before the game
// over plate arrives. Zero means nobody is dying. The pause between the last
// heart and the plate is what makes a defeat read as an event rather than as
// a screen swap.
extern uint8_t   g_hero_death_beat;
// Amount of damage the duel boss just dealt to the PARTNER's capybara on the
// host console; net.c carries it to the client and clears it.
extern uint8_t   g_duel_hit_amt;
// True while the side-view boss duel is running (also during its intro).
extern bool      g_duel_active;

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
// Later regions start partway up the ladder (difficulty opens at the region
// index), so the fifth region is never as gentle as the first.
#define DIFFICULTY_STEP   1650   // 27.5 seconds per step
#define DIFFICULTY_MAX    12
#define SPAWN_BASE          75   // frames between spawn batches at difficulty 0
#define SPAWN_MIN           24   // floor on that interval

// Kill combo. Kills inside the window chain; every chained kill pays
// 2% more experience, up to +40% at a combo of 20.
#define COMBO_WINDOW      150
#define COMBO_BONUS_CAP    20

// Raid waves (兽潮): a periodic spike of spawns led by elites, with a loot
// drop when the wave breaks. The spike is the region's rhythm -- calm farming
// between waves, a real crowd to survive during one.
#define WAVE_FIRST        3000   // first wave at 50s into a region
#define WAVE_INTERVAL     4200   // then every 70s
#define WAVE_DURATION      600   // 10 seconds of intensified spawning
#define WAVE_ELITE_PCT      25   // elite chance during a wave, in percent

// Experience: the first level is a couple of kills, growth stays gentle enough
// that a full hour-long campaign lands around level 18-22.
#define EXP_BASE           40
#define EXP_NEED_MAX    30000

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
// Blit a whole 256x192 8bpp image into the main screen in SCREEN space,
// ignoring the camera. Used by the title screen and the world map, which are
// UI compositions rather than part of the world.
void render_ui_image(const uint8_t *img);
// Is (sx, sy) on the main screen inside the given world-space rectangle? Lets
// the world-space overlays skip work for entities that are off screen.
bool gfx_onscreen(int32_t wx, int32_t wy, int w, int h);
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
void gfx_blit_8bpp_scaled_xy(const uint8_t *tiles, int tw, int th, int frame,
                             int x, int y, bool hflip, bool shadow,
                             int pct_x, int pct_y);

// Draw the next sprite as a solid silhouette in `colour`. Open it immediately
// before one blit and close it straight after: it applies to whatever is drawn
// next, not to a named sprite.
void gfx_flash_begin(uint8_t colour);
void gfx_flash_end(void);
void gfx_hline(int x, int y, int w, uint8_t color);
void gfx_circle(int cx, int cy, int r, uint8_t color);
void gfx_circle_outline(int cx, int cy, int r, uint8_t color);
// Direction of one of 256 angles, each component scaled to about 127. Shared by
// the circle outlines and anything else that needs to place something on a ring.
void gfx_dir_256(int angle, int *dx, int *dy);
void gfx_line(int x0, int y0, int x1, int y1, uint8_t color);
void gfx_hline(int x0, int x1, int y, uint8_t color);
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
// Pixel-placed text, for buttons and badges whose boxes are not on the grid.
void text_draw_px(int px, int py, const char *utf8, uint16_t color);
void text_draw_centered(int bx, int by, int bw, const char *utf8,
                        uint16_t color);
int  text_utf8_width(const char *utf8);
// Wraps to `wrap_cols` grid columns; returns the number of rows it used.
int  text_draw_wrapped(int col, int row, int wrap_cols, const char *utf8,
                       uint16_t color);
void text_flush(void);
// Sub-screen shape drawing: the menus live on the bottom screen, so they must
// not use the main-screen gfx_* helpers.
void sub_fill_rect(int x, int y, int w, int h, uint8_t color);
void sub_rect_outline(int x, int y, int w, int h, uint8_t color);

// Top-screen text and shapes. Same grid and the same pixel coordinates as the
// sub versions, but written into the main buffer so the play HUD can sit over
// the world. There is no top_text_clear() on purpose: the world has just been
// drawn there and must not be wiped.
void top_text_draw(int col, int row, const char *utf8, uint16_t color);
void top_text_draw_px(int px, int py, const char *utf8, uint16_t color);
void top_text_printf(int col, int row, uint16_t color, const char *fmt, ...);
void top_fill_rect(int x, int y, int w, int h, uint8_t color);
void top_rect_outline(int x, int y, int w, int h, uint8_t color);

// input.c -- single entry point for key AND touch state.
//
// Everything reads input through these functions instead of libnds'
// keysDown()/keysHeld()/touchRead() so that the autoplay harness (see main.c)
// can swap the input source without touching gameplay code.
uint16_t input_held(void);
uint16_t input_down(void);
void input_poll(void);
// Bank key edges pressed while the world is frozen (hitstop). The next poll
// merges them into input_down(), so a press the player made in reaction to a
// hit is answered rather than swallowed by the freeze.
void input_latch(uint16_t keys);
// Screen-capture diagnostics (autoplay builds only): live script position.
int  autoplay_debug_step(void);
int  autoplay_debug_left(void);
int  autoplay_debug_frame(void);

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
void draw_telegraph(int index);
int  monster_draw_radius(const Monster *m);

// game.c -- ultimates. Charged by kills, released by the local player; the
// host is authoritative for its own release and for the remote one it is told
// about over the link, so hero_release_ultimate() is also called from net.c.
void hero_release_ultimate(int index);

// audio.c
#define MUS_TITLE     0
#define MUS_FIELD     1
#define MUS_BATTLE    2
#define MUS_BOSS      3
#define MUS_VICTORY   4
#define MUS_BIRTHDAY  5
// One battle theme per region -- the last piece of the region package (map,
// weather, roster, boss) that used to be shared. Each has its own key and
// tempo; gen_audio.py emits them, audio.c degrades to MUS_BATTLE if a track
// is missing from the bank.
#define MUS_BATTLE_RAIN   6
#define MUS_BATTLE_FARM   7
#define MUS_BATTLE_PAST   8
#define MUS_BATTLE_MARSH  9
#define MUS_BATTLE_VOLCANO 10
#define MUS_COUNT     11

void audio_init(void);
void audio_play_music(int module_id);
void audio_stop_music(void);
void audio_play_sfx(int sfx_id);

// Volume levels are steps, not a slider -- the settings screen is driven by a
// d-pad. 0 is silent, AUDIO_VOL_MAX is full.
#define AUDIO_VOL_LEVELS  4
#define AUDIO_VOL_MAX     (AUDIO_VOL_LEVELS - 1)
void audio_set_music_volume(int level);
void audio_set_sfx_volume(int level);
int  audio_get_music_volume(void);
int  audio_get_sfx_volume(void);

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
// When two of the hero's cards are both at CARD_UNLOCK_LEVEL the picker adds a
// fourth, golden offer: fusing them. Taking it spends both parents and leaves
// one card carrying both mechanics, which is how a build becomes an idea.
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
const char *hub_landmark_name(int id);
const char *hub_landmark_verb(int id);
void hero_take_card(int hero_index, int card_id);
void hero_assign_slot(int hero_index, int slot, uint8_t card_id);
void hero_cast_slot(int hero_index, int slot);

void hero_recompute_stats(int hero_index);
void hero_take_damage(int index, int damage, int from_x, int from_y);
int  heroes_alive(void);
int  nearest_hero(int px, int py, int32_t *out_dist);
int  nearest_monster(int32_t px, int32_t py, int32_t max_dist,
                     int32_t *out_x, int32_t *out_y);
void monster_center(const Monster *m, int32_t *cx, int32_t *cy);
void hero_center(const Hero *h, int32_t *cx, int32_t *cy);
void spawn_effect(uint8_t kind, int32_t x, int32_t y, uint8_t life,
                  uint8_t radius);
// Same, but the hazard belongs to the monsters: its ticking damage lands on
// the capybaras (owner 1) instead of on the crowd (owner 0, the default).
void spawn_effect_owned(uint8_t kind, int32_t x, int32_t y, uint8_t life,
                        uint8_t radius, uint8_t owner);
void spawn_gem(uint8_t kind, int32_t x, int32_t y, uint16_t value);
void spawn_floater(int32_t x, int32_t y, int16_t value, uint8_t kind);

// Telegraphs (预警). Every shape is placed before a move fires and is drained
// by telegraph_update() each frame. A shape that never drains would be a lie,
// so the pool is small and reused -- MAX_TELEGRAPHS is sized to the busiest
// pattern in the game (the Cake Lord's finale, eight rings).
Telegraph *spawn_telegraph(uint8_t kind, int32_t x, int32_t y, uint8_t life,
                           uint8_t radius);
void telegraph_update(void);
// Convenience: a chain of TG_CIRCLE rings that light in sequence. `points` is
// an array of {x, y, radius}; each ring starts `stagger` frames after the one
// before it, so the player reads a combo rather than a single slam.
void spawn_telegraph_chain(const int32_t *points, int count,
                           uint8_t life, uint8_t stagger);

// Card fusion. `cards` is a Hero's card-level array; fusion_for_pair() answers
// which fused card two base cards make (or -1), and fusion_available() finds
// the first recipe whose parents are both unlocked.
void hero_fuse_cards(int hero_index, int recipe_index);

// Boss duel (ST_DUEL). The leap-out beat runs inside the field phase; from the
// moment it ends the state is ST_DUEL and duel_update() owns the frame.
void duel_update(void);
// Host side of a linked duel: applies damage a client's capybara dealt to
// the duel boss (the client never simulates it). No-op otherwise.
void duel_apply_client_damage(int dmg);
// Host side of a linked FIELD phase: applies damage a client's capybara dealt
// to whatever hostile creature stands nearest the reported position (the
// client's mirrors sit at the host's exact coordinates).
void field_apply_client_damage(int dmg, int32_t x, int32_t y);

// net.c
bool net_start_host(void);
bool net_start_client(void);
void net_update(void);
void net_shutdown(void);
bool net_ready(void);
// Tells the host that this console released its ultimate. Only meaningful on
// the client side of a live link; a no-op otherwise.
void net_notify_ultimate(void);
// One-shot client -> host notifications, carried on the hero packet the same
// way the ultimate request is. Both are no-ops outside a live link.
//   token_delta: the host owns the shared token purse, so a client's skill
//                purchase (negative) or market exchange (positive) is told,
//                not guessed.
//   farm msg:    1 = water the whole bed, 2..25 = context-action plot msg-2.
//                The client applies locally for instant feedback and reports;
//                the host's authoritative copy confirms on the next sync.
void net_notify_token_delta(int16_t delta);
void net_notify_farm(uint8_t msg);
// Damage this console's capybara dealt to field monsters since the last
// packet. The client never kills anything itself -- the host owns monsters --
// so its swings accumulate here and are applied (and credited) over there.
extern uint16_t  g_field_hit_pending;
extern int32_t   g_field_hit_x, g_field_hit_y;
// Set on the host when a picked-up heart should also heal the partner; the
// world packet carries it once and the client clears it.
extern uint8_t   g_partner_heal;
// Called by net.c on whichever console just lost the other half of a live
// session. Implemented in main.c: it cleans the session up and tells the
// player what happened instead of leaving a frozen partner on screen.
void multiplayer_link_lost(void);

// ---------------------------------------------------------------------------
// First-run guidance (tutorial.c)
//
// The chain once taught only the seed-to-cake loop in the hub; it now also
// covers the field's first minute, the duel and the prologue. Same shape as
// before: one hint at a time, each completes itself the moment the player
// performs the action, and a save that already shows progress marks the whole
// chain done -- a veteran never sees a hint, and a newcomer is never
// modal-blocked by one.
// ---------------------------------------------------------------------------
enum {
    TUT_GATE = 0,       // leave through the gate
    TUT_SEED,           // pick up a seed in the field
    TUT_PLANT,          // plant it in the field bed
    TUT_WATER,          // water the bed
    TUT_SLEEP,          // sleep at the spring with something growing
    TUT_HARVEST,        // harvest a ripe crop
    TUT_OVEN,           // hear that the oven takes the cake
    // Field beats: the first minute of combat taught at the moment it happens.
    TUT_B_MOVE,         // first seconds in a region: you only walk
    TUT_B_HURT,         // first hit taken: hearts and retreats
    TUT_B_LEVEL,        // first card pick: what the choice means
    TUT_B_ULT,          // ultimate first charged: press R
    TUT_B_COMBO,        // linked pair, both charged: the combo ultimate
    TUT_B_WAVE,         // first raid wave announced before it lands
    TUT_B_RAT,          // first theft: chase the thief for double
    TUT_B_ELITE,        // first elite seen: gold means seeds
    // One-shot modals: the duel is a whole second control scheme, and the
    // prologue is the story's first page. Both show once per save.
    TUT_DUEL_CARD,      // the duel's controls card was acknowledged
    TUT_INTRO,          // the prologue was seen (skipped counts)
    TUT_COUNT
};
// Where the action happened; the notifier advances the chain from it.
enum {
    TUT_EV_REGION_ENTER = 0,
    TUT_EV_SEED_PICKED,
    TUT_EV_PLANTED,
    TUT_EV_WATERED,
    TUT_EV_SLEPT,
    TUT_EV_HARVESTED,
    TUT_EV_OVEN,
};
extern uint32_t g_tut_flags;
void tutorial_boot(void);           // after save load: skip for veterans
void tutorial_notify(int event);    // an action happened
void tutorial_mark(int step);       // a one-shot beat happened
bool tutorial_done(int step);       // was this beat already shown/marked?
const char *tutorial_hint(void);    // current hint line, or NULL
// Which landmark the current hint is about, so the hub can point at it:
// a HUB_LM_* id, or 0 when nothing needs pointing at. The field has no
// landmark id of its own; it answers to HUB_TUT_FARM.
#define HUB_TUT_FARM 100
int tutorial_arrow_landmark(void);
// -1, or the region whose boss duel was lost and is still waiting. A region
// on this list re-summons its boss the moment it is entered: the seeds were
// already paid, so the second attempt does not bill them again.
extern int8_t g_duel_pending;

// game.c -- state the duel HUD reads that lives in game.c's private duel
// struct. Exposed as accessors rather than as the struct itself so the FSM
// stays free to change shape.
int  duel_boss_phase(void);        // 0..2, which phase the boss is in
bool duel_boss_is_staggered(void); // true while it is down on its knees
