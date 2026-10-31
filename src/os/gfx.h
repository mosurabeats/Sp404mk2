/* 1-bit drawing for the 128 x 64 screen: a 3x5 font on a 4x6 grid, rectangles, inversion. */
#ifndef MOS_GFX_H
#define MOS_GFX_H

#include <stdint.h>

/* glyphs for the control characters in strings */
#define G_PLAY  "\x01"
#define G_REC   "\x02"
#define G_STOP  "\x03"
#define G_ARROW "\x04"
#define G_NOTE  "\x05"
#define G_BLOCK "\x06"

void g_clear(uint8_t *fb);
void g_pixel(uint8_t *fb, int x, int y, int v);
void g_fill(uint8_t *fb, int x, int y, int w, int h, int v);
void g_frame(uint8_t *fb, int x, int y, int w, int h, int v);
void g_invert(uint8_t *fb, int x, int y, int w, int h);
/* Draws text with its top-left at x, y, each glyph `scale` times its size; returns the x after it. */
int g_text(uint8_t *fb, int x, int y, const char *s, int v, int scale);
int g_text_width(const char *s, int scale);

#endif
