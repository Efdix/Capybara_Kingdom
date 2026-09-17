// Global state + the two data tables that define the game: the card pool and
// the five regions.

#include <string.h>

#include "game.h"
#include "assets.h"

GameState g_state = ST_BOOT;
Hero g_heroes[2];
Monster g_monsters[MAX_MONSTERS];
Projectile g_projectiles[MAX_PROJECTILES];
Gem g_gems[MAX_GEMS];
Effect g_effects[MAX_EFFECTS];

uint8_t  g_region_id = 0;
uint8_t  g_ingredient_have = 0;
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

// ---------------------------------------------------------------------------
// The card pool
//
// Twelve cards: three physical, three magic, two support, four passive. Each
// goes to level 5, where it becomes something qualitatively different rather
// than merely bigger -- that is what makes the fifth level worth chasing.
//
// Cooldowns shorten as the card levels (the level is subtracted in game.c), so
// these are the level-1 values. Radii are level-1 too; both grow with level.
// ---------------------------------------------------------------------------

const CardDef g_card_defs[CARD_COUNT] = {
    // name      kind         cd   pow  r    description                 ultimate
    { "尖牙",   KIND_PHYS,    34,  10,  26,
      "自动咬击最近的敌人",     "连咬三次，并造成流血" },
    { "铁尾",   KIND_PHYS,    70,  14,  34,
      "甩尾扫开身边的敌人",     "击退并眩晕被扫中的敌人" },
    { "冲撞",   KIND_PHYS,   120,  18,  22,
      "向敌人冲刺，撞飞一路上的目标", "冲刺距离翻倍并留下震地冲击波" },
    { "水柱",   KIND_MAGIC,   45,  12,  18,
      "水枪水柱高压水炮，穿透射击", "超宽幅高压水炮全屏轰击" },
    { "灵水",   KIND_MAGIC,   80,  14,  24,
      "周身环绕水球，触碰爆伤敌人", "5颗水球同转且极速凝聚" },
    { "便便",   KIND_MAGIC,  110,  12,  20,
      "身后布设便便中毒减速陷阱",   "多坨地雷并引发恶臭爆破" },
    { "安抚",   KIND_SUPPORT, 240,   0,  64,
      "让附近的敌人失去战意",   "被安抚的敌人转而攻击同伴" },
    { "感化",   KIND_SUPPORT, 300,   0,  90,
      "累积感化值，转化一个敌人", "转化时同时治疗自己" },
    { "厚皮",   KIND_PASSIVE,   0,   0,   0,
      "生命上限提升",           "受击时反弹伤害" },
    { "快腿",   KIND_PASSIVE,   0,   0,   0,
      "移动速度提升",           "冲刺时短暂免疫伤害" },
    { "磁吸",   KIND_PASSIVE,   0,   0,   0,
      "拾取范围提升",           "经验获得量提升" },
    { "利爪",   KIND_PASSIVE,   0,   0,   0,
      "全部伤害提升",           "暴击率大幅提升" },
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

const RegionDef g_regions[INGREDIENT_COUNT] = {
    {
        "雨林", "水果", 1, 0, 1,
        { 1, 0, 2 }, 3, 14,
        "雨林的枝叶间挂着熟透的果子，动物们都在这里觅食。",
        "雨林巨猿",
        "树精长老"
    },
    {
        "农场", "面粉", 2, 2, 3,
        { 4, 3, 5 }, 3, 18,
        "农场的麦子熟了，金灿灿的一片，风一吹就起浪。",
        "狂暴野猪",
        "硕鼠大王"
    },
    {
        "牧场", "牛奶", 3, 4, 5,
        { 7, 6, 8 }, 3, 22,
        "牧场的草又软又厚，走起来很舒服。",
        "夜狼领袖",
        "狂暴黑牛"
    },
    {
        "沼泽", "糖",   4, 6, 7,
        { 10, 9, 11 }, 3, 26,
        "沼泽里的甘蔗长得比水豚还高，泥巴也很深。",
        "泥沼巨蟾",
        "凯门鳄王"
    },
    {
        "可可火山", "巧克力", 5, 8, 9,
        { 13, 12, 14 }, 3, 30,
        "火山脚下的可可树结满了果荚，空气里有股焦香。",
        "焦糖熔岩巨兽",
        "蛋糕魔王"
    },
};
