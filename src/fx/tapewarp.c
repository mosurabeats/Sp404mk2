/*
 * Tape Warp (new): a worn cassette / 4-track deck.
 *
 * Wow is the slow pitch drift of a stretched tape or an uneven capstan,
 * with a random walk on top so it never loops audibly. Flutter is the fast
 * shimmer from the pinch roller. Both modulate a short delay line, so the
 * pitch actually bends. Sat drives the tape (soft compression and odd
 * harmonics), Age rolls off the top end and adds hiss, and Bump is the
 * low-frequency head bump around 90 Hz that makes tape bass feel big.
 *
 * Character model: the rates and depths are in the range of real machines,
 * the voicing is by ear.
 */
#include "../../include/doomfx.h"
#include "../dsp.h"

enum { P_WOW, P_FLUTTER, P_SAT, P_AGE, P_BUMP, P_HISS, P_MIX, P_LEVEL, P_COUNT };

static const dfx_param params[P_COUNT] = {
    { "wow",     "Wow",     0, 100, 30, "%", 0, 0, 0 },
    { "flutter", "Flutter", 0, 100, 20, "%", 0, 0, 0 },
    { "sat",     "Sat",     0, 100, 35, "%", 0, 0, 0 },
    { "age",     "Age",     0, 100, 30, "%", 0, 0, 0 },
    { "bump",    "Bump",    0, 9, 3, "dB", 0, 0, 0 },
    { "hiss",    "Hiss",    0, 100, 15, "%", 0, 0, 0 },
    { "mix",     "Mix",     0, 100, 100, "%", 0, 0, 0 },
    { "level",   "Level",   -24, 12, 0, "dB", 0, 0, 0 },
};

#define BUF 2048 /* 42 ms at 48 kHz, plenty for the modulation */
#define MASK (BUF - 1)

typedef struct {
    float sr;
    float p[P_COUNT];
    dfx_smooth wow, flutter, sat, mix, level;
    int ctrl, fresh;
    float buf[2][BUF];
    unsigned w;
    float wow_ph, flut_ph;
    float drift, drift_target;
    int drift_count;
    uint32_t rng;
    dfx_bq_coef lp, hp, bump;
    dfx_bq_state lp_s[2], hp_s[2], bump_s[2];
} tapewarp;

static void update_coefs(tapewarp *s)
{
    float age = s->p[P_AGE] * 0.01f;
    dfx_bq_design(&s->lp, DFX_BQ_LP, dfx_expmap(19000.0f, 3200.0f, age), 0.6f, 0, s->sr);
    dfx_bq_design(&s->hp, DFX_BQ_HP, 25.0f + 35.0f * age, 0.6f, 0, s->sr);
    dfx_bq_design(&s->bump, DFX_BQ_PEAK, 90.0f, 1.1f, s->p[P_BUMP], s->sr);
}

static void reset(void *state)
{
    tapewarp *s = state;
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < BUF; i++) s->buf[c][i] = 0;
        s->lp_s[c] = s->hp_s[c] = s->bump_s[c] = (dfx_bq_state){ 0 };
    }
    s->w = 0;
    s->wow_ph = s->flut_ph = 0;
    s->drift = s->drift_target = 0;
    s->drift_count = 0;
    s->rng = 0x7A9E;
    s->ctrl = 0;
}

static void set_param(void *state, int i, float v)
{
    tapewarp *s = state;
    if (i < 0 || i >= P_COUNT) return;
    s->p[i] = dfx_clampf(v, params[i].min, params[i].max);
}

static float get_param(const void *state, int i)
{
    const tapewarp *s = state;
    return (i >= 0 && i < P_COUNT) ? s->p[i] : 0.0f;
}

static void init(void *state, float sr)
{
    tapewarp *s = state;
    *s = (tapewarp){ 0 };
    s->sr = sr;
    for (int i = 0; i < P_COUNT; i++) s->p[i] = params[i].def;
    dfx_smooth_init(&s->wow, 0, 40, sr);
    dfx_smooth_init(&s->flutter, 0, 40, sr);
    dfx_smooth_init(&s->sat, 0, 20, sr);
    dfx_smooth_init(&s->mix, 0, 20, sr);
    dfx_smooth_init(&s->level, 1, 20, sr);
    s->fresh = 1;
    reset(s);
}

static void process(void *state, float *l, float *r, int frames)
{
    tapewarp *s = state;
    float *io[2] = { l, r };
    float wow_t = s->p[P_WOW] * 0.01f, flut_t = s->p[P_FLUTTER] * 0.01f, sat_t = s->p[P_SAT] * 0.01f;
    float mix_t = s->p[P_MIX] * 0.01f, level_t = dfx_db(s->p[P_LEVEL]);
    float hiss_amt = s->p[P_HISS] * 0.01f;
    hiss_amt = hiss_amt * hiss_amt * 0.02f;
    if (s->fresh) {
        s->wow.y = wow_t; s->flutter.y = flut_t; s->sat.y = sat_t;
        s->mix.y = mix_t; s->level.y = level_t;
        s->fresh = 0;
    }
    const float base_ms = 6.0f;
    const float two_pi = 2.0f * (float)M_PI;
    float wow_inc = 0.55f / s->sr, flut_inc = 9.5f / s->sr;
    int drift_period = (int)(s->sr * 0.35f);

    for (int n = 0; n < frames; n++) {
        if (s->ctrl-- <= 0) {
            update_coefs(s);
            s->ctrl = DFX_CTRL_INTERVAL - 1;
        }
        float wow = dfx_smooth_step(&s->wow, wow_t);
        float flut = dfx_smooth_step(&s->flutter, flut_t);
        float sat = dfx_smooth_step(&s->sat, sat_t);
        float mix = dfx_smooth_step(&s->mix, mix_t);
        float level = dfx_smooth_step(&s->level, level_t);

        /* random walk for the wow, so the drift never repeats exactly */
        if (s->drift_count-- <= 0) {
            s->drift_target = dfx_randf(&s->rng);
            s->drift_count = drift_period;
        }
        s->drift += (s->drift_target - s->drift) * (3.0f / (float)drift_period);

        s->wow_ph += wow_inc;
        if (s->wow_ph >= 1.0f) s->wow_ph -= 1.0f;
        s->flut_ph += flut_inc;
        if (s->flut_ph >= 1.0f) s->flut_ph -= 1.0f;
        float mod_ms = wow * 3.5f * (0.6f * sinf(two_pi * s->wow_ph) + 0.4f * s->drift)
                     + flut * 0.18f * sinf(two_pi * s->flut_ph);
        float delay = (base_ms + mod_ms) * 0.001f * s->sr;

        float drive = 1.0f + 5.0f * sat;
        float makeup = 1.0f / dfx_sat(drive * 0.5f) * 0.5f;
        for (int c = 0; c < 2; c++) {
            float dry = io[c][n];
            s->buf[c][s->w & MASK] = dry;
            /* the right channel reads a hair later: tape skew */
            float y = dfx_ring_read(s->buf[c], MASK, s->w, delay + (float)c * 0.3f);
            y = dfx_bq_run(&s->bump, &s->bump_s[c], y);
            y = dfx_sat(y * drive) * makeup;
            y += dfx_randf(&s->rng) * hiss_amt;
            y = dfx_bq_run(&s->lp, &s->lp_s[c], y);
            y = dfx_bq_run(&s->hp, &s->hp_s[c], y);
            io[c][n] = (dry + (y - dry) * mix) * level;
        }
        s->w++;
    }
}

const dfx_effect_def dfx_tapewarp = {
    "tapewarp", "Tape Warp",
    "Worn tape deck: wow with drift, flutter, saturation, head bump, age and hiss",
    P_COUNT, params, sizeof(tapewarp),
    init, reset, set_param, get_param, process, NULL,
};
