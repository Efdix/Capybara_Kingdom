// perf.h -- is the renderer still keeping up?
//
// The renderer redraws every visible byte of both screens every frame and copies
// 96 KB into VRAM, and until now the only statement about whether that fits
// inside a refresh was a comment claiming it does. Comments do not survive a
// region getting busier.
//
// This counts frames that missed their refresh. See perf.c for how, and for the
// three instruments that did not work.

#pragma once

#include <stdint.h>

// Use instead of swiWaitForVBlank() in the main loop: same wait, plus the
// overrun check that has to run before the flag is consumed.
void perf_wait_vblank(void);

// Call at the top of the loop body and at the very end, after the last flush.
void perf_frame_begin(void);
void perf_frame_end(void);

// Frames that missed their refresh in the most recent 60-frame window, and the
// worst such window since startup.
uint32_t perf_late(void);
uint32_t perf_late_worst(void);

// Draw the readout. A no-op unless the build asked for it, so the shipping ROM
// carries the counters but no pixels.
void perf_draw_hud(void);
