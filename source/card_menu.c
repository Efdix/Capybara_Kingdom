// The level-up card picker.
//
// This is the screen that makes the game a survivors-like: on every level-up
// the run pauses and three cards are offered, and the player picks one. It is
// also the one moment where the player is *choosing*, so it is the screen that
// has to look the most like a real thing -- hence the drawn cards.
//
// The cards are pictures (tools/gen_cards.py emits a 72x96 image per skill into
// one atlas). This file blits the picture and then draws ONLY the text that has
// to be live: the name, the level pips, and the one-line effect. The generator
// leaves a dark band across the top and the bottom for exactly that, so the
// text can never land on top of the emblem. That division is the point: a card
// with the art baked in and the numbers drawn over it can be retuned by
// re-running the generator, with no C changes at all.
//
// Input is deliberately dual. The cursor and A work, because that is how the
// rest of the game is played; tapping a card also works, because three discrete
// pictures is exactly the case where pointing is faster than scrolling. Both
// paths commit through the same function, so they can never diverge.
//
// PAUSED: the run does not advance while this is open. main.c simply does not
// call game_update() in this state, which is what "the game is paused" means
// here -- see the state machine.

#include <stdio.h>
#include <string.h>

#include "game.h"
#include "assets.h"

// The three offers are laid out across the bottom screen. The bottom screen is
// 256x192 and a card is 72 wide, so three cards plus gaps fill it and then some;
// they are drawn at 2/3 scale (48x64) which leaves room for the detail panel
// underneath and keeps the whole spread readable at arm's length.
#define CARD_SCALE_W   48
#define CARD_SCALE_H   64
#define CARD_GAP        8
#define CARDS_TOTAL_W  (CARD_CHOICES * CARD_SCALE_W + (CARD_CHOICES - 1) * CARD_GAP)
#define CARD_ROW_Y     30
#define CARD_FIRST_X   ((AREA_W - CARDS_TOTAL_W) / 2)

static int g_cursor = 0;
static int g_offered = 0;

// Local PRNG. card_menu.c is called from the level-up path, which runs inside
// the gameplay loop, so it cannot share game.c's generator without making the
// offers depend on how many monsters happened to spawn that frame.
static uint32_t g_card_rng = 0x9E3779B9u;

static uint32_t card_rand(void)
{
    g_card_rng ^= g_card_rng << 13;
    g_card_rng ^= g_card_rng >> 17;
    g_card_rng ^= g_card_rng << 5;
    return g_card_rng;
}

void card_menu_reset(void)
{
    g_cursor = 0;
    g_offered = 0;
}

// Roll three distinct cards that this character can still improve. Cards at
// maximum level are excluded, so an offer is never wasted.
void card_menu_roll(int hero_index)
{
    Hero *h = &g_heroes[hero_index];
    int eligible[CARD_COUNT];
    int n = 0;

    for (int i = 0; i < CARD_COUNT; i++)
        if (h->cards[i] < CARD_MAX_LEVEL)
            eligible[n++] = i;

    g_cursor = 0;
    g_offered = 0;
    memset(h->next_choice, 0xFF, sizeof(h->next_choice));

    if (n == 0)
        return;   // everything mastered; the level-up is just a heal

    // Draw up to CARD_CHOICES distinct entries, removing each pick so the
    // three offers are always different from one another.
    for (int slot = 0; slot < CARD_CHOICES && n > 0; slot++)
    {
        int pick = (int)(card_rand() % (uint32_t)n);
        h->next_choice[slot] = (uint8_t)eligible[pick];
        for (int j = pick; j < n - 1; j++)
            eligible[j] = eligible[j + 1];
        n--;
        g_offered++;
    }
}

static uint8_t kind_color(int kind)
{
    switch (kind)
    {
    case KIND_PHYS:    return PAL_ORANGE_MID;
    case KIND_MAGIC:   return PAL_CYAN_LIGHT;
    case KIND_SUPPORT: return PAL_GREEN_SPARK;
    default:           return PAL_UI_GOLD_LIGHT;
    }
}

static const char *kind_name(int kind)
{
    switch (kind)
    {
    case KIND_PHYS:    return "物理";
    case KIND_MAGIC:   return "魔法";
    case KIND_SUPPORT: return "辅助";
    default:           return "被动";
    }
}

// Screen rectangle of one offer, shared by the draw and the hit test so a tap
// can never land on a different card than the one it is over.
static void card_rect(int slot, int *x, int *y)
{
    *x = CARD_FIRST_X + slot * (CARD_SCALE_W + CARD_GAP);
    *y = CARD_ROW_Y;
}

static int s_drag_card_idx = -1;

static void commit_with_slot(int card_index, int target_slot)
{
    Hero *h = &g_heroes[g_local_hero];
    int card = h->next_choice[card_index];
    if (card < CARD_COUNT)
    {
        hero_take_card(g_local_hero, card);
        if (target_slot >= 0 && target_slot < SKILL_SLOT_COUNT &&
            g_card_defs[card].kind != KIND_PASSIVE)
        {
            hero_assign_slot(g_local_hero, target_slot, (uint8_t)card);
        }
        audio_play_sfx(SND_UI_CONFIRM);
    }
    s_drag_card_idx = -1;
    g_state = ST_PLAY;
}

void card_menu_input(void)
{
    uint16_t down = input_down();

    if (g_offered == 0)
    {
        // Nothing left to learn: any button or a tap resumes.
        if (down & (KEY_A | KEY_B | KEY_START) || input_touch_pressed())
        {
            audio_play_sfx(SND_UI_CONFIRM);
            g_state = ST_PLAY;
        }
        return;
    }

    Hero *h = &g_heroes[g_local_hero];
    int sel_card = h->next_choice[g_cursor];
    bool is_active = (sel_card < CARD_COUNT && g_card_defs[sel_card].kind != KIND_PASSIVE);

    // --- touch: tap card to select, drag to slot, or tap slot to assign ---
    if (input_touch_pressed())
    {
        // 1. Did player tap one of the card offers?
        for (int slot = 0; slot < g_offered; slot++)
        {
            int x, y;
            card_rect(slot, &x, &y);
            if (input_touch_in(x - 2, y - 6, CARD_SCALE_W + 4, CARD_SCALE_H + 16))
            {
                g_cursor = slot;
                s_drag_card_idx = slot;
                audio_play_sfx(SND_UI_MOVE);
                break;
            }
        }

        // 2. Did player tap one of the 4 ABXY slots?
        for (int s = 0; s < 4; s++)
        {
            int sx = 4 + s * 62;
            if (input_touch_in(sx, 150, 58, 38))
            {
                commit_with_slot(g_cursor, s);
                return;
            }
        }
    }
    else if (!input_touch_pressed() && s_drag_card_idx >= 0)
    {
        // Touch released: check if dropped into a slot
        uint16_t tx = input_touch_x();
        uint16_t ty = input_touch_y();
        if (ty >= 145)
        {
            int drop_slot = (tx - 4) / 62;
            if (drop_slot >= 0 && drop_slot < 4)
            {
                commit_with_slot(s_drag_card_idx, drop_slot);
                return;
            }
        }
        s_drag_card_idx = -1;
    }

    // --- buttons ---
    if (down & KEY_LEFT)
    {
        g_cursor = (g_cursor + g_offered - 1) % g_offered;
        audio_play_sfx(SND_UI_MOVE);
    }
    if (down & KEY_RIGHT)
    {
        g_cursor = (g_cursor + 1) % g_offered;
        audio_play_sfx(SND_UI_MOVE);
    }
    if (down & KEY_L)
    {
        g_cursor = 0;
        audio_play_sfx(SND_UI_MOVE);
    }
    if (down & KEY_R)
    {
        g_cursor = g_offered - 1;
        audio_play_sfx(SND_UI_MOVE);
    }

    if (is_active)
    {
        // For active cards, pressing A, B, X, Y slots it into that button directly!
        if (down & KEY_A)
            commit_with_slot(g_cursor, SKILL_SLOT_A);
        else if (down & KEY_B)
            commit_with_slot(g_cursor, SKILL_SLOT_B);
        else if (down & KEY_X)
            commit_with_slot(g_cursor, SKILL_SLOT_X);
        else if (down & KEY_Y)
            commit_with_slot(g_cursor, SKILL_SLOT_Y);
    }
    else
    {
        // Passive cards: A confirms directly
        if (down & KEY_A)
            commit_with_slot(g_cursor, -1);
    }
}

// Level pips, drawn along the footer band the generator left clear. Filled
// squares are levels held, hollow ones the levels still to come -- so the card
// shows both where it is and how far it can go.
static void draw_pips(int x, int y, int owned, int next)
{
    for (int i = 0; i < CARD_MAX_LEVEL; i++)
    {
        int px = x + i * 7;
        if (i < owned)
            sub_fill_rect(px, y + 1, 5, 5, PAL_UI_GOLD);
        else if (i < next)
            sub_fill_rect(px, y + 1, 5, 5, PAL_WHITE_SPARK);
        else
            sub_rect_outline(px, y + 1, 5, 5, PAL_UI_TEXT_DIM);
    }
}

void card_menu_draw(void)
{
    Hero *h = &g_heroes[g_local_hero];

    text_clear();

    // The world keeps drawing behind the picker (main.c draws it and simply
    // stops simulating), so the top screen still shows where you levelled up.
    // The bottom screen is the picker and gets a solid backdrop.
    sub_fill_rect(0, 0, AREA_W, AREA_H, PAL_UI_PANEL);

    text_draw(1, 0, "升级！", PAL_UI_GOLD_LIGHT);
    text_printf(7, 0, PAL_WHITE_SPARK, "Lv%d", h->level);
    text_printf(12, 0, PAL_UI_TEXT_DIM, "选一张卡");
    sub_fill_rect(0, 14, AREA_W, 2, PAL_UI_FRAME);

    if (g_offered == 0)
    {
        text_draw(4, 6, "所有卡都满级了！", PAL_UI_GOLD_LIGHT);
        text_draw(4, 8, "这次升级只回复体力", PAL_UI_TEXT);
        text_draw(6, 11, "按 A 继续", PAL_UI_TEXT_DIM);
        return;
    }

    for (int slot = 0; slot < g_offered; slot++)
    {
        int card = h->next_choice[slot];
        if (card >= CARD_COUNT)
            continue;

        int owned = h->cards[card];
        bool sel = (slot == g_cursor);

        int x, y;
        card_rect(slot, &x, &y);

        // Selection is shown on the frame, not by moving the card: a card that
        // jumps when you move the cursor makes the three pictures hard to
        // compare, and comparing them is the whole decision.
        if (sel)
        {
            sub_fill_rect(x - 3, y - 3, CARD_SCALE_W + 6, CARD_SCALE_H + 6,
                          PAL_UI_GOLD_LIGHT);
            sub_fill_rect(x - 2, y - 2, CARD_SCALE_W + 4, CARD_SCALE_H + 4,
                          PAL_UI_PANEL);
        }

        // The card picture itself.
        sub_blit_card(card, x, y, CARD_SCALE_W, CARD_SCALE_H);

        // Level pips, drawn into the footer band the generator left clear. The
        // card art is fixed, so this is the only thing that changes as a card
        // levels -- which is exactly why the generator reserves that band.
        draw_pips(x + 6, y + CARD_SCALE_H - 12, owned, owned + 1);

        if (sel)
        {
            // A caret over the selected card, so the cursor is unmistakable
            // even in a screenshot.
            for (int i = 0; i < 5; i++)
                sub_fill_rect(x + CARD_SCALE_W / 2 - 4 + i, y - 9 + i, 9 - i * 2, 1,
                              PAL_UI_GOLD_LIGHT);
        }
    }

    // Detail for the highlighted offer: the numbers, and what the level-5 form
    // adds, because level 5 is the reason to keep taking a card.
    int sel_card = h->next_choice[g_cursor];
    if (sel_card < CARD_COUNT)
    {
        const CardDef *d = &g_card_defs[sel_card];
        int owned = h->cards[sel_card];
        int next = owned + 1;

        int panel_y = CARD_ROW_Y + CARD_SCALE_H + 4;
        sub_fill_rect(0, panel_y, AREA_W, AREA_H - panel_y, PAL_UI_PANEL);
        sub_fill_rect(0, panel_y, AREA_W, 2, PAL_UI_FRAME);

        text_printf(1, 7, kind_color(d->kind), "%s", d->name);
        text_printf(6, 7, PAL_UI_TEXT_DIM, "·%s", kind_name(d->kind));
        if (owned > 0)
            text_printf(11, 7, PAL_GREEN_SPARK, "Lv%d→%d", owned, next);
        else
            text_printf(11, 7, PAL_YELLOW_MID, "新卡 Lv1");

        if (d->kind == KIND_PASSIVE)
        {
            text_draw(1, 10, "★ 被动技能: 永久生效, 按A学习", PAL_GREEN_SPARK);
        }
        else
        {
            text_draw(1, 10, "★ 主动技能: 拖入/点槽位或按ABXY绑定", PAL_YELLOW_LIGHT);
        }
    }

    // 4 ABXY Skill Slots at bottom (y: 148..188)
    static const char *s_slot_btn[4] = { "A", "B", "X", "Y" };
    int col_step[4] = { 1, 6, 11, 16 };

    for (int s = 0; s < 4; s++)
    {
        int sx = 4 + s * 62;
        uint8_t cid = h->skill_slots[s];
        bool has_sk = (cid != SKILL_SLOT_NONE && cid < CARD_COUNT);

        sub_rect_outline(sx, 148, 58, 38, PAL_OUTLINE);
        sub_fill_rect(sx + 1, 149, 56, 36, PAL_UI_PANEL);

        text_printf(col_step[s], 11, PAL_UI_GOLD_LIGHT, "[%s键位]", s_slot_btn[s]);
        if (has_sk)
        {
            uint8_t kc = kind_color(g_card_defs[cid].kind);
            text_printf(col_step[s], 12, kc, "%s", g_card_defs[cid].name);
        }
        else
        {
            text_printf(col_step[s], 12, PAL_UI_TEXT_DIM, "空位");
        }
    }
}
