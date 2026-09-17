// Audio: Maxmod soundbank playback.
//
// The soundbank comes from tools/gen_audio.py (ProTracker MOD modules + WAV
// samples) converted by mmutil into soundbank_bin and a header naming every
// module and sample. Assets are generated, so this module tolerates a partial
// or missing soundbank: every lookup is guarded, and music that is not in the
// bank simply plays nothing instead of failing to link. That keeps a build
// from being blocked by an audio-asset problem.

#include <maxmod9.h>

#include "game.h"
#include "assets.h"

#include "soundbank.h"
#include "soundbank_bin.h"

// Map the game's abstract ids onto generated bank symbols. Each entry is
// guarded so that removing a track from the generator degrades to silence.
typedef struct {
    uint16_t present;
    uint16_t module;
} ModuleMap;

// Game module ids (MUS_*) index into this table.
static const ModuleMap g_modules[MUS_COUNT] = {
#ifdef MOD_TITLE
    { 1, MOD_TITLE },
#else
    { 0, 0 },
#endif
#ifdef MOD_FIELD
    { 1, MOD_FIELD },
#else
    { 0, 0 },
#endif
#ifdef MOD_BATTLE
    { 1, MOD_BATTLE },
#else
    { 0, 0 },
#endif
#ifdef MOD_BOSS
    { 1, MOD_BOSS },
#else
    { 0, 0 },
#endif
#ifdef MOD_VICTORY
    { 1, MOD_VICTORY },
#else
    { 0, 0 },
#endif
#ifdef MOD_BIRTHDAY
    { 1, MOD_BIRTHDAY },
#else
    { 0, 0 },
#endif
};

// Sound effect ids (SND_* game-side indices) map onto bank samples the
// same way, guarded per entry.
typedef struct {
    uint16_t present;
    uint16_t sample;
} SampleMap;

static const SampleMap g_samples[SND_COUNT] = {
    // Indexed by the game's SND_* ids, in the order gen_audio.py
    // emits them. Each entry is guarded so that dropping a sample
    // from the generator degrades to silence instead of breaking
    // the build.
#ifdef SFX_UI_MOVE
    { 1, SFX_UI_MOVE },
#else
    { 0, 0 },   // not in this soundbank: play silence
#endif
#ifdef SFX_UI_CONFIRM
    { 1, SFX_UI_CONFIRM },
#else
    { 0, 0 },   // not in this soundbank: play silence
#endif
#ifdef SFX_UI_CANCEL
    { 1, SFX_UI_CANCEL },
#else
    { 0, 0 },   // not in this soundbank: play silence
#endif
#ifdef SFX_ATTACK
    { 1, SFX_ATTACK },
#else
    { 0, 0 },   // not in this soundbank: play silence
#endif
#ifdef SFX_HIT
    { 1, SFX_HIT },
#else
    { 0, 0 },   // not in this soundbank: play silence
#endif
#ifdef SFX_HURT
    { 1, SFX_HURT },
#else
    { 0, 0 },   // not in this soundbank: play silence
#endif
#ifdef SFX_ENEMY_DIE
    { 1, SFX_ENEMY_DIE },
#else
    { 0, 0 },   // not in this soundbank: play silence
#endif
#ifdef SFX_MAGIC_FIRE
    { 1, SFX_MAGIC_FIRE },
#else
    { 0, 0 },   // not in this soundbank: play silence
#endif
#ifdef SFX_MAGIC_ICE
    { 1, SFX_MAGIC_ICE },
#else
    { 0, 0 },   // not in this soundbank: play silence
#endif
#ifdef SFX_MAGIC_THUNDER
    { 1, SFX_MAGIC_THUNDER },
#else
    { 0, 0 },   // not in this soundbank: play silence
#endif
#ifdef SFX_HEAL
    { 1, SFX_HEAL },
#else
    { 0, 0 },   // not in this soundbank: play silence
#endif
#ifdef SFX_PICKUP
    { 1, SFX_PICKUP },
#else
    { 0, 0 },   // not in this soundbank: play silence
#endif
#ifdef SFX_LEVELUP
    { 1, SFX_LEVELUP },
#else
    { 0, 0 },   // not in this soundbank: play silence
#endif
#ifdef SFX_DOOR
    { 1, SFX_DOOR },
#else
    { 0, 0 },   // not in this soundbank: play silence
#endif
#ifdef SFX_EXPLOSION
    { 1, SFX_EXPLOSION },
#else
    { 0, 0 },   // not in this soundbank: play silence
#endif
#ifdef SFX_HEART
    { 1, SFX_HEART },
#else
    { 0, 0 },   // not in this soundbank: play silence
#endif
};

static int g_current_music = -1;
static mm_word g_loaded_module = 0;
static bool g_have_module = false;

void audio_init(void)
{
    mmInitDefaultMem((mm_addr)soundbank_bin);
    g_current_music = -1;
    g_have_module = false;
}

void audio_play_music(int module_id)
{
    if (module_id == g_current_music)
        return;
    g_current_music = module_id;

    if (module_id < 0 || module_id >= MUS_COUNT)
        return;
    if (!g_modules[module_id].present)
        return;   // track not in the soundbank: stay silent

    // Unload the previous module before loading the next one; maxmod keeps a
    // fixed pool of module slots and leaving them loaded would exhaust it.
    if (g_have_module)
    {
        mmStop();
        mmUnload(g_loaded_module);
        g_have_module = false;
    }

    mm_word id = g_modules[module_id].module;
    if (mmLoad(id) == 0)
        return;

    g_loaded_module = id;
    g_have_module = true;

    // The victory jingle is a one-shot fanfare; everything else loops.
    mmStart(id, (module_id == MUS_VICTORY) ? MM_PLAY_ONCE : MM_PLAY_LOOP);
}

void audio_stop_music(void)
{
    g_current_music = -1;
    if (g_have_module)
    {
        mmStop();
        mmUnload(g_loaded_module);
        g_have_module = false;
    }
}

void audio_play_sfx(int sfx_id)
{
    if (sfx_id < 0 || sfx_id >= SND_COUNT)
        return;
    if (!g_samples[sfx_id].present)
        return;
    mmEffect(g_samples[sfx_id].sample);
}
