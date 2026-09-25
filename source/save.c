// save.c -- see save.h for what is stored and why.
//
// The payload is written byte by byte through the index helpers below rather
// than by memcpy'ing a struct. A struct would be shorter to write and would
// break the moment a compiler inserted padding, or the day someone reordered a
// field: the save would still "work", it would just silently read back into the
// wrong variables. An explicit layout with a version byte and a checksum makes
// that failure mode impossible.

#include <string.h>

#include <nds.h>
#include <nds/card.h>

#include "save.h"
#include "game.h"
#include "farm.h"
#include "skill_tree.h"

// --- layout -----------------------------------------------------------------

#define SAVE_MAGIC      0x314B4343u   // "CCK1" little-endian
#define SAVE_VERSION    1

#define SAVE_BYTES      64            // one EEPROM page; keeps the whole payload
                                      // inside a 4Kbit chip's 512 bytes with
                                      // room to spare

// Field offsets. Keep these in the order the header describes and bump
// SAVE_VERSION whenever one moves.
#define OFF_MAGIC       0
#define OFF_VERSION     4
#define OFF_POWER       5
#define OFF_TOUGH       6
#define OFF_AGI         7
#define OFF_HARVEST     8
#define OFF_TOKENS      9
#define OFF_REGIONS     11
#define OFF_DAY         12
#define OFF_SEEDS       14
#define OFF_CAKE        19
#define OFF_PLOTS       24
#define OFF_MUSIC_VOL   48
#define OFF_SFX_VOL     49
#define OFF_TUTORIAL    50   // u32: the first-run guidance chain (tutorial.c)

// 54..61 stay reserved so a future field can be added without moving anything.

static uint8_t s_buf[SAVE_BYTES];

// Cached answer from the probe. `cardEepromGetType()` talks to the cartridge
// over SPI, so a success is remembered rather than asked again mid-frame.
//
// A FAILURE is deliberately not remembered for good. Emulators detect the save
// chip by watching the game issue EEPROM commands, so the first probe can come
// back negative on a cartridge that will answer perfectly a moment later --
// melonDS reported "no save chip" on the first build of this and the title
// screen said so in red. Retrying is throttled to once a second, which is far
// more often than the answer can change but cheap enough to be free.
static int      s_eeprom_type = -1;
static bool     s_probed      = false;
static uint32_t s_probe_frame = 0;
static bool     s_have_data   = false;

// A type established by an actual round trip rather than by the ID probe.
//
// The probe alone is not enough to decide there is no save chip, and the reason
// is a deadlock: `cardEepromGetType()` only issues read commands, while some
// emulators (and some flashcarts) only commit to a save type once the game has
// written something. Refusing to write until a chip is confirmed therefore means
// the chip is never confirmed. So when the probe comes back empty, the first
// write is attempted anyway at the safest type -- address 0 is address 0 in
// every EEPROM encoding, so a wrong guess cannot land anywhere but the start of
// the chip -- and the result is read straight back. If it matches, the channel
// is real and the type is remembered.
static int  s_forced_type = 0;
#define SAVE_FALLBACK_TYPE 1   // 4Kbit / 512-byte EEPROM: the smallest, so the
                               // payload is guaranteed to fit if it answers

// --- little-endian helpers --------------------------------------------------

static void put_u16(int off, uint16_t v)
{
    s_buf[off] = (uint8_t)(v & 0xFF);
    s_buf[off + 1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(int off)
{
    return (uint16_t)(s_buf[off] | ((uint16_t)s_buf[off + 1] << 8));
}

static void put_u32(int off, uint32_t v)
{
    s_buf[off] = (uint8_t)(v & 0xFF);
    s_buf[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    s_buf[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    s_buf[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32(int off)
{
    return (uint32_t)s_buf[off] |
           ((uint32_t)s_buf[off + 1] << 8) |
           ((uint32_t)s_buf[off + 2] << 16) |
           ((uint32_t)s_buf[off + 3] << 24);
}

// A sum and a rolling xor. Not a cryptographic check -- its job is to tell a
// real save from a blank chip, which reads back as all 0x00 or all 0xFF, and
// from a half-written one, which is what a power cut during a write leaves.
static uint16_t checksum_of(const uint8_t *p, int n)
{
    uint16_t sum = 0;
    uint8_t x = 0x5A;
    for (int i = 0; i < n; i++)
    {
        sum = (uint16_t)(sum + p[i]);
        x ^= p[i];
        x = (uint8_t)((x << 1) | (x >> 7));
    }
    return (uint16_t)(sum ^ ((uint16_t)x << 8));
}

// --- the chip ---------------------------------------------------------------

static int eeprom_type(void)
{
    if (s_eeprom_type >= 0)
        return s_eeprom_type;
    if (s_forced_type > 0)
        return s_forced_type;

    // -1 sees no chip, 0 is an unknown chip. Neither is final until the
    // cartridge has had a chance to answer, so a miss is retried once a second
    // -- and save_write() has a second, stronger way to find out.
    if (s_probed && (uint32_t)(g_frame - s_probe_frame) < 60)
        return -1;

    s_probed = true;
    s_probe_frame = g_frame;

    int t = cardEepromGetType();
    if (t >= 1 && t <= 3)
        s_eeprom_type = t;
    return s_eeprom_type;
}

static bool eeprom_read_as(int type, uint8_t *dst, int len)
{
    if (type < 1 || type > 3)
        return false;
    u32 size = cardEepromGetSize();
    if (size > 0 && (u32)len > size)
        return false;
    cardReadEeprom(0, dst, (u32)len, (u32)type);
    return true;
}

static bool eeprom_read(uint8_t *dst, int len)
{
    return eeprom_read_as(eeprom_type(), dst, len);
}

static bool eeprom_write(const uint8_t *src, int len)
{
    int t = eeprom_type();
    if (t < 0)
        return false;
    u32 size = cardEepromGetSize();
    if (size > 0 && (u32)len > size)
        return false;

    // A flash chip has to be erased before it can be written, and it erases in
    // 4KB sectors. Our payload starts at offset 0, so the first sector is the
    // only one that ever needs clearing.
    if (t == 3)
        cardEepromSectorErase(0);

    cardWriteEeprom(0, (u8 *)src, (u32)len, (u32)t);
    return true;
}

// --- public API -------------------------------------------------------------

bool save_available(void)
{
    return eeprom_type() >= 0 || s_forced_type > 0;
}

bool save_has_data(void)
{
    return s_have_data;
}

bool save_load(void)
{
    // Try the probed type first, then the fallback. The second attempt is what
    // makes a save written on a cartridge whose ID probe does not answer
    // readable again next session: s_forced_type lives in RAM and does not
    // survive a power cycle, so the channel has to be re-established from the
    // data itself -- if the magic validates at the fallback type, that type is
    // right by definition.
    if (!eeprom_read(s_buf, SAVE_BYTES))
    {
        if (!eeprom_read_as(SAVE_FALLBACK_TYPE, s_buf, SAVE_BYTES))
        {
            s_have_data = false;
            return false;
        }
        if (get_u32(OFF_MAGIC) != SAVE_MAGIC ||
            s_buf[OFF_VERSION] != SAVE_VERSION ||
            get_u16(SAVE_BYTES - 2) != checksum_of(s_buf, SAVE_BYTES - 2))
        {
            s_have_data = false;
            return false;
        }
        s_forced_type = SAVE_FALLBACK_TYPE;
    }

    // A blank chip reads as 0xFF everywhere, an erased one as 0x00; neither can
    // carry the magic, so both land here.
    if (get_u32(OFF_MAGIC) != SAVE_MAGIC || s_buf[OFF_VERSION] != SAVE_VERSION)
    {
        s_have_data = false;
        return false;
    }
    if (get_u16(SAVE_BYTES - 2) != checksum_of(s_buf, SAVE_BYTES - 2))
    {
        s_have_data = false;
        return false;
    }

    g_skill_tree.power_lv   = s_buf[OFF_POWER];
    g_skill_tree.tough_lv   = s_buf[OFF_TOUGH];
    g_skill_tree.agi_lv     = s_buf[OFF_AGI];
    g_skill_tree.harvest_lv = s_buf[OFF_HARVEST];
    if (g_skill_tree.power_lv   > SKILL_MAX_LEVEL) g_skill_tree.power_lv   = SKILL_MAX_LEVEL;
    if (g_skill_tree.tough_lv   > SKILL_MAX_LEVEL) g_skill_tree.tough_lv   = SKILL_MAX_LEVEL;
    if (g_skill_tree.agi_lv     > SKILL_MAX_LEVEL) g_skill_tree.agi_lv     = SKILL_MAX_LEVEL;
    if (g_skill_tree.harvest_lv > SKILL_MAX_LEVEL) g_skill_tree.harvest_lv = SKILL_MAX_LEVEL;

    g_boss_tokens       = get_u16(OFF_TOKENS);
    g_ingredients_done  = s_buf[OFF_REGIONS];
    g_day               = get_u16(OFF_DAY);

    for (int i = 0; i < FARM_CROP_COUNT; i++)
        g_seeds[i] = s_buf[OFF_SEEDS + i];
    for (int i = 0; i < INGREDIENT_COUNT; i++)
        g_cake_have[i] = s_buf[OFF_CAKE + i];

    for (int i = 0; i < FARM_PLOTS; i++)
        farm_unpack_plot(&g_farm[i], s_buf[OFF_PLOTS + i]);

    audio_set_music_volume(s_buf[OFF_MUSIC_VOL]);
    audio_set_sfx_volume(s_buf[OFF_SFX_VOL]);
    // The chain outgrew 16 bits, so the field is now u32. An older file kept
    // zeros in what was the reserved half, so reading the wide field lands an
    // old save exactly where it was: old bits set, new beats untaught.
    g_tut_flags = get_u32(OFF_TUTORIAL);

    s_have_data = true;
    return true;
}

bool save_write(void)
{
    memset(s_buf, 0, sizeof(s_buf));

    put_u32(OFF_MAGIC, SAVE_MAGIC);
    s_buf[OFF_VERSION] = SAVE_VERSION;

    s_buf[OFF_POWER]   = g_skill_tree.power_lv;
    s_buf[OFF_TOUGH]   = g_skill_tree.tough_lv;
    s_buf[OFF_AGI]     = g_skill_tree.agi_lv;
    s_buf[OFF_HARVEST] = g_skill_tree.harvest_lv;

    put_u16(OFF_TOKENS, g_boss_tokens);
    s_buf[OFF_REGIONS] = g_ingredients_done;
    put_u16(OFF_DAY, g_day);

    for (int i = 0; i < FARM_CROP_COUNT; i++)
        s_buf[OFF_SEEDS + i] = g_seeds[i];
    for (int i = 0; i < INGREDIENT_COUNT; i++)
        s_buf[OFF_CAKE + i] = g_cake_have[i];

    for (int i = 0; i < FARM_PLOTS; i++)
        s_buf[OFF_PLOTS + i] = farm_pack_plot(&g_farm[i]);

    s_buf[OFF_MUSIC_VOL] = (uint8_t)audio_get_music_volume();
    s_buf[OFF_SFX_VOL]   = (uint8_t)audio_get_sfx_volume();
    put_u32(OFF_TUTORIAL, g_tut_flags);

    put_u16(SAVE_BYTES - 2, checksum_of(s_buf, SAVE_BYTES - 2));

    // The probe may have come back empty on a cartridge that does have a chip
    // (see s_forced_type). When that happens this write is also the test: send
    // it at the safest type, read it straight back, and only believe in the
    // channel if the bytes survived the trip.
    if (eeprom_type() < 0)
    {
        // The once-per-session forced write, established below on real
        // cartridges, hangs melonDS 1.1 solid: with the save type still
        // unresolved the emulated card transfer never completes, and libnds'
        // busy-wait inside cardWriteEeprom has no timeout to escape it. The
        // autoplay ROMs exist to run unattended in that emulator, so they
        // compile without the fallback and play by the "no save chip" rules
        // the game already supports end to end. Real hardware keeps it: on a
        // cartridge the bus always answers, and the write-then-verify is how
        // a chip whose ID probe stays silent gets adopted.
#ifndef AUTOPLAY
        static uint8_t verify[SAVE_BYTES];
        int t = SAVE_FALLBACK_TYPE;

        cardWriteEeprom(0, (u8 *)s_buf, SAVE_BYTES, (u32)t);
        cardReadEeprom(0, verify, SAVE_BYTES, (u32)t);

        if (memcmp(verify, s_buf, SAVE_BYTES) != 0)
            return false;

        s_forced_type = t;
        s_have_data = true;
        return true;
#else
        return false;
#endif
    }

    if (!eeprom_write(s_buf, SAVE_BYTES))
        return false;

    s_have_data = true;
    return true;
}

void save_clear(void)
{
    // The chip is not erased -- that is a slow, sector-shaped operation and it
    // is not needed to make the save unreadable. Overwriting the magic is what
    // makes it stale, and the next save_write() fills in the real payload.
    memset(s_buf, 0, sizeof(s_buf));
    s_have_data = false;

    if (eeprom_write(s_buf, SAVE_BYTES))
        return;

    // No chip, or the write failed. Clearing a save that does not exist is not
    // an error worth surfacing: the in-RAM state is already what the caller
    // wanted, and the title screen's offer is driven by s_have_data.
}
