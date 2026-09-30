#if defined(PORTABLE) && defined(__PS2__)
// Draws the GBA's picture with the PS2's Graphics Synthesizer. Each 8x8 tile of
// the backgrounds and sprites is a textured sprite, from textures that the
// GBA's VRAM is copied into every frame, with its palettes as CLUTs. The GBA's
// 15-bit colors are the GS's 16-bit ones as they are.

#include <stdint.h>
#include <string.h>
#include <kernel.h>
#include <gsKit.h>
#include <gsInline.h>
#include <dmaKit.h>
#include "platform/ps2_gs.h"

#define GBA_WIDTH 240
#define GBA_HEIGHT 160
#define SCALE 2

// Tiles are laid out 32 to a row, 8 pixels in from the top and left, so the
// UVs of a flipped tile never go below 0
#define TILES_PER_ROW 32
#define TILE_ORIGIN 8
#define LAYOUT_WIDTH (TILE_ORIGIN + TILES_PER_ROW * 8)
#define LAYOUT_HEIGHT(tiles) (TILE_ORIGIN + (tiles) / TILES_PER_ROW * 8)

#define BG_TILES_4BPP 2048
#define OBJ_TILES_4BPP 1024
#define BG_TILES_8BPP 1024
#define OBJ_TILES_8BPP 512

enum { TEX_BG4, TEX_OBJ4, TEX_BG8, TEX_OBJ8, TEX_IMAGE, TEX_COUNT };
// The palettes, and after them the same brightened or darkened (see Brighten)
enum { CLUT_BG4, CLUT_OBJ4, CLUT_BG8, CLUT_OBJ8, CLUT_BRIGHT, CLUT_COUNT = CLUT_BRIGHT * 2 };

struct Texture
{
    uint8_t *data;
    int width, height; // of the data
    int psm, tbw, tw, th;
    uint32_t vram;
};

static uint8_t sBg4[LAYOUT_WIDTH / 2 * LAYOUT_HEIGHT(BG_TILES_4BPP)] __attribute__((aligned(64)));
static uint8_t sObj4[LAYOUT_WIDTH / 2 * LAYOUT_HEIGHT(OBJ_TILES_4BPP)] __attribute__((aligned(64)));
static uint8_t sBg8[LAYOUT_WIDTH * LAYOUT_HEIGHT(BG_TILES_8BPP)] __attribute__((aligned(64)));
static uint8_t sObj8[LAYOUT_WIDTH * LAYOUT_HEIGHT(OBJ_TILES_8BPP)] __attribute__((aligned(64)));
static uint16_t sImage[GBA_WIDTH * GBA_HEIGHT] __attribute__((aligned(64)));
// 256 colors in the GS's CSM1 order
static uint16_t sClut[CLUT_COUNT][256] __attribute__((aligned(64)));

static struct Texture sTex[TEX_COUNT] = {
    [TEX_BG4] = {sBg4, LAYOUT_WIDTH, LAYOUT_HEIGHT(BG_TILES_4BPP), GS_PSM_T4, 8, 9, 10},
    [TEX_OBJ4] = {sObj4, LAYOUT_WIDTH, LAYOUT_HEIGHT(OBJ_TILES_4BPP), GS_PSM_T4, 8, 9, 9},
    [TEX_BG8] = {sBg8, LAYOUT_WIDTH, LAYOUT_HEIGHT(BG_TILES_8BPP), GS_PSM_T8, 8, 9, 9},
    [TEX_OBJ8] = {sObj8, LAYOUT_WIDTH, LAYOUT_HEIGHT(OBJ_TILES_8BPP), GS_PSM_T8, 8, 9, 8},
    [TEX_IMAGE] = {(uint8_t *)sImage, GBA_WIDTH, GBA_HEIGHT, GS_PSM_CT16, 4, 8, 8},
};
static uint32_t sClutVram[CLUT_COUNT];

// Affine sprites are put together whole, as 8-bit colors, with a clear texel
// all around so the GS finds that outside them, like the GBA does
#define AFFINE_SLOTS 32
#define AFFINE_TEX_SIZE 128
#define AFFINE_STRIDE 72 // the widest sprite, 64, and its clear edges, in 8s
static uint8_t sAffineData[AFFINE_SLOTS][AFFINE_STRIDE * (64 + 2)] __attribute__((aligned(64)));
static uint32_t sAffineVram[AFFINE_SLOTS];
// Each OAM entry's slot this frame, -1 for none
static int sAffineSlot[128];

// An affine background put together whole, as 8-bit colors: as it is when it
// repeats, and otherwise with a clear texel all around, for outside it
#define AFFINE_BG_MAX 512
static uint8_t sAffineBg[AFFINE_BG_MAX * AFFINE_BG_MAX] __attribute__((aligned(64)));
static uint32_t sAffineBgVram;
static int sAffineBgSize, sAffineBgWraps;

static GSGLOBAL *sGs;
// Where the GBA's screen goes in the frame buffer
static int sOriginX, sOriginY;
static uint64_t *sAd;
// The CLUT in each half of the GS's CLUT buffer, -1 for none yet
static int sLoadedClut[2];

// The GBA's OAM entries, as the attributes' bits
#define OAM_Y(a0) ((a0) & 0xFF)
#define OAM_AFFINE(a0) (((a0) >> 8) & 1)
#define OAM_DISABLED(a0) (((a0) >> 9) & 1)
#define OAM_MODE(a0) (((a0) >> 10) & 3)
#define OAM_MOSAIC(a0) (((a0) >> 12) & 1)
#define OAM_8BPP(a0) (((a0) >> 13) & 1)
#define OAM_SHAPE(a0) (((a0) >> 14) & 3)
#define OAM_X(a1) ((a1) & 0x1FF)
#define OAM_HFLIP(a1) (((a1) >> 12) & 1)
#define OAM_VFLIP(a1) (((a1) >> 13) & 1)
#define OAM_SIZE(a1) (((a1) >> 14) & 3)
#define OAM_TILE(a2) ((a2) & 0x3FF)
#define OAM_PRIORITY(a2) (((a2) >> 10) & 3)
#define OAM_PALETTE(a2) (((a2) >> 12) & 0xF)

static const uint8_t sObjSizes[3][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},  // square
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},  // wide
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},  // tall
};

// Writes registers through the GIF, n at a time
static inline void AdBegin(int n)
{
    uint64_t *p = gsKit_heap_alloc(sGs, n, n * 16, GIF_AD);

    p[0] = GIF_TAG_AD(n);
    p[1] = GIF_AD;
    sAd = p + 2;
}

static inline void Ad(uint64_t reg, uint64_t data)
{
    sAd[0] = data;
    sAd[1] = reg;
    sAd += 2;
}

static inline uint64_t Tex0(const struct Texture *tex, int clut, int csa, int cld)
{
    return (uint64_t)(tex->vram / 256)
         | ((uint64_t)tex->tbw << 14)
         | ((uint64_t)tex->psm << 20)
         | ((uint64_t)tex->tw << 26)
         | ((uint64_t)tex->th << 30)
         | (1ull << 34)                      // TCC: the texture's alpha
         | (1ull << 35)                      // TFX: decal, the texture's color
         | ((uint64_t)(sClutVram[clut] / 256) << 37)
         | ((uint64_t)GS_PSM_CT16 << 51)
         | ((uint64_t)csa << 56)
         | ((uint64_t)cld << 61);
}

static inline uint64_t Xyz(int x, int y)
{
    return (uint64_t)(sGs->OffsetX + x * 16) | ((uint64_t)(sGs->OffsetY + y * 16) << 16);
}

static inline uint64_t Uv(int u, int v)
{
    return (uint64_t)u | ((uint64_t)v << 16);
}

void PS2GS_Init(void)
{
    sGs = gsKit_init_global();
    sGs->PSM = GS_PSM_CT16;
    sGs->PSMZ = GS_PSMZ_16S;
    sGs->ZBuffering = GS_SETTING_OFF;
    sGs->DoubleBuffering = GS_SETTING_ON;
    sGs->PrimAlphaEnable = GS_SETTING_OFF;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(sGs);
    gsKit_mode_switch(sGs, GS_ONESHOT);

    sOriginX = (sGs->Width - GBA_WIDTH * SCALE) / 2;
    sOriginY = (sGs->Height - GBA_HEIGHT * SCALE) / 2;

    for (int i = 0; i < TEX_COUNT; i++)
    {
        int width = 1 << sTex[i].tw;
        int height = 1 << sTex[i].th;

        sTex[i].vram = gsKit_vram_alloc(sGs, gsKit_texture_size(width, height, sTex[i].psm), GSKIT_ALLOC_USERBUFFER);
    }
    for (int i = 0; i < CLUT_COUNT; i++)
        sClutVram[i] = gsKit_vram_alloc(sGs, gsKit_texture_size(16, 16, GS_PSM_CT16), GSKIT_ALLOC_USERBUFFER);
    for (int i = 0; i < AFFINE_SLOTS; i++)
        sAffineVram[i] = gsKit_vram_alloc(sGs, gsKit_texture_size(AFFINE_TEX_SIZE, AFFINE_TEX_SIZE, GS_PSM_T8), GSKIT_ALLOC_USERBUFFER);
    sAffineBgVram = gsKit_vram_alloc(sGs, gsKit_texture_size(AFFINE_BG_MAX, AFFINE_BG_MAX, GS_PSM_T8), GSKIT_ALLOC_USERBUFFER);
}

// Tile t's top left in a layout
static inline int TileU(int t) { return TILE_ORIGIN + (t % TILES_PER_ROW) * 8; }
static inline int TileV(int t) { return TILE_ORIGIN + (t / TILES_PER_ROW) * 8; }

static void LayOut4bpp(uint8_t *dst, const uint8_t *tiles, int count)
{
    for (int t = 0; t < count; t++)
    {
        const uint32_t *src = (const uint32_t *)(tiles + t * 32);
        uint8_t *row = dst + TileV(t) * (LAYOUT_WIDTH / 2) + TileU(t) / 2;

        for (int y = 0; y < 8; y++, row += LAYOUT_WIDTH / 2)
            *(uint32_t *)row = src[y];
    }
}

static void LayOut8bpp(uint8_t *dst, const uint8_t *tiles, int count)
{
    for (int t = 0; t < count; t++)
    {
        const uint32_t *src = (const uint32_t *)(tiles + t * 64);
        uint8_t *row = dst + TileV(t) * LAYOUT_WIDTH + TileU(t);

        for (int y = 0; y < 8; y++, row += LAYOUT_WIDTH)
        {
            ((uint32_t *)row)[0] = src[y * 2];
            ((uint32_t *)row)[1] = src[y * 2 + 1];
        }
    }
}

// The frame's brightness change: 2 brighter, 3 darker, by evy / 16
static int sBrightMode, sBrightEvy;

// A color brightened or darkened like the GBA does, from each 5-bit channel
static uint16_t Brighten(uint16_t color)
{
    uint16_t out = 0;

    for (int shift = 0; shift < 15; shift += 5)
    {
        int c = (color >> shift) & 0x1F;

        c = sBrightMode == 2 ? c + (31 - c) * sBrightEvy / 16 : c - c * sBrightEvy / 16;
        out |= c << shift;
    }
    return out;
}

// 256 colors in CSM1 order, where entries 8-15 and 16-23 of every 32 swap.
// Color 0 of every 16 is clear for 4-bit tiles, only color 0 for 8-bit ones.
static void MakeClut(uint16_t *clut, const uint16_t *pltt, int is8bpp, int bright)
{
    for (int i = 0; i < 256; i++)
    {
        int j = (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
        int clear = is8bpp ? (i == 0) : ((i & 0xF) == 0);
        uint16_t color = pltt[i] & 0x7FFF;

        clut[j] = (bright ? Brighten(color) : color) | (clear ? 0 : 0x8000);
    }
}

static void Upload(const struct Texture *tex)
{
    gsKit_texture_send_inline(sGs, (u32 *)tex->data, tex->width, tex->height, tex->vram, tex->psm, tex->tbw, GS_CLUT_TEXTURE);
}

static void UploadClut(int clut)
{
    gsKit_texture_send_inline(sGs, (u32 *)sClut[clut], 16, 16, sClutVram[clut], GS_PSM_CT16, 1, GS_CLUT_PALLETE);
}

// Loads a CLUT into its half of the CLUT buffer if it isn't there: the
// backgrounds' at 0 and the sprites' at 256
static void UseClut(int clut)
{
    int half = (clut % CLUT_BRIGHT == CLUT_OBJ4 || clut % CLUT_BRIGHT == CLUT_OBJ8);

    if (sLoadedClut[half] == clut)
        return;
    sLoadedClut[half] = clut;
    AdBegin(1);
    Ad(GS_TEX0_1, Tex0(&sTex[TEX_BG8], clut, half ? 16 : 0, 1));
}

// Common drawing state: pixels with alpha 0 not drawn, nearest texels
static void SetDrawState(void)
{
    AdBegin(4);
    Ad(GS_TEXFLUSH, 0);
    Ad(GS_TEX1_1, 0);
    Ad(GS_TEST_1, GS_SETREG_TEST(1, 7, 0, 0, 0, 0, 1, 1));
    Ad(GS_RGBAQ, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
}

// The frame buffer's alpha bit marks pixels for blending (see DrawLayer).
// Textures give color 0 alpha 0, which isn't drawn, and the rest the mark.
static void SetMark(int mark)
{
    AdBegin(1);
    Ad(GS_TEXA, (uint64_t)0 | ((uint64_t)(mark ? 0x80 : 0x01) << 32));
}

// Only draws where the mark below is set (1) or clear (0), or anywhere (-1)
static void SetMarkTest(int mark)
{
    AdBegin(1);
    Ad(GS_TEST_1, GS_SETREG_TEST(1, 7, 0, 0, mark >= 0, mark > 0, 1, 1));
}

// The frame buffer being drawn, with bits of its pixels masked from writes,
// as in 32-bit colors: 0xFF000000 for alpha
static inline uint64_t Frame(uint32_t mask)
{
    return GS_SETREG_FRAME_1(sGs->ScreenBuffer[sGs->ActiveBuffer & 1] / 8192, sGs->Width / 64, sGs->PSM, mask);
}

static uint64_t sPrimBlend;

// A tile of 8x8 texels at (u, v) at x, y on the GBA's screen
static inline void DrawTile(int x, int y, int u, int v, int hflip, int vflip)
{
    // Texels are sampled a quarter in from their edges, so rounding in the GS
    // can't pick the neighbor
    int u0 = hflip ? (u + 8) * 16 - 4 : u * 16 + 4;
    int v0 = vflip ? (v + 8) * 16 - 4 : v * 16 + 4;
    int u1 = hflip ? u0 - 128 : u0 + 128;
    int v1 = vflip ? v0 - 128 : v0 + 128;
    int sx = sOriginX + x * SCALE;
    int sy = sOriginY + y * SCALE;

    AdBegin(5);
    Ad(GS_PRIM, GS_PRIM_PRIM_SPRITE | (1 << 4) | (1 << 8) | sPrimBlend); // textured, UV
    Ad(GS_UV, Uv(u0, v0));
    Ad(GS_XYZ2, Xyz(sx, sy));
    Ad(GS_UV, Uv(u1, v1));
    Ad(GS_XYZ2, Xyz(sx + 8 * SCALE, sy + 8 * SCALE));
}

static void DrawRect(int x0, int y0, int x1, int y1, uint16_t color, int alpha, int blend)
{
    uint8_t r = (color & 0x1F) << 3, g = ((color >> 5) & 0x1F) << 3, b = ((color >> 10) & 0x1F) << 3;

    AdBegin(4);
    Ad(GS_PRIM, GS_PRIM_PRIM_SPRITE | (blend ? (1 << 6) : 0));
    Ad(GS_RGBAQ, GS_SETREG_RGBAQ(r, g, b, alpha, 0));
    Ad(GS_XYZ2, Xyz(sOriginX + x0 * SCALE, sOriginY + y0 * SCALE));
    Ad(GS_XYZ2, Xyz(sOriginX + x1 * SCALE, sOriginY + y1 * SCALE));
}

// Whether x or y is inside a window's range, which wraps around when its
// start is past its end
static inline int InsideRange(int start, int end, int i)
{
    return start > end ? (i >= start || i < end) : (i >= start && i < end);
}

// Which windows a line is in vertically: bit 0 for WIN0, bit 1 for WIN1
static int LineWindows(const struct GsLineState *s, int line)
{
    return ((s->dispcnt & (1 << 13)) && InsideRange(s->win0v >> 8, s->win0v & 0xFF, line))
         | (((s->dispcnt & (1 << 14)) && InsideRange(s->win1v >> 8, s->win1v & 0xFF, line)) << 1);
}

// Part of a line's width where the windows let the same layers show: bits
// 0-3 for the backgrounds, 4 for sprites, 5 for color effects
struct Segment
{
    int x0, x1;
    int mask;
};

static int LineSegments(const struct GsLineState *s, int line, struct Segment *segments)
{
    int windows = LineWindows(s, line);
    int count = 0;

    if (!(s->dispcnt & 0xE000))
    {
        segments[0] = (struct Segment){0, GBA_WIDTH, 0x3F};
        return 1;
    }
    for (int x = 0; x < GBA_WIDTH; x++)
    {
        int mask;

        if ((windows & 1) && InsideRange(s->win0h >> 8, s->win0h & 0xFF, x))
            mask = s->winin & 0x3F;
        else if ((windows & 2) && InsideRange(s->win1h >> 8, s->win1h & 0xFF, x))
            mask = (s->winin >> 8) & 0x3F;
        else
            mask = s->winout & 0x3F;
        if (count > 0 && segments[count - 1].mask == mask)
            segments[count - 1].x1 = x + 1;
        else
            segments[count++] = (struct Segment){x, x + 1, mask};
    }
    return count;
}

// Groups of lines drawn with the same registers and windows
static int NextBand(const struct GsLineState *lines, int first)
{
    int last = first;
    int windows = LineWindows(&lines[first], first);

    while (last + 1 < GBA_HEIGHT && memcmp(&lines[last + 1], &lines[first], sizeof(lines[0])) == 0
        && LineWindows(&lines[last + 1], last + 1) == windows)
        last++;
    return last;
}

static int SpriteShows(const uint16_t *attr, int first, int last, int x0, int x1, int *x, int *y, int *w, int *h)
{
    uint16_t a0 = attr[0], a1 = attr[1];
    int shape = OAM_SHAPE(a0);

    if (shape == 3 || (!OAM_AFFINE(a0) && OAM_DISABLED(a0)))
        return 0;
    *w = sObjSizes[shape][OAM_SIZE(a1)][0];
    *h = sObjSizes[shape][OAM_SIZE(a1)][1];
    if (OAM_AFFINE(a0) && OAM_DISABLED(a0)) // double size
    {
        *w *= 2;
        *h *= 2;
    }
    *x = OAM_X(a1);
    *y = OAM_Y(a0);
    if (*x >= GBA_WIDTH)
        *x -= 512;
    if (*y >= GBA_HEIGHT)
        *y -= 256;
    return *x + *w > x0 && *x < x1 && *y + *h > first && *y <= last;
}

int PS2GS_CanDraw(const struct GsLineState *lines, const uint16_t *oam)
{
    int brightMode = 0, brightEvy = 0;
    uint8_t affineSeen[128] = {0};
    int affineCount = 0;
    int affineBgControl = -1; // one affine background for the frame

    for (int first = 0; first < GBA_HEIGHT;)
    {
        const struct GsLineState *s = &lines[first];
        int last = NextBand(lines, first);
        int mode = s->dispcnt & 7;
        int blend = (s->bldcnt >> 6) & 3;
        struct Segment segments[GBA_WIDTH];
        int count = LineSegments(s, first, segments);
        int objOn = s->dispcnt & 0x1000;

        if (mode > 1)
            return 0;
        for (int i = 0; i < count; i++)
        {
            int mask = segments[i].mask;
            int bgs = (s->dispcnt >> 8) & 0xF & mask;
            int shown = bgs | (objOn && (mask & 0x10) ? 0x10 : 0) | 0x20;

            // BG2 in mode 1 is affine: without mosaic, and one that doesn't
            // repeat needs its clear edge to fit
            if (mode == 1 && (bgs & 4))
            {
                int size = 128 << ((s->bgcnt[2] >> 14) & 3);

                if ((s->bgcnt[2] & 0x40) && (s->mosaic & 0xFF))
                    return 0;
                if ((s->bgcnt[2] & (1 << 13)) ? size > AFFINE_BG_MAX : size + 2 > AFFINE_BG_MAX)
                    return 0;
                if (affineBgControl >= 0 && affineBgControl != (s->bgcnt[2] & 0xFFBF))
                    return 0;
                affineBgControl = s->bgcnt[2] & 0xFFBF;
            }
            for (int bg = 0; bg < 4; bg++)
            {
                if ((bgs & (1 << bg)) && (s->bgcnt[bg] & 0x40) && (s->mosaic & 0xFF))
                    return 0;
            }
            (void)shown;
            // One brightness change for the frame, for its palettes
            if ((mask & 0x20) && blend >= 2 && (s->bldy & 0x1F))
            {
                int evy = (s->bldy & 0x1F) > 16 ? 16 : (s->bldy & 0x1F);

                if (brightMode && (brightMode != blend || brightEvy != evy))
                    return 0;
                brightMode = blend;
                brightEvy = evy;
            }
        }
        if (objOn)
        {
            if (!(s->dispcnt & 0x40)) // 2D sprite tiles
                return 0;
            for (int i = 0; i < 128; i++)
            {
                const uint16_t *attr = oam + i * 4;
                int x, y, w, h;

                if (!SpriteShows(attr, first, last, 0, GBA_WIDTH, &x, &y, &w, &h))
                    continue;
                // The sprite window only matters where it's on
                if (OAM_MODE(attr[0]) == 2 && !(s->dispcnt & 0x8000))
                    continue;
                if (OAM_MODE(attr[0]) >= 2
                 || (OAM_MOSAIC(attr[0]) && (s->mosaic & 0xFF00))
                 || (OAM_8BPP(attr[0]) && (OAM_TILE(attr[2]) & 1)))
                    return 0;
                if (OAM_AFFINE(attr[0]) && !affineSeen[i])
                {
                    affineSeen[i] = 1;
                    if (++affineCount > AFFINE_SLOTS)
                        return 0;
                }
            }
        }
        first = last + 1;
    }
    sBrightMode = brightMode;
    sBrightEvy = brightEvy;
    return 1;
}

struct BgDraw
{
    const struct GsLineState *s;
    int bg, first, last, x0, x1;
    const uint8_t *vram;
    int bright;
};

static void DrawBackground(const void *arg)
{
    static const uint16_t sMapSizes[4][2] = {{256, 256}, {512, 256}, {256, 512}, {512, 512}};
    const struct BgDraw *d = arg;
    uint16_t control = d->s->bgcnt[d->bg];
    int is8bpp = (control >> 7) & 1;
    int charBase = (control >> 2) & 3;
    const uint16_t *map = (const uint16_t *)(d->vram + ((control >> 8) & 0x1F) * 0x800);
    int mapWidth = sMapSizes[control >> 14][0];
    int mapHeight = sMapSizes[control >> 14][1];
    int hofs = d->s->hofs[d->bg] & 0x1FF;
    int vofs = d->s->vofs[d->bg] & 0x1FF;
    const struct Texture *tex = &sTex[is8bpp ? TEX_BG8 : TEX_BG4];
    int clut = (is8bpp ? CLUT_BG8 : CLUT_BG4) + (d->bright ? CLUT_BRIGHT : 0);
    int palette = -1;

    UseClut(clut);
    if (is8bpp)
    {
        AdBegin(1);
        Ad(GS_TEX0_1, Tex0(tex, clut, 0, 0));
    }
    for (int y = d->first - ((d->first + vofs) & 7); y <= d->last; y += 8)
    {
        int mapY = (y + vofs) & (mapHeight - 1);
        const uint16_t *row = map + (mapY & 0xFF) / 8 * 32;

        if (mapY > 255)
            row += mapWidth > 256 ? 0x800 : 0x400;
        for (int x = d->x0 - ((d->x0 + hofs) & 7); x < d->x1; x += 8)
        {
            int mapX = (x + hofs) & (mapWidth - 1);
            uint16_t entry = (mapX > 255 ? row + 0x400 : row)[(mapX & 0xFF) / 8];
            int tile;

            if (is8bpp)
            {
                tile = (charBase * 256 + (entry & 0x3FF)) % BG_TILES_8BPP;
            }
            else
            {
                tile = (charBase * 512 + (entry & 0x3FF)) % BG_TILES_4BPP;
                if ((entry >> 12) != palette)
                {
                    palette = entry >> 12;
                    AdBegin(1);
                    Ad(GS_TEX0_1, Tex0(tex, clut, palette, 0));
                }
            }
            DrawTile(x, y, TileU(tile), TileV(tile), (entry >> 10) & 1, (entry >> 11) & 1);
        }
    }
}

struct SpriteDraw
{
    const uint16_t *attr;
    int x, y, w, h;
    int first, last, x0, x1;
    int bright;
};

static void DrawSprite(const void *arg)
{
    const struct SpriteDraw *d = arg;
    const uint16_t *attr = d->attr;
    int is8bpp = OAM_8BPP(attr[0]);
    int hflip = OAM_HFLIP(attr[1]);
    int vflip = OAM_VFLIP(attr[1]);
    int tilesWide = d->w / 8;
    const struct Texture *tex = &sTex[is8bpp ? TEX_OBJ8 : TEX_OBJ4];

    int clut = (is8bpp ? CLUT_OBJ8 : CLUT_OBJ4) + (d->bright ? CLUT_BRIGHT : 0);

    UseClut(clut);
    AdBegin(1);
    Ad(GS_TEX0_1, Tex0(tex, clut, 16 + (is8bpp ? 0 : OAM_PALETTE(attr[2])), 0));
    for (int ty = 0; ty < d->h / 8; ty++)
    {
        int screenY = d->y + (vflip ? d->h / 8 - 1 - ty : ty) * 8;

        if (screenY + 8 <= d->first || screenY > d->last)
            continue;
        for (int tx = 0; tx < tilesWide; tx++)
        {
            int screenX = d->x + (hflip ? tilesWide - 1 - tx : tx) * 8;
            int tile = ty * tilesWide + tx;

            if (screenX + 8 <= d->x0 || screenX >= d->x1)
                continue;
            tile = is8bpp ? (OAM_TILE(attr[2]) / 2 + tile) % OBJ_TILES_8BPP
                          : (OAM_TILE(attr[2]) + tile) % OBJ_TILES_4BPP;
            DrawTile(screenX, screenY, TileU(tile), TileV(tile), hflip, vflip);
        }
    }
}

// Puts an affine sprite together for the GS: its 4-bit colors with the
// palette's number added, which is where they are in CLUT_OBJ4, or 8-bit ones
static void BuildAffineSprite(int slot, const uint16_t *attr, const uint8_t *vram)
{
    int w = sObjSizes[OAM_SHAPE(attr[0])][OAM_SIZE(attr[1])][0];
    int h = sObjSizes[OAM_SHAPE(attr[0])][OAM_SIZE(attr[1])][1];
    int stride = (w + 2 + 7) & ~7;
    int is8bpp = OAM_8BPP(attr[0]);
    int tileNum = OAM_TILE(attr[2]);
    int palette = OAM_PALETTE(attr[2]) * 16;
    const uint8_t *objTiles = vram + 0x10000;
    uint8_t *data = sAffineData[slot];

    memset(data, 0, stride * (h + 2));
    for (int ty = 0; ty < h / 8; ty++)
    {
        for (int tx = 0; tx < w / 8; tx++)
        {
            int block = ty * (w / 8) + tx;
            uint8_t *dst = data + (1 + ty * 8) * stride + 1 + tx * 8;

            if (is8bpp)
            {
                const uint8_t *src = objTiles + (((block * 2 + tileNum) * 32) & 0x7FFF);

                for (int r = 0; r < 8; r++, dst += stride)
                    memcpy(dst, src + r * 8, 8);
            }
            else
            {
                const uint8_t *src = objTiles + (((block + tileNum) * 32) & 0x7FFF);

                for (int r = 0; r < 8; r++, dst += stride)
                {
                    for (int c = 0; c < 8; c++)
                    {
                        int color = (src[r * 4 + c / 2] >> ((c & 1) * 4)) & 0xF;

                        dst[c] = color ? palette + color : 0;
                    }
                }
            }
        }
    }
    gsKit_texture_send_inline(sGs, (u32 *)data, stride, h + 2, sAffineVram[slot], GS_PSM_T8,
                              AFFINE_TEX_SIZE / 64, GS_CLUT_TEXTURE);
}

static inline uint32_t FloatBits(float f)
{
    union { float f; uint32_t u; } bits = {f};

    return bits.u;
}

struct AffineDraw
{
    const uint16_t *attr;
    const uint16_t *oam;
    int x, y, boxWidth, boxHeight;
    int slot;
    int bright;
};

// A sprite turned, scaled or sheared by its matrix, which maps a pixel's
// offset from the sprite's center on the screen to one in the sprite. The
// GS works the texels out from the corners' across the triangles, which is
// the same, but at every pixel of the PS2's screen, so it's finer.
static void DrawAffineSprite(const void *arg)
{
    const struct AffineDraw *d = arg;
    const uint16_t *attr = d->attr;
    int w = sObjSizes[OAM_SHAPE(attr[0])][OAM_SIZE(attr[1])][0];
    int h = sObjSizes[OAM_SHAPE(attr[0])][OAM_SIZE(attr[1])][1];
    int matrix = (attr[1] >> 9) & 0x1F;
    float pa = (int16_t)d->oam[(matrix * 4 + 0) * 4 + 3] / 256.0f;
    float pb = (int16_t)d->oam[(matrix * 4 + 1) * 4 + 3] / 256.0f;
    float pc = (int16_t)d->oam[(matrix * 4 + 2) * 4 + 3] / 256.0f;
    float pd = (int16_t)d->oam[(matrix * 4 + 3) * 4 + 3] / 256.0f;
    int is8bpp = OAM_8BPP(attr[0]);
    int clut = (is8bpp ? CLUT_OBJ8 : CLUT_OBJ4) + (d->bright ? CLUT_BRIGHT : 0);
    struct Texture tex = {NULL, 0, 0, GS_PSM_T8, AFFINE_TEX_SIZE / 64, 7, 7, sAffineVram[d->slot]};
    float centerX = d->x + d->boxWidth / 2.0f;
    float centerY = d->y + d->boxHeight / 2.0f;
    static const int sCorners[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};

    UseClut(clut);
    AdBegin(4);
    Ad(GS_TEX0_1, Tex0(&tex, clut, 16, 0));
    Ad(GS_CLAMP_1, GS_SETREG_CLAMP(GS_CMODE_REGION_CLAMP, GS_CMODE_REGION_CLAMP, 0, w + 1, 0, h + 1));
    Ad(GS_RGBAQ, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0) | ((uint64_t)FloatBits(1.0f) << 32));
    Ad(GS_PRIM, GS_PRIM_PRIM_TRISTRIP | (1 << 4) | sPrimBlend); // textured, STQ
    for (int i = 0; i < 4; i++)
    {
        float gx = sCorners[i][0] * d->boxWidth / 2.0f;
        float gy = sCorners[i][1] * d->boxHeight / 2.0f;
        // Texels, with the clear edge, and sampled a quarter in like tiles are
        float u = pa * gx + pb * gy + w / 2 + 1 + 0.25f;
        float v = pc * gx + pd * gy + h / 2 + 1 + 0.25f;

        AdBegin(2);
        Ad(GS_ST, (uint64_t)FloatBits(u / AFFINE_TEX_SIZE) | ((uint64_t)FloatBits(v / AFFINE_TEX_SIZE) << 32));
        Ad(GS_XYZ2, Xyz(sOriginX + (int)((centerX + gx) * SCALE), sOriginY + (int)((centerY + gy) * SCALE)));
    }
    AdBegin(2);
    Ad(GS_CLAMP_1, 0);
    Ad(GS_RGBAQ, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
}

// Puts BG2 together for the GS, from its 8-bit tiles and byte map
static void BuildAffineBackground(uint16_t control, const uint8_t *vram)
{
    int size = 128 << ((control >> 14) & 3);
    int wraps = (control >> 13) & 1;
    int edge = wraps ? 0 : 1;
    int stride = wraps ? size : (size + 2 + 7) & ~7;
    const uint8_t *tiles = vram + ((control >> 2) & 3) * 0x4000;
    const uint8_t *map = vram + ((control >> 8) & 0x1F) * 0x800;

    if (!wraps)
        memset(sAffineBg, 0, stride * (size + 2));
    for (int ty = 0; ty < size / 8; ty++)
    {
        for (int tx = 0; tx < size / 8; tx++)
        {
            const uint8_t *tile = tiles + ((map[ty * (size / 8) + tx] * 64) & 0xFFFF);
            uint8_t *dst = sAffineBg + (edge + ty * 8) * stride + edge + tx * 8;

            for (int r = 0; r < 8; r++, dst += stride)
                memcpy(dst, tile + r * 8, 8);
        }
    }
    sAffineBgSize = size;
    sAffineBgWraps = wraps;
    gsKit_texture_send_inline(sGs, (u32 *)sAffineBg, stride, wraps ? size : size + 2, sAffineBgVram, GS_PSM_T8,
                              AFFINE_BG_MAX / 64, GS_CLUT_TEXTURE);
}

// BG2's texels at a pixel of the GBA's screen, like the software renderer:
// the reference point plus the line times PB and PD, and x times PA and PC
static void AffineBgTexel(const struct GsLineState *s, float x, float y, float *u, float *v)
{
    const uint16_t *a = s->bg2Affine;
    int32_t refX = (int32_t)((a[4] | ((uint32_t)a[5] << 16)) << 4) >> 4; // 28 bits
    int32_t refY = (int32_t)((a[6] | ((uint32_t)a[7] << 16)) << 4) >> 4;

    *u = (refX + y * (int16_t)a[1] + x * (int16_t)a[0]) / 256.0f;
    *v = (refY + y * (int16_t)a[3] + x * (int16_t)a[2]) / 256.0f;
}

static void DrawAffineBackground(const void *arg)
{
    const struct BgDraw *d = arg;
    int clut = CLUT_BG8 + (d->bright ? CLUT_BRIGHT : 0);
    int texSize = AFFINE_BG_MAX;
    struct Texture tex = {NULL, 0, 0, GS_PSM_T8, AFFINE_BG_MAX / 64, 9, 9, sAffineBgVram};
    int edge = sAffineBgWraps ? 0 : 1;
    const float corners[4][2] = {{d->x0, d->first}, {d->x1, d->first}, {d->x0, d->last + 1}, {d->x1, d->last + 1}};

    // A repeating one is its own size, for the GS to repeat
    if (sAffineBgWraps)
    {
        tex.tw = tex.th = __builtin_ctz(sAffineBgSize);
        texSize = sAffineBgSize;
    }
    UseClut(clut);
    AdBegin(4);
    Ad(GS_TEX0_1, Tex0(&tex, clut, 0, 0));
    Ad(GS_CLAMP_1, sAffineBgWraps ? 0 : GS_SETREG_CLAMP(GS_CMODE_REGION_CLAMP, GS_CMODE_REGION_CLAMP,
                                                         0, sAffineBgSize + 1, 0, sAffineBgSize + 1));
    Ad(GS_RGBAQ, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0) | ((uint64_t)FloatBits(1.0f) << 32));
    Ad(GS_PRIM, GS_PRIM_PRIM_TRISTRIP | (1 << 4) | sPrimBlend); // textured, STQ
    for (int i = 0; i < 4; i++)
    {
        float u, v;

        AffineBgTexel(d->s, corners[i][0], corners[i][1], &u, &v);
        // Outside a background that doesn't repeat is its clear edge
        if (!sAffineBgWraps)
        {
            u += edge;
            v += edge;
        }
        u += 0.25f;
        v += 0.25f;
        AdBegin(2);
        Ad(GS_ST, (uint64_t)FloatBits(u / texSize) | ((uint64_t)FloatBits(v / texSize) << 32));
        Ad(GS_XYZ2, Xyz(sOriginX + (int)(corners[i][0] * SCALE), sOriginY + (int)(corners[i][1] * SCALE)));
    }
    AdBegin(2);
    Ad(GS_CLAMP_1, 0);
    Ad(GS_RGBAQ, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
}

// Draws a layer, with its pixels marked or not: when blending, marked if it's
// a second target, and for a brightness change, if it's a first target. A
// first target of blending is blended where the pixel below is marked, like
// the GBA blends a pixel with the one under it when that's a second target:
// A * eva / 16 + B * evb / 16.
static void DrawLayer(void (*draw)(const void *), const void *arg, int firstTarget, int mark, int eva, int evb)
{
    SetMark(mark);
    if (!firstTarget)
    {
        draw(arg);
        return;
    }
    // Both passes mark what they draw, so the second doesn't draw over the
    // first's pixels: blended over marked pixels, then as it is elsewhere
    SetMark(1);
    SetMarkTest(1);
    sPrimBlend = 1 << 6;
    if (eva + evb != 16 && evb != 16)
    {
        // B * evb first, then A * eva added
        AdBegin(1);
        Ad(GS_ALPHA_1, GS_SETREG_ALPHA(1, 2, 2, 2, evb * 8));
        draw(arg);
    }
    AdBegin(1);
    if (eva + evb == 16)
        Ad(GS_ALPHA_1, GS_SETREG_ALPHA(0, 1, 2, 1, eva * 8)); // (A - B) * eva + B
    else
        Ad(GS_ALPHA_1, GS_SETREG_ALPHA(0, 2, 2, 1, eva * 8)); // A * eva + what's there
    draw(arg);
    sPrimBlend = 0;
    SetMarkTest(0);
    draw(arg);
    SetMarkTest(-1);
    // Then the layer's own mark, if it's not a second target, written over
    // its pixels without their colors
    if (!mark)
    {
        SetMark(0);
        AdBegin(1);
        Ad(GS_FRAME_1, Frame(0x00FFFFFF));
        draw(arg);
        AdBegin(1);
        Ad(GS_FRAME_1, Frame(0));
    }
}

static void BeginFrame(void)
{
    sLoadedClut[0] = sLoadedClut[1] = -1;
    gsKit_clear(sGs, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));
}

// What of a part of the screen to draw: the backdrop, a background, or the
// sprites of a priority
enum { PART_BACKDROP, PART_BACKGROUND, PART_SPRITES };

static void DrawSegmentPart(const struct GsLineState *s, int first, int last, const struct Segment *seg, int part, int which,
                            const uint8_t *vram, const uint16_t *pltt, const uint16_t *oam)
{
    int mask = seg->mask;
    int bgs = (s->dispcnt >> 8) & 0xF & mask;
    int objOn = (s->dispcnt & 0x1000) && (mask & 0x10);
    int effects = mask & 0x20;
    int blend = (s->bldcnt >> 6) & 3;
    int eva = s->bldalpha & 0x1F, evb = (s->bldalpha >> 8) & 0x1F;
    int evy = s->bldy & 0x1F;
    // First targets of a brightness change use brightened palettes
    int brighten = effects && blend >= 2 && evy;

    if (eva > 16)
        eva = 16;
    if (evb > 16)
        evb = 16;
    if ((part == PART_BACKGROUND && !(bgs & (1 << which))) || (part == PART_SPRITES && !objOn))
        return;

    AdBegin(1);
    Ad(GS_SCISSOR_1, GS_SETREG_SCISSOR(sOriginX + seg->x0 * SCALE, sOriginX + seg->x1 * SCALE - 1,
                                       sOriginY + first * SCALE, sOriginY + (last + 1) * SCALE - 1));
    if (part == PART_BACKDROP)
    {
        uint16_t backdrop = pltt[0] & 0x7FFF;

        if (brighten && (s->bldcnt & 0x20))
            backdrop = Brighten(backdrop);
        // Marked if it's a second target of blending
        DrawRect(seg->x0, first, seg->x1, last + 1, backdrop, (s->bldcnt & (1 << 13)) ? 0x80 : 0x01, 0);
    }
    else if (part == PART_BACKGROUND)
    {
        int bg = which;
        struct BgDraw d = {s, bg, first, last, seg->x0, seg->x1, vram, brighten && (s->bldcnt & (1 << bg))};

        DrawLayer((s->dispcnt & 7) == 1 && bg == 2 ? DrawAffineBackground : DrawBackground, &d,
                  effects && blend == 1 && (s->bldcnt & (1 << bg)), (s->bldcnt >> (8 + bg)) & 1, eva, evb);
    }
    else
    {
        for (int i = 127; i >= 0; i--)
        {
            const uint16_t *attr = oam + i * 4;
            // Semi-transparent sprites blend whatever the effects, and aren't
            // brightened
            int semi = OAM_MODE(attr[0]) == 1;
            int firstTarget = semi || (effects && blend == 1 && (s->bldcnt & 0x10));
            struct SpriteDraw d = {attr, 0, 0, 0, 0, first, last, seg->x0, seg->x1,
                                   brighten && !semi && (s->bldcnt & 0x10)};

            if (OAM_PRIORITY(attr[2]) != which || OAM_MODE(attr[0]) == 2
             || !SpriteShows(attr, first, last, seg->x0, seg->x1, &d.x, &d.y, &d.w, &d.h))
                continue;
            if (OAM_AFFINE(attr[0]))
            {
                struct AffineDraw a = {attr, oam, d.x, d.y, d.w, d.h, sAffineSlot[i], d.bright};

                DrawLayer(DrawAffineSprite, &a, firstTarget, (s->bldcnt >> 12) & 1, eva, evb);
                continue;
            }
            DrawLayer(DrawSprite, &d, firstTarget, (s->bldcnt >> 12) & 1, eva, evb);
        }
    }
}

// Draws a part of lines first..last, in each part of their width the windows
// make, with the registers of the first
static void DrawPartOfLines(const struct GsLineState *lines, int first, int last, int part, int which,
                            const uint8_t *vram, const uint16_t *pltt, const uint16_t *oam)
{
    struct Segment segments[GBA_WIDTH];
    int count = LineSegments(&lines[first], first, segments);

    for (int i = 0; i < count; i++)
        DrawSegmentPart(&lines[first], first, last, &segments[i], part, which, vram, pltt, oam);
}

// Whether the lines only differ in the backgrounds' scrolling and BG2's
// matrix, so each layer can be drawn whole in turn
static int OnlyScrollChanges(const struct GsLineState *lines)
{
    struct GsLineState first = lines[0];

    memset(first.hofs, 0, sizeof(first.hofs));
    memset(first.vofs, 0, sizeof(first.vofs));
    memset(first.bg2Affine, 0, sizeof(first.bg2Affine));
    for (int i = 1; i < GBA_HEIGHT; i++)
    {
        struct GsLineState line = lines[i];

        memset(line.hofs, 0, sizeof(line.hofs));
        memset(line.vofs, 0, sizeof(line.vofs));
        memset(line.bg2Affine, 0, sizeof(line.bg2Affine));
        if (memcmp(&line, &first, sizeof(line)) != 0)
            return 0;
    }
    return 1;
}

// The last line from first with the same windows and, for a background, the
// same scrolling (and matrix, for BG2)
static int LastLineLike(const struct GsLineState *lines, int first, int bg)
{
    int last = first;
    int windows = LineWindows(&lines[first], first);

    while (last + 1 < GBA_HEIGHT && LineWindows(&lines[last + 1], last + 1) == windows)
    {
        const struct GsLineState *a = &lines[first], *b = &lines[last + 1];

        if (bg >= 0 && (a->hofs[bg] != b->hofs[bg] || a->vofs[bg] != b->vofs[bg]
                     || (bg == 2 && memcmp(a->bg2Affine, b->bg2Affine, sizeof(a->bg2Affine)) != 0)))
            break;
        last++;
    }
    return last;
}

void PS2GS_DrawLines(const struct GsLineState *lines, const uint8_t *vram, const uint16_t *pltt, const uint16_t *oam)
{
    int uses8bpp = 0;

    BeginFrame();
    LayOut4bpp(sBg4, vram, BG_TILES_4BPP);
    LayOut4bpp(sObj4, vram + 0x10000, OBJ_TILES_4BPP);
    Upload(&sTex[TEX_BG4]);
    Upload(&sTex[TEX_OBJ4]);
    for (int i = 0; i < 128; i++)
        uses8bpp |= OAM_8BPP(oam[i * 4]);
    for (int i = 0; i < GBA_HEIGHT; i++)
        for (int bg = 0; bg < 4; bg++)
            uses8bpp |= (lines[i].bgcnt[bg] >> 7) & 1;
    if (uses8bpp)
    {
        LayOut8bpp(sBg8, vram, BG_TILES_8BPP);
        LayOut8bpp(sObj8, vram + 0x10000, OBJ_TILES_8BPP);
        Upload(&sTex[TEX_BG8]);
        Upload(&sTex[TEX_OBJ8]);
    }
    for (int bright = 0; bright < 2; bright++)
    {
        int i = bright ? CLUT_BRIGHT : 0;

        if (bright && !sBrightMode)
            break;
        MakeClut(sClut[i + CLUT_BG4], pltt, 0, bright);
        MakeClut(sClut[i + CLUT_OBJ4], pltt + 256, 0, bright);
        MakeClut(sClut[i + CLUT_BG8], pltt, 1, bright);
        MakeClut(sClut[i + CLUT_OBJ8], pltt + 256, 1, bright);
        for (int j = 0; j < CLUT_BRIGHT; j++)
            UploadClut(i + j);
    }
    for (int i = 0; i < GBA_HEIGHT; i++)
    {
        if ((lines[i].dispcnt & 7) == 1 && (lines[i].dispcnt & 0x400))
        {
            BuildAffineBackground(lines[i].bgcnt[2], vram);
            break;
        }
    }
    for (int i = 0, slots = 0; i < 128; i++)
    {
        const uint16_t *attr = oam + i * 4;
        int x, y, w, h;

        sAffineSlot[i] = -1;
        if (OAM_AFFINE(attr[0]) && OAM_MODE(attr[0]) != 2 && slots < AFFINE_SLOTS
         && SpriteShows(attr, 0, GBA_HEIGHT - 1, 0, GBA_WIDTH, &x, &y, &w, &h))
        {
            sAffineSlot[i] = slots++;
            BuildAffineSprite(sAffineSlot[i], attr, vram);
        }
    }
    SetDrawState();

    if (OnlyScrollChanges(lines))
    {
        // Each layer whole, in the order they cover each other, split only
        // where its own scrolling changes, like with waves or clouds
        const struct GsLineState *s = &lines[0];

        for (int first = 0, last; first < GBA_HEIGHT; first = last + 1)
        {
            last = LastLineLike(lines, first, -1);
            DrawPartOfLines(lines, first, last, PART_BACKDROP, 0, vram, pltt, oam);
        }
        for (int priority = 3; priority >= 0; priority--)
        {
            for (int bg = 3; bg >= 0; bg--)
            {
                if (!(s->dispcnt & (0x100 << bg)) || (s->bgcnt[bg] & 3) != priority)
                    continue;
                for (int first = 0, last; first < GBA_HEIGHT; first = last + 1)
                {
                    last = LastLineLike(lines, first, bg);
                    DrawPartOfLines(lines, first, last, PART_BACKGROUND, bg, vram, pltt, oam);
                }
            }
            for (int first = 0, last; first < GBA_HEIGHT; first = last + 1)
            {
                last = LastLineLike(lines, first, -1);
                DrawPartOfLines(lines, first, last, PART_SPRITES, priority, vram, pltt, oam);
            }
        }
        return;
    }
    // Otherwise band by band of lines with the same registers
    for (int first = 0, last; first < GBA_HEIGHT; first = last + 1)
    {
        const struct GsLineState *s = &lines[first];

        last = NextBand(lines, first);
        DrawPartOfLines(lines, first, last, PART_BACKDROP, 0, vram, pltt, oam);
        for (int priority = 3; priority >= 0; priority--)
        {
            for (int bg = 3; bg >= 0; bg--)
            {
                if ((s->bgcnt[bg] & 3) == priority)
                    DrawPartOfLines(lines, first, last, PART_BACKGROUND, bg, vram, pltt, oam);
            }
            DrawPartOfLines(lines, first, last, PART_SPRITES, priority, vram, pltt, oam);
        }
    }
}

void PS2GS_DrawImage(const uint16_t *image)
{
    const struct Texture *tex = &sTex[TEX_IMAGE];

    BeginFrame();
    for (int i = 0; i < GBA_WIDTH * GBA_HEIGHT; i++)
        sImage[i] = image[i] | 0x8000;
    Upload(tex);
    SetDrawState();
    SetMark(0);
    AdBegin(7);
    Ad(GS_SCISSOR_1, GS_SETREG_SCISSOR(sOriginX, sOriginX + GBA_WIDTH * SCALE - 1, sOriginY, sOriginY + GBA_HEIGHT * SCALE - 1));
    Ad(GS_TEX0_1, Tex0(tex, 0, 0, 0));
    Ad(GS_PRIM, GS_PRIM_PRIM_SPRITE | (1 << 4) | (1 << 8));
    Ad(GS_UV, Uv(4, 4));
    Ad(GS_XYZ2, Xyz(sOriginX, sOriginY));
    Ad(GS_UV, Uv(GBA_WIDTH * 16 + 4, GBA_HEIGHT * 16 + 4));
    Ad(GS_XYZ2, Xyz(sOriginX + GBA_WIDTH * SCALE, sOriginY + GBA_HEIGHT * SCALE));
}

void PS2GS_Present(void)
{
    FlushCache(0);
    gsKit_queue_exec(sGs);
    gsKit_finish();
    gsKit_sync_flip(sGs);
}
#endif // PORTABLE && __PS2__
