#include "wav.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

int wav_alloc(wav_audio *a, int sample_rate, int frames)
{
    a->sample_rate = sample_rate;
    a->frames = frames;
    a->left = calloc((size_t)frames + 1, sizeof(float));
    a->right = calloc((size_t)frames + 1, sizeof(float));
    return (a->left && a->right) ? 0 : -1;
}

void wav_free(wav_audio *a)
{
    free(a->left);
    free(a->right);
    a->left = a->right = NULL;
}

static float decode(const unsigned char *p, int bits, int is_float)
{
    if (is_float) {
        float f;
        uint32_t u = rd32(p);
        memcpy(&f, &u, 4);
        return f;
    }
    switch (bits) {
    case 8: return ((int)p[0] - 128) / 128.0f;
    case 16: return (int16_t)rd16(p) / 32768.0f;
    case 24: return (int32_t)((uint32_t)(p[0] << 8 | p[1] << 16 | (uint32_t)p[2] << 24)) / 2147483648.0f;
    default: return (int32_t)rd32(p) / 2147483648.0f;
    }
}

int wav_read(const char *path, wav_audio *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "error: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f); free(buf);
        fprintf(stderr, "error: cannot read %s\n", path);
        return -1;
    }
    fclose(f);

    int ret = -1;
    if (size < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        fprintf(stderr, "error: %s is not a WAV file\n", path);
        goto done;
    }
    int channels = 0, rate = 0, bits = 0, is_float = 0;
    const unsigned char *data = NULL;
    uint32_t data_size = 0;
    for (long pos = 12; pos + 8 <= size;) {
        uint32_t len = rd32(buf + pos + 4);
        const unsigned char *body = buf + pos + 8;
        if (len > (uint32_t)(size - pos - 8)) len = (uint32_t)(size - pos - 8);
        if (!memcmp(buf + pos, "fmt ", 4) && len >= 16) {
            int format = rd16(body);
            channels = rd16(body + 2);
            rate = (int)rd32(body + 4);
            bits = rd16(body + 14);
            if (format == 0xFFFE && len >= 26) format = rd16(body + 24);
            if (format == 3) is_float = 1;
            else if (format != 1) { fprintf(stderr, "error: %s: unsupported WAV format %d\n", path, format); goto done; }
        } else if (!memcmp(buf + pos, "data", 4)) {
            data = body;
            data_size = len;
        }
        pos += 8 + len + (len & 1);
    }
    if (!data || !channels || !rate) { fprintf(stderr, "error: %s: missing fmt or data chunk\n", path); goto done; }
    if (is_float ? bits != 32 : (bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
        fprintf(stderr, "error: %s: unsupported bit depth %d\n", path, bits);
        goto done;
    }
    int frame_bytes = channels * bits / 8;
    int frames = (int)(data_size / (uint32_t)frame_bytes);
    if (wav_alloc(out, rate, frames)) { fprintf(stderr, "error: out of memory\n"); goto done; }
    for (int i = 0; i < frames; i++) {
        const unsigned char *p = data + (size_t)i * frame_bytes;
        out->left[i] = decode(p, bits, is_float);
        out->right[i] = channels > 1 ? decode(p + bits / 8, bits, is_float) : out->left[i];
    }
    ret = 0;
done:
    free(buf);
    return ret;
}

static void put32(FILE *f, uint32_t v) { unsigned char b[4] = { v, v >> 8, v >> 16, v >> 24 }; fwrite(b, 1, 4, f); }
static void put16(FILE *f, uint16_t v) { unsigned char b[2] = { v, v >> 8 }; fwrite(b, 1, 2, f); }

int wav_write(const char *path, const wav_audio *in, int bits)
{
    if (bits != 16) bits = 24;
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "error: cannot write %s\n", path); return -1; }
    uint32_t data_size = (uint32_t)in->frames * 2 * (uint32_t)(bits / 8);
    fwrite("RIFF", 1, 4, f); put32(f, 36 + data_size); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 2);
    put32(f, (uint32_t)in->sample_rate); put32(f, (uint32_t)in->sample_rate * 2 * (bits / 8));
    put16(f, (uint16_t)(2 * bits / 8)); put16(f, (uint16_t)bits);
    fwrite("data", 1, 4, f); put32(f, data_size);
    double full = bits == 16 ? 32767.0 : 8388607.0;
    for (int i = 0; i < in->frames; i++) {
        float s[2] = { in->left[i], in->right[i] };
        for (int c = 0; c < 2; c++) {
            double v = s[c] * full;
            if (v != v) v = 0; /* NaN */
            v = v > full ? full : (v < -full - 1 ? -full - 1 : v);
            int32_t q = (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
            if (bits == 16) put16(f, (uint16_t)(int16_t)q);
            else { unsigned char b[3] = { q, q >> 8, q >> 16 }; fwrite(b, 1, 3, f); }
        }
    }
    int err = ferror(f);
    fclose(f);
    if (err) { fprintf(stderr, "error: writing %s failed\n", path); return -1; }
    return 0;
}
