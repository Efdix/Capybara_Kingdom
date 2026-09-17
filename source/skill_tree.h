#pragma once

#include <stdint.h>
#include <stdbool.h>

#define SKILL_BRANCH_POWER    0
#define SKILL_BRANCH_TOUGH    1
#define SKILL_BRANCH_AGI      2
#define SKILL_BRANCH_HARVEST  3
#define SKILL_BRANCH_MAX      4

#define SKILL_MAX_LEVEL       3

typedef struct {
    uint8_t power_lv;     // 力量 (0..3): 攻击力+2, 暴击+15%, 技能伤害+30%
    uint8_t tough_lv;     // 坚韧 (0..3): 免伤率25%, 最大生命+2, 每10秒自愈
    uint8_t agi_lv;       // 敏捷 (0..3): 移速+1, 无敌时间+50%, 技能冷却-20%
    uint8_t harvest_lv;   // 丰饶 (0..3): 拾取半径+25, 经验加成+35%, 全图磁吸
} SkillTree;

extern SkillTree g_skill_tree;
extern uint16_t g_boss_tokens;

void skill_tree_init(void);
void skill_tree_draw_top(void);
void skill_tree_draw_bottom(void);
void skill_tree_update(void);
