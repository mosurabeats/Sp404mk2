/*
 * Dust (new): a record on a worn turntable, laid over anything.
 *
 * Crackle is a stream of random clicks with a mostly-small, sometimes-big
 * size distribution, plus rare deeper pops. Wear narrows the band the way
 * a played-out pressing and a cheap cartridge do, with a little mid
 * presence and grit. Noise is surface hiss, Rumble is low motor and
 * platter noise, and Mono folds the low end to mono like a mastering
 * engineer cutting for vinyl.
 */
#include "../../include/doomfx.h"
#include "../dsp.h"

enum { P_CRACKLE, P_WEAR, P_NOISE, P_RUMBLE, P_MONO, P_MIX, P_LEVEL, P_COUNT };

static const dfx_param params[P_COUNT] = {
    { "crackle", "Crackle", 0, 100, 35, "%", 0, 0, 0 },
    { "wear",    "Wear",    0, 100, 40, "%", 0, 0, 0 },
    { "noise",   "Noise",   0, 100, 20, "%", 0, 0, 0 },
    { "rumble",  "Rumble",  0, 100, 15, "%", 0, 0, 0 },
    { "mono",    "Mono",    0, 100, 60, "%", 0, 0, 0 },
    { "mix",     "Mix",     0, 100, 100, "%", 0, 0, 0 },
    { "level",   "Level",   -24, 12, 0, "dB", 0, 0, 0 },
};

typedef struct {
    float sr;
    float p[P_COUNT];
    dfx_smooth mix, level, wear;
    int ctrl, fresh;
    uint32_t rng;
    float click_env[2], click_sign[2];
    float pop_env, pop_ph;
    dfx_bq_coef lp, hp, mid, click_hp, hiss_lp, rumble_lp, side_lp;
    dfx_bq_state lp_s[2], hp_s[2], mid_s[2], click_s[2], hiss_s[2], rumble_s, side_s;
} dust;

static void update_coefs(dust *s)
{
    float wear = s->p[P_WEAR] * 0.01f;
    dfx_bq_design(&s->lp, DFX_BQ_LP, dfx_expmap(20000.0f, 3800.0f, wear), 0.75f, 0, s->sr);
    dfx_bq_design(&s->hp, DFX_BQ_HP, dfx_expmap(20.0f, 160.0f, wear), 0.6f, 0, s->sr);
    dfx_bq_design(&s->mid, DFX_BQ_PEAK, 1700.0f, 0.8f, 4.0f * wear, s->sr);
    dfx_bq_design(&s->click_hp, DFX_BQ_HP, 1400.0f, 0.7f, 0, s->sr);
    dfx_bq_design(&s->hiss_lp, DFX_BQ_LP, 7000.0f, 0.7f, 0, s->sr);
    dfx_bq_design(&s->rumble_lp, DFX_BQ_LP, 28.0f, 0.9f, 0, s->sr);
    dfx_bq_design(&s->side_lp, DFX_BQ_LP, 150.0f, 0.7f, 0, s->sr);
}

static void reset(void *state)
{
    dust *s = state;
    for (int c = 0; c < 2; c++) {
        s->lp_s[c] = s->hp_s[c] = s->mid_s[c] = s->click_s[c] = s->hiss_s[c] = (dfx_bq_state){ 0 };
        s->click_env[c] = 0;
        s->click_sign[c] = 1;
    }
    s->rumble_s = s->side_s = (dfx_bq_state){ 0 };
    s->pop_env = s->pop_ph = 0;
    s->rng = 0xD057;
    s->ctrl = 0;
}

static void set_param(void *state, int i, float v)
{
    dust *s = state;
    if (i < 0 || i >= P_COUNT) return;
    s->p[i] = dfx_clampf(v, params[i].min, params[i].max);
}

static float get_param(const void *state, int i)
{
    const dust *s = state;
    return (i >= 0 && i < P_COUNT) ? s->p[i] : 0.0f;
}

static void init(void *state, float sr)
{
    dust *s = state;
    *s = (dust){ 0 };
    s->sr = sr;
    for (int i = 0; i < P_COUNT; i++) s->p[i] = params[i].def;
    dfx_smooth_init(&s->mix, 0, 20, sr);
    dfx_smooth_init(&s->level, 1, 20, sr);
    dfx_smooth_init(&s->wear, 0, 20, sr);
    s->fresh = 1;
    reset(s);
}

/* uniform 0..1 */
static inline float rand01(uint32_t *r) { return (float)(dfx_rand(r) >> 8) * (1.0f / 16777216.0f); }

static void process(void *state, float *l, float *r, int frames)
{
    dust *s = state;
    float mix_t = s->p[P_MIX] * 0.01f, level_t = dfx_db(s->p[P_LEVEL]), wear_t = s->p[P_WEAR] * 0.01f;
    if (s->fresh) {
        s->mix.y = mix_t; s->level.y = level_t; s->wear.y = wear_t;
        s->fresh = 0;
    }
    float crackle = s->p[P_CRACKLE] * 0.01f;
    float click_prob = crackle * crackle * 600.0f / s->sr; /* up to ~600 clicks per second */
    float pop_prob = crackle * 1.5f / s->sr;
    float click_decay = expf(-1.0f / (0.00025f * s->sr));
    float pop_decay = expf(-1.0f / (0.012f * s->sr));
    float noise = s->p[P_NOISE] * 0.01f;
    noise = noise * noise * 0.03f;
    float rumble = s->p[P_RUMBLE] * 0.01f * 0.6f;
    float mono = s->p[P_MONO] * 0.01f;
    float pop_inc = 2.0f * (float)M_PI * 70.0f / s->sr;

    for (int n = 0; n < frames; n++) {
        if (s->ctrl-- <= 0) {
            update_coefs(s);
            s->ctrl = DFX_CTRL_INTERVAL - 1;
        }
        float mix = dfx_smooth_step(&s->mix, mix_t);
        float level = dfx_smooth_step(&s->level, level_t);
        float wear = dfx_smooth_step(&s->wear, wear_t);
        float in[2] = { l[n], r[n] };

        /* mono the lows: take the low band out of the side signal */
        float mid = 0.5f * (in[0] + in[1]), side = 0.5f * (in[0] - in[1]);
        side -= dfx_bq_run(&s->side_lp, &s->side_s, side) * mono;
        float w[2] = { mid + side, mid - side };

        /* rumble is shared: it is the platter, not the groove */
        float rum = dfx_bq_run(&s->rumble_lp, &s->rumble_s, dfx_randf(&s->rng)) * rumble * 8.0f;

        /* pops: rare, deep, mostly mono */
        if (rand01(&s->rng) < pop_prob) {
            s->pop_env = 0.15f + 0.25f * rand01(&s->rng);
            s->pop_ph = 0;
        }
        s->pop_ph += pop_inc;
        float pop = s->pop_env * sinf(s->pop_ph);
        s->pop_env *= pop_decay;

        float common_click = 0;
        if (rand01(&s->rng) < click_prob) {
            float a = rand01(&s->rng);
            common_click = a * a * a * 0.9f * (dfx_rand(&s->rng) & 1 ? 1.0f : -1.0f);
        }

        for (int c = 0; c < 2; c++) {
            float y = w[c];
            y = dfx_bq_run(&s->mid, &s->mid_s[c], y);
            y = dfx_bq_run(&s->lp, &s->lp_s[c], y);
            y = dfx_bq_run(&s->hp, &s->hp_s[c], y);
            y = dfx_lerp(y, dfx_sat(y * 1.6f) * 0.625f, wear * 0.5f);

            /* clicks: mostly in both channels, some only in one */
            if (rand01(&s->rng) < click_prob * 0.4f) {
                float a = rand01(&s->rng);
                s->click_env[c] = a * a * a * 0.7f;
                s->click_sign[c] = dfx_rand(&s->rng) & 1 ? 1.0f : -1.0f;
            }
            if (common_click != 0) {
                s->click_env[c] = fabsf(common_click);
                s->click_sign[c] = common_click > 0 ? 1.0f : -1.0f;
            }
            float click = s->click_env[c] * s->click_sign[c] * (0.6f + 0.4f * dfx_randf(&s->rng));
            s->click_env[c] *= click_decay;
            click = dfx_bq_run(&s->click_hp, &s->click_s[c], click);

            float hiss = dfx_bq_run(&s->hiss_lp, &s->hiss_s[c], dfx_randf(&s->rng)) * noise;
            y += click + pop + rum + hiss;
            in[c] = (in[c] + (y - in[c]) * mix) * level;
        }
        l[n] = in[0];
        r[n] = in[1];
    }
}

const dfx_effect_def dfx_dust = {
    "dust", "Dust",
    "Worn record: crackle and pops, groove wear, surface noise, rumble, mono lows",
    P_COUNT, params, sizeof(dust),
    init, reset, set_param, get_param, process, NULL,
};
