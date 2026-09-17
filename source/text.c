// CJK text layer for the sub screen.
//
// The libnds console only knows ASCII and blocksds ships no CJK font, so the
// game carries its own atlas: tools/gen_font.py renders every character the
// script uses with SimHei at 12px, thresholds it to 1bpp, and packs it into a
// 16x16 glyph cell. Font_chars.h lists the atlas order, so mapping a UTF-8
// character to its glyph is a string search over FONT_CHARS (403 glyphs --
// a linear scan per character is fine, and results are cached by the caller's
// static text layout).
//
// We draw into a RAM shadow bitmap for the sub screen (8bpp, same global
// palette) rather than using the console, which keeps the bottom screen
// visually consistent with the game.

#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "game.h"
#include "assets.h"
#include "font_chars.h"

#define SUB_STRIDE  256
#define VIS_ROWS    AREA_H

extern uint8_t *text_sub_buffer(void);

// We keep our own pointer to the sub shadow buffer (defined in render.c).
static uint8_t *g_sub = NULL;

// Character cell used for the text grid.
#define CELL_W  12
#define CELL_H  14
#define GRID_COLS 20
#define GRID_ROWS 13

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void text_init(void)
{
    g_sub = text_sub_buffer();
    text_clear();
}

// ---------------------------------------------------------------------------
// Glyph lookup
// ---------------------------------------------------------------------------

// Decode one UTF-8 character, returning the codepoint and advancing *s.
static uint32_t utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp = 0;
    int extra = 0;

    if (p[0] < 0x80) { cp = p[0]; extra = 0; }
    else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; extra = 1; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; extra = 2; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; extra = 3; }
    else { (*s)++; return 0; }

    for (int i = 1; i <= extra; i++)
    {
        if ((p[i] & 0xC0) != 0x80) { *s += i; return 0; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += extra + 1;
    return cp;
}

// Atlas index of a codepoint, or -1. FONT_CHARS is UTF-8, so we walk it with
// the same decoder.
static int glyph_index(uint32_t cp)
{
    const char *s = FONT_CHARS;
    for (int i = 0; i < FONT_GLYPH_COUNT; i++)
    {
        if (*s == '\0')
            break;
        uint32_t c = utf8_next(&s);
        if (c == cp)
            return i;
    }
    return -1;
}

// Cache the last lookups: dialogue redraws the same text every frame, and the
// linear scan would otherwise dominate the frame budget.
#define LCACHE_SIZE 64
static struct { uint32_t cp; int idx; } g_lcache[LCACHE_SIZE];
static int g_lcache_next = 0;

static int glyph_index_cached(uint32_t cp)
{
    for (int i = 0; i < LCACHE_SIZE; i++)
    {
        // idx is stored biased by +1 so that 0 marks an empty slot.
        if (g_lcache[i].idx != 0 && g_lcache[i].cp == cp)
            return g_lcache[i].idx - 1;
    }
    int idx = glyph_index(cp);
    g_lcache[g_lcache_next].cp = cp;
    g_lcache[g_lcache_next].idx = idx + 1;
    g_lcache_next = (g_lcache_next + 1) % LCACHE_SIZE;
    return idx;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// Draw one atlas glyph (4bpp, 1 = ink) at pixel position (px, py).
static void draw_glyph(int index, int px, int py, uint8_t ink)
{
    if (index < 0 || index >= FONT_GLYPH_COUNT)
        return;
    const uint8_t *src = font_glyphs_bin + (size_t)index * GLYPH_BYTES;

    // 4bpp 16x16: four 8x8 tiles (TL, TR, BL, BR), 32 bytes each, two pixels
    // per byte (low nibble = left pixel).
    for (int tile = 0; tile < 4; tile++)
    {
        int tx = (tile & 1) * 8;
        int ty = (tile >> 1) * 8;
        const uint8_t *t = src + tile * 32;
        for (int y = 0; y < 8; y++)
        {
            for (int bx = 0; bx < 4; bx++)
            {
                uint8_t v = t[y * 4 + bx];
                int lo = v & 0x0F;
                int hi = (v >> 4) & 0x0F;
                if (hi)
                {
                    int dx = px + tx + bx * 2 + 1;
                    int dy = py + ty + y;
                    if ((unsigned)dx < AREA_W && (unsigned)dy < VIS_ROWS)
                        g_sub[dy * SUB_STRIDE + dx] = ink;
                }
                if (lo)
                {
                    int dx = px + tx + bx * 2;
                    int dy = py + ty + y;
                    if ((unsigned)dx < AREA_W && (unsigned)dy < VIS_ROWS)
                        g_sub[dy * SUB_STRIDE + dx] = ink;
                }
            }
        }
    }
}

void text_clear(void)
{
    if (!g_sub)
        g_sub = text_sub_buffer();
    // Fill with the panel color rather than index 0: the sub screen is a
    // bitmap layer, so index 0 would display as the (magenta) transparent
    // marker color instead of a proper background.
    memset(g_sub, PAL_UI_PANEL, SUB_STRIDE * VIS_ROWS);
}

void text_draw(int col, int row, const char *utf8, uint16_t color)
{
    if (!g_sub)
        g_sub = text_sub_buffer();
    uint8_t ink = (uint8_t)color;

    int px = col * CELL_W;
    int py = row * CELL_H;
    const char *s = utf8;

    while (*s)
    {
        uint32_t cp = utf8_next(&s);
        if (cp == '\n')
        {
            px = col * CELL_W;
            py += CELL_H;
            continue;
        }
        if (cp == ' ')
        {
            px += CELL_W;
            continue;
        }
        int idx = glyph_index_cached(cp);
        if (idx >= 0)
        {
            draw_glyph(idx, px, py, ink);
            px += CELL_W;
        }
        else
        {
            // Unknown glyph: draw a hollow box so missing characters are
            // obvious during development instead of silently vanishing.
            for (int i = 0; i < 10; i++)
            {
                int dx = px + i, dy = py + 2;
                if ((unsigned)dx < AREA_W && (unsigned)dy < VIS_ROWS)
                    g_sub[dy * SUB_STRIDE + dx] = ink;
                dy = py + 11;
                if ((unsigned)dx < AREA_W && (unsigned)dy < VIS_ROWS)
                    g_sub[dy * SUB_STRIDE + dx] = ink;
            }
            px += CELL_W;
        }
    }
}

void text_printf(int col, int row, uint16_t color, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    text_draw(col, row, buf, color);
}

// A framed panel: fills a dark rounded-looking box with a border, sized in
// cells. Used for dialogue boxes and menus.
void text_panel(int col, int row, int w, int h)
{
    int x = col * CELL_W - 3;
    int y = row * CELL_H - 3;
    int pw = w * CELL_W + 6;
    int ph = h * CELL_H + 6;

    for (int j = 0; j < ph; j++)
    {
        int dy = y + j;
        if ((unsigned)dy >= VIS_ROWS)
            continue;
        uint8_t *line = g_sub + dy * SUB_STRIDE;
        for (int i = 0; i < pw; i++)
        {
            int dx = x + i;
            if ((unsigned)dx >= AREA_W)
                continue;
            bool edge = (i < 2 || i >= pw - 2 || j < 2 || j >= ph - 2);
            line[dx] = edge ? PAL_UI_FRAME : PAL_UI_PANEL;
        }
    }
}

void text_flush(void)
{
    // The sub buffer is presented by render_flush().
}

// ---------------------------------------------------------------------------
// Sub-screen shape drawing
//
// The HUD lives on the bottom screen, but the gfx_* helpers in render.c draw
// into the main-screen buffer. These are the sub-screen equivalents so HUD
// elements never bleed onto the game world.
// ---------------------------------------------------------------------------

void sub_fill_rect(int x, int y, int w, int h, uint8_t color)
{
    if (!g_sub)
        g_sub = text_sub_buffer();
    if (w <= 0 || h <= 0)
        return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > AREA_W ? AREA_W : x + w;
    int y1 = y + h > VIS_ROWS ? VIS_ROWS : y + h;
    for (int j = y0; j < y1; j++)
        memset(g_sub + j * SUB_STRIDE + x0, color, x1 - x0);
}

void sub_rect_outline(int x, int y, int w, int h, uint8_t color)
{
    sub_fill_rect(x, y, w, 1, color);
    sub_fill_rect(x, y + h - 1, w, 1, color);
    sub_fill_rect(x, y, 1, h, color);
    sub_fill_rect(x + w - 1, y, 1, h, color);
}
