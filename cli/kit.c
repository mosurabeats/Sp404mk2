/* A small synthesised drum kit, so the simulator makes sound without any sample files. */
#include "kit.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const char *const kit_names[KIT_SOUNDS] = { "kick", "snare", "hat", "openhat", "clap", "rim", "tom", "shaker" };

static float noise(uint32_t *r)
{
    *r ^= *r << 13; *r ^= *r >> 17; *r ^= *r << 5;
    return (float)(*r >> 8) / 8388608.0f - 1.0f;
}

int kit_render(int sound, float sr, float **out)
{
    static const float dur[KIT_SOUNDS] = { 0.6f, 0.3f, 0.08f, 0.45f, 0.3f, 0.08f, 0.5f, 0.12f };
    int len = (int)(dur[sound] * sr);
    float *x = calloc((size_t)len, sizeof *x);
    if (!x) return 0;
    uint32_t rng = 0x404u + (uint32_t)sound;
    float ph = 0, hp = 0, prev = 0, bp1 = 0, bp2 = 0;
    for (int i = 0; i < len; i++) {
        float t = i / sr, n = noise(&rng), y = 0;
        hp = 0.7f * (hp + n - prev); /* crude highpass for the metals */
        prev = n;
        switch (sound) {
        case 0: { /* kick: pitch-dropping sine with a click */
            float f = 48.0f + 110.0f * expf(-t * 28.0f);
            ph += 2.0f * (float)M_PI * f / sr;
            y = sinf(ph) * expf(-t * 6.0f) * 1.3f + n * expf(-t * 400.0f) * 0.3f;
            break;
        }
        case 1: /* snare: body and rattle */
            ph += 2.0f * (float)M_PI * 190.0f / sr;
            y = 0.55f * sinf(ph) * expf(-t * 22.0f) + 0.6f * n * expf(-t * 13.0f);
            break;
        case 2: y = hp * expf(-t * 60.0f) * 0.6f; break;
        case 3: y = hp * expf(-t * 7.0f) * 0.45f; break;
        case 4: { /* clap: three quick bursts then a tail */
            float env = 0;
            for (int k = 0; k < 3; k++) {
                float tk = t - k * 0.011f;
                if (tk >= 0) env = fmaxf(env, expf(-tk * 180.0f));
            }
            env = fmaxf(env, t > 0.022f ? 0.5f * expf(-(t - 0.022f) * 14.0f) : 0);
            bp1 += 0.25f * (n - bp1); /* rough band */
            bp2 += 0.25f * (bp1 - bp2);
            y = (bp1 - bp2) * env * 2.2f;
            break;
        }
        case 5: /* rim */
            ph += 2.0f * (float)M_PI * 1650.0f / sr;
            y = (sinf(ph) * 0.6f + hp * 0.3f) * expf(-t * 70.0f);
            break;
        case 6: { /* tom */
            float f = 95.0f + 40.0f * expf(-t * 12.0f);
            ph += 2.0f * (float)M_PI * f / sr;
            y = sinf(ph) * expf(-t * 7.0f);
            break;
        }
        default: { /* shaker: noise swell */
            float env = t < 0.03f ? t / 0.03f : expf(-(t - 0.03f) * 35.0f);
            y = hp * env * 0.35f;
            break;
        }
        }
        x[i] = tanhf(y);
    }
    *out = x;
    return len;
}
