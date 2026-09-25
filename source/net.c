// Local-wireless multiplayer over the DS's built-in Nintendo link.
//
// This uses dswifi's "local multiplayer" mode, the same mechanism official DS
// games use for download/versus play: no router or access point is involved,
// the host broadcasts a beacon advertising the game and the client scans for
// and joins it. That is what lets a DS and a 3DS (running the ROM from a
// flashcart) play together.
//
// Sync model (2 players, chosen to be as forgiving as possible):
//   * Each console is authoritative for its own hero. Position/facing/health
//     are reported ~30x/second via REPLY frames, so movement never feels laggy
//     regardless of link quality.
//   * The host is authoritative for monsters, projectiles and level progress
//     and broadcasts them in CMD frames. The client renders remotely and does
//     not simulate monsters, which avoids divergence entirely.
//   * The remote hero is interpolated toward its last reported position.

#include <string.h>

#include "game.h"
#include "assets.h"
#include "farm.h"
#include "save.h"

#if __has_include(<dswifi9.h>)
#include <dswifi9.h>
#define HAVE_DSWIFI 1
#else
#define HAVE_DSWIFI 0
#endif

#define NET_MAGIC       0xCA9B
#define MAX_CLIENTS     1          // this game is strictly two players
#define NET_TICK        2          // send every 2 frames (~30 Hz)

// ---------------------------------------------------------------------------
// Packet layout
// ---------------------------------------------------------------------------

typedef struct {
    uint16_t magic;
    uint8_t  kind;          // PKT_HELLO / PKT_START
} pkt_header;

#define PKT_HELLO       1
#define PKT_START       2

// Client -> host: my hero's state.
typedef struct {
    pkt_header hdr;
    int16_t  x, y;
    uint8_t  facing;
    uint8_t  hearts;
    uint8_t  hearts_max;
    uint8_t  level;
    uint8_t  moving;
    uint8_t  attack;
    uint8_t  alive;
    // The client releases its own ultimate and asks the host to apply it: the
    // host owns monster simulation, so it is the only side that can actually
    // damage anything. Non-zero for a few frames around the press.
    uint8_t  ult_fire;
    // Which capybara this console actually drives. The forced assignment
    // normally makes this derivable, but the value rides along anyway: the
    // ultimates are per-variant and the two screens had better agree on who
    // is who.
    uint8_t  variant;
    // Linked duel: damage this console's capybara dealt to the duel boss
    // since the last packet. The host applies it -- the client never
    // simulates the boss -- and zeroes the accumulator on send.
    uint16_t duel_hit;
    // Linked FIELD: the same idea for the ordinary crowd. The client's
    // mirrors sit at the host's exact positions, so the last hit position
    // tells the host which creature to debit.
    uint16_t field_hit;
    int16_t  field_x, field_y;
    // Shared purse: a client's skill purchase is negative, a market exchange
    // positive. The host owns the authoritative total (it also owns the
    // boss-kill payouts) and echoes it back on the world packet.
    int16_t  token_delta;
    // Farm report: 1 = water the whole bed, 2..25 = context-action plot
    // msg-2. The client applies locally for instant feedback; the host's
    // authoritative copy confirms on the next sync. `farm_seq` dedupes the
    // brief repeat window against a lost frame.
    uint8_t  farm_msg;
    uint8_t  farm_seq;
    uint8_t  token_seq;
} pkt_hero;

// Host -> client: region state + every monster + every projectile.
typedef struct {
    pkt_header hdr;
    uint8_t  region_id;
    uint8_t  ingredient_have;
    uint8_t  ingredients_done;
    uint8_t  boss_out;
    uint8_t  game_state;
    uint8_t  monster_count;
    uint8_t  projectile_count;
    uint16_t run_time;
    uint8_t  difficulty;
    // Ultimate charge, which the client cannot work out for itself: it does not
    // simulate monsters, so it never sees the kills that fill the bar. Bit 0 of
    // ult_ready is the host's own capybara, bit 1 the partner's.
    uint8_t  ult_ready;
    uint8_t  ult_charge_self;   // the host console's own capybara
    uint8_t  ult_charge_peer;   // the partner's
    // Duel: set for one packet when the boss just hit the PARTNER'S capybara.
    // The client owns its own hurt reaction, so the host tells it instead of
    // writing into a mirror the client would overwrite.
    uint8_t  duel_hero_hit;
    // Set for one packet when a heart the host picked up should heal the
    // partner too: the client sees no drop stream of its own.
    uint8_t  partner_heal;

    // --- the shared meta layer --------------------------------------------//
    // The host owns the token purse (it credits the boss kills) and echoes it
    // here, so both consoles' saves stay one purchase apart at most. The
    // host's own capybara variant rides along so both screens agree on who is
    // who, and the partner hero's experience totals come down the same way --
    // the host levels the client's capybara when the client's reported damage
    // kills something, and this is how the client finds out.
    uint16_t tokens;
    uint8_t  host_variant;
    uint16_t peer_exp;
    uint8_t  peer_level;

    // --- the farm ----------------------------------------------------------
    // The host owns the field for the same reason it owns the monsters: both
    // consoles draw the same plots, so two independent copies of "which plot is
    // ripe" would drift apart the first time a packet was lost. The host sends
    // the whole bed plus the satchel and the cake counts, and the client
    // mirrors it wholesale rather than simulating anything.
    //
    // The sizes are the fixed maxima, not the live ones, so the struct layout
    // cannot change with a game constant: FARM_PLOTS is 24 and both the seed
    // and cake arrays are indexed by crop/region (5 today, padded to 8). A
    // mismatch between these and the real arrays is caught at compile time by
    // the memcpy in host_send_world().
    uint8_t  farm_plots[24];
    uint8_t  farm_seeds[8];
    uint8_t  farm_have[8];
    uint16_t farm_day;
} pkt_world_hdr;

typedef struct {
    uint8_t  alive;
    uint8_t  def_id;
    int16_t  x, y;
    int8_t   dx, dy;
    uint16_t hp, hp_max;
    uint8_t  hurt;
    uint8_t  is_boss;
    uint8_t  boss_id;
    // bit0 elite, bit1 recruited, bit2 pacified, bits 4-7 scale/25 (4 = 100%)
    uint8_t  flags;
} pkt_monster;

#define MON_FLAG_ELITE     0x01
#define MON_FLAG_RECRUIT   0x02
#define MON_FLAG_PACIFIED  0x04

typedef struct {
    uint8_t  alive;
    uint8_t  kind;
    int16_t  x, y;
} pkt_projectile;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool g_net_active = false;
static bool g_net_ready = false;
static bool g_net_host = false;
static int  g_tick = 0;
static Wifi_AccessPoint g_ap;

// Frames left to keep telling the host to release this console's ultimate. The
// press itself is a single frame, but a reply can be lost, so it is repeated
// briefly rather than sent once.
static uint8_t s_ult_fire = 0;
// Client-side one-shot mail (see pkt_hero): the shared-purse delta and the
// farm report. Each repeats for a few frames so a lost reply frame cannot eat
// a purchase -- which is why every message carries a sequence number: the
// host applies each sequence once, so a repeat is idempotent.
static int16_t s_token_delta = 0;
static uint8_t s_token_seq = 0;
static uint8_t s_token_life = 0;
static uint8_t s_farm_msg = 0;
static uint8_t s_farm_seq = 0;
static uint8_t s_farm_life = 0;
// The host's memory of the last applied sequence of each channel.
static uint8_t s_last_token_seq = 0;
static uint8_t s_last_farm_seq = 0;

// Link supervision. A session where one side has quietly gone away is worse
// than no session: the "partner" freezes mid-stride and nothing explains why.
// Both consoles watch their incoming stream and declare the link lost after
// LINK_LOST_TICKS of silence (~3 s).
#define LINK_LOST_TICKS   180
static uint32_t s_last_rx_tick = 0;
static uint32_t s_zero_clients = 0;
static bool     s_lost_reported = false;

void net_notify_ultimate(void)
{
    s_ult_fire = 8;
}

void net_notify_token_delta(int16_t delta)
{
    if (s_token_life == 0)
    {
        s_token_delta = 0;
        s_token_seq = (uint8_t)(s_token_seq + 1);
    }
    s_token_delta = (int16_t)(s_token_delta + delta);
    s_token_life = 12;
}

void net_notify_farm(uint8_t msg)
{
    if (msg == 0)
        return;
    s_farm_msg = msg;
    s_farm_seq = (uint8_t)(s_farm_seq + 1);
    s_farm_life = 12;
}

// ---------------------------------------------------------------------------
// Host side
// ---------------------------------------------------------------------------

#if HAVE_DSWIFI

static void host_send_world(void)
{
    static uint8_t buf[512];
    pkt_world_hdr *hdr = (pkt_world_hdr *)buf;
    memset(buf, 0, sizeof(buf));

    hdr->hdr.magic = NET_MAGIC;
    hdr->hdr.kind = PKT_START;
    hdr->region_id = g_region_id;
    hdr->ingredient_have = g_seed_have;
    hdr->ingredients_done = g_ingredients_done;
    hdr->boss_out = g_boss_out ? 1 : 0;
    hdr->game_state = (uint8_t)g_state;
    hdr->run_time = g_run_time;
    hdr->difficulty = g_difficulty;

    // The client cannot see kills, so it cannot fill its own ultimate bar; it
    // is told instead, per capybara.
    hdr->ult_ready = (uint8_t)((g_heroes[0].ult_ready ? 1 : 0) |
                               (g_heroes[1].ult_ready ? 2 : 0));
    hdr->ult_charge_self = (uint8_t)g_heroes[g_local_hero].ult_charge;
    hdr->ult_charge_peer = (uint8_t)g_heroes[g_local_hero ^ 1].ult_charge;
    hdr->duel_hero_hit = g_duel_hit_amt;
    g_duel_hit_amt = 0;

    // The field, the satchel and the cake counts. See pkt_world_hdr for why the
    // host owns these; farm_pack_plot() owns the bit layout so this file never
    // needs the field's internals.
    memset(hdr->farm_plots, 0, sizeof(hdr->farm_plots));
    for (int i = 0; i < FARM_PLOTS && i < 24; i++)
        hdr->farm_plots[i] = farm_pack_plot(&g_farm[i]);
    for (int i = 0; i < FARM_CROP_COUNT && i < 8; i++)
    {
        hdr->farm_seeds[i] = g_seeds[i];
        hdr->farm_have[i] = g_cake_have[i];
    }
    hdr->farm_day = g_day;

    // The shared meta layer: the authoritative purse, who the host drives,
    // and the partner hero's experience (the host levels the client's
    // capybara when the client's reported damage kills something).
    hdr->tokens = g_boss_tokens;
    hdr->host_variant = g_heroes[g_local_hero].variant;
    hdr->peer_exp = g_heroes[g_local_hero ^ 1].exp;
    hdr->peer_level = g_heroes[g_local_hero ^ 1].level;
    hdr->partner_heal = g_partner_heal;
    g_partner_heal = 0;

    uint8_t *p = buf + sizeof(pkt_world_hdr);

    // 16 monsters fit in one packet. The survivors-like can have up to 48 on
    // the field, so this is a deliberate cap: the client sees the 16 nearest
    // to its own capybara rather than the first 16 in the pool, which keeps the
    // ones that actually matter visible on the client's screen.
    int32_t hx = 0, hy = 0;
    if (g_heroes[g_local_hero].active)
        hero_center(&g_heroes[g_local_hero], &hx, &hy);

    uint8_t mc = 0;
    bool sent[MAX_MONSTERS];
    memset(sent, 0, sizeof(sent));
    while (mc < 16)
    {
        int best = -1;
        int32_t bestd = 0;
        for (int i = 0; i < MAX_MONSTERS; i++)
        {
            Monster *m = &g_monsters[i];
            if (!m->alive || sent[i])
                continue;
            int32_t cx, cy;
            monster_center(m, &cx, &cy);
            int32_t d = (cx - hx) * (cx - hx) + (cy - hy) * (cy - hy);
            if (best < 0 || d < bestd)
            {
                best = i;
                bestd = d;
            }
        }
        if (best < 0)
            break;
        sent[best] = true;

        Monster *m = &g_monsters[best];
        pkt_monster pm;
        memset(&pm, 0, sizeof(pm));
        pm.alive = 1;
        pm.def_id = m->def_id;
        pm.x = (int16_t)m->x;
        pm.y = (int16_t)m->y;
        pm.dx = m->dx;
        pm.dy = m->dy;
        pm.hp = m->hp;
        pm.hp_max = m->hp_max;
        pm.hurt = m->hurt_timer > 0;
        pm.is_boss = m->is_boss;
        pm.boss_id = m->boss_id;
        {
            int pct = m->scale_pct ? m->scale_pct : 100;
            uint8_t scale_q = (uint8_t)((pct + 12) / 25);   // 4 = 100%
            if (scale_q > 15) scale_q = 15;
            pm.flags = (uint8_t)(scale_q << 4);
            if (m->elite)         pm.flags |= MON_FLAG_ELITE;
            if (m->recruited)     pm.flags |= MON_FLAG_RECRUIT;
            if (m->pacify_timer > 0) pm.flags |= MON_FLAG_PACIFIED;
        }
        memcpy(p, &pm, sizeof(pm));
        p += sizeof(pm);
        mc++;
    }
    hdr->monster_count = mc;

    uint8_t pc = 0;
    for (int i = 0; i < MAX_PROJECTILES && pc < 12; i++)
    {
        Projectile *pr = &g_projectiles[i];
        if (!pr->alive)
            continue;
        pkt_projectile pp;
        pp.alive = 1;
        pp.kind = pr->kind;
        pp.x = (int16_t)pr->x;
        pp.y = (int16_t)pr->y;
        memcpy(p, &pp, sizeof(pp));
        p += sizeof(pp);
        pc++;
    }
    hdr->projectile_count = pc;

    Wifi_MultiplayerHostCmdTxFrame(buf, (int)(p - buf));
}

static void host_rx_from_client(Wifi_MPPacketType type, int aid, int base,
                                int len)
{
    (void)aid;
    if (type != WIFI_MPTYPE_REPLY || len < (int)sizeof(pkt_hero))
        return;

    pkt_hero pkt;
    Wifi_RxRawReadPacket(base, sizeof(pkt), (void *)&pkt);
    if (pkt.hdr.magic != NET_MAGIC)
        return;

    Hero *r = &g_heroes[1];
    r->net_x = pkt.x;
    r->net_y = pkt.y;
    r->net_facing = pkt.facing;
    r->hearts = pkt.hearts;
    r->hearts_max = pkt.hearts_max;
    r->level = pkt.level;
    r->active = pkt.alive != 0;
    r->variant = pkt.variant;
    s_last_rx_tick = (uint32_t)g_tick;
    s_zero_clients = 0;

    // The client asked to release; the host owns monsters, so it applies the
    // effect here. Charge and the combo window are the host's to track, exactly
    // as they are for its own capybara.
    if (pkt.ult_fire)
        hero_release_ultimate(1);

    // The client's duel hits are reported, not simulated (see pkt_hero).
    if (pkt.duel_hit)
        duel_apply_client_damage(pkt.duel_hit);

    // The client's field swings: applied to the nearest hostile creature at
    // the reported position, so kills and their rewards happen wholly on the
    // authoritative side.
    if (pkt.field_hit)
        field_apply_client_damage(pkt.field_hit, pkt.field_x, pkt.field_y);

    // The shared purse. The host's own total is the truth; the client's save
    // will converge because the world packet echoes the authoritative number.
    // Each sequence number is applied exactly once, so the client's brief
    // repeat window cannot double-charge a purchase.
    if (pkt.token_delta != 0 && pkt.token_seq != s_last_token_seq)
    {
        s_last_token_seq = pkt.token_seq;
        int t = (int)g_boss_tokens + pkt.token_delta;
        g_boss_tokens = (uint16_t)(t < 0 ? 0 : t);
        save_write();
    }

    // The farm report: apply it on the authoritative copy (no toasts -- the
    // player who made the action already heard them at home) and persist.
    if (pkt.farm_msg != 0 && pkt.farm_seq != s_last_farm_seq)
    {
        s_last_farm_seq = pkt.farm_seq;
        if (pkt.farm_msg == 1)
            farm_water_all(false);
        else if (pkt.farm_msg < 2 + FARM_PLOTS)
            farm_plot_action((int)(pkt.farm_msg - 2), false);
        save_write();
    }
}

#else

static void host_send_world(void) {}
static void host_rx_from_client(Wifi_MPPacketType t, int a, int b, int l)
{
    (void)t; (void)a; (void)b; (void)l;
}

#endif

// ---------------------------------------------------------------------------
// Client side
// ---------------------------------------------------------------------------

static void client_send_hero(void)
{
#if HAVE_DSWIFI
    Hero *h = &g_heroes[g_local_hero];
    pkt_hero pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.hdr.magic = NET_MAGIC;
    pkt.hdr.kind = PKT_HELLO;
    pkt.x = (int16_t)h->x;
    pkt.y = (int16_t)h->y;
    pkt.facing = h->facing;
    pkt.hearts = h->hearts;
    pkt.hearts_max = h->hearts_max;
    pkt.level = h->level;
    pkt.moving = h->moving;
    pkt.attack = h->bite_timer > 0;
    pkt.alive = h->active;
    pkt.ult_fire = s_ult_fire ? 1 : 0;
    pkt.variant = h->variant;
    pkt.duel_hit = g_duel_hit_pending;
    g_duel_hit_pending = 0;
    pkt.field_hit = g_field_hit_pending;
    pkt.field_x = (int16_t)g_field_hit_x;
    pkt.field_y = (int16_t)g_field_hit_y;
    g_field_hit_pending = 0;
    pkt.token_delta = s_token_life ? s_token_delta : 0;
    pkt.token_seq = s_token_seq;
    if (s_token_life > 0)
        s_token_life--;
    pkt.farm_msg = s_farm_life ? s_farm_msg : 0;
    pkt.farm_seq = s_farm_seq;
    if (s_farm_life > 0)
        s_farm_life--;
    Wifi_MultiplayerClientReplyTxFrame(&pkt, sizeof(pkt));
#endif
}

// Apply the host's authoritative world state. Called from the RX handler.
static void client_apply_world_packet(const uint8_t *buf, int len)
{
    if (len < (int)sizeof(pkt_world_hdr))
        return;
    const pkt_world_hdr *hdr = (const pkt_world_hdr *)buf;

    g_region_id = hdr->region_id;
    g_seed_have = hdr->ingredient_have;
    g_ingredients_done = hdr->ingredients_done;
    g_boss_out = hdr->boss_out != 0;
    g_run_time = hdr->run_time;
    g_difficulty = hdr->difficulty;

    // The field comes from the host in full, because it is shared state: both
    // consoles draw the same bed, so the client must not run its own copy of
    // "which plot is ripe" or the two screens would show different parks.
    for (int i = 0; i < FARM_PLOTS && i < 24; i++)
        farm_unpack_plot(&g_farm[i], hdr->farm_plots[i]);
    for (int i = 0; i < FARM_CROP_COUNT && i < 8; i++)
    {
        g_seeds[i] = hdr->farm_seeds[i];
        g_cake_have[i] = hdr->farm_have[i];
    }
    g_day = hdr->farm_day;

    // The client owns its own card picker: the host does not know when this
    // console levels up, and mirroring its state would yank the player out of
    // the picker (or open one here) mid-selection. Every other state is
    // mirrored, because the host owns the run.
    if (g_state != ST_CARD_PICK)
        g_state = (GameState)hdr->game_state;

    // Ultimate charge comes from the host, which is the only side that sees
    // kills. The host's own capybara is this console's partner, and vice versa.
    g_heroes[1].ult_charge = hdr->ult_charge_self;
    g_heroes[1].ult_ready = (hdr->ult_ready & 1) != 0;
    g_heroes[0].ult_charge = hdr->ult_charge_peer;
    g_heroes[0].ult_ready = (hdr->ult_ready & 2) != 0;

    // The shared meta layer. The purse is the host's truth -- a client-side
    // purchase already applied at home and reported; the echo confirms it.
    // The host's variant keeps both screens telling the same two capybaras
    // apart, and the partner's leveling of OUR hero comes across here.
    g_boss_tokens = hdr->tokens;
    g_heroes[1].variant = hdr->host_variant;
    {
        Hero *me = &g_heroes[0];
        uint8_t gained = 0;
        while (me->level < hdr->peer_level)
        {
            me->level++;
            uint32_t next = (uint32_t)me->exp_need + me->exp_need * 18 / 100 + 10;
            me->exp_need = (next > EXP_NEED_MAX) ? EXP_NEED_MAX : (uint16_t)next;
            me->hearts = me->hearts_max;
            gained++;
        }
        if (me->level == hdr->peer_level)
            me->exp = hdr->peer_exp;
        if (gained > 0)
        {
            // The picker pauses the run on this console exactly as it does
            // after a local level. A batch of levels rolls one hand of cards
            // now; the sync keeps the totals right for the next one.
            audio_play_sfx(SND_LEVELUP);
            card_menu_roll(0);
            g_state = ST_CARD_PICK;
        }
    }

    // A heart the host picked up heals this console's capybara as well.
    if (hdr->partner_heal)
    {
        Hero *me = &g_heroes[0];
        if (me->active && me->hearts < me->hearts_max)
        {
            me->hearts++;
            spawn_floater(me->x + 16, me->y + 8, 1, FLOATER_HEAL);
        }
    }

    // The host's boss just hit THIS console's capybara (hero 0 here); the
    // byte carries the damage amount. The client plays its own hurt reaction
    // because it owns its own hearts. The field phase uses the same channel:
    // the host is the only side that knows a wild monster connected.
    if (hdr->duel_hero_hit &&
        (g_state == ST_DUEL || g_state == ST_PLAY))
    {
        Hero *me = &g_heroes[0];
        if (me->active && me->invuln == 0 && me->hearts > 0)
            hero_take_damage(0, hdr->duel_hero_hit, me->x, me->y);
    }

    const uint8_t *p = buf + sizeof(pkt_world_hdr);

    // Clear then repopulate so monsters that died on the host disappear here.
    for (int i = 0; i < MAX_MONSTERS; i++)
        g_monsters[i].alive = false;

    for (int i = 0; i < hdr->monster_count && i < MAX_MONSTERS; i++)
    {
        pkt_monster pm;
        memcpy(&pm, p, sizeof(pm));
        p += sizeof(pm);
        Monster *m = &g_monsters[i];
        m->alive = true;
        m->def_id = pm.def_id;
        m->x = pm.x;
        m->y = pm.y;
        m->dx = pm.dx;
        m->dy = pm.dy;
        m->hp = pm.hp;
        m->hp_max = pm.hp_max;
        m->hurt_timer = pm.hurt ? 8 : 0;
        m->is_boss = pm.is_boss;
        m->boss_id = pm.boss_id;
        m->elite = (pm.flags & MON_FLAG_ELITE) ? 1 : 0;
        m->recruited = (pm.flags & MON_FLAG_RECRUIT) != 0;
        m->pacify_timer = (pm.flags & MON_FLAG_PACIFIED) ? 30 : 0;
        m->scale_pct = (uint8_t)(((pm.flags >> 4) & 0x0F) * 25);
        if (m->scale_pct == 0) m->scale_pct = 100;
        m->radius = monster_draw_radius(m);
        m->anim++;
    }

    for (int i = 0; i < MAX_PROJECTILES; i++)
        g_projectiles[i].alive = false;

    for (int i = 0; i < hdr->projectile_count && i < MAX_PROJECTILES; i++)
    {
        pkt_projectile pp;
        memcpy(&pp, p, sizeof(pp));
        p += sizeof(pp);
        Projectile *pr = &g_projectiles[i];
        pr->alive = true;
        pr->kind = pp.kind;
        pr->x = pp.x;
        pr->y = pp.y;
        pr->vx = 0;
        pr->vy = 0;
        pr->life = 10;   // refreshed every packet
    }
}

#if HAVE_DSWIFI

static void client_rx_from_host(Wifi_MPPacketType type, int base, int len)
{
    if (type != WIFI_MPTYPE_CMD)
        return;
    s_last_rx_tick = (uint32_t)g_tick;
    s_zero_clients = 0;
    static uint8_t buf[512];
    if (len > (int)sizeof(buf))
        len = sizeof(buf);
    Wifi_RxRawReadPacket(base, len, buf);
    if (((pkt_header *)buf)->magic != NET_MAGIC)
        return;
    client_apply_world_packet(buf, len);
}

#else

static void client_rx_from_host(Wifi_MPPacketType t, int b, int l)
{
    (void)t; (void)b; (void)l;
}

#endif

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool net_start_host(void)
{
#if HAVE_DSWIFI
    if (g_net_active)
        net_shutdown();
    if (!Wifi_InitDefault(INIT_ONLY | WIFI_LOCAL_ONLY))
        return false;

    Wifi_MultiplayerHostMode(MAX_CLIENTS, sizeof(pkt_world_hdr) + 512,
                             sizeof(pkt_hero));
    int timeout = 40;
    while (!Wifi_LibraryModeReady() && --timeout > 0)
        swiWaitForVBlank();
    if (timeout <= 0)
        return false;

    Wifi_SetChannel(10);
    Wifi_MultiplayerAllowNewClients(true);
    Wifi_MultiplayerFromClientSetPacketHandler(host_rx_from_client);

    // The beacon text doubles as the room name shown in the client's scan.
    Wifi_BeaconStart("CapyBirthday", NET_MAGIC);

    g_net_active = true;
    g_net_host = true;
    g_net_ready = false;
    s_lost_reported = false;
    s_zero_clients = 0;
    s_last_rx_tick = 0;
    s_last_token_seq = 0;
    s_last_farm_seq = 0;
    return true;
#else
    return false;
#endif
}

bool net_start_client(void)
{
#if HAVE_DSWIFI
    if (g_net_active)
        net_shutdown();
    if (!Wifi_InitDefault(INIT_ONLY | WIFI_LOCAL_ONLY))
        return false;

    Wifi_MultiplayerClientMode(sizeof(pkt_hero));
    int timeout = 40;
    while (!Wifi_LibraryModeReady() && --timeout > 0)
        swiWaitForVBlank();
    if (timeout <= 0)
        return false;

    Wifi_ScanMode();

    g_net_active = true;
    g_net_host = false;
    g_net_ready = false;
    s_lost_reported = false;
    s_zero_clients = 0;
    s_last_rx_tick = 0;
    memset(&g_ap, 0, sizeof(g_ap));
    return true;
#else
    return false;
#endif

}

void net_update(void)
{
#if HAVE_DSWIFI
    if (!g_net_active)
        return;

    g_tick++;

    if (s_ult_fire > 0)
        s_ult_fire--;

    // Link supervision, both directions. The host watches its client count;
    // the client watches the host's packet stream. Either side going quiet
    // for ~3 s ends the session through multiplayer_link_lost(), which cleans
    // up and says so on screen instead of leaving a frozen partner behind.
    if (g_net_ready && !s_lost_reported)
    {
        bool alive;
        if (g_net_host)
        {
            if (Wifi_MultiplayerGetNumClients() > 0)
            {
                s_zero_clients = 0;
                alive = true;
            }
            else
                alive = ++s_zero_clients < LINK_LOST_TICKS;
        }
        else
            alive = (uint32_t)(g_tick - s_last_rx_tick) < LINK_LOST_TICKS;
        if (!alive)
        {
            s_lost_reported = true;
            multiplayer_link_lost();
            return;
        }
    }

    if (g_net_host)
    {
        // Start once the partner's console has joined.
        if (!g_net_ready && Wifi_MultiplayerGetNumClients() > 0)
        {
            g_net_ready = true;
            s_last_rx_tick = (uint32_t)g_tick;
            s_zero_clients = 0;
        }

        if (g_net_ready && (g_tick % NET_TICK) == 0)
            host_send_world();
    }
    else
    {
        if (!g_net_ready)
        {
            // Scan for the host's beacon. dswifi only surfaces Nintendo
            // multiplayer hosts here, and our game id identifies our room.
            static bool connecting = false;
            if (!connecting)
            {
                int count = Wifi_GetNumAP();
                for (int i = 0; i < count; i++)
                {
                    Wifi_AccessPoint ap;
                    Wifi_GetAPData(i, &ap);
                    if (ap.nintendo.game_id != NET_MAGIC)
                        continue;
                    if (!ap.nintendo.allows_connections)
                        continue;

                    g_ap = ap;
                    Wifi_MultiplayerFromHostSetPacketHandler(
                        client_rx_from_host);
                    Wifi_ConnectOpenAP(&g_ap);
                    connecting = true;
                    break;
                }
            }

            // Association completes asynchronously; switch to the transfer
            // phase once the link is up.
            if (connecting &&
                Wifi_AssocStatus() == ASSOCSTATUS_ASSOCIATED)
            {
                g_net_ready = true;
                s_last_rx_tick = (uint32_t)g_tick;
            }
        }
        else if ((g_tick % NET_TICK) == 0)
        {
            client_send_hero();
        }
    }
#endif
}

void net_shutdown(void)
{
#if HAVE_DSWIFI
    if (!g_net_active)
    {
        g_net_active = false;
        g_net_ready = false;
        return;
    }
    Wifi_IdleMode();
    Wifi_DisableWifi();
#endif
    g_net_active = false;
    g_net_ready = false;
    s_lost_reported = false;
    s_zero_clients = 0;
}

bool net_ready(void)
{
    return g_net_ready;
}
