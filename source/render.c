// Rendering: software-composited 8bpp bitmaps on both screens, with a camera.
//
// The world is tile-based and much larger than the screen (see tools/gen_world.py):
// 128x96 tiles = 2048x1536 px, which is 8x8 screens. A world that size cannot be
// stored as pixels -- 8bpp would be 3 MB, and the DS has 4 MB of RAM shared with
// everything else -- so it is stored as a 48-tile atlas plus one byte per tile
// position, and this file composites only the tiles the camera can see.
//
// The camera is why every drawing helper in here takes WORLD coordinates. Game
// code places monsters, effects and projectiles in world space and never
// subtracts the camera itself; the translation happens once, here. The only
// code that works in screen space is the bottom-screen UI (text.c), which is
// not part of the world at all.
//
// Double buffering: the 192 visible lines of a 256x256 8bpp bitmap occupy
// 48 KB. Two such buffers live in the 128 KB of VRAM bank A at map bases 0 and
// 4; we draw into a RAM shadow buffer and memcpy it into the hidden one, then
// switch the map base during VBlank so the swap never tears.

#include <string.h>

#include "game.h"
#include "assets.h"

#define BMP_STRIDE   256                    // bitmap row pitch in VRAM
#define VIS_ROWS     AREA_H                 // 192 rows actually used
#define VIS_BYTES    (BMP_STRIDE * VIS_ROWS)

static int      g_bg_main = -1;
static int      g_bg_sub = -1;

// RAM shadow buffers: one composited frame each, so entity drawing never
// fights the DMA that presents the previous frame.
static uint8_t  g_main_bmp[BMP_STRIDE * VIS_ROWS] __attribute__((aligned(4)));
static uint8_t  g_sub_bmp[BMP_STRIDE * VIS_ROWS] __attribute__((aligned(4)));

// --- the current region's world --------------------------------------------
static const uint8_t *g_atlas = NULL;   // ATLAS_TILES x 256 B of 16x16 tiles
static const uint8_t *g_map   = NULL;   // WORLD_COLS*WORLD_ROWS tile ids
static const uint8_t *g_prop  = NULL;   // ATLAS_TILES flags, nonzero = solid

// --- camera ----------------------------------------------------------------
static int g_cam_x = 0;
static int g_cam_y = 0;

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void render_init(void)
{
    videoSetMode(MODE_5_2D);
    videoSetModeSub(MODE_5_2D);

    // Bank A: main background (both buffers). Bank C: sub background.
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);
    vramSetBankB(VRAM_B_LCD);
    vramSetBankD(VRAM_D_LCD);

    g_bg_main = bgInit(3, BgType_Bmp8, BgSize_B8_256x256, 0, 0);
    g_bg_sub = bgInitSub(3, BgType_Bmp8, BgSize_B8_256x256, 0, 0);

    bgSetCenter(g_bg_main, 0, 0);
    bgSetCenter(g_bg_sub, 0, 0);
    bgSetMapBase(g_bg_main, 0);
    bgSetMapBase(g_bg_sub, 0);

    memcpy(BG_PALETTE, palette_bin, palette_bin_size);
    memcpy(BG_PALETTE_SUB, palette_bin, palette_bin_size);

    memset(g_main_bmp, 0, sizeof(g_main_bmp));
    memset(g_sub_bmp, 0, sizeof(g_sub_bmp));

    // Clear the full VRAM bitmap once so the 64 rows below our 192-line play
    // area never hold undefined data.
    memset(bgGetGfxPtr(g_bg_main), 0, BMP_STRIDE * 256);
    memset(bgGetGfxPtr(g_bg_sub), 0, BMP_STRIDE * 256);
}

// ---------------------------------------------------------------------------
// Camera and world binding
// ---------------------------------------------------------------------------

void render_set_world(const uint8_t *atlas, const uint8_t *map,
                      const uint8_t *prop)
{
    g_atlas = atlas;
    g_map = map;
    g_prop = prop;
}

void render_camera(int x, int y)
{
    if (g_screen_shake > 0)
    {
        int sx = (g_screen_shake & 1) ? 2 : -2;
        int sy = (g_screen_shake & 2) ? -2 : 2;
        x += sx;
        y += sy;
        g_screen_shake--;
    }

    // Clamp so the viewport never leaves the world: the border of the map is a
    // solid wall, and showing past it would break the illusion of a big place.
    int max_x = WORLD_W - AREA_W;
    int max_y = WORLD_H - AREA_H;
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > max_x) x = max_x;
    if (y > max_y) y = max_y;
    g_cam_x = x;
    g_cam_y = y;
}

void render_camera_center_on(int32_t wx, int32_t wy)
{
    render_camera(wx - AREA_W / 2, wy - AREA_H / 2);
}

void render_camera_get(int *x, int *y)
{
    if (x) *x = g_cam_x;
    if (y) *y = g_cam_y;
}

// ---------------------------------------------------------------------------
// World compositing
// ---------------------------------------------------------------------------

// The ground colour at a screen pixel, recomputed from the tile map.
//
// The sprite shadow pass needs to know whether a pixel still holds bare
// terrain (and so may be darkened) or has already had an entity drawn on it
// (and must be left alone). The one-screen version kept a pointer to the whole
// pristine background for this; with a tiled world the same information is one
// lookup away, which also means no 48 KB background copy has to be kept.
static inline uint8_t ground_px(int sx, int sy)
{
    // No world bound yet (title, ending, hub-before-bind): report "no terrain"
    // so the shadow pass simply skips. Those screens draw with the camera
    // pinned at the origin and are not part of the world.
    if (!g_atlas || !g_map)
        return 0;
    int wx = sx + g_cam_x;
    int wy = sy + g_cam_y;
    if ((unsigned)wx >= WORLD_W || (unsigned)wy >= WORLD_H)
        return 0;
    unsigned id = g_map[(wy >> 4) * WORLD_COLS + (wx >> 4)];
    if (id >= ATLAS_TILES)
        return 0;
    return g_atlas[(id << 8) + ((wy & 15) << 4) + (wx & 15)];
}

void render_world(void)
{
    if (!g_atlas || !g_map)
    {
        // No world bound yet (title screens): just clear to a neutral tone so
        // nothing stale from a previous frame shows through.
        for (int y = 0; y < VIS_ROWS; y++)
            memset(g_main_bmp + y * BMP_STRIDE, PAL_UI_PANEL, AREA_W);
        return;
    }

    int tx0 = g_cam_x >> 4;
    int ty0 = g_cam_y >> 4;
    int ox  = g_cam_x & 15;
    int oy  = g_cam_y & 15;

    // 16 columns fill the screen exactly; the extra column and row cover the
    // part-tile slice left by the sub-tile camera offset. That bound is what
    // makes the per-frame cost independent of how large the world is.
    for (int ty = 0; ty <= 12; ty++)
    {
        int mty = ty0 + ty;
        int sy = ty * 16 - oy;
        for (int tx = 0; tx <= 16; tx++)
        {
            int mtx = tx0 + tx;
            int sx = tx * 16 - ox;

            unsigned id = 0;
            if ((unsigned)mtx < WORLD_COLS && (unsigned)mty < WORLD_ROWS)
                id = g_map[mty * WORLD_COLS + mtx];
            if (id >= ATLAS_TILES)
                id = 0;
            const uint8_t *src = g_atlas + (id << 8);

            // Clip the 16x16 tile against the screen.
            int x0 = sx < 0 ? -sx : 0;
            int y0 = sy < 0 ? -sy : 0;
            int x1 = (sx + 16 > AREA_W) ? AREA_W - sx : 16;
            int y1 = (sy + 16 > AREA_H) ? AREA_H - sy : 16;
            if (x0 >= x1 || y0 >= y1)
                continue;

            // Fast path: a tile fully on screen is 16 rows of 16 contiguous
            // bytes, which gcc turns into word moves. Only the tiles along the
            // right and bottom edges need the per-pixel path.
            if (x0 == 0 && x1 == 16)
            {
                uint8_t *dst = g_main_bmp + (sy + y0) * BMP_STRIDE + sx;
                const uint8_t *s = src + y0 * 16;
                for (int j = y0; j < y1; j++)
                {
                    memcpy(dst, s, 16);
                    dst += BMP_STRIDE;
                    s += 16;
                }
            }
            else
            {
                for (int j = y0; j < y1; j++)
                {
                    uint8_t *dst = g_main_bmp + (sy + j) * BMP_STRIDE + sx + x0;
                    const uint8_t *s = src + j * 16 + x0;
                    for (int i = x0; i < x1; i++)
                        *dst++ = *s++;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Low-level blitters (all operate on the main RAM shadow buffer)
//
// Every coordinate here is a WORLD coordinate; the camera is applied on the way
// in, so callers never deal with the viewport.
// ---------------------------------------------------------------------------

static inline void put_px(int x, int y, uint8_t c)
{
    int sx = x - g_cam_x;
    int sy = y - g_cam_y;
    if ((unsigned)sx < AREA_W && (unsigned)sy < AREA_H)
        g_main_bmp[sy * BMP_STRIDE + sx] = c;
}

void gfx_fill_rect(int x, int y, int w, int h, uint8_t color)
{
    if (w <= 0 || h <= 0)
        return;
    int x0 = x - g_cam_x;
    int y0 = y - g_cam_y;
    int x1 = x0 + w;
    int y1 = y0 + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > AREA_W) x1 = AREA_W;
    if (y1 > AREA_H) y1 = AREA_H;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (int j = y0; j < y1; j++)
        memset(g_main_bmp + j * BMP_STRIDE + x0, color, x1 - x0);
}

void gfx_rect_outline(int x, int y, int w, int h, uint8_t color)
{
    gfx_fill_rect(x, y, w, 1, color);
    gfx_fill_rect(x, y + h - 1, w, 1, color);
    gfx_fill_rect(x, y, 1, h, color);
    gfx_fill_rect(x + w - 1, y, 1, h, color);
}

// Filled circle. The survivors-like draws its areas of effect (mud, stink,
// bite, tail sweep) as circles, so this sits on the hot path and uses an
// integer scanline rather than a per-pixel distance test.
void gfx_circle(int cx, int cy, int r, uint8_t color)
{
    if (r <= 0)
        return;
    for (int dy = -r; dy <= r; dy++)
    {
        int rem = r * r - dy * dy;
        if (rem < 0)
            continue;
        // Integer sqrt by increment; cheap at the radii this game uses.
        int span = 0;
        while ((span + 1) * (span + 1) <= rem)
            span++;
        gfx_fill_rect(cx - span, cy + dy, span * 2 + 1, 1, color);
    }
}

// A 1px circle outline, from a 65-entry sine table. 256 angular steps is ample
// for the radii used here and avoids pulling floating point into the renderer.
void gfx_circle_outline(int cx, int cy, int r, uint8_t color)
{
    static const int8_t sin_t[65] = {
          0,   3,   6,   9,  12,  16,  19,  22,  25,  28,  31,  34,  37,
         40,  43,  46,  49,  51,  54,  57,  60,  63,  65,  68,  71,  73,
         76,  78,  81,  83,  85,  88,  90,  92,  94,  96,  98, 100, 102,
        104, 106, 107, 109, 111, 112, 113, 115, 116, 117, 118, 120, 121,
        122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127, 127,
    };
    if (r <= 0)
        return;
    for (int a = 0; a < 256; a++)
    {
        int q = a & 63;
        int sx = sin_t[q];
        int sy = sin_t[(q + 64) & 63];
        if (a & 64) sx = -sx;
        if (a & 128) sy = -sy;
        put_px(cx + (sx * r) / 127, cy + (sy * r) / 127, color);
    }
}

void gfx_line(int x0, int y0, int x1, int y1, uint8_t color)
{
    int dx = x1 - x0;
    if (dx < 0) dx = -dx;
    int dy = y1 - y0;
    if (dy < 0) dy = -dy;
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (1)
    {
        put_px(x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 > -dy)
        {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

void gfx_blit_8bpp(const uint8_t *tiles, int tw, int th, int frame,
                   int x, int y, bool hflip, bool shadow)
{
    // `tiles` is a flat 8bpp blob: tw*th bytes per frame, frames consecutive.
    // For sprite sizes that are multiples of 8 the NDS tile order is identical
    // to plain row-major pixels, so this is a straight row copy.
    const uint8_t *src = tiles + (size_t)frame * tw * th;

    // (x, y) is the sprite's world-space top-left; convert once, then clip
    // everything below in screen space.
    int bx = x - g_cam_x;
    int by = y - g_cam_y;

    int sx0 = 0, sx1 = tw;
    if (bx < 0)
        sx0 = -bx;
    if (bx + tw > AREA_W)
        sx1 = AREA_W - bx;
    int sy0 = 0, sy1 = th;
    if (by < 0)
        sy0 = -by;
    if (by + th > AREA_H)
        sy1 = AREA_H - by;
    if (sx0 >= sx1 || sy0 >= sy1)
        return;

    // Shadow pass first: a darkened offset copy so the sprite appears to be
    // standing on the ground. A pixel is only shadowed when the terrain is
    // still bare there and nothing has been drawn on top yet, which keeps
    // shadows from painting over characters standing in front.
    if (shadow)
    {
        for (int j = sy0; j < sy1; j++)
        {
            const uint8_t *row = src + j * tw;
            int dy = by + j + 3;
            if ((unsigned)dy >= AREA_H)
                continue;
            uint8_t *dst = g_main_bmp + dy * BMP_STRIDE;
            for (int i = sx0; i < sx1; i++)
            {
                int sxi = hflip ? (tw - 1 - i) : i;
                if (row[sxi] == 0)
                    continue;
                int dx = bx + i + 3;
                if ((unsigned)dx >= AREA_W)
                    continue;
                uint8_t ground = ground_px(dx, dy);
                if (ground != 0 && dst[dx] == ground)
                    dst[dx] = PAL_SHADOW;
            }
        }
    }

    for (int j = sy0; j < sy1; j++)
    {
        const uint8_t *row = src + j * tw;
        uint8_t *dst = g_main_bmp + (by + j) * BMP_STRIDE + bx;
        for (int i = sx0; i < sx1; i++)
        {
            int sxi = hflip ? (tw - 1 - i) : i;
            uint8_t c = row[sxi];
            if (c != 0)
                dst[i] = c;
        }
    }
}

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

void render_flush(void)
{
    // Copy both shadow buffers into VRAM at map base 0. A 48 KB copy plus the
    // sprite compositing fits comfortably inside one VBlank at 60 fps, so no
    // page flipping is needed -- and single buffering guarantees the visible
    // frame is always complete.
    uint8_t *gfx = (uint8_t *)bgGetGfxPtr(g_bg_main);
    for (int y = 0; y < VIS_ROWS; y++)
        memcpy(gfx + y * BMP_STRIDE, g_main_bmp + y * BMP_STRIDE, AREA_W);

    uint8_t *sub = (uint8_t *)bgGetGfxPtr(g_bg_sub);
    for (int y = 0; y < VIS_ROWS; y++)
        memcpy(sub + y * BMP_STRIDE, g_sub_bmp + y * BMP_STRIDE, AREA_W);
}

// The sub-screen shadow buffer, shared with text.c so text and any sub-screen
// graphics composite into the same image.
uint8_t *text_sub_buffer(void)
{
    return g_sub_bmp;
}

// ---------------------------------------------------------------------------
// Card art (tools/gen_cards.py)
//
// The picker is UI, so it draws onto the SUB screen rather than into the world.
// Scaling is a nearest-neighbour box sample: the cards are authored at 72x96
// and shown at 48x64, and smoothing them would only add shimmer on a DS panel.
// ---------------------------------------------------------------------------

const uint8_t *card_art(int card_id)
{
    if (card_id < 0 || card_id >= CARD_COUNT)
        card_id = 0;
    return card_atlas_bin + (size_t)card_id * CARD_BYTES;
}

void sub_blit_card(int card_id, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    const uint8_t *src = card_art(card_id);

    for (int j = 0; j < h; j++)
    {
        int sy = (j * CARD_H) / h;
        int dy = y + j;
        if ((unsigned)dy >= VIS_ROWS)
            continue;
        uint8_t *dst = g_sub_bmp + dy * BMP_STRIDE;
        const uint8_t *row = src + sy * CARD_W;
        for (int i = 0; i < w; i++)
        {
            int sx = (i * CARD_W) / w;
            int dx = x + i;
            if ((unsigned)dx >= AREA_W)
                continue;
            uint8_t c = row[sx];
            if (c != 0)                 // 0 is the card's transparent corner
                dst[dx] = c;
        }
    }
}

// ---------------------------------------------------------------------------
// Collision
//
// Solidity comes from the atlas flags emitted by the world generator, so the
// list of which tiles are walls lives in exactly one place (tools/gen_world.py)
// and can never drift out of sync with the art.
// ---------------------------------------------------------------------------

bool collision_at(int px, int py)
{
    if (px < 0 || py < 0 || px >= WORLD_W || py >= WORLD_H)
        return true;
    if (!g_map || !g_prop)
        return false;
    unsigned id = g_map[(py >> 4) * WORLD_COLS + (px >> 4)];
    if (id >= ATLAS_TILES)
        return true;
    return g_prop[id] != 0;
}

bool collision_rect(int px, int py, int w, int h)
{
    return collision_at(px, py) ||
           collision_at(px + w - 1, py) ||
           collision_at(px, py + h - 1) ||
           collision_at(px + w - 1, py + h - 1) ||
           collision_at(px + w / 2, py) ||
           collision_at(px + w / 2, py + h - 1) ||
           collision_at(px, py + h / 2) ||
           collision_at(px + w - 1, py + h / 2);
}

// Is there a walkable tile anywhere within `radius` of a point? Used by the
// spawner, which needs to place monsters on open ground near the player rather
// than in the middle of a thicket.
bool collision_open_near(int px, int py, int radius)
{
    if (!collision_at(px, py))
        return true;
    for (int r = 16; r <= radius; r += 16)
    {
        if (!collision_at(px + r, py) || !collision_at(px - r, py) ||
            !collision_at(px, py + r) || !collision_at(px, py - r))
            return true;
    }
    return false;
}
