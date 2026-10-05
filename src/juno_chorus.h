/*
 * Juno style BBD chorus. Internal to the library.
 *
 * A mono input feeds two delay taps swept by one triangle LFO in opposite
 * phase, and each side is dry plus one tap. That's what gives the Juno its
 * wide, moving stereo from a mono synth. The wet signal is lowpassed like a
 * bucket brigade delay's clock filters, with optional BBD hiss.
 *
 * Mode timings are the commonly cited measurements of the Juno chorus:
 *   I     0.513 Hz, 1.66 .. 5.35 ms
 *   II    0.863 Hz, 1.66 .. 5.35 ms
 *   I+II  9.75 Hz,  3.28 .. 3.71 ms (fast, shallow: almost a vibrato)
 * Treat them as approximations.
 */
#ifndef DOOMFX_JUNO_CHORUS_H
#define DOOMFX_JUNO_CHORUS_H

#include "dsp.h"

#define JC_BUF 2048 /* power of two; holds 5.35 ms up to ~380 kHz */

enum { JC_OFF, JC_I, JC_II, JC_I_II };

typedef struct {
    float buf[JC_BUF];
    int pos;
    float phase;
    float rate, center, excursion; /* Hz, samples, samples */
    dfx_bq_coef lp;
    dfx_bq_state lp_s[2];
    uint32_t rng;
} juno_chorus;

/* depth scales the sweep: 1 = the original */
static inline void juno_chorus_set(juno_chorus *c, int mode, float depth, float sr)
{
    float rate = 0.513f, lo = 1.66f, hi = 5.35f;
    if (mode == JC_II) rate = 0.863f;
    if (mode == JC_I_II) { rate = 9.75f; lo = 3.28f; hi = 3.71f; }
    float mid = 0.5f * (lo + hi), half = 0.5f * (hi - lo) * depth;
    c->rate = rate;
    c->center = mid * 0.001f * sr;
    c->excursion = half * 0.001f * sr;
    dfx_bq_design(&c->lp, DFX_BQ_LP, 9000.0f, 0.707f, 0, sr);
}

static inline void juno_chorus_reset(juno_chorus *c)
{
    for (int i = 0; i < JC_BUF; i++) c->buf[i] = 0.0f;
    c->pos = 0;
    c->phase = 0.0f;
    c->lp_s[0] = c->lp_s[1] = (dfx_bq_state){ 0 };
    c->rng = 0x1234567u;
}

static inline float juno_chorus_tap(const juno_chorus *c, float delay)
{
    float max = (float)(JC_BUF - 2);
    delay = dfx_clampf(delay, 1.0f, max);
    float rp = (float)c->pos - delay;
    if (rp < 0) rp += (float)JC_BUF;
    int i0 = (int)rp;
    float frac = rp - (float)i0;
    float a = c->buf[i0 & (JC_BUF - 1)], b = c->buf[(i0 + 1) & (JC_BUF - 1)];
    return a + (b - a) * frac;
}

/* One sample: writes the wet left and right taps. hiss is a linear noise level. */
static inline void juno_chorus_run(juno_chorus *c, float mono, float hiss, float sr, float *wet_l, float *wet_r)
{
    c->buf[c->pos] = mono + hiss * dfx_randf(&c->rng);
    c->phase += c->rate / sr;
    if (c->phase >= 1.0f) c->phase -= 1.0f;
    float tri = 4.0f * fabsf(c->phase - 0.5f) - 1.0f;
    float l = juno_chorus_tap(c, c->center + c->excursion * tri);
    float r = juno_chorus_tap(c, c->center - c->excursion * tri);
    *wet_l = dfx_bq_run(&c->lp, &c->lp_s[0], l);
    *wet_r = dfx_bq_run(&c->lp, &c->lp_s[1], r);
    c->pos = (c->pos + 1) & (JC_BUF - 1);
}

#endif
