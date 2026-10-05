/*
 * MPC60: 40 kHz sampling with 12-bit non-linear conversion.
 *
 * The MPC60 stores 12-bit samples but uses a non-linear (companding)
 * converter, so quiet sounds keep more detail than plain 12-bit would
 * give, and loud ones get a coarser, grittier grain. That is modelled
 * here as a mu-law style quantiser; "Comp" morphs it from linear 12-bit
 * (0%) to strongly companded (100%). Unlike the SP-1200 the MPC60 filters
 * its converters properly, so there is an anti-alias filter before the
 * sample & hold and a reconstruction filter after it.
 *
 * "Punch" is the output stage: a low-mid lift and soft saturation that
 * fattens drums. It is a character model, not a circuit model.
 */
#include "../../include/doomfx.h"
#include "../dsp.h"

enum { P_DRIVE, P_PUNCH, P_MIX, P_BITS, P_COMP, P_RATE, P_LEVEL, P_COUNT };

static const dfx_param params[P_COUNT] = {
    { "drive", "Drive", 0, 24, 0, "dB", 0, 0, 0 },
    { "punch", "Punch", 0, 100, 40, "%", 0, 0, 0 },
    { "mix",   "Mix",   0, 100, 100, "%", 0, 0, 0 },
    { "bits",  "Bits",  8, 16, 12, "", 9, 0, 0 },
    { "comp",  "Comp",  0, 100, 100, "%", 0, 0, 0 },
    { "rate",  "Rate",  8000, 48000, 40000, "Hz", 0, 0, DFX_PARAM_LOG },
    { "level", "Level", -24, 12, 0, "dB", 0, 0, 0 },
};

typedef struct {
    float sr;
    float p[P_COUNT];
    dfx_smooth drive, mix, level, punch;
    int ctrl;
    int fresh;
    float ratio, mu;
    dfx_lp4_coef aa;     /* anti-alias and reconstruction */
    dfx_bq_coef body;    /* low-mid lift for punch */
    dfx_lp4_state aa_in[2], aa_out[2];
    dfx_bq_state body_s[2];
    dfx_sh sh[2];
} mpc60;

static void update_coefs(mpc60 *s)
{
    float rate = s->p[P_RATE];
    s->ratio = rate / s->sr;
    s->mu = s->p[P_COMP] * 0.01f * 255.0f;
    dfx_lp4_design(&s->aa, rate * 0.45f, s->sr);
    dfx_bq_design(&s->body, DFX_BQ_PEAK, 110.0f, 0.8f, s->punch.y * 5.0f, s->sr);
}

static void reset(void *state)
{
    mpc60 *s = state;
    for (int c = 0; c < 2; c++) {
        s->aa_in[c] = (dfx_lp4_state){ 0 };
        s->aa_out[c] = (dfx_lp4_state){ 0 };
        s->body_s[c] = (dfx_bq_state){ 0 };
        s->sh[c] = (dfx_sh){ 0 };
    }
    s->ctrl = 0;
}

static void set_param(void *state, int i, float v)
{
    mpc60 *s = state;
    if (i < 0 || i >= P_COUNT) return;
    v = dfx_clampf(v, params[i].min, params[i].max);
    if (params[i].steps) v = floorf(v + 0.5f);
    s->p[i] = v;
}

static float get_param(const void *state, int i)
{
    const mpc60 *s = state;
    return (i >= 0 && i < P_COUNT) ? s->p[i] : 0.0f;
}

static void init(void *state, float sr)
{
    mpc60 *s = state;
    *s = (mpc60){ 0 };
    s->sr = sr;
    for (int i = 0; i < P_COUNT; i++) s->p[i] = params[i].def;
    dfx_smooth_init(&s->drive, dfx_db(s->p[P_DRIVE]), 20, sr);
    dfx_smooth_init(&s->mix, s->p[P_MIX] * 0.01f, 20, sr);
    dfx_smooth_init(&s->level, dfx_db(s->p[P_LEVEL]), 20, sr);
    dfx_smooth_init(&s->punch, s->p[P_PUNCH] * 0.01f, 30, sr);
    s->fresh = 1;
    reset(s);
}

static void process(void *state, float *l, float *r, int frames)
{
    mpc60 *s = state;
    float *io[2] = { l, r };
    float drive_t = dfx_db(s->p[P_DRIVE]), mix_t = s->p[P_MIX] * 0.01f;
    float level_t = dfx_db(s->p[P_LEVEL]), punch_t = s->p[P_PUNCH] * 0.01f;
    float bits = s->p[P_BITS];

    /* the first block after init starts at the set values instead of gliding from the defaults */
    if (s->fresh) {
        s->punch.y = punch_t;
        s->drive.y = drive_t;
        s->mix.y = mix_t;
        s->level.y = level_t;
        s->fresh = 0;
    }

    for (int n = 0; n < frames; n++) {
        float punch = dfx_smooth_step(&s->punch, punch_t);
        if (s->ctrl-- <= 0) {
            update_coefs(s);
            s->ctrl = DFX_CTRL_INTERVAL - 1;
        }
        float drive = dfx_smooth_step(&s->drive, drive_t);
        float mix = dfx_smooth_step(&s->mix, mix_t);
        float level = dfx_smooth_step(&s->level, level_t);
        float sat_gain = 1.0f + punch * 1.5f;

        for (int c = 0; c < 2; c++) {
            float dry = io[c][n];
            float y = dry * drive;
            int ticked;
            if (s->ratio < 1.0f) y = dfx_lp4_run(&s->aa, &s->aa_in[c], y);
            y = dfx_sh_run(&s->sh[c], y, s->ratio, bits, s->mu, &ticked);
            if (s->ratio < 1.0f) y = dfx_lp4_run(&s->aa, &s->aa_out[c], y);
            /* output stage: lift the body, then saturate and level-match */
            y = dfx_bq_run(&s->body, &s->body_s[c], y);
            y = dfx_sat(y * sat_gain) / sat_gain;
            io[c][n] = (dry + (y - dry) * mix) * level;
        }
    }
}

const dfx_effect_def dfx_mpc60 = {
    "mpc60", "MPC60",
    "40 kHz / 12-bit non-linear converters and a punchy output stage",
    P_COUNT, params, sizeof(mpc60),
    init, reset, set_param, get_param, process,
};
