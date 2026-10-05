/*
 * SP-1200: 26.04 kHz / 12-bit sampling, the 45 rpm pitch-down trick and
 * the three kinds of output.
 *
 * Signal path per channel:
 *   drive -> sample & hold at the effective rate, no anti-alias filter
 *   (aliasing) -> 12-bit quantiser (hard clip at full scale) -> held
 *   staircase (zero-order hold, imaging) -> output filter -> output stage
 *
 * Pitch trick: sampling a record N semitones fast and tuning it back down
 * on the SP leaves the audio at its original pitch but captured at
 * 26.04 kHz * 2^(-N/12), with the bandwidth and aliasing that go with
 * that rate. This effect models the result directly as a lower effective
 * rate.
 *
 * Output filters: outputs 1-2 run through SSM2044 4-pole lowpass filters
 * whose cutoff follows the pitch, so here the cutoff knob is scaled by the
 * pitch trick too. Outputs 3-6 have fixed filters, and 7-8 have none. The
 * fixed cutoff and resonance below are an approximation, not measured from
 * hardware.
 */
#include "../../include/doomfx.h"
#include "../dsp.h"

#define SP_RATE 26040.0f
#define SP_FIXED_CUTOFF 8000.0f /* outputs 3-6: approximation */

enum { P_PITCH, P_CUTOFF, P_DRIVE, P_OUTPUT, P_RESO, P_BITS, P_MIX, P_LEVEL, P_COUNT };
enum { OUT_SWEEP, OUT_FIXED, OUT_RAW };

static const char *const output_labels[] = { "1-2", "3-6", "7-8" };

static const dfx_param params[P_COUNT] = {
    { "pitch",  "Pitch",  0, 24, 0, "st", 0, 0, 0 },
    { "cutoff", "Filter", 200, 20000, 10000, "Hz", 0, 0, DFX_PARAM_LOG },
    { "drive",  "Drive",  0, 24, 0, "dB", 0, 0, 0 },
    { "output", "Output", 0, 2, 0, "", 3, output_labels, 0 },
    { "reso",   "Reso",   0, 100, 20, "%", 0, 0, 0 },
    { "bits",   "Bits",   8, 16, 12, "", 9, 0, 0 },
    { "mix",    "Mix",    0, 100, 100, "%", 0, 0, 0 },
    { "level",  "Level",  -24, 12, 0, "dB", 0, 0, 0 },
};

typedef struct {
    float sr;
    float p[P_COUNT];
    dfx_smooth drive, mix, level, cutoff;
    int ctrl;
    int fresh;          /* samples until the next coefficient update */
    float ratio, k, makeup;
    dfx_ladder_coef lc;
    dfx_sh sh[2];
    dfx_ladder lad[2];
} sp1200;

static void update_coefs(sp1200 *s)
{
    float pitch_scale = exp2f(-s->p[P_PITCH] / 12.0f);
    s->ratio = SP_RATE * pitch_scale / s->sr;
    int out = (int)s->p[P_OUTPUT];
    float fc, reso;
    if (out == OUT_SWEEP) {
        fc = s->cutoff.y * pitch_scale;
        reso = s->p[P_RESO] * 0.01f;
    } else {
        fc = SP_FIXED_CUTOFF;
        reso = 0.1f;
    }
    s->k = reso * 3.6f;
    s->makeup = 1.0f + 0.5f * s->k;
    dfx_ladder_design(&s->lc, fc, s->sr);
}

static void reset(void *state)
{
    sp1200 *s = state;
    for (int c = 0; c < 2; c++) {
        s->sh[c] = (dfx_sh){ 0 };
        s->lad[c] = (dfx_ladder){ { 0 } };
    }
    s->ctrl = 0;
}

static void set_param(void *state, int i, float v)
{
    sp1200 *s = state;
    if (i < 0 || i >= P_COUNT) return;
    v = dfx_clampf(v, params[i].min, params[i].max);
    if (params[i].steps) v = floorf(v + 0.5f);
    s->p[i] = v;
}

static float get_param(const void *state, int i)
{
    const sp1200 *s = state;
    return (i >= 0 && i < P_COUNT) ? s->p[i] : 0.0f;
}

static void init(void *state, float sr)
{
    sp1200 *s = state;
    *s = (sp1200){ 0 };
    s->sr = sr;
    for (int i = 0; i < P_COUNT; i++) s->p[i] = params[i].def;
    dfx_smooth_init(&s->drive, dfx_db(s->p[P_DRIVE]), 20, sr);
    dfx_smooth_init(&s->mix, s->p[P_MIX] * 0.01f, 20, sr);
    dfx_smooth_init(&s->level, dfx_db(s->p[P_LEVEL]), 20, sr);
    dfx_smooth_init(&s->cutoff, s->p[P_CUTOFF], 30, sr);
    s->fresh = 1;
    reset(s);
}

static void process(void *state, float *l, float *r, int frames)
{
    sp1200 *s = state;
    float *io[2] = { l, r };
    float drive_t = dfx_db(s->p[P_DRIVE]), mix_t = s->p[P_MIX] * 0.01f;
    float level_t = dfx_db(s->p[P_LEVEL]);
    float bits = s->p[P_BITS];
    int out = (int)s->p[P_OUTPUT];

    /* the first block after init starts at the set values instead of gliding from the defaults */
    if (s->fresh) {
        s->cutoff.y = s->p[P_CUTOFF];
        s->drive.y = drive_t;
        s->mix.y = mix_t;
        s->level.y = level_t;
        s->fresh = 0;
    }

    for (int n = 0; n < frames; n++) {
        dfx_smooth_step(&s->cutoff, s->p[P_CUTOFF]);
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
            float y = dfx_sh_run(&s->sh[c], dry * drive, s->ratio, bits, 0.0f, &ticked);
            if (out != OUT_RAW)
                y = dfx_ladder_run(&s->lad[c], &s->lc, y, s->k) * s->makeup;
            y = dfx_sat(y);
            io[c][n] = (dry + (y - dry) * mix) * level;
        }
    }
}

const dfx_effect_def dfx_sp1200 = {
    "sp1200", "SP-1200",
    "26.04 kHz / 12-bit sampler with the pitch-down trick and SSM2044 style output filters",
    P_COUNT, params, sizeof(sp1200),
    init, reset, set_param, get_param, process,
};
