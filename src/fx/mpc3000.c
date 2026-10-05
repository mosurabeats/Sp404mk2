/*
 * MPC3000: 16-bit / 44.1 kHz converters, a resonant lowpass per pad and the
 * warm, heavy low end the machine is known for.
 *
 * "Filter" and "Reso" are a 2-pole resonant lowpass in the spirit of the
 * MPC3000's per-pad filter. "Warmth" is the output stage: a low shelf, a
 * gentle top-end rolloff and asymmetric soft saturation, which adds even
 * harmonics. Both are character models rather than circuit models.
 */
#include "../../include/doomfx.h"
#include "../dsp.h"

#define MPC3000_RATE 44100.0f

enum { P_CUTOFF, P_RESO, P_WARMTH, P_DRIVE, P_BITS, P_MIX, P_LEVEL, P_COUNT };

static const dfx_param params[P_COUNT] = {
    { "cutoff", "Filter", 60, 20000, 20000, "Hz", 0, 0, DFX_PARAM_LOG },
    { "reso",   "Reso",   0, 100, 0, "%", 0, 0, 0 },
    { "warmth", "Warmth", 0, 100, 50, "%", 0, 0, 0 },
    { "drive",  "Drive",  0, 24, 0, "dB", 0, 0, 0 },
    { "bits",   "Bits",   8, 16, 16, "", 9, 0, 0 },
    { "mix",    "Mix",    0, 100, 100, "%", 0, 0, 0 },
    { "level",  "Level",  -24, 12, 0, "dB", 0, 0, 0 },
};

typedef struct {
    float sr;
    float p[P_COUNT];
    dfx_smooth drive, mix, level, cutoff, warmth;
    int ctrl;
    float ratio;
    dfx_svf_coef fc;
    dfx_bq_coef shelf, top;
    dfx_svf svf[2];
    dfx_bq_state shelf_s[2], top_s[2];
    dfx_sh sh[2];
} mpc3000;

static void update_coefs(mpc3000 *s)
{
    s->ratio = MPC3000_RATE / s->sr;
    float q = 0.707f + s->p[P_RESO] * 0.01f * 9.0f;
    dfx_svf_design(&s->fc, s->cutoff.y, q, s->sr);
    float w = s->warmth.y;
    dfx_bq_design(&s->shelf, DFX_BQ_LOWSHELF, 120.0f, 0.7f, w * 4.0f, s->sr);
    dfx_bq_design(&s->top, DFX_BQ_HIGHSHELF, 9000.0f, 0.7f, -w * 3.0f, s->sr);
}

static void reset(void *state)
{
    mpc3000 *s = state;
    for (int c = 0; c < 2; c++) {
        s->svf[c] = (dfx_svf){ 0 };
        s->shelf_s[c] = (dfx_bq_state){ 0 };
        s->top_s[c] = (dfx_bq_state){ 0 };
        s->sh[c] = (dfx_sh){ 0 };
    }
    s->ctrl = 0;
}

static void set_param(void *state, int i, float v)
{
    mpc3000 *s = state;
    if (i < 0 || i >= P_COUNT) return;
    v = dfx_clampf(v, params[i].min, params[i].max);
    if (params[i].steps) v = floorf(v + 0.5f);
    s->p[i] = v;
}

static float get_param(const void *state, int i)
{
    const mpc3000 *s = state;
    return (i >= 0 && i < P_COUNT) ? s->p[i] : 0.0f;
}

static void init(void *state, float sr)
{
    mpc3000 *s = state;
    *s = (mpc3000){ 0 };
    s->sr = sr;
    for (int i = 0; i < P_COUNT; i++) s->p[i] = params[i].def;
    dfx_smooth_init(&s->drive, dfx_db(s->p[P_DRIVE]), 20, sr);
    dfx_smooth_init(&s->mix, s->p[P_MIX] * 0.01f, 20, sr);
    dfx_smooth_init(&s->level, dfx_db(s->p[P_LEVEL]), 20, sr);
    dfx_smooth_init(&s->cutoff, s->p[P_CUTOFF], 30, sr);
    dfx_smooth_init(&s->warmth, s->p[P_WARMTH] * 0.01f, 30, sr);
    reset(s);
}

static void process(void *state, float *l, float *r, int frames)
{
    mpc3000 *s = state;
    float *io[2] = { l, r };
    float drive_t = dfx_db(s->p[P_DRIVE]), mix_t = s->p[P_MIX] * 0.01f;
    float level_t = dfx_db(s->p[P_LEVEL]), warmth_t = s->p[P_WARMTH] * 0.01f;
    float bits = s->p[P_BITS];
    /* a fully open filter with no resonance is bypassed, so it adds no phase shift;
       it keeps running so turning it back on does not click */
    int filter_on = s->p[P_CUTOFF] < params[P_CUTOFF].max || s->p[P_RESO] > 0.0f;

    for (int n = 0; n < frames; n++) {
        dfx_smooth_step(&s->cutoff, s->p[P_CUTOFF]);
        float warmth = dfx_smooth_step(&s->warmth, warmth_t);
        if (s->ctrl-- <= 0) {
            update_coefs(s);
            s->ctrl = DFX_CTRL_INTERVAL - 1;
        }
        float drive = dfx_smooth_step(&s->drive, drive_t);
        float mix = dfx_smooth_step(&s->mix, mix_t);
        float level = dfx_smooth_step(&s->level, level_t);
        float sat_gain = 1.0f + warmth * 2.0f;
        float bias = warmth * 0.15f;
        float bias_out = dfx_sat(bias);

        for (int c = 0; c < 2; c++) {
            float dry = io[c][n];
            int ticked;
            float y = dfx_sh_run(&s->sh[c], dry * drive, s->ratio, bits, 0.0f, &ticked);
            float lp = dfx_svf_lp(&s->svf[c], &s->fc, y);
            if (filter_on) y = lp;
            y = dfx_bq_run(&s->shelf, &s->shelf_s[c], y);
            y = dfx_bq_run(&s->top, &s->top_s[c], y);
            /* biased saturation gives even harmonics; the bias is removed again after */
            y = (dfx_sat(y * sat_gain + bias) - bias_out) / sat_gain;
            io[c][n] = (dry + (y - dry) * mix) * level;
        }
    }
}

const dfx_effect_def dfx_mpc3000 = {
    "mpc3000", "MPC3000",
    "16-bit converters, resonant pad filter and a warm, heavy output stage",
    P_COUNT, params, sizeof(mpc3000),
    init, reset, set_param, get_param, process,
};
