// The Black 2 / White 2 control kit.
//
// Every screen draws its boxes, buttons, bars, wells and cursors through these
// functions, so "what a button looks like" lives in exactly one file instead of
// being re-invented as two nested rectangles at each call site. The language it
// implements is the Black 2 / White 2 menu family:
//
//   * near-white plates with a hard navy outline and a soft blue falloff toward
//     the bottom edge, so a plate reads as a lit object rather than a hole;
//   * ONE saturated blue that means "selected" and is never used for anything
//     else -- it is the only colour on a screen the eye reads as "press me",
//     which is what makes a menu navigable at a glance;
//   * recessed dark navy wells for anything that holds a value (bars, readouts,
//     key caps), because a value has to look like it sits *in* something;
//   * chunky rounded corners everywhere. A 1px square corner is the single
//     biggest reason a plain rectangle reads as "text in a box" rather than as
//     a control -- which is exactly the complaint that got this file written.
//
// Both screens are served by the same call, because the sub screen is the menus
// and the main screen carries the in-play HUD over the world; a HUD drawn in a
// different style from the menus would look like a different game.

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Which of the two 8bpp bitmaps a primitive writes into.
typedef enum {
    UI_SUB = 0,     // bottom screen: menus, lists, touch controls
    UI_MAIN = 1,    // top screen: the in-play HUD, drawn over the world
} UiScreen;

// Button / plate states. Selection is the blue one; OFF is a greyed-out
// control; PRESS is the momentary "finger is on it" state, which has to look
// different from OFF or a tap gives no feedback at all.
typedef enum {
    UI_ON = 0,
    UI_SEL,
    UI_OFF,
    UI_PRESS,
} UiState;

// Corner radii, named by role rather than by number so a screen does not have
// to know that a tab and a badge happen to share a radius.
#define UI_R_TAB     3
#define UI_R_BTN     5
#define UI_R_PLATE   6
#define UI_R_BIG     8

// ---------------------------------------------------------------------------
// Background
// ---------------------------------------------------------------------------

// The blue field every menu sits on: a vertical gradient plus two soft light
// pools. A flat fill makes plates look pasted on; the gradient is what gives
// the screen any depth at all.
void ui_field(UiScreen scr);

// ---------------------------------------------------------------------------
// Plates and wells
// ---------------------------------------------------------------------------

// A filled rounded rectangle in one colour.
void ui_round(UiScreen scr, int x, int y, int w, int h, int r, uint8_t fill);

// Rounded rectangle with a hard 1px outline, in the given state's colours.
void ui_plate_st(UiScreen scr, int x, int y, int w, int h, int r, UiState st);

// The default plate / the two special cases worth naming.
void ui_plate(UiScreen scr, int x, int y, int w, int h, int r);
void ui_plate_sel(UiScreen scr, int x, int y, int w, int h, int r);
void ui_plate_off(UiScreen scr, int x, int y, int w, int h, int r);

// A recessed dark navy well: bars, readouts, minimap frames, key caps. `fill`
// is the interior (UI2_SCREEN for a value track, or a solid colour for a key
// cap); pass it explicitly because a key cap is a well that happens to be blue.
void ui_well(UiScreen scr, int x, int y, int w, int h, int r, uint8_t fill);

// ---------------------------------------------------------------------------
// Bars
// ---------------------------------------------------------------------------

// A value bar: outline, recessed track, gradient fill, lit leading edge. The
// leading edge is what makes a nearly-full bar readable without measuring it
// against the frame, and the track is what makes an empty bar look empty
// rather than broken.
//
// `pct` is 0..100. A zero-width fill draws the track only.
void ui_bar(UiScreen scr, int x, int y, int w, int h, int pct,
            uint8_t c_lo, uint8_t c_hi);

// Same, but with an explicit pixel count instead of a percentage: cooldown
// meters already count frames and converting to percent would only lose
// precision.
void ui_bar_px(UiScreen scr, int x, int y, int w, int h, int fill_w,
               uint8_t c_lo, uint8_t c_hi);

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

// A touch button (or a menu row styled as one). Draws frame and fill only;
// put the label on with ui_label / ui_label_st.
void ui_btn(UiScreen scr, int x, int y, int w, int h, UiState st);

// A small key cap: a rounded badge holding one letter, as used by the A/B/X/Y
// skill slots. `lit` selects the blue cap over the neutral one.
void ui_keycap(UiScreen scr, int x, int y, int w, int h, const char *key,
               bool lit);

// Width a key cap needs for its glyph, so a caller can lay a row of them out
// without hardcoding a width per key.
int  ui_keycap_w(const char *key);

// A region/status tab: the small rounded chip used for the five map nodes and
// the collectible counters.
void ui_tab(UiScreen scr, int x, int y, int w, int h, UiState st,
            const char *label);

// ---------------------------------------------------------------------------
// Text
//
// The kit's own text helpers exist for one reason: a glyph's ink sits on rows
// 2..12 of its 16px cell, so centring on the full cell puts every label about
// a pixel high. These centre on the ink instead, which is what makes a label
// look centred in its box rather than merely close to it.
// ---------------------------------------------------------------------------

void ui_text(UiScreen scr, int x, int y, const char *s, uint8_t ink);

// Text at x, vertically centred on `cy`. This is the form every list row and
// button wants: rows are laid out by their centre line, and the ink offset is
// a property of the font rather than of the row.
void ui_text_mid(UiScreen scr, int x, int cy, const char *s, uint8_t ink);

// Text ending flush at `right_x`, vertically centred on `cy`. For the value
// column of a settings row, where the numbers have to line up on their right
// edge as the names change width.
void ui_text_right(UiScreen scr, int right_x, int cy, const char *s,
                   uint8_t ink);

// Centred both ways inside the box. Returns the drawn width.
int  ui_label(UiScreen scr, int bx, int by, int bw, int bh, const char *s,
              uint8_t ink);

int  ui_label_st(UiScreen scr, int bx, int by, int bw, int bh, const char *s,
                 UiState st);

// Text with a hard 1px outline, for anything drawn straight over the world
// where the background can be any colour the terrain happens to be. Nine
// passes is cheap for the few short strings the play HUD needs, and it is the
// only way white text stays legible over a snowfield.
void ui_text_lit(UiScreen scr, int x, int y, const char *s, uint8_t ink);

// ---------------------------------------------------------------------------
// Cursors
// ---------------------------------------------------------------------------

// The chunky rounded arrow of a selected list row or card. Points right; the
// caller flips it by using ui_cursor_l. (x, y) is the tip's vertical centre.
void ui_cursor(UiScreen scr, int x, int y, uint8_t color);
void ui_cursor_l(UiScreen scr, int x, int y, uint8_t color);

// A downward-pointing caret, for "this is the card you are choosing" above a
// grid of them.
void ui_caret_down(UiScreen scr, int cx, int y, uint8_t color);

// ---------------------------------------------------------------------------
// Small composite pieces
// ---------------------------------------------------------------------------

// A panel header: a blue tab strip with a title in it, drawn across the top
// inside edge of a plate. Saves every screen from re-deriving the same inset.
void ui_header_tab(UiScreen scr, int x, int y, int w, const char *title);

// A diamond bullet, used to mark a completed entry on the ingredient list.
void ui_bullet(UiScreen scr, int x, int y, bool done);

// Convenience: the ink colour a plate state expects. Screens that draw a label
// separately from its button need this so the two cannot disagree.
uint8_t ui_ink(UiState st);
