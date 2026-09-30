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
enum { CLUT_BG4, CLUT_OBJ4, CLUT_BG8, CLUT_OBJ8, CLUT_COUNT };

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

// The part of the frame buffer that lines first..last of the GBA's screen are
static void SetScissor(int first, int last)
{
    AdBegin(1);
    Ad(GS_SCISSOR_1, GS_SETREG_SCISSOR(sOriginX, sOriginX + GBA_WIDTH * SCALE - 1,
                                       sOriginY + first * SCALE, sOriginY + (last + 1) * SCALE - 1));
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

// 256 colors in CSM1 order, where entries 8-15 and 16-23 of every 32 swap.
// Color 0 of every 16 is clear for 4-bit tiles, only color 0 for 8-bit ones.
static void MakeClut(uint16_t *clut, const uint16_t *pltt, int is8bpp)
{
    for (int i = 0; i < 256; i++)
    {
        int j = (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
        int clear = is8bpp ? (i == 0) : ((i & 0xF) == 0);

        clut[j] = (pltt[i] & 0x7FFF) | (clear ? 0 : 0x8000);
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
    int half = (clut == CLUT_OBJ4 || clut == CLUT_OBJ8);

    if (sLoadedClut[half] == clut)
        return;
    sLoadedClut[half] = clut;
    AdBegin(1);
    Ad(GS_TEX0_1, Tex0(&sTex[TEX_BG8], clut, half ? 16 : 0, 1));
}

// Common drawing state: textures' alpha 0 for color 0 and 128 for the rest,
// pixels with alpha 0 not drawn, nearest texels
static void SetDrawState(void)
{
    AdBegin(5);
    Ad(GS_TEXFLUSH, 0);
    Ad(GS_TEXA, (uint64_t)0 | (0x80ull << 32));
    Ad(GS_TEX1_1, 0);
    Ad(GS_TEST_1, GS_SETREG_TEST(1, 7, 0, 0, 0, 0, 1, 1));
    Ad(GS_RGBAQ, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
}

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
    Ad(GS_PRIM, GS_PRIM_PRIM_SPRITE | (1 << 4) | (1 << 8)); // textured, UV
    Ad(GS_UV, Uv(u0, v0));
    Ad(GS_XYZ2, Xyz(sx, sy));
    Ad(GS_UV, Uv(u1, v1));
    Ad(GS_XYZ2, Xyz(sx + 8 * SCALE, sy + 8 * SCALE));
}

static void DrawRect(int x0, int y0, int x1, int y1, uint16_t color, int alpha)
{
    uint8_t r = (color & 0x1F) << 3, g = ((color >> 5) & 0x1F) << 3, b = ((color >> 10) & 0x1F) << 3;

    AdBegin(4);
    Ad(GS_PRIM, GS_PRIM_PRIM_SPRITE | (alpha ? (1 << 6) : 0));
    Ad(GS_RGBAQ, GS_SETREG_RGBAQ(r, g, b, 0x80, 0));
    Ad(GS_XYZ2, Xyz(sOriginX + x0 * SCALE, sOriginY + y0 * SCALE));
    Ad(GS_XYZ2, Xyz(sOriginX + x1 * SCALE, sOriginY + y1 * SCALE));
}

static int WindowCovers(uint16_t h, uint16_t v, int first, int last)
{
    int left = h >> 8, right = h & 0xFF, top = v >> 8, bottom = v & 0xFF;

    // Every pixel of the lines inside, with the GBA's wraparound
    if (left > right || left != 0 || right < GBA_WIDTH)
        return 0;
    for (int y = first; y <= last; y++)
    {
        int inside = top > bottom ? (y >= top || y < bottom) : (y >= top && y < bottom);

        if (!inside)
            return 0;
    }
    return 1;
}

// The layers that show and whether color effects apply, for lines where the
// windows allow it all the same everywhere. -1 if they don't.
static int LayerMask(const struct GsLineState *s, int first, int last)
{
    int winEnabled = (s->dispcnt >> 13) & 7;

    if (!winEnabled)
        return 0x3F;
    // Only a WIN0 over the whole of the lines, where WININ applies
    if ((s->dispcnt & (1 << 13)) && WindowCovers(s->win0h, s->win0v, first, last))
        return s->winin & 0x3F;
    return -1;
}

// Groups of lines drawn with the same registers
static int NextBand(const struct GsLineState *lines, int first)
{
    int last = first;

    while (last + 1 < GBA_HEIGHT && memcmp(&lines[last + 1], &lines[first], sizeof(lines[0])) == 0)
        last++;
    return last;
}

static int SpriteShows(const uint16_t *attr, int first, int last, int *x, int *y, int *w, int *h)
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
    return *x + *w > 0 && *x < GBA_WIDTH && *y + *h > first && *y <= last;
}

int PS2GS_CanDraw(const struct GsLineState *lines, const uint16_t *oam)
{
    for (int first = 0; first < GBA_HEIGHT;)
    {
        const struct GsLineState *s = &lines[first];
        int last = NextBand(lines, first);
        int mode = s->dispcnt & 7;
        int bgs = (s->dispcnt >> 8) & 0xF;
        int mask = LayerMask(s, first, last);
        int blend = (s->bldcnt >> 6) & 3;

        if (mask < 0)
            return 0;
        bgs &= mask;
        if (mode > 1 || (mode == 1 && (bgs & 4)))
            return 0;
        for (int bg = 0; bg < 4; bg++)
        {
            if ((bgs & (1 << bg)) && (s->bgcnt[bg] & 0x40) && (s->mosaic & 0xFF))
                return 0;
        }
        // Color effects on some of what shows: only a fade of all of it
        if ((mask & 0x20) && blend != 0)
        {
            int shown = bgs | ((s->dispcnt & 0x1000) && (mask & 0x10) ? 0x10 : 0) | 0x20;
            int targets = s->bldcnt & 0x3F;

            if (blend == 1 && (targets & shown))
                return 0;
            if (blend >= 2 && (s->bldy & 0x1F) && (targets & shown) && (targets & shown) != shown)
                return 0;
        }
        if ((s->dispcnt & 0x1000) && (mask & 0x10))
        {
            if (!(s->dispcnt & 0x40)) // 2D sprite tiles
                return 0;
            for (int i = 0; i < 128; i++)
            {
                const uint16_t *attr = oam + i * 4;
                int x, y, w, h;

                if (!SpriteShows(attr, first, last, &x, &y, &w, &h))
                    continue;
                if (OAM_AFFINE(attr[0]) || OAM_MODE(attr[0]) != 0
                 || (OAM_MOSAIC(attr[0]) && (s->mosaic & 0xFF00))
                 || (OAM_8BPP(attr[0]) && (OAM_TILE(attr[2]) & 1)))
                    return 0;
            }
        }
        first = last + 1;
    }
    return 1;
}

static void DrawBackground(const struct GsLineState *s, int bg, int first, int last, const uint8_t *vram)
{
    static const uint16_t sMapSizes[4][2] = {{256, 256}, {512, 256}, {256, 512}, {512, 512}};
    uint16_t control = s->bgcnt[bg];
    int is8bpp = (control >> 7) & 1;
    int charBase = (control >> 2) & 3;
    const uint16_t *map = (const uint16_t *)(vram + ((control >> 8) & 0x1F) * 0x800);
    int mapWidth = sMapSizes[control >> 14][0];
    int mapHeight = sMapSizes[control >> 14][1];
    int hofs = s->hofs[bg] & 0x1FF;
    int vofs = s->vofs[bg] & 0x1FF;
    const struct Texture *tex = &sTex[is8bpp ? TEX_BG8 : TEX_BG4];
    int palette = -1;

    UseClut(is8bpp ? CLUT_BG8 : CLUT_BG4);
    if (is8bpp)
    {
        AdBegin(1);
        Ad(GS_TEX0_1, Tex0(tex, CLUT_BG8, 0, 0));
    }
    for (int y = first - ((first + vofs) & 7); y <= last; y += 8)
    {
        int mapY = (y + vofs) & (mapHeight - 1);
        const uint16_t *row = map + (mapY & 0xFF) / 8 * 32;

        if (mapY > 255)
            row += mapWidth > 256 ? 0x800 : 0x400;
        for (int x = -(hofs & 7); x < GBA_WIDTH; x += 8)
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
                    Ad(GS_TEX0_1, Tex0(tex, CLUT_BG4, palette, 0));
                }
            }
            DrawTile(x, y, TileU(tile), TileV(tile), (entry >> 10) & 1, (entry >> 11) & 1);
        }
    }
}

static void DrawSprites(const struct GsLineState *s, int priority, int first, int last, const uint16_t *oam)
{
    for (int i = 127; i >= 0; i--)
    {
        const uint16_t *attr = oam + i * 4;
        int x, y, w, h;

        if (OAM_PRIORITY(attr[2]) != priority || !SpriteShows(attr, first, last, &x, &y, &w, &h))
            continue;

        int is8bpp = OAM_8BPP(attr[0]);
        int hflip = OAM_HFLIP(attr[1]);
        int vflip = OAM_VFLIP(attr[1]);
        int tilesWide = w / 8;
        const struct Texture *tex = &sTex[is8bpp ? TEX_OBJ8 : TEX_OBJ4];

        UseClut(is8bpp ? CLUT_OBJ8 : CLUT_OBJ4);
        AdBegin(1);
        Ad(GS_TEX0_1, Tex0(tex, is8bpp ? CLUT_OBJ8 : CLUT_OBJ4, 16 + (is8bpp ? 0 : OAM_PALETTE(attr[2])), 0));
        for (int ty = 0; ty < h / 8; ty++)
        {
            int screenY = y + (vflip ? h / 8 - 1 - ty : ty) * 8;

            if (screenY + 8 <= first || screenY > last)
                continue;
            for (int tx = 0; tx < tilesWide; tx++)
            {
                int screenX = x + (hflip ? tilesWide - 1 - tx : tx) * 8;
                int tile = ty * tilesWide + tx;

                if (screenX + 8 <= 0 || screenX >= GBA_WIDTH)
                    continue;
                tile = is8bpp ? (OAM_TILE(attr[2]) / 2 + tile) % OBJ_TILES_8BPP
                              : (OAM_TILE(attr[2]) + tile) % OBJ_TILES_4BPP;
                DrawTile(screenX, screenY, TileU(tile), TileV(tile), hflip, vflip);
            }
        }
    }
}

static void BeginFrame(void)
{
    sLoadedClut[0] = sLoadedClut[1] = -1;
    gsKit_clear(sGs, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));
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
    MakeClut(sClut[CLUT_BG4], pltt, 0);
    MakeClut(sClut[CLUT_OBJ4], pltt + 256, 0);
    MakeClut(sClut[CLUT_BG8], pltt, 1);
    MakeClut(sClut[CLUT_OBJ8], pltt + 256, 1);
    for (int i = 0; i < CLUT_COUNT; i++)
        UploadClut(i);
    SetDrawState();

    for (int first = 0; first < GBA_HEIGHT;)
    {
        const struct GsLineState *s = &lines[first];
        int last = NextBand(lines, first);
        int mask = LayerMask(s, first, last);
        int bgs = (s->dispcnt >> 8) & 0xF & mask;
        int blend = (s->bldcnt >> 6) & 3;
        int evy = s->bldy & 0x1F;

        SetScissor(first, last);
        DrawRect(0, first, GBA_WIDTH, last + 1, pltt[0], 0);
        for (int priority = 3; priority >= 0; priority--)
        {
            for (int bg = 3; bg >= 0; bg--)
            {
                if ((bgs & (1 << bg)) && (s->bgcnt[bg] & 3) == priority)
                    DrawBackground(s, bg, first, last, vram);
            }
            if ((s->dispcnt & 0x1000) && (mask & 0x10))
                DrawSprites(s, priority, first, last, oam);
        }
        // A fade of all of it, which PS2GS_CanDraw made sure of
        if ((mask & 0x20) && blend >= 2 && evy && (s->bldcnt & 0x20))
        {
            if (evy > 16)
                evy = 16;
            AdBegin(1);
            Ad(GS_ALPHA_1, GS_SETREG_ALPHA(0, 1, 2, 1, evy * 8)); // (Cs - Cd) * evy / 16 + Cd
            DrawRect(0, first, GBA_WIDTH, last + 1, blend == 2 ? 0x7FFF : 0, 1);
        }
        first = last + 1;
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
    SetScissor(0, GBA_HEIGHT - 1);
    AdBegin(6);
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
