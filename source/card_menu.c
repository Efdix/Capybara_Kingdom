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
#include "ui_draw.h"

// The three offers are laid out across the bottom screen. The bottom screen is
// 256x192 and a card is 72 wide, so three cards plus gaps fill it and then some;
// they are drawn at 2/3 scale (48x64) which leaves room for the detail panel
// underneath and keeps the whole spread readable at arm's length.
#define CARD_SCALE_W   48
#define CARD_SCALE_H   64
#define CARD_GAP        8
#define CARDS_TOTAL_W  (CARD_CHOICES * CARD_SCALE_W + (CARD_CHOICES - 1) * CARD_GAP)
#define CARD_ROW_Y     28
#define CARD_FIRST_X   ((AREA_W - CARDS_TOTAL_W) / 2)

// The fusion offer, laid along the row between the cards and the slot bar. It
// is a plate, not a card: a fusion is a different kind of decision from picking
// a card (it spends two and returns one), so it does not look like one.
#define FUSE_PLATE_X   6
#define FUSE_PLATE_Y  (CARD_ROW_Y + CARD_SCALE_H + 2)
#define FUSE_PLATE_W  (AREA_W - 12)
#define FUSE_PLATE_H  30

static int g_cursor = 0;
static int g_offered = 0;
// The fusion offer, when one is available. It sits past the three rolled cards
// and is committed through its own path, because taking it spends two cards
// rather than adding one.
static int g_fusion = -1;

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
    g_fusion = -1;
}

// Roll three distinct cards that this character can still improve. Cards at
// maximum level are excluded, so an offer is never wasted. Fused cards are
// never offered directly -- they only exist as the product of a fusion, so the
// eligibility test is capped at the base pool.
void card_menu_roll(int hero_index)
{
    Hero *h = &g_heroes[hero_index];
    int eligible[CARD_COUNT];
    int n = 0;

    for (int i = 0; i < CARD_BASE_COUNT; i++)
        if (h->cards[i] < CARD_MAX_LEVEL)
            eligible[n++] = i;

    g_cursor = 0;
    g_offered = 0;
    memset(h->next_choice, 0xFF, sizeof(h->next_choice));

    // The fusion offer, if the hero has two cards that are both unlocked.
    // It is independent of the roll: the player earned it by investing, so it
    // should not also have to be lucky.
    g_fusion = fusion_available(h->cards);

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

// The fusion offer's own row, drawn under the three cards. It is a wide plate
// rather than a card because it is not a fourth option -- it is a different
// kind of decision, and the shape says so before the text does.
static int g_slot_count(void)
{
    return g_offered + (g_fusion >= 0 ? 1 : 0);
}

static bool cursor_on_fusion(void)
{
    return g_fusion >= 0 && g_cursor == g_offered;
}

// Commit the fusion. It spends both parents and leaves the fused card behind,
// so this is the one pick that can make a hand smaller.
static void commit_fusion(void)
{
    hero_fuse_cards(g_local_hero, g_fusion);
    g_fusion = -1;
    s_drag_card_idx = -1;
    g_state = ST_PLAY;
}

void card_menu_input(void)
{
    uint16_t down = input_down();

    if (g_slot_count() == 0)
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
    int sel_card = (g_cursor < g_offered) ? h->next_choice[g_cursor] : -1;
    bool is_active = (sel_card >= 0 && sel_card < CARD_COUNT &&
                      g_card_defs[sel_card].kind != KIND_PASSIVE);
    int n_slots = g_slot_count();

    // --- touch: tap card to select, drag to slot, or tap slot to assign ---
    if (input_touch_pressed())
    {
        // The fusion plate, when it is up.
        if (g_fusion >= 0 &&
            input_touch_in(FUSE_PLATE_X, FUSE_PLATE_Y, FUSE_PLATE_W, FUSE_PLATE_H))
        {
            commit_fusion();
            return;
        }

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
        if (!cursor_on_fusion())
        {
            for (int s = 0; s < 4; s++)
            {
                if (input_touch_in(SLOT_X(s), SLOT_Y, SLOT_W, SLOT_H))
                {
                    commit_with_slot(g_cursor, s);
                    return;
                }
            }
        }
    }
    else if (!input_touch_pressed() && s_drag_card_idx >= 0)
    {
        // Touch released: check if dropped into a slot
        uint16_t tx = input_touch_x();
        uint16_t ty = input_touch_y();
        if (ty >= SLOT_Y - 3)
        {
            int drop_slot = (tx - 4) / 63;
            if (drop_slot >= 0 && drop_slot < 4 && tx < 4 + drop_slot * 63 + 59)
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
        g_cursor = (g_cursor + n_slots - 1) % n_slots;
        audio_play_sfx(SND_UI_MOVE);
    }
    if (down & KEY_RIGHT)
    {
        g_cursor = (g_cursor + 1) % n_slots;
        audio_play_sfx(SND_UI_MOVE);
    }
    if (down & KEY_L)
    {
        g_cursor = 0;
        audio_play_sfx(SND_UI_MOVE);
    }
    if (down & KEY_R)
    {
        g_cursor = n_slots - 1;
        audio_play_sfx(SND_UI_MOVE);
    }

    // The fusion plate takes A on its own: it has no slot to be dragged into,
    // and giving it the ABXY drag would imply it stacks like a card.
    if (cursor_on_fusion())
    {
        if (down & KEY_A)
            commit_fusion();
        return;
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
        int px = x + i * 8;
        if (i < owned)
            ui_round(UI_SUB, px, y, 6, 6, 2, PAL_UI2_BLUE);
        else if (i < next)
            ui_round(UI_SUB, px, y, 6, 6, 2, PAL_UI2_BLUE_GLOW);
        else
        {
            // Hollow: ring first, then the plate colour inside, so "levels
            // remaining" reads as absence rather than as a third colour.
            ui_round(UI_SUB, px, y, 6, 6, 2, PAL_UI2_INK_DIM);
            ui_round(UI_SUB, px + 1, y + 1, 4, 4, 1, PAL_UI2_PLATE);
        }
    }
}

void card_menu_draw(void)
{
    Hero *h = &g_heroes[g_local_hero];

    ui_field(UI_SUB);

    // Header: the level-up banner. The level jump is the news here, so it gets
    // the accent tab and the rest of the strip stays quiet. The first pick in
    // a save swaps the strip's quiet prompt for the two facts a newcomer needs
    // -- the choice applies now, and passing on a card is not losing it.
    bool first_pick = !tutorial_done(TUT_B_LEVEL);
    if (first_pick)
        tutorial_mark(TUT_B_LEVEL);
    sub_fill_rect(0, 0, AREA_W, 30, PAL_UI2_LINE);
    sub_fill_rect(0, 0, AREA_W, 28, PAL_UI2_SCREEN);
    sub_fill_rect(0, 0, AREA_W, 1, PAL_UI2_BLUE_DARK);
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "升级！  Lv%d", h->level);
        ui_text_mid(UI_SUB, 12, 14, buf, PAL_WHITE);
        ui_text_right(UI_SUB, 248, 14,
                      first_pick ? "选一张 · 立刻生效" : "选一张卡",
                      first_pick ? PAL_UI_GOLD_LIGHT : PAL_UI2_BLUE_HI);
    }

    if (g_slot_count() == 0)
    {
        // Everything mastered: the level-up is just a heal, so say so and get
        // out of the way.
        ui_plate(UI_SUB, 28, 66, 200, 60, UI_R_PLATE);
        ui_label(UI_SUB, 28, 78, 200, 20, "所有卡都满级了！", PAL_UI2_INK);
        ui_label(UI_SUB, 28, 100, 200, 20, "这次升级只回复体力",
                 PAL_UI2_INK_DIM);
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
            ui_round(UI_SUB, x - 4, y - 4, CARD_SCALE_W + 8, CARD_SCALE_H + 8,
                     UI_R_PLATE, PAL_UI2_LINE);
            ui_round(UI_SUB, x - 3, y - 3, CARD_SCALE_W + 6, CARD_SCALE_H + 6,
                     UI_R_PLATE, PAL_UI2_BLUE);
        }

        // The card picture itself.
        sub_blit_card(card, x, y, CARD_SCALE_W, CARD_SCALE_H);

        // Level pips, drawn into the footer band the generator left clear. The
        // card art is fixed, so this is the only thing that changes as a card
        // levels -- which is exactly why the generator reserves that band.
        draw_pips(x + 5, y + CARD_SCALE_H - 10, owned, owned + 1);

        // A card that has reached the unlock level wears a small gold mark, so
        // "this one can fuse" and "this one has its mechanic now" are both
        // visible without opening anything.
        if (owned >= CARD_UNLOCK_LEVEL)
        {
            int mx = x + CARD_SCALE_W - 9;
            int my = y + 3;
            ui_round(UI_SUB, mx, my, 7, 7, 2, PAL_UI2_AMBER);
            ui_round(UI_SUB, mx + 2, my + 2, 3, 3, 1, PAL_WHITE);
        }
    }

    // --- the fusion offer --------------------------------------------------
    // Its two parent cards are named, so the player can see exactly what it
    // costs before pressing A. That is the whole reason it is a plate rather
    // than a fourth card: a card slot would hide the price.
    //
    // It now carries a thumbnail of the result as well. The eight fused cards
    // had no art until recently, so this offer was the one card in the game the
    // player could take without ever seeing it; the thumbnail is what makes the
    // decision "spend two cards on that picture" instead of "spend two cards on
    // a sentence".
    if (g_fusion >= 0)
    {
        const FusionRecipe *fr = &g_fusion_recipes[g_fusion];
        bool sel = cursor_on_fusion();
        bool blink = ((g_frame >> 3) & 1);

        ui_plate(UI_SUB, FUSE_PLATE_X, FUSE_PLATE_Y, FUSE_PLATE_W, FUSE_PLATE_H,
                 UI_R_PLATE);
        if (sel)
        {
            uint8_t edge = blink ? PAL_UI2_AMBER : PAL_UI_GOLD;
            ui_round(UI_SUB, FUSE_PLATE_X, FUSE_PLATE_Y, FUSE_PLATE_W,
                     FUSE_PLATE_H, UI_R_PLATE, edge);
        }

        // The result's own face, at the right end of the plate. 20x26 keeps the
        // 3:4 of the source art and fits the plate's inner height exactly.
        {
            int tw = 20, th = 26;
            int tx = FUSE_PLATE_X + FUSE_PLATE_W - 6 - tw;
            int ty = FUSE_PLATE_Y + (FUSE_PLATE_H - th) / 2;
            sub_blit_card(fr->result, tx, ty, tw, th);
            ui_round(UI_SUB, tx - 1, ty - 1, tw + 2, th + 2, 2, PAL_UI_GOLD);
        }

        char buf[64];
        snprintf(buf, sizeof(buf), "★ 合成 %s", fr->name);
        ui_text(UI_SUB, FUSE_PLATE_X + 8, FUSE_PLATE_Y + 6,
                buf, sel ? PAL_UI2_AMBER : PAL_UI2_PLATE_HI);

        snprintf(buf, sizeof(buf), "%s + %s → Lv%d",
                 g_card_defs[fr->a].name, g_card_defs[fr->b].name,
                 h->cards[fr->a] > h->cards[fr->b] ? h->cards[fr->a]
                                                   : h->cards[fr->b]);
        ui_text_right(UI_SUB, FUSE_PLATE_X + FUSE_PLATE_W - 32,
                      FUSE_PLATE_Y + 7, buf, PAL_UI2_INK_DIM);
        ui_text(UI_SUB, FUSE_PLATE_X + 8, FUSE_PLATE_Y + 19, fr->desc,
                PAL_UI2_INK);
    }

    // Detail for the highlighted offer: the numbers, and what the level-5 form
    // adds, because level 5 is the reason to keep taking a card.
    int sel_card = (g_cursor < g_offered) ? h->next_choice[g_cursor] : -1;
    if (sel_card >= 0 && sel_card < CARD_COUNT)
    {
        const CardDef *d = &g_card_defs[sel_card];
        int owned = h->cards[sel_card];

        int panel_y = CARD_ROW_Y + CARD_SCALE_H + 4;
        int panel_h = SLOT_Y - 5 - panel_y;
        ui_plate(UI_SUB, 4, panel_y, 248, panel_h, UI_R_PLATE);

        char buf[64];
        snprintf(buf, sizeof(buf), "%s · %s", d->name, kind_name(d->kind));
        ui_text(UI_SUB, 14, panel_y + 6, buf, kind_color(d->kind));

        if (owned > 0)
            snprintf(buf, sizeof(buf), "Lv%d → %d", owned, owned + 1);
        else
            snprintf(buf, sizeof(buf), "新卡 Lv1");
        ui_text_right(UI_SUB, 242, panel_y + 6, buf,
                      owned > 0 ? PAL_UI2_GREEN : PAL_UI2_AMBER);

        // The unlock line is the one the player needs at the picker: it says
        // what this card becomes at level 3, which is where it turns into an
        // answer to something the bosses do.
        if (d->kind == KIND_PASSIVE)
            ui_text(UI_SUB, 14, panel_y + 25,
                    "★ 被动技能：永久生效，按 A 学习", PAL_UI2_INK);
        else
            ui_text(UI_SUB, 14, panel_y + 25,
                    "★ 主动技能：拖入槽位，或按 ABXY", PAL_UI2_INK);
    }

    // 4 ABXY Skill Slots at bottom (y: 148..188). Same treatment as the in-play
    // skill bar, so a slot looks like a slot wherever the player meets one.
    static const char *s_slot_btn[4] = { "A", "B", "X", "Y" };

    for (int s = 0; s < 4; s++)
    {
        int sx = SLOT_X(s);
        uint8_t cid = h->skill_slots[s];
        bool has_sk = (cid != SKILL_SLOT_NONE && cid < CARD_COUNT);

        ui_btn(UI_SUB, sx, SLOT_Y, SLOT_W, SLOT_H,
               has_sk ? UI_ON : UI_OFF);
        ui_keycap(UI_SUB, sx + 4, SLOT_Y + 4, 15, 13, s_slot_btn[s], has_sk);

        if (has_sk)
            ui_text(UI_SUB, sx + 22, SLOT_Y + 10, g_card_defs[cid].name,
                    kind_color(g_card_defs[cid].kind));
        else
            ui_text(UI_SUB, sx + 22, SLOT_Y + 14, "空", PAL_UI2_INK_DIM);
    }
}
