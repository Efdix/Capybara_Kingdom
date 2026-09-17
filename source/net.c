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
} pkt_monster;

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

void net_notify_ultimate(void)
{
    s_ult_fire = 8;
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
    hdr->ingredient_have = g_ingredient_have;
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

    // The client asked to release; the host owns monsters, so it applies the
    // effect here. Charge and the combo window are the host's to track, exactly
    // as they are for its own capybara.
    if (pkt.ult_fire)
        hero_release_ultimate(1);
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
    g_ingredient_have = hdr->ingredient_have;
    g_ingredients_done = hdr->ingredients_done;
    g_boss_out = hdr->boss_out != 0;
    g_run_time = hdr->run_time;
    g_difficulty = hdr->difficulty;

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
    return true;
#else
    return false;
#endif
}

bool net_start_client(void)
{
#if HAVE_DSWIFI
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

    if (g_net_host)
    {
        // Start once the partner's console has joined.
        if (!g_net_ready && Wifi_MultiplayerGetNumClients() > 0)
            g_net_ready = true;

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
        return;
    Wifi_IdleMode();
    Wifi_DisableWifi();
#endif
    g_net_active = false;
    g_net_ready = false;
}

bool net_ready(void)
{
    return g_net_ready;
}
