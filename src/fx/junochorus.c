/*
 * Juno Chorus: the Juno-60 / Juno-106 BBD chorus as an effect, so anything
 * on the SP can go through it. Modes I, II and I+II as on the synth's
 * buttons. The input is summed to mono first, as on the synth; Mix
 * crossfades from the original stereo signal to the chorus.
 */
#include "../../include/doomfx.h"
#include "../dsp.h"
#include "../juno_chorus.h"

enum { P_MODE, P_DEPTH, P_MIX, P_HISS, P_LEVEL, P_COUNT };

static const char *const mode_labels[] = { "I", "II", "I+II" };

static const dfx_param params[P_COUNT] = {
    { "mode",  "Mode",  1, 3, 1, "", 3, mode_labels, 0 },
    { "depth", "Depth", 0, 150, 100, "%", 0, 0, 0 },
    { "mix",   "Mix",   0, 100, 100, "%", 0, 0, 0 },
    { "hiss",  "Hiss",  0, 100, 0, "%", 0, 0, 0 },
    { "level", "Level", -24, 12, 0, "dB", 0, 0, 0 },
};

typedef struct {
    float sr;
    float p[P_COUNT];
    dfx_smooth mix, level, depth;
    int ctrl;
    int fresh;
    juno_chorus ch;
} junochorus;

static void reset(void *state)
{
    junochorus *s = state;
    juno_chorus_reset(&s->ch);
    s->ctrl = 0;
}

static void set_param(void *state, int i, float v)
{
    junochorus *s = state;
    if (i < 0 || i >= P_COUNT) return;
    v = dfx_clampf(v, params[i].min, params[i].max);
    if (params[i].steps) v = floorf(v + 0.5f);
    s->p[i] = v;
}

static float get_param(const void *state, int i)
{
    const junochorus *s = state;
    return (i >= 0 && i < P_COUNT) ? s->p[i] : 0.0f;
}

static void init(void *state, float sr)
{
    junochorus *s = state;
    *s = (junochorus){ 0 };
    s->sr = sr;
    for (int i = 0; i < P_COUNT; i++) s->p[i] = params[i].def;
    dfx_smooth_init(&s->mix, s->p[P_MIX] * 0.01f, 20, sr);
    dfx_smooth_init(&s->level, dfx_db(s->p[P_LEVEL]), 20, sr);
    dfx_smooth_init(&s->depth, s->p[P_DEPTH] * 0.01f, 50, sr);
    s->fresh = 1;
    reset(s);
}

static void process(void *state, float *l, float *r, int frames)
{
    junochorus *s = state;
    float mix_t = s->p[P_MIX] * 0.01f, level_t = dfx_db(s->p[P_LEVEL]);
    float depth_t = s->p[P_DEPTH] * 0.01f;
    float hiss = s->p[P_HISS] * 0.01f * 0.0005f; /* about -66 dBFS at 100% */

    /* the first block after init starts at the set values instead of gliding from the defaults */
    if (s->fresh) {
        s->depth.y = depth_t;
        s->mix.y = mix_t;
        s->level.y = level_t;
        s->fresh = 0;
    }

    for (int n = 0; n < frames; n++) {
        dfx_smooth_step(&s->depth, depth_t);
        if (s->ctrl-- <= 0) {
            juno_chorus_set(&s->ch, (int)s->p[P_MODE], s->depth.y, s->sr);
            s->ctrl = DFX_CTRL_INTERVAL - 1;
        }
        float mix = dfx_smooth_step(&s->mix, mix_t);
        float level = dfx_smooth_step(&s->level, level_t);
        float mono = 0.5f * (l[n] + r[n]);
        float wl, wr;
        juno_chorus_run(&s->ch, mono, hiss, s->sr, &wl, &wr);
        /* dry + wet at equal level, scaled so the sum doesn't get louder */
        float yl = 0.7f * (mono + wl), yr = 0.7f * (mono + wr);
        l[n] = (l[n] + (yl - l[n]) * mix) * level;
        r[n] = (r[n] + (yr - r[n]) * mix) * level;
    }
}

const dfx_effect_def dfx_junochorus = {
    "junochorus", "Juno Chorus",
    "Juno-60 / 106 style stereo BBD chorus: modes I, II and I+II",
    P_COUNT, params, sizeof(junochorus),
    init, reset, set_param, get_param, process,
};
