#ifndef GUARD_PLATFORM_PS2_GS_H
#define GUARD_PLATFORM_PS2_GS_H

// Draws the GBA's picture with the PS2's Graphics Synthesizer, from the video
// registers of each line and the GBA's VRAM, palettes and OAM. It only takes
// plain C types, as ps2sdk's u32 and the game's u32 are different types.

#include <stdint.h>

// The video registers a line is drawn with
struct GsLineState
{
    uint16_t dispcnt;
    uint16_t bgcnt[4];
    uint16_t hofs[4];
    uint16_t vofs[4];
    uint16_t bldcnt;
    uint16_t bldalpha;
    uint16_t bldy;
    uint16_t winin;
    uint16_t winout;
    uint16_t win0h, win0v;
    uint16_t win1h, win1v;
    uint16_t mosaic;
};

void PS2GS_Init(void);
// Whether the GS can draw a frame like the GBA would. Otherwise it's drawn in
// software and shown with PS2GS_DrawImage.
int PS2GS_CanDraw(const struct GsLineState *lines, const uint16_t *oam);
void PS2GS_DrawLines(const struct GsLineState *lines, const uint8_t *vram, const uint16_t *pltt, const uint16_t *oam);
// A 240x160 picture in the GBA's colors
void PS2GS_DrawImage(const uint16_t *image);
// Shows what was drawn, at the next vertical blank
void PS2GS_Present(void);

#endif // GUARD_PLATFORM_PS2_GS_H
