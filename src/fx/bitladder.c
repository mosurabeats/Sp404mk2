/*
 * Bit Ladder (new): a 4-pole ladder filter that runs inside a crushed
 * converter. The filter itself is clocked at the reduced rate and its
 * output is quantised, so resonance and sweeps pick up stepped, aliased
 * grit instead of just being followed by a bitcrusher. An envelope
 * follower opens (or with a negative amount, closes) the filter on every
 * hit, which makes it work as a dirty auto-wah on drums.
 *
 * Crush 0% runs at the host rate with 16 bits; 100% runs at 3 kHz with 6 bits.
 */
#include "../../include/doomfx.h"
#include "../dsp.h"

enum { P_CUTOFF, P_RESO, P_CRUSH, P_ENV, P_DRIVE, P_MIX, P_LEVEL, P_COUNT };

static const dfx_param params[P_COUNT] = {
    { "cutoff", "Cutoff", 40, 18000, 1200, "Hz", 0, 0, DFX_PARAM_LOG },
    { "reso",   "Reso",   0, 100, 50, "%", 0, 0, 0 },
    { "crush",  "Crush",  0, 100, 30, "%", 0, 0, 0 },
    { "env",    "Env",    -100, 100, 40, "%", 0, 0, 0 },
    { "drive",  "Drive",  0, 24, 6, "dB", 0, 0, 0 },
    { "mix",    "Mix",    0, 100, 100, "%", 0, 0, 0 },
    { "level",  "Level",  -24, 12, 0, "dB", 0, 0, 0 },
};

typedef struct {
    float sr;
    float p[P_COUNT];
    dfx_smooth drive, mix, level, cutoff, crush, reso;
    int ctrl;
    int fresh;
    float env, env_att, env_rel;
    float ratio, bits, k, makeup;
    dfx_ladder_coef lc;
    dfx_sh sh[2];
    dfx_ladder lad[2];
    float out[2];
} bitladder;

static void update_coefs(bitladder *s)
{
    float c = s->crush.y;
    float rate = dfx_expmap(s->sr, 3000.0f, c);
    s->ratio = rate / s->sr;
    s->bits = 16.0f - 10.0f * c;
    s->k = s->reso.y * 3.9f;
    s->makeup = 1.0f + 0.5f * s->k;
    float env = dfx_clampf(s->env, 0.0f, 1.0f);
    float fc = s->cutoff.y * exp2f(s->p[P_ENV] * 0.01f * 4.0f * env);
    dfx_ladder_design(&s->lc, fc, rate);
}

static void reset(void *state)
{
    bitladder *s = state;
    for (int c = 0; c < 2; c++) {
        s->sh[c] = (dfx_sh){ 0 };
        s->lad[c] = (dfx_ladder){ { 0 } };
        s->out[c] = 0.0f;
    }
    s->env = 0.0f;
    s->ctrl = 0;
}

static void set_param(void *state, int i, float v)
{
    bitladder *s = state;
    if (i < 0 || i >= P_COUNT) return;
    v = dfx_clampf(v, params[i].min, params[i].max);
    if (params[i].steps) v = floorf(v + 0.5f);
    s->p[i] = v;
}

static float get_param(const void *state, int i)
{
    const bitladder *s = state;
    return (i >= 0 && i < P_COUNT) ? s->p[i] : 0.0f;
}

static void init(void *state, float sr)
{
    bitladder *s = state;
    *s = (bitladder){ 0 };
    s->sr = sr;
    for (int i = 0; i < P_COUNT; i++) s->p[i] = params[i].def;
    dfx_smooth_init(&s->drive, dfx_db(s->p[P_DRIVE]), 20, sr);
    dfx_smooth_init(&s->mix, s->p[P_MIX] * 0.01f, 20, sr);
    dfx_smooth_init(&s->level, dfx_db(s->p[P_LEVEL]), 20, sr);
    dfx_smooth_init(&s->cutoff, s->p[P_CUTOFF], 30, sr);
    dfx_smooth_init(&s->crush, s->p[P_CRUSH] * 0.01f, 30, sr);
    dfx_smooth_init(&s->reso, s->p[P_RESO] * 0.01f, 30, sr);
    s->env_att = 1.0f - expf(-1.0f / (0.005f * sr));
    s->env_rel = 1.0f - expf(-1.0f / (0.150f * sr));
    s->fresh = 1;
    reset(s);
}

static void process(void *state, float *l, float *r, int frames)
{
    bitladder *s = state;
    float *io[2] = { l, r };
    float drive_t = dfx_db(s->p[P_DRIVE]), mix_t = s->p[P_MIX] * 0.01f;
    float level_t = dfx_db(s->p[P_LEVEL]);
    float crush_t = s->p[P_CRUSH] * 0.01f, reso_t = s->p[P_RESO] * 0.01f;

    /* the first block after init starts at the set values instead of gliding from the defaults */
    if (s->fresh) {
        s->cutoff.y = s->p[P_CUTOFF];
        s->crush.y = crush_t;
        s->reso.y = reso_t;
        s->drive.y = drive_t;
        s->mix.y = mix_t;
        s->level.y = level_t;
        s->fresh = 0;
    }

    for (int n = 0; n < frames; n++) {
        float peak = fmaxf(fabsf(l[n]), fabsf(r[n]));
        s->env += (peak - s->env) * (peak > s->env ? s->env_att : s->env_rel);
        dfx_smooth_step(&s->cutoff, s->p[P_CUTOFF]);
        dfx_smooth_step(&s->crush, crush_t);
        dfx_smooth_step(&s->reso, reso_t);
        if (s->ctrl-- <= 0) {
            update_coefs(s);
            s->ctrl = DFX_CTRL_INTERVAL - 1;
        }
        float drive = dfx_smooth_step(&s->drive, drive_t);
        float mix = dfx_smooth_step(&s->mix, mix_t);
        float level = dfx_smooth_step(&s->level, level_t);

        for (int c = 0; c < 2; c++) {
            float dry = io[c][n];
            int ticked;
            float x = dfx_sh_run(&s->sh[c], dry * drive, s->ratio, 24.0f, 0.0f, &ticked);
            if (ticked) {
                float y = dfx_ladder_run(&s->lad[c], &s->lc, x, s->k) * s->makeup;
                s->out[c] = dfx_quantize(dfx_sat(y), s->bits, 0.0f);
            }
            io[c][n] = (dry + (s->out[c] - dry) * mix) * level;
        }
    }
}

const dfx_effect_def dfx_bitladder = {
    "bitladder", "Bit Ladder",
    "Resonant ladder filter clocked inside a crushed converter, with an envelope follower",
    P_COUNT, params, sizeof(bitladder),
    init, reset, set_param, get_param, process,
};
