#include "gfx.h"
#include "../../include/mos.h"

/*
 * 3x5 font. Each glyph is five rows, top first; bit 2 is the left column.
 * Square digits on purpose: it reads like the MPC's LCD. Lower case is
 * drawn as upper case.
 */
static const uint8_t font[96][5] = {
    { 0, 0, 0, 0, 0 }, /* space */
    { 2, 2, 2, 0, 2 }, /* ! */
    { 5, 5, 0, 0, 0 }, /* " */
    { 5, 7, 5, 7, 5 }, /* # */
    { 3, 6, 2, 3, 6 }, /* $ */
    { 5, 1, 2, 4, 5 }, /* % */
    { 2, 5, 2, 5, 3 }, /* & */
    { 2, 2, 0, 0, 0 }, /* ' */
    { 1, 2, 2, 2, 1 }, /* ( */
    { 4, 2, 2, 2, 4 }, /* ) */
    { 0, 5, 2, 5, 0 }, /* * */
    { 0, 2, 7, 2, 0 }, /* + */
    { 0, 0, 0, 2, 4 }, /* , */
    { 0, 0, 7, 0, 0 }, /* - */
    { 0, 0, 0, 0, 2 }, /* . */
    { 1, 1, 2, 4, 4 }, /* / */
    { 7, 5, 5, 5, 7 }, /* 0 */
    { 2, 6, 2, 2, 7 }, /* 1 */
    { 7, 1, 7, 4, 7 }, /* 2 */
    { 7, 1, 3, 1, 7 }, /* 3 */
    { 5, 5, 7, 1, 1 }, /* 4 */
    { 7, 4, 7, 1, 7 }, /* 5 */
    { 7, 4, 7, 5, 7 }, /* 6 */
    { 7, 1, 1, 2, 2 }, /* 7 */
    { 7, 5, 7, 5, 7 }, /* 8 */
    { 7, 5, 7, 1, 7 }, /* 9 */
    { 0, 2, 0, 2, 0 }, /* : */
    { 0, 2, 0, 2, 4 }, /* ; */
    { 1, 2, 4, 2, 1 }, /* < */
    { 0, 7, 0, 7, 0 }, /* = */
    { 4, 2, 1, 2, 4 }, /* > */
    { 7, 1, 3, 0, 2 }, /* ? */
    { 2, 5, 7, 4, 3 }, /* @ */
    { 2, 5, 7, 5, 5 }, /* A */
    { 6, 5, 6, 5, 6 }, /* B */
    { 3, 4, 4, 4, 3 }, /* C */
    { 6, 5, 5, 5, 6 }, /* D */
    { 7, 4, 6, 4, 7 }, /* E */
    { 7, 4, 6, 4, 4 }, /* F */
    { 3, 4, 5, 5, 3 }, /* G */
    { 5, 5, 7, 5, 5 }, /* H */
    { 7, 2, 2, 2, 7 }, /* I */
    { 1, 1, 1, 5, 2 }, /* J */
    { 5, 5, 6, 5, 5 }, /* K */
    { 4, 4, 4, 4, 7 }, /* L */
    { 5, 7, 7, 5, 5 }, /* M */
    { 6, 5, 5, 5, 5 }, /* N */
    { 2, 5, 5, 5, 2 }, /* O */
    { 6, 5, 6, 4, 4 }, /* P */
    { 2, 5, 5, 6, 3 }, /* Q */
    { 6, 5, 6, 5, 5 }, /* R */
    { 3, 4, 2, 1, 6 }, /* S */
    { 7, 2, 2, 2, 2 }, /* T */
    { 5, 5, 5, 5, 7 }, /* U */
    { 5, 5, 5, 5, 2 }, /* V */
    { 5, 5, 7, 7, 5 }, /* W */
    { 5, 5, 2, 5, 5 }, /* X */
    { 5, 5, 2, 2, 2 }, /* Y */
    { 7, 1, 2, 4, 7 }, /* Z */
    { 6, 4, 4, 4, 6 }, /* [ */
    { 4, 4, 2, 1, 1 }, /* \ */
    { 3, 1, 1, 1, 3 }, /* ] */
    { 2, 5, 0, 0, 0 }, /* ^ */
    { 0, 0, 0, 0, 7 }, /* _ */
    { 4, 2, 0, 0, 0 }, /* ` */
    /* control glyphs, reached through codes 1..6 */
    { 4, 6, 7, 6, 4 }, /* 1: play */
    { 2, 7, 7, 7, 2 }, /* 2: record */
    { 0, 7, 7, 7, 0 }, /* 3: stop */
    { 0, 4, 6, 4, 0 }, /* 4: small arrow */
    { 3, 2, 2, 6, 6 }, /* 5: note */
    { 7, 7, 7, 7, 7 }, /* 6: block */
};

/* rows 97..: { | } ~ */
static const uint8_t font_tail[4][5] = {
    { 3, 2, 6, 2, 3 }, /* { */
    { 2, 2, 2, 2, 2 }, /* | */
    { 6, 2, 3, 2, 6 }, /* } */
    { 0, 3, 6, 0, 0 }, /* ~ */
};

static const uint8_t *glyph(unsigned char c)
{
    if (c >= 1 && c <= 6) return font[64 + c];
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
    if (c >= 32 && c <= 96) return font[c - 32];
    if (c >= 123 && c <= 126) return font_tail[c - 123];
    return font[31]; /* ? */
}

void g_clear(uint8_t *fb)
{
    for (int i = 0; i < MOS_LCD_W * MOS_LCD_H; i++) fb[i] = 0;
}

void g_pixel(uint8_t *fb, int x, int y, int v)
{
    if (x < 0 || y < 0 || x >= MOS_LCD_W || y >= MOS_LCD_H) return;
    fb[y * MOS_LCD_W + x] = (uint8_t)(v ? 1 : 0);
}

void g_fill(uint8_t *fb, int x, int y, int w, int h, int v)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) g_pixel(fb, x + i, y + j, v);
}

void g_frame(uint8_t *fb, int x, int y, int w, int h, int v)
{
    for (int i = 0; i < w; i++) { g_pixel(fb, x + i, y, v); g_pixel(fb, x + i, y + h - 1, v); }
    for (int j = 0; j < h; j++) { g_pixel(fb, x, y + j, v); g_pixel(fb, x + w - 1, y + j, v); }
}

void g_invert(uint8_t *fb, int x, int y, int w, int h)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && j >= 0 && i < MOS_LCD_W && j < MOS_LCD_H) fb[j * MOS_LCD_W + i] ^= 1;
}

int g_text(uint8_t *fb, int x, int y, const char *s, int v, int scale)
{
    for (; *s; s++) {
        const uint8_t *g = glyph((unsigned char)*s);
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (g[row] & (4 >> col)) g_fill(fb, x + col * scale, y + row * scale, scale, scale, v);
        x += 4 * scale;
    }
    return x;
}

int g_text_width(const char *s, int scale)
{
    int n = 0;
    while (s[n]) n++;
    return n ? n * 4 * scale - scale : 0;
}
