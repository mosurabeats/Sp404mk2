/*
 * Minimal PNG writer: 8-bit RGB, no zlib needed. Compression is deflate with
 * the fixed Huffman code and two kinds of match, "same as the last pixel"
 * and "same as the row above", which is all that flat screenshots need.
 */
#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const unsigned char *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 255] ^ (c >> 8);
    return ~c;
}

static void be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

static void chunk(FILE *f, const char *type, const unsigned char *data, size_t n)
{
    unsigned char b[4];
    be32(b, (uint32_t)n);
    fwrite(b, 1, 4, f);
    uint32_t c = crc(0, (const unsigned char *)type, 4);
    c = crc(c, data, n);
    fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    be32(b, c);
    fwrite(b, 1, 4, f);
}

typedef struct { unsigned char *p; size_t n; uint32_t bits; int nbits; } bitw;

static void put_bits(bitw *w, uint32_t v, int n)
{
    w->bits |= v << w->nbits;
    w->nbits += n;
    while (w->nbits >= 8) { w->p[w->n++] = (unsigned char)w->bits; w->bits >>= 8; w->nbits -= 8; }
}

/* Huffman codes go most significant bit first. */
static void put_code(bitw *w, uint32_t code, int n)
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++) r |= ((code >> i) & 1u) << (n - 1 - i);
    put_bits(w, r, n);
}

static void put_sym(bitw *w, int sym)
{
    if (sym < 144) put_code(w, 0x30u + (uint32_t)sym, 8);
    else if (sym < 256) put_code(w, 0x190u + (uint32_t)(sym - 144), 9);
    else if (sym < 280) put_code(w, (uint32_t)(sym - 256), 7);
    else put_code(w, 0xC0u + (uint32_t)(sym - 280), 8);
}

static const int len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const int len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const int dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const int dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void put_match(bitw *w, int len, int dist)
{
    int i = 28;
    while (len_base[i] > len) i--;
    put_sym(w, 257 + i);
    put_bits(w, (uint32_t)(len - len_base[i]), len_extra[i]);
    int d = 29;
    while (dist_base[d] > dist) d--;
    put_code(w, (uint32_t)d, 5);
    put_bits(w, (uint32_t)(dist - dist_base[d]), dist_extra[d]);
}

static size_t match_len(const unsigned char *p, size_t i, size_t n, size_t dist)
{
    if (dist > i || dist > 32768) return 0;
    size_t l = 0;
    while (l < 258 && i + l < n && p[i + l] == p[i + l - dist]) l++;
    return l;
}

int png_write_rgb(const char *path, const uint8_t *rgb, int w, int h)
{
    size_t row = (size_t)w * 3 + 1, raw_n = row * (size_t)h;
    unsigned char *raw = malloc(raw_n);
    unsigned char *z = malloc(raw_n + raw_n / 4 + 64); /* fixed codes never grow data by more than 9/8 */
    if (!raw || !z) { free(raw); free(z); return -1; }
    for (int y = 0; y < h; y++) {
        raw[y * row] = 0; /* filter: none */
        memcpy(raw + y * row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    bitw bw = { z, 0, 0, 0 };
    z[bw.n++] = 0x78; z[bw.n++] = 0x01;
    put_bits(&bw, 1, 1); /* final block */
    put_bits(&bw, 1, 2); /* fixed Huffman */
    for (size_t i = 0; i < raw_n;) {
        size_t a = match_len(raw, i, raw_n, 3), b = match_len(raw, i, raw_n, row);
        size_t l = a >= b ? a : b, d = a >= b ? 3 : row;
        if (l >= 3) {
            put_match(&bw, (int)l, (int)d);
            i += l;
        } else {
            put_sym(&bw, raw[i++]);
        }
    }
    put_sym(&bw, 256);
    if (bw.nbits) put_bits(&bw, 0, 8 - bw.nbits);
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    be32(z + bw.n, (b << 16) | a);
    size_t o = bw.n + 4;

    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return -1; }
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    unsigned char ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, o);
    chunk(f, "IEND", NULL, 0);
    int err = ferror(f);
    fclose(f);
    free(raw);
    free(z);
    return err ? -1 : 0;
}
