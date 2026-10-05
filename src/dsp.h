/* Shared DSP building blocks. Internal to the library. */
#ifndef DOOMFX_DSP_H
#define DOOMFX_DSP_H

#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Expensive coefficients (tan, exp) are refreshed every this many samples. */
#define DFX_CTRL_INTERVAL 16

static inline float dfx_clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float dfx_db(float db) { return powf(10.0f, db * 0.05f); }
static inline float dfx_lerp(float a, float b, float t) { return a + (b - a) * t; }

/* Exponential map of t in 0..1 onto lo..hi (both > 0). */
static inline float dfx_expmap(float lo, float hi, float t) { return lo * powf(hi / lo, t); }

/* Smooth tanh-like clipper, exact at 0, flat at +/-1 beyond |x| = 3. */
static inline float dfx_sat(float x)
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Flush tiny values to zero so filters decaying into silence stay cheap. */
static inline float dfx_undenormal(float x) { return fabsf(x) < 1e-20f ? 0.0f : x; }

/* --- one-pole smoother ------------------------------------------------- */

typedef struct { float y, a; } dfx_smooth;

static inline void dfx_smooth_init(dfx_smooth *s, float value, float time_ms, float sr)
{
    s->y = value;
    s->a = 1.0f - expf(-1.0f / (time_ms * 0.001f * sr));
}
static inline float dfx_smooth_step(dfx_smooth *s, float target) { s->y += (target - s->y) * s->a; return s->y; }

/* --- biquad (RBJ cookbook, transposed direct form II) ------------------ */

typedef struct { float b0, b1, b2, a1, a2; } dfx_bq_coef;
typedef struct { float z1, z2; } dfx_bq_state;

static inline float dfx_bq_run(const dfx_bq_coef *c, dfx_bq_state *s, float x)
{
    float y = c->b0 * x + s->z1;
    s->z1 = dfx_undenormal(c->b1 * x - c->a1 * y + s->z2);
    s->z2 = dfx_undenormal(c->b2 * x - c->a2 * y);
    return y;
}

typedef enum { DFX_BQ_LP, DFX_BQ_HP, DFX_BQ_LOWSHELF, DFX_BQ_HIGHSHELF, DFX_BQ_PEAK } dfx_bq_type;

static inline void dfx_bq_design(dfx_bq_coef *c, dfx_bq_type type, float f, float q, float gain_db, float sr)
{
    f = dfx_clampf(f, 5.0f, sr * 0.49f);
    float w = 2.0f * (float)M_PI * f / sr;
    float cw = cosf(w), sw = sinf(w);
    float alpha = sw / (2.0f * q);
    float A = powf(10.0f, gain_db / 40.0f);
    float b0, b1, b2, a0, a1, a2;
    switch (type) {
    case DFX_BQ_LP:
        b0 = (1 - cw) * 0.5f; b1 = 1 - cw; b2 = b0;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case DFX_BQ_HP:
        b0 = (1 + cw) * 0.5f; b1 = -(1 + cw); b2 = b0;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case DFX_BQ_PEAK:
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
        break;
    case DFX_BQ_LOWSHELF: {
        float sa = 2 * sqrtf(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + sa);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - sa);
        a0 = (A + 1) + (A - 1) * cw + sa;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - sa;
        break;
    }
    default: { /* DFX_BQ_HIGHSHELF */
        float sa = 2 * sqrtf(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + sa);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - sa);
        a0 = (A + 1) - (A - 1) * cw + sa;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - sa;
        break;
    }
    }
    c->b0 = b0 / a0; c->b1 = b1 / a0; c->b2 = b2 / a0;
    c->a1 = a1 / a0; c->a2 = a2 / a0;
}

/* 4th-order Butterworth lowpass as two biquads, used as converter anti-alias / reconstruction filters. */
typedef struct { dfx_bq_coef c[2]; } dfx_lp4_coef;
typedef struct { dfx_bq_state s[2]; } dfx_lp4_state;

static inline void dfx_lp4_design(dfx_lp4_coef *c, float f, float sr)
{
    dfx_bq_design(&c->c[0], DFX_BQ_LP, f, 0.5411961f, 0, sr);
    dfx_bq_design(&c->c[1], DFX_BQ_LP, f, 1.3065630f, 0, sr);
}
static inline float dfx_lp4_run(const dfx_lp4_coef *c, dfx_lp4_state *s, float x)
{
    return dfx_bq_run(&c->c[1], &s->s[1], dfx_bq_run(&c->c[0], &s->s[0], x));
}

/* --- random ------------------------------------------------------------- */

static inline uint32_t dfx_rand(uint32_t *state)
{
    uint32_t x = *state ? *state : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *state = x;
    return x;
}
/* Uniform in [-1, 1). */
static inline float dfx_randf(uint32_t *state) { return (float)(dfx_rand(state) >> 8) * (2.0f / 16777216.0f) - 1.0f; }

/* --- converter model ---------------------------------------------------- */

/*
 * Quantise x (full scale +/-1) to `bits`, with optional mu-law style
 * companding (mu = 0 is linear). Values beyond full scale clip hard, the
 * way a real converter does.
 */
static inline float dfx_quantize(float x, float bits, float mu)
{
    float steps = exp2f(bits - 1.0f);
    x = dfx_clampf(x, -1.0f, 1.0f);
    if (mu > 0.001f) {
        float lmu = logf(1.0f + mu);
        float ax = logf(1.0f + mu * fabsf(x)) / lmu;
        ax = dfx_clampf(floorf(ax * steps + 0.5f), 0.0f, steps - 1.0f) / steps;
        ax = (expf(ax * lmu) - 1.0f) / mu;
        return x < 0 ? -ax : ax;
    }
    return dfx_clampf(floorf(x * steps + 0.5f), -steps, steps - 1.0f) / steps;
}

/*
 * Sample-and-hold at a lower rate than the host, with no filtering: this
 * is what makes the aliasing and the staircase (zero-order hold) of an
 * old sampler's converters. The input is captured at the exact fractional
 * time of each slow-clock tick by linear interpolation, so the slow clock
 * does not jitter to the host's sample grid.
 */
typedef struct {
    float phase;  /* 0..1 position of the slow clock */
    float prev_in;
    float held;
} dfx_sh;

/* ratio = slow rate / host rate. Returns the held value; *ticked is set when a new sample was taken. */
static inline float dfx_sh_run(dfx_sh *s, float x, float ratio, float bits, float mu, int *ticked)
{
    *ticked = 0;
    if (ratio >= 1.0f) {
        s->held = dfx_quantize(x, bits, mu);
        *ticked = 1;
    } else {
        s->phase += ratio;
        if (s->phase >= 1.0f) {
            s->phase -= 1.0f;
            /* the tick happened phase/ratio host samples ago */
            float back = s->phase / ratio;
            s->held = dfx_quantize(x + (s->prev_in - x) * back, bits, mu);
            *ticked = 1;
        }
    }
    s->prev_in = x;
    return s->held;
}

/* --- zero-delay-feedback 4-pole ladder (Zavalishin TPT) --------------- */

/*
 * A transistor-ladder style lowpass used for the SSM2044 style output
 * filters and the bit ladder. `k` is the feedback amount (0..4, self
 * oscillation near 4). The input to the ladder is soft clipped, which
 * keeps it stable and gives the familiar squash at high resonance.
 */
typedef struct { float s[4]; } dfx_ladder;
typedef struct { float G, g1; } dfx_ladder_coef; /* G = g/(1+g), g1 = 1/(1+g) */

static inline void dfx_ladder_design(dfx_ladder_coef *c, float f, float sr)
{
    f = dfx_clampf(f, 10.0f, sr * 0.45f);
    float g = tanf((float)M_PI * f / sr);
    c->G = g / (1.0f + g);
    c->g1 = 1.0f / (1.0f + g);
}

static inline float dfx_ladder_run(dfx_ladder *l, const dfx_ladder_coef *c, float x, float k)
{
    float G = c->G, G2 = G * G;
    float S = G2 * G * l->s[0] * c->g1 + G2 * l->s[1] * c->g1 + G * l->s[2] * c->g1 + l->s[3] * c->g1;
    float u = (x - k * S) / (1.0f + k * G2 * G2);
    u = dfx_sat(u);
    for (int i = 0; i < 4; i++) {
        float v = (u - l->s[i]) * G;
        float y = v + l->s[i];
        l->s[i] = dfx_undenormal(y + v);
        u = y;
    }
    return u;
}

/* --- 2-pole state variable filter (TPT) -------------------------------- */

typedef struct { float ic1, ic2; } dfx_svf;
typedef struct { float a1, a2, a3, k; } dfx_svf_coef;

static inline void dfx_svf_design(dfx_svf_coef *c, float f, float q, float sr)
{
    f = dfx_clampf(f, 10.0f, sr * 0.45f);
    float g = tanf((float)M_PI * f / sr);
    c->k = 1.0f / q;
    c->a1 = 1.0f / (1.0f + g * (g + c->k));
    c->a2 = g * c->a1;
    c->a3 = g * c->a2;
}

/* Returns the lowpass output. */
static inline float dfx_svf_lp(dfx_svf *s, const dfx_svf_coef *c, float x)
{
    float v3 = x - s->ic2;
    float v1 = c->a1 * s->ic1 + c->a2 * v3;
    float v2 = s->ic2 + c->a2 * s->ic1 + c->a3 * v3;
    s->ic1 = dfx_undenormal(2.0f * v1 - s->ic1);
    s->ic2 = dfx_undenormal(2.0f * v2 - s->ic2);
    return v2;
}

#endif
