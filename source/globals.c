// Global state + the two data tables that define the game: the card pool and
// the five regions.

#include <string.h>

#include "game.h"
#include "assets.h"
#include "hub_layout.h"

GameState g_state = ST_BOOT;
Hero g_heroes[2];
Monster g_monsters[MAX_MONSTERS];
Projectile g_projectiles[MAX_PROJECTILES];
Gem g_gems[MAX_GEMS];
Effect g_effects[MAX_EFFECTS];
Telegraph g_telegraphs[MAX_TELEGRAPHS];

uint8_t  g_region_id = 0;
uint8_t  g_seed_have = 0;
uint8_t  g_ingredients_done = 0;
bool     g_mid_boss_out = false;
bool     g_boss_out = false;
uint16_t g_run_time = 0;
uint8_t  g_difficulty = 0;
uint16_t g_frame = 0;
uint8_t  g_local_hero = 0;
bool     g_multiplayer = false;
bool     g_is_host = false;
uint8_t  g_menu_cursor = 0;
uint8_t  g_fade = 16;
int8_t   g_screen_shake = 0;
uint8_t  g_hitstop = 0;

uint16_t g_combo = 0;
uint16_t g_combo_timer = 0;
uint16_t g_region_kills = 0;
uint16_t g_duel_hit_pending = 0;
uint8_t  g_hero_death_beat = 0;
uint8_t  g_duel_hit_amt = 0;
bool     g_duel_active = false;
// Client -> host field damage accumulator (see damage_monster in game.c) and
// the position of its last hit; net.c drains both onto the hero packet.
uint16_t g_field_hit_pending = 0;
int32_t  g_field_hit_x = 0;
int32_t  g_field_hit_y = 0;
// Set on the host when a picked-up heart should also heal the partner; sent
// once in the world packet and cleared.
uint8_t  g_partner_heal = 0;
// First-run guidance chain, persisted in the save (see tutorial.c). A set bit
// means that step is done; 0 means nothing has been taught yet. Widened to 32
// bits when the chain grew past the hub: the save reads the low 16 first, so
// an older file loads with the new bits at zero -- exactly the fresh state.
uint32_t g_tut_flags = 0;
// The region whose boss duel was lost, or -1. RAM-only: a reboot forgets it,
// which is fine -- the duel's price was the run, not the seeds.
int8_t g_duel_pending = -1;
// The bound world's size, in tiles. Regions are all 128x96; the kingdom is a
// fraction of that (hub_layout.h). See world_set_size().
int g_world_cols = WORLD_COLS;
int g_world_rows = WORLD_ROWS;

void world_set_size(int cols, int rows)
{
    if (cols < AREA_COLS) cols = AREA_COLS;
    if (rows < AREA_ROWS) rows = AREA_ROWS;
    g_world_cols = cols;
    g_world_rows = rows;
}

// ---------------------------------------------------------------------------
// Region art lookup
//
// The world is a tile atlas plus a tile map per region (see gen_world.py), so
// the engine never holds a whole region's pixels. These three functions are the
// only place that knows the file-to-region mapping, which is also the order the
// generator emits them in.
// ---------------------------------------------------------------------------

const uint8_t *area_atlas(int area_id)
{
    switch (area_id)
    {
    case 0: return area_hub_atlas_bin;
    case 1: return area_rain_atlas_bin;
    case 2: return area_farm_atlas_bin;
    case 3: return area_past_atlas_bin;
    case 4: return area_marsh_atlas_bin;
    default: return area_cacao_atlas_bin;
    }
}

const uint8_t *area_map(int area_id)
{
    switch (area_id)
    {
    case 0: return area_hub_map_bin;
    case 1: return area_rain_map_bin;
    case 2: return area_farm_map_bin;
    case 3: return area_past_map_bin;
    case 4: return area_marsh_map_bin;
    default: return area_cacao_map_bin;
    }
}

const uint8_t *area_prop(int area_id)
{
    switch (area_id)
    {
    case 0: return area_hub_prop_bin;
    case 1: return area_rain_prop_bin;
    case 2: return area_farm_prop_bin;
    case 3: return area_past_prop_bin;
    case 4: return area_marsh_prop_bin;
    default: return area_cacao_prop_bin;
    }
}

// Each area's tile dimensions. The kingdom is small on purpose (it is a place
// to walk across between runs), the five battle regions are all 8x8 screens.
// The map buffers have no length header, so this table is the only place the
// size is known -- binding an area without calling world_set_size() from here
// would let the camera and the collision test read past the end of the map.
void area_size(int area_id, int *cols, int *rows)
{
    static const uint8_t sizes[AREA_REGION_COUNT][2] = {
        { HUB_COLS, HUB_ROWS },
        { WORLD_COLS, WORLD_ROWS },
        { WORLD_COLS, WORLD_ROWS },
        { WORLD_COLS, WORLD_ROWS },
        { WORLD_COLS, WORLD_ROWS },
        { WORLD_COLS, WORLD_ROWS },
    };
    if (area_id < 0 || area_id >= AREA_REGION_COUNT)
        area_id = 0;
    if (cols) *cols = sizes[area_id][0];
    if (rows) *rows = sizes[area_id][1];
}

// ---------------------------------------------------------------------------
// The card pool
//
// Twelve cards, three physical, three magic, two support and four passive,
// plus eight fused cards that only exist as the product of a fusion. Each goes
// to level 5, where it becomes something qualitatively different rather than
// merely bigger -- that is what makes the fifth level worth chasing.
//
// Level 3 is the second gate, and the one that matters in a boss fight: that is
// where a card turns into the thing which answers a boss move. 铁尾 guards at
// level 3 and only at level 3; below that it is a sweep that happens to damage.
// The unlock lives in one place per mechanic (see the *_unlocked helpers in
// game.c) so the field form and the duel form can never disagree about whether
// the player has it yet.
//
// Cooldowns shorten as the card levels (the level is subtracted in game.c), so
// these are the level-1 values. Radii are level-1 too; both grow with level.
// `duel_cd` is the arena's own pacing: the arena is a fighting game, so its
// abilities come back faster than the field's automatic volleys.
// ---------------------------------------------------------------------------

const CardDef g_card_defs[CARD_COUNT] = {
    // name      kind         cd   pow  r    description                     ultimate                        duel_cd duel form
    { "尖牙",   KIND_PHYS,    34,  10,  26,
      "自动咬击最近的敌人",     "连咬三次，并造成流血",
      52, "突进撕咬：向前扑咬，Lv5 三连段" },
    { "铁尾",   KIND_PHYS,    70,  14,  34,
      "甩尾扫开身边的敌人",     "击退并眩晕被扫中的敌人",
      150, "招架：举尾格挡，挡下近战并反击" },
    { "冲撞",   KIND_PHYS,   120,  18,  22,
      "向敌人冲刺，撞飞一路上的目标", "冲刺距离翻倍并留下震地冲击波",
      96, "突进撞击：撞飞一路上的目标" },
    { "水柱",   KIND_MAGIC,   45,  12,  18,
      "水枪水柱高压水炮，穿透射击", "超宽幅高压水炮全屏轰击",
      60, "蓄力水炮：蓄满穿透全场并消弹" },
    { "灵水",   KIND_MAGIC,   80,  14,  24,
      "周身环绕水球，触碰爆伤敌人", "5颗水球同转且极速凝聚",
      0,  "水球环绕：自动挡下敌方弹幕" },
    { "便便",   KIND_MAGIC,  110,  12,  20,
      "身后布设便便中毒减速陷阱",   "多坨地雷并引发恶臭爆破",
      240, "布设毒沼：限制走位，再按引爆" },
    { "安抚",   KIND_SUPPORT, 240,   0,  64,
      "让附近的敌人失去战意",   "被安抚的敌人转而攻击同伴",
      300, "镇魂歌：延长霸主的预警窗口" },
    { "感化",   KIND_SUPPORT, 300,   0,  90,
      "累积感化值，转化一个敌人", "转化时同时治疗自己",
      360, "魅惑：转化霸主的召唤物反咬" },
    { "厚皮",   KIND_PASSIVE,   0,   0,   0,
      "生命上限提升",           "受击时反弹伤害",
      0,  "被动：受击不硬直" },
    { "快腿",   KIND_PASSIVE,   0,   0,   0,
      "移动速度提升",           "冲刺时短暂免疫伤害",
      0,  "被动：翻滚冷却缩短" },
    { "磁吸",   KIND_PASSIVE,   0,   0,   0,
      "拾取范围提升",           "经验获得量提升",
      0,  "被动：掉落自动吸附" },
    { "利爪",   KIND_PASSIVE,   0,   0,   0,
      "全部伤害提升",           "暴击率大幅提升",
      0,  "被动：命中累积踉跄" },

    // --- fused cards (融合卡) ----------------------------------------------
    // A fused card's parents stop existing when it is made, so its level-5 form
    // has to carry both ideas forward. The descriptions below are what the
    // picker shows when the fusion is offered.
    { "掠食突袭", KIND_PHYS,    0,  22,  30,
      "长距突进绕到背后撕咬",   "突进全程无敌并追加三段",
      84, "长距突进绕后，突进期间无敌" },
    { "荆棘铁壁", KIND_PHYS,    0,  16,  40,
      "招架成功时自动反震",     "反击必中并眩晕",
      140, "招架成功自动反震并眩晕" },
    { "潮汐双炮", KIND_MAGIC,   0,  20,  22,
      "蓄力炮球数加倍并消弹",   "双联穿透水炮",
      54, "蓄力水炮加倍，消弹翻倍" },
    { "恶臭香氛", KIND_MAGIC,   0,  10,  40,
      "毒沼中的霸主预警延长",   "毒沼持续伤害翻倍",
      220, "毒沼削弱霸主：预警延长" },
    { "百兽盟主", KIND_SUPPORT, 0,   0,  70,
      "魅惑物强化，踉跄更快",   "感化三次即成",
      300, "魅惑物强化：威力翻倍" },
    { "疾风吸取", KIND_PASSIVE, 0,   0,   0,
      "翻滚生成吸附波",         "翻滚无敌延长",
      0,  "翻滚吸附掉落并延长无敌" },
    { "沸泉地雷", KIND_MAGIC,   0,  18,  28,
      "水炮命中毒沼引发爆炸",   "陷阱被水弹引爆",
      90, "水炮命中毒沼目标引发爆炸" },
    { "回旋冲角", KIND_PHYS,    0,  20,  26,
      "突进终点自动招架",       "突进终点格挡半秒",
      110, "突进终点自动招架半秒" },
};

// ---------------------------------------------------------------------------
// Fusion recipes
//
// Eight pairs, each covering a different way to play. The pairing rule is
// "two cards that already want the same fight", so the fusion reads as a
// realisation rather than as a random upgrade: 尖牙 and 冲撞 both want to be
// in the boss's face, so together they become a predator's dive.
// ---------------------------------------------------------------------------

const FusionRecipe g_fusion_recipes[FUSION_COUNT] = {
    { CARD_TEETH,   CARD_RAM,     CARD_FUSE_PREDATOR, "掠食突袭", "突进绕后撕咬，机动斩杀" },
    { CARD_TAIL,    CARD_HIDE,    CARD_FUSE_BASTION,  "荆棘铁壁", "招架反震，站桩对攻" },
    { CARD_WATER,   CARD_MUD,     CARD_FUSE_TIDE,     "潮汐双炮", "蓄力水炮，远程消弹" },
    { CARD_DUNG,    CARD_PACIFY,  CARD_FUSE_MIASMA,   "恶臭香氛", "毒沼削弱，控场压制" },
    { CARD_RECRUIT, CARD_FANG,    CARD_FUSE_KING,     "百兽盟主", "魅惑召唤物，围攻踉跄" },
    { CARD_SWIFT,   CARD_MAGNET,  CARD_FUSE_GALE,     "疾风吸取", "翻滚无敌，机动续航" },
    { CARD_WATER,   CARD_DUNG,    CARD_FUSE_GEYSER,   "沸泉地雷", "水炮引爆毒沼，范围爆伤" },
    { CARD_TAIL,    CARD_RAM,     CARD_FUSE_WHIRL,    "回旋冲角", "突进接招架，攻守一体" },
};

// ---------------------------------------------------------------------------
// Regions
//
// Ingredient <-> scenery is the point of each region, so the pairing is spelled
// out here and nowhere else:
//
//   雨林     fruit      tropical rainforest, area 1
//   农场     flour      wheat fields, area 2
//   牧场     milk       pasture, area 3
//   沼泽     sugar      cane marsh, area 4
//   可可火山 chocolate  cacao grove, area 5
//
// Monster def ids (see g_monster_defs in sprites.c):
//   0 rain_frog, 1 rain_wasp, 2 rain_slime
//   3 farm_rat, 4 farm_crow, 5 farm_mudlet
//   6 past_dog, 7 past_ram, 8 past_hornet
//   9 marsh_slime, 10 marsh_viper, 11 marsh_toad
//   12 volc_imp, 13 volc_bat, 14 volc_orb
//
// Boss def ids:
//   0 rain_ape (雨林巨猿), 1 rain_treant (树精长老)
//   2 farm_boar (狂暴野猪), 3 farm_ratking (硕鼠大王)
//   4 past_wolf (夜狼领袖), 5 past_bull (狂暴黑牛)
//   6 marsh_toadlord (泥沼巨蟾), 7 marsh_caiman (凯门鳄王)
//   8 volc_golem (焦糖熔岩巨兽), 9 volc_cakelord (蛋糕魔王)
// ---------------------------------------------------------------------------

// Ingredient targets are the region clock: drops land on a fraction of kills,
// so the target is roughly "how many kills long is this region". The ramp
// below, with per-mob drop rates, puts the five regions at about 5 / 6 / 7.5 /
// 9 / 10.5 minutes of fighting each -- an hour-long campaign once the two boss
// phases, the hub walks and the level-up picks are added in.
const RegionDef g_regions[INGREDIENT_COUNT] = {
    {
        "雨林", "水果", 1, 0, 1,
        { 1, 0, 2 }, 3, 20,
        "雨林的枝叶间挂着熟透的果子，动物们都在这里觅食。",
        "雨林巨猿", "树精长老",
        "林间的暴君", "根须深处的古老怨念",
        "果子……本来也想留给谁的"
    },
    {
        "农场", "面粉", 2, 2, 3,
        { 4, 3, 5 }, 3, 26,
        "农场的麦子熟了，金灿灿的一片，风一吹就起浪。",
        "狂暴野猪", "硕鼠大王",
        "踏平麦浪的莽夫", "麦仓的窃国者",
        "分出去的麦子，会香回来"
    },
    {
        "牧场", "牛奶", 3, 4, 5,
        { 7, 6, 8 }, 3, 32,
        "牧场的草又软又厚，走起来很舒服。",
        "夜狼领袖", "狂暴黑牛",
        "月下的猎群之首", "挣脱缰绳的怒角",
        "原来你们……不是狼啊"
    },
    {
        "沼泽", "糖",   4, 6, 7,
        { 10, 9, 11 }, 3, 38,
        "沼泽里的甘蔗长得比水豚还高，泥巴也很深。",
        "泥沼巨蟾", "凯门鳄王",
        "毒泥里的伏兵", "泥水的暴君",
        "下次还来陪我翻滚吗"
    },
    {
        "可可火山", "巧克力", 5, 8, 9,
        { 13, 12, 14 }, 3, 45,
        "火山脚下的可可树结满了果荚，空气里有股焦香。",
        "焦糖熔岩巨兽", "蛋糕魔王",
        "岩浆凝成的甜味巨影", "生日宴的终审官",
        "生日……是什么味道的？"
    },
};
