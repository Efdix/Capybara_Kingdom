// perf.c -- see perf.h.
//
// HOW THIS MEASURES, AND WHAT IT COST TO FIND OUT
//
// The obvious instrument is a hardware timer, and the first three attempts here
// used one. All of them failed. The reason is worth recording: the DS timer
// block is shared between the two CPUs, and on a cartridge linking dswifi and
// maxmod the ARM7 firmware has already claimed it -- the sound hardware drives
// timers 0 and 1 for its channel rates, and dswifi's beacon timing takes the
// last one. A timer read from the ARM9 across a frame is not reliably ours: the
// calibration returned a sane rate on one build and zero on the next, and
// per-frame deltas came back as zero whatever the frame did. Timer 2 behaved the
// same way.
//
// The fourth attempt installed a VBlank IRQ and counted interrupts. That
// measured something, but it also *changed* the thing being measured: without
// the matching acknowledgement bookkeeping, the interrupt re-entered instead of
// returning, and the game ran at a crawl -- the in-game clock advanced one
// second in nearly two minutes of real time. A measurement that degrades the
// subject is not a measurement, and it was live in the release build too, since
// the counter ran outside the PERF_HUD guard.
//
// What is left is the one signal the display gives away for free. libnds
// implements swiWaitForVBlank() by clearing the VBlank bit of REG_IF and then
// polling it, so the bit is a latch: it is raised by hardware at the start of
// every VBlank and stays raised until a waiter consumes it. If a frame's work
// overruns its refresh, the hardware raises the bit while the game is still
// drawing, and the next call to wait finds it already up. Reading that bit
// costs one load and touches nothing.
//
// So the metric is "how many of the last sixty frames were late", which is the
// question that matters anyway -- not how many microseconds the render takes,
// but whether it still fits.

#include <nds.h>

#include "game.h"
#include "perf.h"
#include "save.h"

#define PERF_WINDOW       60

static bool     s_started;
static uint32_t s_frames;
static uint32_t s_late;          // late frames this window
static uint32_t s_late_last;     // published
static uint32_t s_late_worst;    // worst window seen

void perf_wait_vblank(void)
{
    // Before the wait, not after: swiWaitForVBlank() clears the flag, so the
    // evidence of an overrun has to be read while it is still standing.
    if (s_started && (REG_IF & IRQ_VBLANK))
        s_late++;

    swiWaitForVBlank();
}

void perf_frame_begin(void)
{
    // Set after the first wait, so the flag raised before the game had begun
    // drawing is not counted against it.
    if (!s_started)
    {
        s_started = true;
        return;
    }
}

#ifdef PERF_HUD
// The save channel, exercised once so a screenshot can settle whether this
// cartridge (or emulator) will actually store anything. Dev ROM only: the
// release build never takes this path, and nothing here writes unless the
// probe failed -- in which case the write IS the test, as documented in save.c.
static bool     s_save_tried;
static bool     s_save_wrote;
static uint16_t s_save_type;

static void perf_save_probe(void)
{
    if (s_save_tried)
        return;
    // Late enough that the title screen has already drawn its own verdict, so
    // this cannot be mistaken for the game's own state.
    if (g_frame < 300)
        return;
    s_save_tried = true;
    s_save_wrote = save_write();
    s_save_type = (uint16_t)(save_available() ? 1 : 0);
}
#endif

void perf_frame_end(void)
{
    s_frames++;
    if (s_frames < PERF_WINDOW)
        return;

    s_late_last = s_late;
    if (s_late > s_late_worst)
        s_late_worst = s_late;
    s_late = 0;
    s_frames = 0;
}

uint32_t perf_late(void)        { return s_late_last; }
uint32_t perf_late_worst(void)  { return s_late_worst; }

void perf_draw_hud(void)
{
#ifdef PERF_HUD
    char line[64];

    // A solid strip over the middle of the top screen: the readout has to
    // survive being measured under the busiest frame the game can produce, and
    // be tall enough for every line -- an earlier version was not, and the last
    // line landed on bare terrain in a dark colour.
    top_fill_rect(0, 92, 190, 64, PAL_UI2_SCREEN);

    snprintf(line, sizeof(line), "late/60 %lu", (unsigned long)s_late_last);
    top_text_draw_px(3, 95, line,
                     s_late_last ? PAL_RED_LIGHT : PAL_GREEN_SPARK);

    snprintf(line, sizeof(line), "worst   %lu", (unsigned long)s_late_worst);
    top_text_draw_px(3, 107, line,
                     s_late_worst ? PAL_YELLOW_LIGHT : PAL_GREEN_SPARK);

    snprintf(line, sizeof(line), "frames  %lu", (unsigned long)s_frames);
    top_text_draw_px(3, 119, line, PAL_UI2_INK);

    perf_save_probe();
    snprintf(line, sizeof(line), "save %lu w %lu",
             (unsigned long)s_save_type, (unsigned long)(s_save_tried ? s_save_wrote : 2));
    top_text_draw_px(3, 131, line,
                     (s_save_tried && s_save_wrote) ? PAL_GREEN_SPARK
                                                    : PAL_RED_LIGHT);

    // How many weather specks the last frame drew: see g_weather_specks.
    snprintf(line, sizeof(line), "wx %lu", (unsigned long)g_weather_specks);
    top_text_draw_px(3, 143, line, PAL_UI2_INK);
#endif
}
