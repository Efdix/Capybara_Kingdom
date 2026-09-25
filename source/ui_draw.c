// The UI control kit -- implementation.
//
// Everything here is plain rectangles and 1px spans written straight into the
// 8bpp shadow bitmap of whichever screen was asked for. No sprites, no tiles,
// no separate asset: the DS has two 256x192 bitmaps and this kit is what makes
// boxes drawn into them look like objects.
//
// The look is the DS-era Nintendo menu language, and it comes down to four
// rules that every control below obeys:
//
//   1. FLAT FILLS WITH A BEVEL, never a smooth gradient. A raised control is a
//      light body, a 1px bright line along its top inside edge, and a 2px
//      shadow along its bottom one. That top-light / bottom-dark pair is the
//      whole illusion; anything more elaborate fights the pixel grid.
//   2. CHECKERBOARD SEAMS between adjacent tones, not dashed lines. Where two
//      fills have to meet softly, the seam row alternates the two colours
//      pixel by pixel. A 2px dash pattern (an earlier version of this file)
//      reads as a rendering fault rather than as a blend.
//   3. ONE ACCENT COLOUR. A single saturated blue means "this is selected" and
//      is never used for decoration. It is the only colour on a screen the eye
//      reads as "press me", which is what makes a menu navigable at a glance.
//   4. RECESSED AREAS ARE LIT UPSIDE DOWN -- dark along the top inner edge,
//      light along the bottom. That inversion is the only cue separating "a
//      value goes in here" from "press me", so nothing else may use it.
//
// Corner radii are the pixel-art approximation of a quarter circle rather than
// a mathematical arc: at 3..8px a true circle leaves a notch that reads as
// damage to the shape.

#include <string.h>

#include "game.h"
#include "ui_draw.h"

#define UI_STRIDE  256

// One row of pixels, on the screen the caller asked for. Every primitive below
// funnels through here, so the two screens cannot drift apart.
static inline void ui_fill(UiScreen scr, int x, int y, int w, int h,
                           uint8_t col)
{
    if (scr == UI_SUB)
        sub_fill_rect(x, y, w, h, col);
    else
        top_fill_rect(x, y, w, h, col);
}

static void ui_text_at(UiScreen scr, int x, int y, const char *s, uint8_t ink)
{
    if (scr == UI_SUB)
        text_draw_px(x, y, s, ink);
    else
        top_text_draw_px(x, y, s, ink);
}

// ---------------------------------------------------------------------------
// Rounded rectangles
// ---------------------------------------------------------------------------

// Horizontal inset of each corner row, indexed [radius][row from that end].
//
// Derived from the quarter circle of the same radius, taking the first whole
// pixel column that falls inside it. r=6 gives insets 4,2,1,1,0,0: a corner
// that cuts four pixels off the first row and then opens out fast, which is
// the silhouette a DS menu plate has. A true arc at this size would leave a
// visible stair-step notch instead.
static const uint8_t s_corner[9][8] = {
    { 0 },                          // r = 0 -- a plain rectangle
    { 0 },                          // r = 1 -- indistinguishable from r = 0
    { 1, 0 },                       // r = 2
    { 1, 0, 0 },                    // r = 3
    { 2, 1, 0, 0 },                 // r = 4
    { 3, 1, 1, 0, 0 },              // r = 5
    { 4, 2, 1, 1, 0, 0 },           // r = 6
    { 4, 3, 2, 1, 0, 0, 0 },        // r = 7
    { 5, 3, 2, 1, 1, 0, 0, 0 },     // r = 8
};

static inline int clamp_radius(int r, int w, int h)
{
    int half = (w < h ? w : h) / 2;
    if (r > half) r = half;
    if (r > 8) r = 8;
    return r < 0 ? 0 : r;
}

// Inset for the corner row `j` of a shape `h` tall with radius `r`. Shared by
// every layer of a control so its fill, bevel and outline stay concentric --
// computing it separately in each is what makes a framed box look lopsided by
// a pixel.
static inline int corner_inset(int r, int h, int j)
{
    if (r < 2)
        return 0;
    if (j < r)
        return s_corner[r][j];
    if (j >= h - r)
        return s_corner[r][h - 1 - j];
    return 0;
}

static void round_spans(UiScreen scr, int x, int y, int w, int h, int r,
                        uint8_t col)
{
    if (w <= 0 || h <= 0)
        return;
    r = clamp_radius(r, w, h);

    if (r < 2)
    {
        ui_fill(scr, x, y, w, h, col);
        return;
    }

    for (int j = 0; j < h; j++)
    {
        int inset = corner_inset(r, h, j);
        int iw = w - inset * 2;
        if (iw > 0)
            ui_fill(scr, x + inset, y + j, iw, 1, col);
    }
}

// The seam row between two fill tones: a 1px checkerboard of both colours.
//
// Half the pixels are each colour, so at normal viewing distance they average
// into the tone between them -- which is how a 3-tone palette gets a 5-step
// gradient without spending palette entries. A dashed seam (which this used to
// be) does not average: the eye resolves 2-on/2-off as a stripe pattern and it
// reads as corruption.
//
// One row per seam, and only where the shape is wide enough for the seam to be
// worth having -- at the width of a key cap the alternation is just noise.
static void seam_row(UiScreen scr, int x, int y, int w, uint8_t a, uint8_t b)
{
    if (w < 12)
        return;
    for (int i = 0; i < w; i++)
        ui_fill(scr, x + i, y, 1, 1, ((x + i + y) & 1) ? b : a);
}

// A rounded rectangle filled top-to-bottom with a list of solid bands, with a
// checkerboard seam between each pair.
//
// `rows[i]` is the height of band i and the caller guarantees they sum to `h`.
// This is the primitive every plate, button and bar is built from; a control's
// whole appearance is its band list.
static void round_bevel(UiScreen scr, int x, int y, int w, int h, int r,
                        const uint8_t *cols, const uint8_t *rows, int nband)
{
    if (w <= 0 || h <= 0 || nband <= 0)
        return;
    r = clamp_radius(r, w, h);

    int j = 0;
    for (int b = 0; b < nband && j < h; b++)
    {
        int bh = rows[b];
        if (j + bh > h)
            bh = h - j;
        if (bh <= 0)
            continue;

        uint8_t col = cols[b];
        for (int k = 0; k < bh; k++, j++)
        {
            int inset = corner_inset(r, h, j);
            int iw = w - inset * 2;
            if (iw <= 0)
                continue;
            ui_fill(scr, x + inset, y + j, iw, 1, col);

            // Blend into the next band on this band's last row.
            if (k == bh - 1 && b + 1 < nband)
                seam_row(scr, x + inset, y + j, iw, col, cols[b + 1]);
        }
    }
}

// ---------------------------------------------------------------------------
// Background
// ---------------------------------------------------------------------------

// The blue field every menu sits on: a light-to-mid blue vertical wash with a
// faint dot grid.
//
// The wash is six hard bands with checkerboard seams, which is enough steps to
// read as a gradient while costing only the palette tones the UI already has.
// The dots are the DS-menu tell -- a flat field looks like a missing asset, and
// a dot grid reads as texture without competing with anything drawn on top.
void ui_field(UiScreen scr)
{
    static const uint8_t cols[6] = {
        PAL_UI2_BG_HI, PAL_UI2_BG_HI, PAL_UI2_BG,
        PAL_UI2_BG,    PAL_UI2_BG,    PAL_UI2_PLATE_DEEP,
    };
    // Seams at 18%, 42% and 78%: the field is brightest at the top, holds its
    // mid tone across the middle where the panels sit, and darkens only at the
    // very bottom.
    int h = AREA_H;
    uint8_t rows[6] = {
        (uint8_t)(h * 18 / 100), (uint8_t)(h * 12 / 100),
        (uint8_t)(h * 12 / 100), (uint8_t)(h * 24 / 100),
        (uint8_t)(h * 20 / 100), 0,
    };
    rows[5] = (uint8_t)(h - rows[0] - rows[1] - rows[2] - rows[3] - rows[4]);

    round_bevel(scr, 0, 0, AREA_W, h, 0, cols, rows, 6);

    // The dot grid: one pixel every 12, only a little darker than the band it
    // sits on. A flat field reads as a missing asset, but a coarse or dark grid
    // competes with everything drawn on top -- it has to be found, not seen.
    for (int y = 20; y < h; y += 12)
    {
        for (int x = ((y / 12) & 1) ? 6 : 0; x < AREA_W; x += 12)
            ui_fill(scr, x, y, 1, 1, PAL_UI2_DOT);
    }
}

// ---------------------------------------------------------------------------
// Plates and wells
// ---------------------------------------------------------------------------

// The band list for each button state. Each entry is: how many rows of which
// colour, from the top down.
//
// The shapes are deliberately consistent -- 1px outline, 2px lit top, body,
// 2px shaded bottom, 1px outline -- so that a white button and a blue one read
// as the same object in different colours rather than as two different widgets.
static void plate_bands(UiState st, const uint8_t **cols, const uint8_t **rows,
                        int *nband, int h)
{
    static const uint8_t on_c[4]  = { PAL_UI2_PLATE_HI, PAL_UI2_PLATE,
                                      PAL_UI2_PLATE_LO, PAL_UI2_PLATE_DEEP };
    static const uint8_t sel_c[4] = { PAL_UI2_BLUE_GLOW, PAL_UI2_BLUE_HI,
                                      PAL_UI2_BLUE, PAL_UI2_BLUE_DARK };
    static const uint8_t off_c[4] = { PAL_UI2_PLATE_DEEP, PAL_UI2_PLATE_DEEP,
                                      PAL_UI2_PLATE_DEEP, PAL_UI2_PLATE_LO };
    static const uint8_t prs_c[4] = { PAL_UI2_BLUE_HI, PAL_UI2_BLUE,
                                      PAL_UI2_BLUE_DARK, PAL_UI2_LINE };
    static uint8_t r[4];

    switch (st)
    {
    case UI_SEL:   *cols = sel_c; break;
    case UI_OFF:   *cols = off_c; break;
    case UI_PRESS: *cols = prs_c; break;
    default:       *cols = on_c;  break;
    }

    // Two rows for the lit top and two for the shaded bottom, with the body
    // taking what is left. Buttons in this game are 22..50px tall, so that
    // leaves a body of at least 18px -- the bevel never eats the label.
    int top = 2, bot = 2;
    if (h < 16) { top = 1; bot = 1; }
    if (h < 10) { top = 0; bot = 0; }

    r[0] = (uint8_t)top;
    r[1] = (uint8_t)(h - top - bot - 2);
    r[2] = (uint8_t)bot;
    r[3] = 2;
    *rows = r;
    *nband = 4;
}

void ui_plate_st(UiScreen scr, int x, int y, int w, int h, int r, UiState st)
{
    if (w <= 0 || h <= 0)
        return;

    // Outline first, then the bevel inset by one pixel so the bands sit inside
    // it. Drawn as two calls rather than as outline bands so the outer edge
    // stays a single clean colour in every state.
    uint8_t edge = (st == UI_OFF) ? PAL_UI2_LINE_SOFT : PAL_UI2_LINE;
    round_spans(scr, x, y, w, h, r, edge);
    if (w <= 2 || h <= 2)
        return;

    const uint8_t *cols, *rows;
    int nband;
    plate_bands(st, &cols, &rows, &nband, h - 2);
    round_bevel(scr, x + 1, y + 1, w - 2, h - 2, r - 1, cols, rows, nband);
}

void ui_plate(UiScreen scr, int x, int y, int w, int h, int r)
{
    ui_plate_st(scr, x, y, w, h, r, UI_ON);
}

void ui_plate_sel(UiScreen scr, int x, int y, int w, int h, int r)
{
    ui_plate_st(scr, x, y, w, h, r, UI_SEL);
}

void ui_plate_off(UiScreen scr, int x, int y, int w, int h, int r)
{
    ui_plate_st(scr, x, y, w, h, r, UI_OFF);
}

void ui_round(UiScreen scr, int x, int y, int w, int h, int r, uint8_t fill)
{
    round_spans(scr, x, y, w, h, r, fill);
}

void ui_well(UiScreen scr, int x, int y, int w, int h, int r, uint8_t fill)
{
    if (w <= 0 || h <= 0)
        return;

    round_spans(scr, x, y, w, h, r, PAL_UI2_LINE);
    if (w <= 2 || h <= 2)
        return;
    round_spans(scr, x + 1, y + 1, w - 2, h - 2,
                clamp_radius(r - 1, w - 2, h - 2), fill);

    // Inverted lighting, and kept clear of the rounded ends where a straight
    // line would break the corner. A well is the one place this kit is allowed
    // to be dark on top.
    int ins = (r >= 2) ? r : 2;
    int iw = w - 2 - ins * 2;
    if (iw >= 2)
    {
        ui_fill(scr, x + 1 + ins, y + 1, iw, 1, PAL_UI2_SHADOW);
        if (h > 3)
            ui_fill(scr, x + 1 + ins, y + h - 2, iw, 1, PAL_UI2_LINE_SOFT);
    }
}

// ---------------------------------------------------------------------------
// Bars
// ---------------------------------------------------------------------------

void ui_bar_px(UiScreen scr, int x, int y, int w, int h, int fill_w,
               uint8_t c_lo, uint8_t c_hi)
{
    ui_well(scr, x, y, w, h, UI_R_TAB, PAL_UI2_SCREEN);

    int maxw = w - 4;
    if (fill_w > maxw) fill_w = maxw;
    if (fill_w < 2 || h < 8)
        return;

    // Track, then a lit top half, then a bright cap at the leading end. The
    // cap is what makes a nearly-full bar readable without measuring it
    // against the frame, and the track under it is what makes an empty bar
    // look empty rather than broken.
    int fh = h - 4;
    static const uint8_t rows[2] = { 0, 0 };
    (void)rows;
    round_spans(scr, x + 2, y + 2, fill_w, fh, 2, c_lo);
    round_spans(scr, x + 2, y + 2, fill_w, (fh + 1) / 2, 2, c_hi);
    ui_fill(scr, x + 2 + fill_w - 1, y + 3, 1, fh - 2, PAL_WHITE_SPARK);
}

void ui_bar(UiScreen scr, int x, int y, int w, int h, int pct,
            uint8_t c_lo, uint8_t c_hi)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    int inner = w - 4;
    int fill = (inner * pct) / 100;
    // A non-zero value always shows at least a sliver: a bar that reads empty
    // while the number beside it is not zero looks like a bug.
    if (pct > 0 && fill < 3)
        fill = 3;

    ui_bar_px(scr, x, y, w, h, fill, c_lo, c_hi);
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

void ui_btn(UiScreen scr, int x, int y, int w, int h, UiState st)
{
    ui_plate_st(scr, x, y, w, h, UI_R_BTN, st);
}

void ui_keycap(UiScreen scr, int x, int y, int w, int h, const char *key,
               bool lit)
{
    // A key cap is a well with a lit face, not a small button: it labels the
    // control the player presses on the *console*, so it should look like a
    // printed key rather than like something to tap.
    if (lit)
    {
        ui_plate_st(scr, x, y, w, h, UI_R_TAB, UI_SEL);
        ui_label(scr, x, y, w, h, key, PAL_WHITE);
    }
    else
    {
        ui_plate_st(scr, x, y, w, h, UI_R_TAB, UI_ON);
        ui_label(scr, x, y, w, h, key, PAL_UI2_INK);
    }
}

// Width a key cap needs for its glyph: one Latin character or one arrow fits in
// 16px, a two-glyph run ("←→") needs more. Derived rather than hardcoded so a
// cap can never clip its own glyph.
int ui_keycap_w(const char *key)
{
    int w = text_utf8_width(key) + 6;
    return w < 16 ? 16 : w;
}

void ui_tab(UiScreen scr, int x, int y, int w, int h, UiState st,
            const char *label)
{
    ui_plate_st(scr, x, y, w, h, UI_R_TAB, st);
    ui_label_st(scr, x, y, w, h, label, st);
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

// Ink rows 2..12 of the 16px cell, measured off the atlas rather than derived
// from the cell size: the glyphs are drawn centred in their cell with a margin,
// so the ink centre sits on row 7, not on row 8.
#define INK_TOP     2
#define INK_CENTRE  7

int ui_label(UiScreen scr, int bx, int by, int bw, int bh, const char *s,
             uint8_t ink)
{
    int w = text_utf8_width(s);

    // Horizontal centring has to account for the ink inset: a cell is 12px wide
    // but its ink spans columns 2..9, so a string of n cells has 2px of leading
    // and 3px of trailing slack. Centring the CELL box therefore pushes the
    // visible text about a pixel and a half left of centre, which is exactly
    // the kind of thing that makes a label look wrong without looking wrong.
    int x = bx + (bw - w + 1) / 2;
    if (x < bx)
        x = bx;

    ui_text_at(scr, x, by + bh / 2 - INK_CENTRE, s, ink);
    return w;
}

int ui_label_st(UiScreen scr, int bx, int by, int bw, int bh, const char *s,
                UiState st)
{
    return ui_label(scr, bx, by, bw, bh, s, ui_ink(st));
}

uint8_t ui_ink(UiState st)
{
    switch (st)
    {
    case UI_SEL:
    case UI_PRESS:
        return PAL_WHITE;
    case UI_OFF:
        return PAL_UI2_INK_DIM;
    default:
        return PAL_UI2_INK;
    }
}

void ui_text(UiScreen scr, int x, int y, const char *s, uint8_t ink)
{
    ui_text_at(scr, x, y, s, ink);
}

void ui_text_mid(UiScreen scr, int x, int cy, const char *s, uint8_t ink)
{
    ui_text_at(scr, x, cy - INK_CENTRE, s, ink);
}

void ui_text_right(UiScreen scr, int right_x, int cy, const char *s,
                   uint8_t ink)
{
    // The last cell carries 3px of trailing slack, so a right-aligned string
    // has to add it back or values of different widths end at visibly different
    // distances from the edge.
    int x = right_x - text_utf8_width(s) + 3;
    ui_text_at(scr, x, cy - INK_CENTRE, s, ink);
}

void ui_text_lit(UiScreen scr, int x, int y, const char *s, uint8_t ink)
{
    // Eight offset passes of the shadow colour, then the text. The offsets are
    // the four sides plus the diagonals: the four sides alone leave the corners
    // open and light text over a bright tile bleeds through them.
    static const int8_t off[8][2] = {
        { -1,  0 }, { 1,  0 }, { 0, -1 }, { 0,  1 },
        { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 },
    };
    for (int i = 0; i < 8; i++)
        ui_text_at(scr, x + off[i][0], y + off[i][1], s, PAL_UI2_SHADOW);
    ui_text_at(scr, x, y, s, ink);
}

// ---------------------------------------------------------------------------
// Cursors
// ---------------------------------------------------------------------------

// Half-widths of the selection arrow, top row first. The leading edge is
// convex -- 2,3,4,5,7 -- which is what makes it read as a rounded pointer
// rather than as a triangle.
static const uint8_t s_arrow_w[9] = { 2, 3, 4, 5, 7, 5, 4, 3, 2 };

void ui_cursor(UiScreen scr, int x, int y, uint8_t color)
{
    ui_fill(scr, x - 1, y - 5, 3, 1, PAL_UI2_LINE);
    ui_fill(scr, x - 1, y + 5, 3, 1, PAL_UI2_LINE);
    for (int dy = -4; dy <= 4; dy++)
        ui_fill(scr, x - 1, y + dy, s_arrow_w[dy + 4] + 1, 1, PAL_UI2_LINE);
    for (int dy = -4; dy <= 4; dy++)
        ui_fill(scr, x, y + dy, s_arrow_w[dy + 4], 1, color);
}

void ui_cursor_l(UiScreen scr, int x, int y, uint8_t color)
{
    // Mirror of ui_cursor: the right edge is the flat one, so every row is
    // right-aligned on a common edge instead of left-aligned.
    ui_fill(scr, x + 5, y - 5, 3, 1, PAL_UI2_LINE);
    ui_fill(scr, x + 5, y + 5, 3, 1, PAL_UI2_LINE);
    for (int dy = -4; dy <= 4; dy++)
    {
        int w = s_arrow_w[dy + 4];
        ui_fill(scr, x + 7 - w, y + dy, w + 1, 1, PAL_UI2_LINE);
    }
    for (int dy = -4; dy <= 4; dy++)
    {
        int w = s_arrow_w[dy + 4];
        ui_fill(scr, x + 8 - w, y + dy, w, 1, color);
    }
}

void ui_caret_down(UiScreen scr, int cx, int y, uint8_t color)
{
    ui_fill(scr, cx - 6, y - 1, 13, 1, PAL_UI2_LINE);
    for (int d = 0; d <= 5; d++)
    {
        int w = 11 - d * 2;
        if (w < 1) w = 1;
        ui_fill(scr, cx - w / 2 - 1, y + d, w + 2, 1, PAL_UI2_LINE);
    }
    ui_fill(scr, cx - 1, y + 6, 3, 1, PAL_UI2_LINE);

    for (int d = 0; d <= 5; d++)
    {
        int w = 11 - d * 2;
        if (w < 1) w = 1;
        ui_fill(scr, cx - w / 2, y + d, w, 1, color);
    }
}

// ---------------------------------------------------------------------------
// Composite pieces
// ---------------------------------------------------------------------------

void ui_header_tab(UiScreen scr, int x, int y, int w, const char *title)
{
    ui_well(scr, x, y, w, 22, UI_R_TAB, PAL_UI2_SCREEN);
    ui_fill(scr, x + 1 + UI_R_TAB, y + 2, w - 2 - UI_R_TAB * 2, 1,
            PAL_UI2_BLUE_DARK);
    ui_label(scr, x, y + 1, w, 20, title, PAL_WHITE);
}

// A 5x5 status dot: a solid rounded square when the item is done, a hollow ring
// when it is not. Two states that differ by fill rather than by colour, so they
// stay distinguishable at a glance and in a greyscale screenshot.
void ui_bullet(UiScreen scr, int x, int y, bool done)
{
    if (done)
    {
        ui_round(scr, x, y, 5, 5, 2, PAL_UI2_GREEN);
        // One lit pixel, so the dot reads as a bead rather than as a square.
        ui_fill(scr, x + 1, y + 1, 1, 1, PAL_UI2_BLUE_GLOW);
        return;
    }

    uint8_t c = PAL_UI2_INK_DIM;
    ui_fill(scr, x + 1, y, 3, 1, c);
    ui_fill(scr, x, y + 1, 1, 3, c);
    ui_fill(scr, x + 4, y + 1, 1, 3, c);
    ui_fill(scr, x + 1, y + 4, 3, 1, c);
}
