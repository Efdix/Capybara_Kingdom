// Input: the single source of key state for the whole game.
//
// Gameplay and menus call input_held()/input_down() rather than libnds'
// keysHeld()/keysDown() directly. That indirection exists so the autoplay
// harness (compiled in with -DAUTOPLAY, see main.c) can substitute a scripted
// input sequence for the hardware state, which is what makes automated
// screenshot verification of every screen possible.

#include "game.h"

// ---------------------------------------------------------------------------
// Touch
//
// The bottom screen is a touch screen, and the level-up card picker is the one
// place in the game where tapping is unambiguously better than scrolling a
// cursor: the choice is three discrete pictures, so pointing at the one you
// want is the obvious gesture.
//
// This is exposed as a press/release edge plus a position, and it is routed
// through the same indirection as the buttons so the autoplay harness can
// script a tap and verify the screen.
// ---------------------------------------------------------------------------

static bool     g_touch_down = false;    // touching right now
static bool     g_touch_pressed = false; // went down since the last poll
static uint16_t g_touch_x = 0, g_touch_y = 0;

bool input_touch_pressed(void) { return g_touch_pressed; }
uint16_t input_touch_x(void) { return g_touch_x; }
uint16_t input_touch_y(void) { return g_touch_y; }

// Is the touch currently inside a rectangle on the bottom screen?
bool input_touch_in(int x, int y, int w, int h)
{
    if (!g_touch_down)
        return false;
    return (int)g_touch_x >= x && (int)g_touch_x < x + w &&
           (int)g_touch_y >= y && (int)g_touch_y < y + h;
}

#ifndef AUTOPLAY

// ---------------------------------------------------------------------------
// Normal build: read the hardware once per frame.
// ---------------------------------------------------------------------------

static uint16_t g_held = 0;
static uint16_t g_down = 0;

void input_poll(void)
{
    scanKeys();
    g_held = keysHeld();
    g_down = keysDown();

    // libnds reports the raw panel state; a touch is only meaningful once the
    // panel has settled, so the position is only trusted while it is down.
    touchPosition tp;
    touchRead(&tp);
    bool down = (keysHeld() & KEY_TOUCH) != 0;
    g_touch_pressed = down && !g_touch_down;
    g_touch_down = down;
    if (down)
    {
        g_touch_x = tp.px;
        g_touch_y = tp.py;
    }
}

uint16_t input_held(void) { return g_held; }
uint16_t input_down(void) { return g_down; }

#else

// ---------------------------------------------------------------------------
// Autoplay build: the script below replaces hardware input entirely.
// ---------------------------------------------------------------------------

typedef struct {
    int frames;       // how long to hold this step
    uint16_t keys;
} AutoplayStep;

// Sequence: title -> mode select -> hub -> region intro -> survival, then move
// around long enough to level up and pick cards.
//
// The game is a survivors-like now, so there is no attack button to press:
// moving IS the player's input. These scripts therefore mostly hold a direction
// and let the capybara's automatic cards do the fighting.
#ifdef AUTOPLAY_MENU
// Route used to verify the card picker: reach the hub, walk into the arena, and
// keep moving until a level-up opens the picker, then work both the cursor and
// the confirm button.
//
// In the --tap build, the two KEY_A steps at the end are replaced by nothing at
// all: the picker is committed by the scripted touch tap instead (see the tap
// block in input_poll). Leaving the A press in would mean the test could pass
// without the touch code ever running, which is exactly the kind of test that
// verifies nothing.
#ifdef AUTOPLAY_TAP_CARD
static const AutoplayStep script[] = {
    {  50, 0 },
    {   3, KEY_A },             // title -> mode select
    {  20, 0 },
    {   3, KEY_A },             // mode select -> hub (single player)
    {  60, 0 },
    {   3, KEY_A },             // hub -> region intro
    {  20, 0 },
    {   3, KEY_A },             // intro -> play
    {  90, KEY_UP },            // walk into the arena
    { 120, KEY_LEFT },          // keep moving; cards fire on their own
    { 120, KEY_RIGHT },
    { 120, KEY_UP },
    { 120, KEY_DOWN },
    { 300, 0 },                 // let the auto-bite earn exp
    { 200, KEY_LEFT },
    { 240, 0 },                 // the picker should be open here; taps hit it
    { 300, 0 },
    { 200, KEY_RIGHT },         // keep playing
    { 400, 0 },                 // hold so the screenshot loop can capture it
};
#else
static const AutoplayStep script[] = {
    {  50, 0 },
    {   3, KEY_A },             // title -> mode select
    {  20, 0 },
    {   3, KEY_A },             // mode select -> hub (single player)
    {  60, 0 },
    {   3, KEY_A },             // hub -> region intro
    {  20, 0 },
    {   3, KEY_A },             // intro -> play
    {  90, KEY_UP },            // walk into the arena
    { 120, KEY_LEFT },          // keep moving; cards fire on their own
    { 120, KEY_RIGHT },
    { 120, KEY_UP },
    { 120, KEY_DOWN },
    { 300, 0 },                 // let the auto-bite earn exp
    { 200, KEY_LEFT },
    { 240, 0 },
    {   3, KEY_RIGHT },         // if the picker is open, move the cursor
    {  40, 0 },
    {   3, KEY_A },             // take a card
    { 300, 0 },
    { 200, KEY_RIGHT },         // keep playing
    { 400, 0 },                 // hold so the screenshot loop can capture it
};
#endif   // AUTOPLAY_TAP_CARD
#elif defined(AUTOPLAY_BOSS)
// Route used to verify the boss fight: play normally for long enough that the
// region's ingredient drops accumulate past the summon threshold, dismissing
// the card picker whenever it opens. The boss appears on its own; nothing here
// pokes the debug shortcut, so this exercises the real progression path.
static const AutoplayStep script[] = {
    {  60, 0 },
    {   3, KEY_A },             // title -> mode select
    {  20, 0 },
    {   3, KEY_A },             // mode select -> hub
    {  40, 0 },
    {   3, KEY_A },             // hub -> region intro
    {  20, 0 },
    {   3, KEY_A },             // intro -> play
    {  20, 0 },
    {   3, KEY_SELECT },        // debug: summon the boss right away
    {  60, 0 },
    { 100, KEY_UP },
    // Walk a long circuit, pausing to fight. A is pressed periodically so any
    // card picker that opens gets dismissed and the run keeps going.
    { 260, KEY_LEFT },
    {   3, KEY_A },
    { 260, KEY_RIGHT },
    {   3, KEY_A },
    { 260, KEY_DOWN },
    {   3, KEY_A },
    { 300, KEY_LEFT },
    {   3, KEY_A },
    { 300, KEY_UP },
    {   3, KEY_A },
    { 300, KEY_RIGHT },
    {   3, KEY_A },
    { 300, KEY_DOWN },
    {   3, KEY_A },
    { 400, KEY_LEFT },
    {   3, KEY_A },
    { 400, KEY_RIGHT },
    {   3, KEY_A },
    { 400, KEY_UP },
    {   3, KEY_A },
    { 500, KEY_DOWN },
    {   3, KEY_A },
    { 600, KEY_LEFT },
    {   3, KEY_A },
    { 600, KEY_RIGHT },
    { 900, 0 },                 // hold so the boss fight can be captured
};
#elif defined(AUTOPLAY_ENDING)
// Alternate route used to verify the finale: go straight to the hub, secure
// every ingredient through the debug shortcut, and bake the cake. Playing all
// five regions first would make this check slow and prone to flaking.
static const AutoplayStep script[] = {
    {  60, 0 },
    {   3, KEY_A },             // title -> mode select
    {  20, 0 },
    {   3, KEY_A },             // mode select -> hub
    {  40, 0 },
    {   3, KEY_SELECT },        // debug: secure every ingredient
    {  40, 0 },
    {   3, KEY_Y },             // bake the cake -> ending
    { 900, 0 },                 // let the whole finale play
};
#else
static const AutoplayStep script[] = {
    {  90, 0 },                 // let the title settle
    {   3, KEY_A },             // title -> mode select
    {  30, 0 },
    {   3, KEY_A },             // mode select -> single-player hub
    // The hub is a screen worth verifying on its own (it binds the hub world
    // and its own camera), and a dwell of a second is too short to catch it
    // with an external screenshot loop.
    { 150, 0 },
    {   3, KEY_A },             // hub -> region intro
    {  30, 0 },
    {   3, KEY_A },             // intro -> play
    { 110, KEY_UP },            // walk up into the arena
    { 120, KEY_LEFT },
    { 120, KEY_RIGHT },
    { 120, KEY_UP },
    { 120, KEY_DOWN },
    { 300, 0 },                 // stand and fight: cards fire automatically
    {   3, KEY_A },             // dismiss a card picker if one opened
    {  60, 0 },
    { 150, KEY_LEFT },
    { 150, KEY_RIGHT },
    { 240, 0 },
    {   3, KEY_A },
    {  60, 0 },
    { 200, KEY_UP },
    { 200, KEY_DOWN },
    { 400, 0 },                 // keep fighting
};
#endif   // AUTOPLAY_ENDING

#define SCRIPT_LEN (int)(sizeof(script) / sizeof(script[0]))

static int      g_step = 0;
static int      g_left = -1;
static uint16_t g_held = 0;
static uint16_t g_down = 0;
static int      g_script_frame = 0;

// Scripted taps, so the touch path can be verified in an emulator the same way
// the buttons are.
//
// The tap is PERIODIC rather than scheduled at one frame, because the card
// picker opens whenever the character happens to level up -- a frame number
// cannot be known in advance, so a single scheduled tap would usually land in
// the middle of ordinary gameplay and test nothing. Tapping every TAP_PERIOD
// frames guarantees one lands while the picker is open, and a tap during play
// is harmless: nothing in the running game reads the touch screen.
#ifdef AUTOPLAY_TAP_CARD
#define TAP_PERIOD   40
#define TAP_DOWN      2       // frames the "finger" stays on the screen
#define TAP_X       128       // middle card of the three
#define TAP_Y        60
#endif

void input_poll(void)
{
    if (g_left < 0)
        g_left = script[0].frames;

    uint16_t keys = script[g_step].keys;
    g_down = (uint16_t)(keys & ~g_held);   // edge detection for "just pressed"
    g_held = keys;

#ifdef AUTOPLAY_TAP_CARD
    int phase = g_script_frame % TAP_PERIOD;
    bool down = (phase < TAP_DOWN);
    g_touch_pressed = down && !g_touch_down;
    g_touch_down = down;
    if (down)
    {
        g_touch_x = TAP_X;
        g_touch_y = TAP_Y;
    }
#endif
    g_script_frame++;

    if (g_left > 0)
        g_left--;
    if (g_left == 0)
    {
        if (g_step < SCRIPT_LEN - 1)
        {
            g_step++;
            g_left = script[g_step].frames;
        }
        else
        {
            g_left = 1;   // hold the last step indefinitely
        }
    }
}

uint16_t input_held(void) { return g_held; }
uint16_t input_down(void) { return g_down; }

#endif  // AUTOPLAY
