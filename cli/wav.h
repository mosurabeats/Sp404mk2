/* Minimal WAV reading and writing for the command line tool. */
#ifndef DOOMFX_WAV_H
#define DOOMFX_WAV_H

typedef struct {
    int sample_rate;
    int frames;
    float *left, *right; /* mono files are copied to both sides */
} wav_audio;

/* Reads 8/16/24/32-bit PCM or 32-bit float. Returns 0 on success, or prints an error and returns -1. */
int wav_read(const char *path, wav_audio *out);

/* Writes stereo 16 or 24-bit PCM with clipping. Returns 0 on success. */
int wav_write(const char *path, const wav_audio *in, int bits);

int wav_alloc(wav_audio *a, int sample_rate, int frames);
void wav_free(wav_audio *a);

#endif
