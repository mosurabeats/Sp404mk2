/*
 * Bus Glue (new): a drum bus compressor with an SP-1200 layer underneath.
 *
 * "Squash" is one knob for threshold and ratio, with automatic makeup gain.
 * Detection is stereo linked and runs through a sidechain highpass, so the
 * kick does not pump the whole loop. "Dirt" blends in a parallel copy of
 * the compressed signal sent through a 26.04 kHz / 12-bit converter, the
 * classic trick of layering the crunchy machine under the clean one.
 */
#include "../../include/doomfx.h"
#include "../dsp.h"

enum { P_SQUASH, P_DIRT, P_MIX, P_ATTACK, P_RELEASE, P_SCHPF, P_LEVEL, P_COUNT };

static const dfx_param params[P_COUNT] = {
    { "squash",  "Squash",  0, 100, 40, "%", 0, 0, 0 },
    { "dirt",    "Dirt",    0, 100, 25, "%", 0, 0, 0 },
    { "mix",     "Mix",     0, 100, 100, "%", 0, 0, 0 },
    { "attack",  "Attack",  0.1f, 30, 10, "ms", 0, 0, DFX_PARAM_LOG },
    { "release", "Release", 30, 1200, 200, "ms", 0, 0, DFX_PARAM_LOG },
    { "schpf",   "SC HPF",  20, 300, 90, "Hz", 0, 0, DFX_PARAM_LOG },
    { "level",   "Level",   -24, 12, 0, "dB", 0, 0, 0 },
};

typedef struct {
    float sr;
    float p[P_COUNT];
    dfx_smooth squash, dirt, mix, level;
    int ctrl;
    float att, rel;
    float gr_db;          /* current gain reduction, dB (<= 0) */
    dfx_bq_coef sc;
    dfx_bq_state sc_s[2];
    dfx_ladder_coef dirt_lp;
    dfx_ladder dirt_lad[2];
    dfx_sh sh[2];
} busglue;

static void update_coefs(busglue *s)
{
    s->att = 1.0f - expf(-1.0f / (s->p[P_ATTACK] * 0.001f * s->sr));
    s->rel = 1.0f - expf(-1.0f / (s->p[P_RELEASE] * 0.001f * s->sr));
    dfx_bq_design(&s->sc, DFX_BQ_HP, s->p[P_SCHPF], 0.707f, 0, s->sr);
    dfx_ladder_design(&s->dirt_lp, 9000.0f, s->sr);
}

static void reset(void *state)
{
    busglue *s = state;
    for (int c = 0; c < 2; c++) {
        s->sc_s[c] = (dfx_bq_state){ 0 };
        s->dirt_lad[c] = (dfx_ladder){ { 0 } };
        s->sh[c] = (dfx_sh){ 0 };
    }
    s->gr_db = 0.0f;
    s->ctrl = 0;
}

static void set_param(void *state, int i, float v)
{
    busglue *s = state;
    if (i < 0 || i >= P_COUNT) return;
    v = dfx_clampf(v, params[i].min, params[i].max);
    if (params[i].steps) v = floorf(v + 0.5f);
    s->p[i] = v;
}

static float get_param(const void *state, int i)
{
    const busglue *s = state;
    return (i >= 0 && i < P_COUNT) ? s->p[i] : 0.0f;
}

static void init(void *state, float sr)
{
    busglue *s = state;
    *s = (busglue){ 0 };
    s->sr = sr;
    for (int i = 0; i < P_COUNT; i++) s->p[i] = params[i].def;
    dfx_smooth_init(&s->squash, s->p[P_SQUASH] * 0.01f, 30, sr);
    dfx_smooth_init(&s->dirt, s->p[P_DIRT] * 0.01f, 20, sr);
    dfx_smooth_init(&s->mix, s->p[P_MIX] * 0.01f, 20, sr);
    dfx_smooth_init(&s->level, dfx_db(s->p[P_LEVEL]), 20, sr);
    reset(s);
}

/* Static gain curve with a 6 dB soft knee; returns gain change in dB (<= 0). */
static float gain_computer(float in_db, float thr, float ratio)
{
    const float knee = 6.0f;
    float over = in_db - thr;
    if (over <= -knee * 0.5f) return 0.0f;
    if (over >= knee * 0.5f) return -over * (1.0f - 1.0f / ratio);
    float x = over + knee * 0.5f;
    return -(1.0f - 1.0f / ratio) * x * x / (2.0f * knee);
}

static void process(void *state, float *l, float *r, int frames)
{
    busglue *s = state;
    float *io[2] = { l, r };
    float squash_t = s->p[P_SQUASH] * 0.01f, dirt_t = s->p[P_DIRT] * 0.01f;
    float mix_t = s->p[P_MIX] * 0.01f, level_t = dfx_db(s->p[P_LEVEL]);
    float sp_ratio = 26040.0f / s->sr;

    for (int n = 0; n < frames; n++) {
        if (s->ctrl-- <= 0) {
            update_coefs(s);
            s->ctrl = DFX_CTRL_INTERVAL - 1;
        }
        float sq = dfx_smooth_step(&s->squash, squash_t);
        float dirt = dfx_smooth_step(&s->dirt, dirt_t);
        float mix = dfx_smooth_step(&s->mix, mix_t);
        float level = dfx_smooth_step(&s->level, level_t);
        float thr = -6.0f - 24.0f * sq;
        float ratio = 1.5f + 8.5f * sq;
        float makeup_db = -0.5f * gain_computer(0.0f, thr, ratio);

        /* stereo linked detector on the highpassed sidechain */
        float d0 = dfx_bq_run(&s->sc, &s->sc_s[0], l[n]);
        float d1 = dfx_bq_run(&s->sc, &s->sc_s[1], r[n]);
        float peak = fmaxf(fabsf(d0), fabsf(d1));
        float in_db = 20.0f * log10f(peak + 1e-9f);
        float target = gain_computer(in_db, thr, ratio);
        s->gr_db += (target - s->gr_db) * (target < s->gr_db ? s->att : s->rel);
        float gain = dfx_db(s->gr_db + makeup_db);

        for (int c = 0; c < 2; c++) {
            float dry = io[c][n];
            float y = dry * gain;
            int ticked;
            float crunch = dfx_sh_run(&s->sh[c], y, sp_ratio, 12.0f, 0.0f, &ticked);
            crunch = dfx_ladder_run(&s->dirt_lad[c], &s->dirt_lp, crunch, 0.4f) * 1.2f;
            y = dfx_sat(y + crunch * dirt);
            io[c][n] = (dry + (y - dry) * mix) * level;
        }
    }
}

const dfx_effect_def dfx_busglue = {
    "busglue", "Bus Glue",
    "Drum bus compressor with a parallel SP-1200 crunch layer",
    P_COUNT, params, sizeof(busglue),
    init, reset, set_param, get_param, process,
};
