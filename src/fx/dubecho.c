/*
 * Dub Echo (new): a tempo-synced tape echo built for throws.
 *
 * Time is a note length locked to the host tempo. Changing it glides the
 * delay time instead of jumping, so the repeats bend in pitch like a tape
 * echo's head being moved: twist Time during a throw for the siren sweep.
 * The feedback path is band-limited by Tone and saturated by Drive, so
 * Feedback above 100% runs away into a held, distorting wash without ever
 * blowing up. Spread goes from mono echoes to full ping-pong, and Wobble
 * adds tape wow to the repeats.
 *
 * Mix is a crossfade: 100% is echoes only (what a send bus wants).
 */
#include "../../include/doomfx.h"
#include "../dsp.h"

enum { P_TIME, P_FEEDBACK, P_MIX, P_TONE, P_DRIVE, P_WOBBLE, P_SPREAD, P_LEVEL, P_COUNT };

static const dfx_param params[P_COUNT] = {
    { "time",     "Time",     0, DFX_DIV_COUNT - 1, 7, "", DFX_DIV_COUNT, dfx_div_labels, 0 },
    { "feedback", "Feedback", 0, 110, 45, "%", 0, 0, 0 },
    { "mix",      "Mix",      0, 100, 35, "%", 0, 0, 0 },
    { "tone",     "Tone",     -100, 100, 0, "", 0, 0, 0 },
    { "drive",    "Drive",    0, 100, 30, "%", 0, 0, 0 },
    { "wobble",   "Wobble",   0, 100, 20, "%", 0, 0, 0 },
    { "spread",   "Spread",   0, 100, 70, "%", 0, 0, 0 },
    { "level",    "Level",    -24, 12, 0, "dB", 0, 0, 0 },
};

#define BUF 65536
#define MASK (BUF - 1)

typedef struct {
    float sr;
    float p[P_COUNT];
    dfx_smooth fb, mix, level, spread, drive;
    float delay;          /* current delay in samples, glides to the target */
    float glide;          /* glide coefficient */
    int ctrl, fresh;
    float bpm;
    unsigned w;
    float lfo;
    uint32_t rng;
    float drift, drift_t;
    int drift_n;
    dfx_bq_coef hp, lp;
    dfx_bq_state hp_s[2], lp_s[2];
    float buf[2][BUF];
} dubecho;

static float target_delay(const dubecho *s)
{
    float d = dfx_div_to_beats(s->p[P_TIME]) * 60.0f / s->bpm * s->sr;
    return dfx_clampf(d, 16.0f, (float)(BUF - 64));
}

static void update_coefs(dubecho *s)
{
    float tone = s->p[P_TONE] * 0.01f; /* -1 dark .. +1 bright */
    float hp = tone < 0 ? dfx_lerp(120.0f, 60.0f, -tone) : dfx_lerp(120.0f, 650.0f, tone);
    float lp = tone < 0 ? dfx_lerp(4500.0f, 1300.0f, -tone) : dfx_lerp(4500.0f, 11000.0f, tone);
    dfx_bq_design(&s->hp, DFX_BQ_HP, hp, 0.7f, 0, s->sr);
    dfx_bq_design(&s->lp, DFX_BQ_LP, lp, 0.7f, 0, s->sr);
}

static void reset(void *state)
{
    dubecho *s = state;
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < BUF; i++) s->buf[c][i] = 0;
        s->hp_s[c] = s->lp_s[c] = (dfx_bq_state){ 0 };
    }
    s->w = 0;
    s->lfo = 0;
    s->rng = 0xDB;
    s->drift = s->drift_t = 0;
    s->drift_n = 0;
    s->delay = target_delay(s);
    s->ctrl = 0;
}

static void set_param(void *state, int i, float v)
{
    dubecho *s = state;
    if (i < 0 || i >= P_COUNT) return;
    v = dfx_clampf(v, params[i].min, params[i].max);
    if (params[i].steps) v = floorf(v + 0.5f);
    s->p[i] = v;
}

static float get_param(const void *state, int i)
{
    const dubecho *s = state;
    return (i >= 0 && i < P_COUNT) ? s->p[i] : 0.0f;
}

static void set_tempo(void *state, float bpm, double beat)
{
    (void)beat; /* echoes follow tempo, not position */
    dubecho *s = state;
    s->bpm = dfx_clampf(bpm, 30.0f, 300.0f);
}

static void init(void *state, float sr)
{
    dubecho *s = state;
    *s = (dubecho){ 0 };
    s->sr = sr;
    s->bpm = 120;
    for (int i = 0; i < P_COUNT; i++) s->p[i] = params[i].def;
    dfx_smooth_init(&s->fb, 0, 30, sr);
    dfx_smooth_init(&s->mix, 0, 20, sr);
    dfx_smooth_init(&s->level, 1, 20, sr);
    dfx_smooth_init(&s->spread, 0, 30, sr);
    dfx_smooth_init(&s->drive, 0, 30, sr);
    s->glide = 1.0f - expf(-1.0f / (0.12f * sr)); /* 120 ms tape glide */
    s->fresh = 1;
    reset(s);
}

static void process(void *state, float *l, float *r, int frames)
{
    dubecho *s = state;
    float fb_t = s->p[P_FEEDBACK] * 0.01f, mix_t = s->p[P_MIX] * 0.01f;
    float level_t = dfx_db(s->p[P_LEVEL]), spread_t = s->p[P_SPREAD] * 0.01f, drive_t = s->p[P_DRIVE] * 0.01f;
    if (s->fresh) {
        s->fb.y = fb_t; s->mix.y = mix_t; s->level.y = level_t;
        s->spread.y = spread_t; s->drive.y = drive_t;
        s->delay = target_delay(s);
        s->fresh = 0;
    }
    float target = target_delay(s);
    float wob_depth = s->p[P_WOBBLE] * 0.01f * 0.0025f * s->sr; /* up to 2.5 ms */
    float lfo_inc = 2.0f * (float)M_PI * 0.45f / s->sr;
    int drift_period = (int)(s->sr * 0.25f);

    for (int n = 0; n < frames; n++) {
        if (s->ctrl-- <= 0) {
            update_coefs(s);
            s->ctrl = DFX_CTRL_INTERVAL - 1;
        }
        float fb = dfx_smooth_step(&s->fb, fb_t);
        float mix = dfx_smooth_step(&s->mix, mix_t);
        float level = dfx_smooth_step(&s->level, level_t);
        float sp = dfx_smooth_step(&s->spread, spread_t);
        float drive = dfx_smooth_step(&s->drive, drive_t);
        s->delay += (target - s->delay) * s->glide;

        if (s->drift_n-- <= 0) { s->drift_t = dfx_randf(&s->rng); s->drift_n = drift_period; }
        s->drift += (s->drift_t - s->drift) * (2.0f / (float)drift_period);
        s->lfo += lfo_inc;
        if (s->lfo > 2.0f * (float)M_PI) s->lfo -= 2.0f * (float)M_PI;
        float d = s->delay + wob_depth * (0.7f * sinf(s->lfo) + 0.3f * s->drift) + wob_depth;

        float tap[2], proc[2];
        for (int c = 0; c < 2; c++) {
            tap[c] = dfx_ring_read(s->buf[c], MASK, s->w, d);
            float y = dfx_bq_run(&s->hp, &s->hp_s[c], tap[c]);
            y = dfx_bq_run(&s->lp, &s->lp_s[c], y);
            float k = 1.0f + 3.0f * drive;
            proc[c] = dfx_sat(y * k) / k * (1.0f + 0.5f * drive);
        }
        float in_l = l[n], in_r = r[n], mono = 0.5f * (in_l + in_r);
        float wl = in_l * (1 - sp) + mono * sp + fb * ((1 - sp) * proc[0] + sp * proc[1]);
        float wr = in_r * (1 - sp) + fb * ((1 - sp) * proc[1] + sp * proc[0]);
        s->buf[0][s->w & MASK] = dfx_undenormal(wl);
        s->buf[1][s->w & MASK] = dfx_undenormal(wr);
        s->w++;

        float dry_g = fminf(1.0f, 2.0f * (1.0f - mix)), wet_g = fminf(1.0f, 2.0f * mix);
        l[n] = (in_l * dry_g + tap[0] * wet_g) * level;
        r[n] = (in_r * dry_g + tap[1] * wet_g) * level;
    }
}

const dfx_effect_def dfx_dubecho = {
    "dubecho", "Dub Echo",
    "Tempo-synced tape echo with gliding time, runaway feedback, tone, drive and ping-pong",
    P_COUNT, params, sizeof(dubecho),
    init, reset, set_param, get_param, process, set_tempo,
};
