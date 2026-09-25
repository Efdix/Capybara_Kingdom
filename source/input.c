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

// Sequence: title menu -> character select -> mode select -> hub -> region
// intro -> survival, then move
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
    {   3, KEY_A },             // title menu -> character select
    {  20, 0 },
    {   3, KEY_A },             // character select -> mode select
    {  20, 0 },
    {   3, KEY_A },             // mode select -> hub (single player)
    {  60, 0 },
    { 100, KEY_UP },            // walk north out of the plaza to the gate
    {   3, KEY_A },             // gate -> world map
    {  30, 0 },
    {   3, KEY_A },             // world map -> play
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
    {   3, KEY_A },             // title menu -> character select
    {  20, 0 },
    {   3, KEY_A },             // character select -> mode select
    {  20, 0 },
    {   3, KEY_A },             // mode select -> hub (single player)
    {  60, 0 },
    { 100, KEY_UP },            // walk north out of the plaza to the gate
    {   3, KEY_A },             // gate -> world map
    {  30, 0 },
    {   3, KEY_A },             // world map -> play
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
static const AutoplayStep script[] = {
    {  60, 0 },
    {   3, KEY_A },             // title menu -> character select
    {  20, 0 },
    {   3, KEY_A },             // character select -> mode select
    {  20, 0 },
    {   3, KEY_A },             // mode select -> hub
    {  40, 0 },
    // The hub's walking route depends on the kingdom layout, which changes
    // whenever the map is reworked; L drops straight into the region from the
    // hub, so this script only has to know which state it is in, not which way
    // the gate happens to be from wherever the capybara spawned. (SELECT is
    // taken by the ending route's "fill every ingredient" shortcut.)
    {   3, KEY_L },             // debug: hub -> region 1
    {  40, 0 },
    {   3, KEY_A },             // dismiss a picker if one is up
    {   3, KEY_R },             // debug: spawn an elite beside us
    {  50, 0 },
    {   3, KEY_A },             // dismiss a picker if one is up
    {   3, KEY_L },             // debug: force the raid wave now
    { 120, 0 },                 // fight the wave for a moment
    {   3, KEY_A },             // dismiss a picker if one is up
    {  20, 0 },
    // Repeat the fill: a press that lands while the card picker is paused is
    // swallowed by the picker, so keep pressing until one lands in play.
    {   3, KEY_SELECT },        // debug: fill the bar -> the boss leaps out
    {  30, 0 },
    {   3, KEY_A },
    {  20, 0 },
    {   3, KEY_SELECT },
    {  30, 0 },
    {   3, KEY_A },
    {  20, 0 },
    {   3, KEY_SELECT },
    {  60, 0 },                 // the leap-out beat plays here
    // The duel runs from here on: sweep side to side and hop. A is jump in
    // the arena; the periodic presses double as "dismiss any picker" and
    // "hop".
    { 300, KEY_UP },
    {   3, KEY_A },
    { 300, KEY_LEFT },
    {   3, KEY_A },
    { 300, KEY_DOWN },
    {   3, KEY_A },
    { 300, KEY_RIGHT },
    {   3, KEY_A },
    { 400, KEY_UP },
    {   3, KEY_A },
    { 400, KEY_LEFT },
    {   3, KEY_A },
    { 400, KEY_DOWN },
    {   3, KEY_A },
    { 400, KEY_RIGHT },
    {   3, KEY_A },
    { 500, KEY_UP },
    {   3, KEY_A },
    { 500, KEY_LEFT },
    {   3, KEY_A },
    { 500, KEY_DOWN },
    {   3, KEY_A },
    { 500, KEY_RIGHT },
    {   3, KEY_A },
    { 500, KEY_UP },
    {   3, KEY_A },
    { 500, KEY_LEFT },
    {   3, KEY_A },
    { 500, KEY_DOWN },
    {   3, KEY_A },
    { 500, KEY_RIGHT },
    {   3, KEY_A },
    // The duel: sweep side to side and hop. A is jump in the arena; the
    // periodic presses double as "dismiss any picker" and "hop".
    { 120, KEY_LEFT },
    {   3, KEY_A },
    { 120, KEY_RIGHT },
    {   3, KEY_A },
    { 120, KEY_LEFT },
    {   3, KEY_A },
    { 120, KEY_RIGHT },
    {   3, KEY_A },
    { 120, KEY_LEFT },
    {   3, KEY_A },
    { 120, KEY_RIGHT },
    {   3, KEY_A },
    { 600, 0 },                 // hold so the duel can be captured
};

#elif defined(AUTOPLAY_ENDING)
// Alternate route used to verify the finale: go straight to the hub, secure
// every ingredient through the debug shortcut, and bake the cake. Playing all
// five regions first would make this check slow and prone to flaking.
static const AutoplayStep script[] = {
    {  60, 0 },
    {   3, KEY_A },             // title menu -> character select
    {  20, 0 },
    {   3, KEY_A },             // character select -> mode select
    {  20, 0 },
    {   3, KEY_A },             // mode select -> hub
    {  40, 0 },
    {   3, KEY_SELECT },        // debug: secure every ingredient
    {  40, 0 },
    {   3, KEY_Y },             // bake the cake -> ending
    { 900, 0 },                 // let the whole finale play
};
#elif defined(AUTOPLAY_UI)
// Route used to review the menu screens a battle route never visits: settings,
// the skill tree, and the wireless lobby (reached through a scripted tap on the
// hub's shortcut bar). Each screen holds long enough that a screenshot loop can
// catch it cleanly.
static const AutoplayStep script[] = {
    {  60, 0 },
    {   3, KEY_DOWN },          // title: move to 设置
    {  20, 0 },
    {   3, KEY_A },             // -> settings
    { 150, 0 },                 // hold for the screenshot
    {   3, KEY_DOWN },          // select 返回
    {  10, 0 },
    {   3, KEY_A },             // back to the title
    {  30, 0 },
    {   3, KEY_A },             // title -> character select
    {  60, 0 },
    {   3, KEY_RIGHT },         // select the second capybara, so both get seen
    {  10, 0 },
    {   3, KEY_A },             // character select -> mode select
    {  60, 0 },
    {   3, KEY_A },             // mode select -> hub (single player)
    {  90, 0 },
    {   3, KEY_X },             // hub -> skill tree
    { 150, 0 },                 // hold for the screenshot
    {   3, KEY_B },             // back to the hub
    {  90, 0 },
    {   3, 0 },                 // the scripted tap lands at frame 790
    {  40, 0 },                 // -> lobby menu
    { 150, 0 },                 // hold for the screenshot
    {   3, KEY_B },             // back to the hub
    {  60, 0 },
    {  90, KEY_UP },            // walk north out of the plaza to the gate
    {   3, KEY_A },             // gate -> world map
    { 150, 0 },                 // hold for the screenshot
    {   3, KEY_A },             // world map -> play
    { 300, 0 },                 // hold: the play HUD, for the capture loop
    {   3, KEY_A },             // commit a card picker if one opened, so the
    {  20, 0 },                 // presentation keys below are not eaten by it
    {   3, KEY_START },         // -> level-clear plate
    { 150, 0 },                 // hold for the screenshot
    {   3, KEY_A },             // back to the hub
    {  60, 0 },
    {   3, KEY_A },             // commit a picker again, if one opened
    {  20, 0 },
    {   3, KEY_L },             // -> game-over plate
    { 150, 0 },                 // hold for the screenshot
    {   3, KEY_A },             // back to the hub
    {  60, 0 },
    {   3, KEY_A },             // and again before the R press
    {  20, 0 },
    {   3, KEY_R },             // -> the ending
    { 420, 0 },                 // the ending plays out
    {   3, KEY_A },             // -> credits
    { 300, 0 },                 // hold for the screenshot
    {   3, KEY_B },             // back to the title
    { 300, 0 },
};
#elif defined(AUTOPLAY_FARM)
// Route for the field. The hub route above never walks to the farm, so the one
// screen this build adds would otherwise have no automated coverage at all --
// and the field is exactly the kind of thing that silently breaks, because it
// is drawn at runtime instead of being baked into the map.
//
// It walks west out of the plaza to the plots, then plants, waters, sleeps a
// night at the spring and comes back to look at what grew. Every step is
// separated by a dwell so a screenshot loop running at 0.4s can catch each
// state rather than only the last one.
static const AutoplayStep script[] = {
    { 120, 0 },                 // let the title settle (and be captured)
    {   3, KEY_A },             // title -> character select
    {  30, 0 },
    {   3, KEY_A },             // character select -> mode select
    {  30, 0 },
    {   3, KEY_A },             // mode select -> hub
    { 180, 0 },                 // dwell: the park with an empty field
    {   3, KEY_SELECT },        // debug: fill the seed satchel
    { 150, 0 },                 // dwell: the satchel panel, full

    // --- walk to the field ------------------------------------------------
    // The hero spawns at the plaza's centre (world 384,344 = tile 24,21).
    // Walking straight west from there runs into the house at tile (17,21) --
    // verified by walking the generated hub map -- so the route steps DOWN
    // first, into a lane that is clear all the way to the bed.
    //
    //   DOWN 12 frames = 3 tiles  -> centre (384,392), tile row 24
    //   LEFT 40 frames = 10 tiles -> centre (224,392), tile col 14
    //
    // That lands the centre inside the field rectangle (144,336)-(240,400) and
    // well inside its 26 px interaction reach. The numbers were derived from
    // the generated map rather than guessed, and the four-frame nudges below
    // (one tile each) keep every subsequent press on the bed: 8-frame nudges
    // would step off its left edge by the fourth press.
    {  12, KEY_DOWN },
    {  40, KEY_LEFT },
    { 180, 0 },                 // dwell: standing at the field, panel swapped

    // --- plant four plots -------------------------------------------------
    // A is the context action, and it works on the plot the hero is standing
    // on. Nudging one tile between presses is what moves the action along the
    // bed, so the four presses plant four different plots.
    {   3, KEY_A },             // plant
    {  30, 0 },
    {   4, KEY_LEFT },
    {   3, KEY_A },
    {  30, 0 },
    {   4, KEY_LEFT },
    {   3, KEY_A },
    {  30, 0 },
    {   4, KEY_LEFT },
    {   3, KEY_A },
    { 180, 0 },                 // dwell: sprouts on the bed

    // --- water the whole bed, then sleep ----------------------------------
    {   3, KEY_B },             // water every planted plot at once
    { 180, 0 },                 // dwell: wet soil (a darker, sheened tile)
    // The spring anchor is tile (34,29), i.e. east 20 tiles and south 5 from
    // the bed. The walk is padded so it still lands inside the landmark's own
    // 48 px reach if the hub layout moves.
    { 100, KEY_RIGHT },
    {  40, KEY_DOWN },
    { 120, 0 },
    {   3, KEY_A },             // the spring: the night passes, crops grow
    { 240, 0 },                 // dwell: the day counter and the toast

    // --- back to the field, to see what the night did ---------------------
    { 100, KEY_LEFT },
    {  40, KEY_UP },
    { 240, 0 },                 // dwell: the crops a stage further on
};

#else
static const AutoplayStep script[] = {
    { 240, 0 },                 // let the title settle (and be captured)
    {   3, KEY_A },             // title menu -> character select
    {  20, 0 },
    {   3, KEY_A },             // character select -> mode select
    {  30, 0 },
    {   3, KEY_A },             // mode select -> single-player hub
    // The hub is a screen worth verifying on its own (it binds the hub world
    // and its own camera), and a dwell of a second is too short to catch it
    // with an external screenshot loop.
    { 150, 0 },
    { 100, KEY_UP },            // walk north out of the plaza to the gate
    {   3, KEY_A },             // gate -> world map
    {   3, KEY_RIGHT },         // move the cursor off the first node, so a
    {  20, 0 },                 // screenshot shows the chart marks the selection
    { 240, 0 },                 // dwell: the map is a screen worth capturing
    {   3, KEY_LEFT },          // back onto the first node before starting it
    {  20, 0 },
    {   3, KEY_A },             // world map -> play
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

// A one-shot tap for routes that need to press a specific touch button once at
// a known frame -- the hub's shortcut bar is reachable no other way from a
// script, because it has no D-pad binding.
#if defined(AUTOPLAY_UI)
#define TAP_ONCE_FRAME 790     // the hub, after the skill-tree visit
#define TAP_ONCE_X      96     // the second shortcut: 无线联机
#define TAP_ONCE_Y     169
#endif

void input_poll(void)
{
    if (g_left < 0)
        g_left = script[0].frames;

    uint16_t keys = script[g_step].keys;
    g_down = (uint16_t)(keys & ~g_held);   // edge detection for "just pressed"
    g_held = keys;

#if defined(AUTOPLAY_TAP_CARD)
    int phase = g_script_frame % TAP_PERIOD;
    bool down = (phase < TAP_DOWN);
    g_touch_pressed = down && !g_touch_down;
    g_touch_down = down;
    if (down)
    {
        g_touch_x = TAP_X;
        g_touch_y = TAP_Y;
    }
#endif   // AUTOPLAY_TAP_CARD

#ifdef AUTOPLAY_UI
    // One tap, one frame long, at a scripted frame and position. The edge
    // detection above means the game sees a press only on the frame the finger
    // goes down, which is what a real panel produces.
    if (g_script_frame == TAP_ONCE_FRAME ||
        g_script_frame == TAP_ONCE_FRAME + 1)
    {
        g_touch_pressed = (g_script_frame == TAP_ONCE_FRAME);
        g_touch_down = true;
        g_touch_x = TAP_ONCE_X;
        g_touch_y = TAP_ONCE_Y;
    }
    else
    {
        g_touch_pressed = false;
        g_touch_down = false;
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

// Diagnostics for the screen-capture loop: which script step is live and how
// far into it we are. Drawn by the UI route so a stalled run shows WHERE it
// stalled instead of presenting a frozen menu with no story.
int autoplay_debug_step(void) { return g_step; }
int autoplay_debug_left(void) { return g_left; }
int autoplay_debug_frame(void) { return g_script_frame; }

#endif  // AUTOPLAY
