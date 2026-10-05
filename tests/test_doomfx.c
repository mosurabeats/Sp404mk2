#include "../include/doomfx.h"
#include "../include/doomfx_groove.h"
#include "../src/dsp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define SR 48000
#define LEN (SR / 2)

static float L[LEN], R[LEN], L2[LEN], R2[LEN];

/* Magnitude of frequency f in x, normalised so a full-scale sine reads 1. */
static float goertzel(const float *x, int n, float f, float sr)
{
    float w = 2.0f * (float)M_PI * f / sr, c = 2.0f * cosf(w), s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) {
        float s0 = x[i] + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return sqrtf(s1 * s1 + s2 * s2 - c * s1 * s2) * 2.0f / n;
}

static void fill_test_signal(float *l, float *r, int n, uint32_t seed)
{
    uint32_t rng = seed;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float burst = (i % 12000) < 2000 ? 1.0f : 0.2f; /* drum-like level jumps */
        l[i] = burst * (0.5f * sinf(2 * (float)M_PI * 220 * t) + 0.3f * dfx_randf(&rng));
        r[i] = burst * (0.5f * sinf(2 * (float)M_PI * 3300 * t) + 0.3f * dfx_randf(&rng));
    }
}

static void *new_state(const dfx_effect_def *fx)
{
    void *s = calloc(1, fx->state_size);
    fx->init(s, SR);
    return s;
}

static void test_groove(void)
{
    /* MPC at 96 PPQN, sixteenths: a pair is 48 ticks */
    CHECK(dfx_swing_offset(24, 50, 1) == 0, "50%% is straight");
    CHECK(dfx_swing_offset(24, 66, 1) == 8, "66%% moves the off step to 32, got +%d", dfx_swing_offset(24, 66, 1));
    CHECK(dfx_swing_offset(24, 75, 1) == 12, "75%% is a dotted feel");
    CHECK(dfx_swing_offset(24, 66, 2) == 0, "even steps never move");
    CHECK(dfx_swing_offset(24, 90, 1) == 12, "swing is clamped to 75%%");

    /* SP-1200 settings land on whole ticks of 24 PPQN eighths: 12..17 */
    for (int i = 0; i < DFX_SP1200_SWING_COUNT; i++) {
        int off = dfx_swing_offset(12, DFX_SP1200_SWING[i], 1);
        CHECK(off == i, "SP-1200 swing %g%% should be +%d ticks, got +%d", DFX_SP1200_SWING[i], i, off);
    }

    dfx_groove g = dfx_groove_mpc(66);
    CHECK(dfx_groove_target(&g, 23) == 32, "23 snaps to the swung 32, got %d", dfx_groove_target(&g, 23));
    CHECK(dfx_groove_target(&g, 5) == 0, "5 snaps to 0");
    CHECK(dfx_groove_target(&g, 45) == 48, "45 snaps to 48");
    CHECK(dfx_groove_target(&g, 100) == 96, "100 snaps to 96");

    dfx_note n[3] = { { 23, 10, 36, 100 }, { 40, 10, 36, 100 }, { 3, 10, 36, 100 } };
    g.strength = 0.5f;
    dfx_groove_apply(&g, n, 3);
    CHECK(n[0].tick == 28 && n[1].tick == 36 && n[2].tick == 1, "half strength (halves round towards the grid): %d %d %d", n[0].tick, n[1].tick, n[2].tick);
    CHECK(n[0].length == 10, "lengths are kept");

    dfx_note w[2] = { { 30, 1, 0, 1 }, { 40, 1, 0, 1 } };
    g.strength = 1; g.window = 4;
    dfx_groove_apply(&g, w, 2);
    CHECK(w[0].tick == 32 && w[1].tick == 40, "window leaves far notes alone: %d %d", w[0].tick, w[1].tick);

    dfx_note h1[64], h2[64];
    for (int i = 0; i < 64; i++) h1[i] = (dfx_note){ i * 24, 12, 36, (uint8_t)(i == 0 ? 1 : 127) };
    memcpy(h2, h1, sizeof h1);
    dfx_humanize(h1, 64, 4, 10, 7);
    dfx_humanize(h2, 64, 4, 10, 7);
    CHECK(memcmp(h1, h2, sizeof h1) == 0, "humanise is repeatable for a seed");
    int ok = 1, moved = 0;
    for (int i = 0; i < 64; i++) {
        int d = h1[i].tick - i * 24;
        if (h1[i].tick < 0 || (i > 0 && abs(d) > 4) || h1[i].velocity < 1 || h1[i].velocity > 127) ok = 0;
        if (d) moved++;
    }
    CHECK(ok, "humanise stays in range");
    CHECK(moved > 32, "humanise moves most notes (%d)", moved);

    CHECK(dfx_convert_ticks(32, 96, 24) == 8, "96 -> 24 PPQN");
    CHECK(dfx_convert_ticks(30, 96, 24) == 8, "96 -> 24 PPQN rounds to nearest");
    CHECK(dfx_convert_ticks(8, 24, 96) == 32, "24 -> 96 PPQN");
}

static void test_converter(void)
{
    /* 12 bits: at most 4096 distinct levels on a full scale ramp */
    int levels = 0;
    float last = 2;
    for (int i = 0; i <= 200000; i++) {
        float q = dfx_quantize(-1.0f + 2.0f * i / 200000, 12, 0);
        if (q != last) { levels++; last = q; }
    }
    CHECK(levels <= 4096 && levels > 4000, "12-bit levels: %d", levels);
    CHECK(dfx_quantize(1.5f, 12, 0) < 1.0f && dfx_quantize(-1.5f, 12, 0) == -1.0f, "clips at full scale");

    /* companding: finer steps near zero, coarser near full scale than linear */
    float small = 0.001f, big = 0.9f;
    float err_small_mu = fabsf(dfx_quantize(small, 12, 255) - small), err_small_lin = fabsf(dfx_quantize(small, 12, 0) - small);
    CHECK(err_small_mu < err_small_lin, "mu-law is more precise near zero (%g vs %g)", err_small_mu, err_small_lin);
    float step_big = dfx_quantize(big + 0.01f, 12, 255) - dfx_quantize(big, 12, 255);
    CHECK(step_big > 2.0f / 4096, "mu-law is coarser near full scale");

    /* sample & hold ticks at the slow rate */
    dfx_sh sh = { 0 };
    int ticks = 0, t;
    for (int i = 0; i < SR; i++) { dfx_sh_run(&sh, 0, 26040.0f / SR, 12, 0, &t); ticks += t; }
    CHECK(abs(ticks - 26040) <= 1, "26.04 kHz clock ticks %d times a second", ticks);
}

static void test_sp1200_aliasing(void)
{
    const dfx_effect_def *fx = dfx_find("sp1200");
    void *s = new_state(fx);
    fx->set_param(s, dfx_param_index(fx, "output"), 2); /* 7-8: no filter */
    for (int i = 0; i < LEN; i++) L[i] = R[i] = 0.5f * sinf(2 * (float)M_PI * 10000 * i / SR);
    fx->process(s, L, R, LEN);
    float alias = goertzel(L + 4800, LEN - 4800, 26040 - 10000, SR);
    float tone = goertzel(L + 4800, LEN - 4800, 10000, SR);
    CHECK(alias > 0.1f, "a 10 kHz tone aliases to 16.04 kHz without a filter (%.3f)", alias);
    CHECK(tone > 0.2f, "the tone itself survives (%.3f)", tone);

    /* pitch trick: +12 st halves the rate to 13.02 kHz, so 10 kHz folds to 3.02 kHz */
    fx->init(s, SR);
    fx->set_param(s, dfx_param_index(fx, "output"), 2);
    fx->set_param(s, dfx_param_index(fx, "pitch"), 12);
    for (int i = 0; i < LEN; i++) L[i] = R[i] = 0.5f * sinf(2 * (float)M_PI * 10000 * i / SR);
    fx->process(s, L, R, LEN);
    alias = goertzel(L + 4800, LEN - 4800, 13020 - 10000, SR);
    CHECK(alias > 0.1f, "pitch trick folds 10 kHz to 3.02 kHz (%.3f)", alias);

    /* outputs 1-2 with the filter down remove the alias */
    fx->init(s, SR);
    fx->set_param(s, dfx_param_index(fx, "cutoff"), 2000);
    for (int i = 0; i < LEN; i++) L[i] = R[i] = 0.5f * sinf(2 * (float)M_PI * 10000 * i / SR);
    fx->process(s, L, R, LEN);
    alias = goertzel(L + 4800, LEN - 4800, 16040, SR);
    CHECK(alias < 0.01f, "the 2 kHz output filter removes the alias (%.4f)", alias);
    free(s);
}

static void test_mpc3000_transparent(void)
{
    /* filter open, no warmth, 16 bits, at 44.1 kHz: close to a wire */
    const dfx_effect_def *fx = dfx_find("mpc3000");
    void *s = calloc(1, fx->state_size);
    fx->init(s, 44100);
    fx->set_param(s, dfx_param_index(fx, "warmth"), 0);
    float maxdiff = 0;
    for (int i = 0; i < LEN; i++) L[i] = R[i] = 0.3f * sinf(2 * (float)M_PI * 1000 * i / 44100.0f);
    memcpy(L2, L, sizeof L);
    fx->process(s, L, R, LEN);
    for (int i = 4410; i < LEN; i++) maxdiff = fmaxf(maxdiff, fabsf(L[i] - L2[i]));
    CHECK(maxdiff < 0.01f, "neutral MPC3000 is near transparent (max diff %.4f)", maxdiff);
    free(s);
}

static void test_all_effects(void)
{
    int count;
    const dfx_effect_def *const *fx = dfx_effects(&count);
    CHECK(count == 5, "five effects");
    for (int e = 0; e < count; e++) {
        const dfx_effect_def *f = fx[e];
        CHECK(f->num_params >= 3 && f->num_params <= DFX_MAX_PARAMS, "%s: 3..8 params", f->id);
        CHECK(dfx_find(f->id) == f, "%s: findable", f->id);
        for (int p = 0; p < f->num_params; p++) {
            const dfx_param *pp = &f->params[p];
            CHECK(pp->def >= pp->min && pp->def <= pp->max, "%s.%s default in range", f->id, pp->id);
            CHECK(!pp->steps || pp->steps == (int)(pp->max - pp->min) + 1, "%s.%s steps match range", f->id, pp->id);
            float k = dfx_param_to_knob(pp, pp->def);
            CHECK(fabsf(dfx_param_from_knob(pp, k) - pp->def) < 1e-3f * (pp->max - pp->min) + 1e-3f,
                  "%s.%s knob mapping round trips", f->id, pp->id);
        }

        void *s = new_state(f);
        for (int p = 0; p < f->num_params; p++)
            CHECK(f->get_param(s, p) == f->params[p].def, "%s.%s starts at default", f->id, f->params[p].id);

        /* stability: every parameter at min, max and random, on a nasty signal */
        uint32_t rng = 99;
        for (int trial = 0; trial < 12; trial++) {
            f->init(s, SR);
            for (int p = 0; p < f->num_params; p++) {
                const dfx_param *pp = &f->params[p];
                float v = trial == 0 ? pp->min : trial == 1 ? pp->max : dfx_param_from_knob(pp, 0.5f + 0.5f * dfx_randf(&rng));
                f->set_param(s, p, v);
            }
            fill_test_signal(L, R, LEN, (uint32_t)trial + 1);
            for (int i = 0; i < LEN; i += 1000) L[i] = R[i] = (i & 1) ? 1.0f : -1.0f; /* clicks */
            f->process(s, L, R, LEN);
            int finite = 1;
            float peak = 0;
            for (int i = 0; i < LEN; i++) {
                if (!isfinite(L[i]) || !isfinite(R[i])) finite = 0;
                peak = fmaxf(peak, fmaxf(fabsf(L[i]), fabsf(R[i])));
            }
            CHECK(finite, "%s trial %d: output is finite", f->id, trial);
            CHECK(peak < 8.0f, "%s trial %d: output is bounded (peak %.2f)", f->id, trial, peak);

            /* silence decays to silence */
            memset(L, 0, sizeof L); memset(R, 0, sizeof R);
            f->process(s, L, R, LEN);
            float tail = 0;
            for (int i = LEN - 4800; i < LEN; i++) tail = fmaxf(tail, fmaxf(fabsf(L[i]), fabsf(R[i])));
            CHECK(tail < 1e-3f, "%s trial %d: silence in, silence out (%.5f)", f->id, trial, tail);
        }

        /* output does not depend on block size */
        void *s2 = calloc(1, f->state_size);
        f->init(s, SR);
        f->init(s2, SR);
        for (int p = 0; p < 2; p++) {
            float v = dfx_param_from_knob(&f->params[p], p ? 0.7f : 0.3f);
            f->set_param(s, p, v);
            f->set_param(s2, p, v);
        }
        fill_test_signal(L, R, LEN, 5);
        memcpy(L2, L, sizeof L); memcpy(R2, R, sizeof R);
        f->process(s, L, R, LEN);
        for (int pos = 0, b = 1; pos < LEN; pos += b, b = b % 97 + 13)
            f->process(s2, L2 + pos, R2 + pos, pos + b > LEN ? LEN - pos : b);
        CHECK(!memcmp(L, L2, sizeof L) && !memcmp(R, R2, sizeof R), "%s: output is independent of block size", f->id);
        free(s2);
        free(s);
    }
}

int main(void)
{
    test_groove();
    test_converter();
    test_sp1200_aliasing();
    test_mpc3000_transparent();
    test_all_effects();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
