#include "../include/doomfx_groove.h"

#include <math.h>
#include <stdlib.h>

const float DFX_SP1200_SWING[DFX_SP1200_SWING_COUNT] = { 50, 54, 58, 63, 67, 71 };

dfx_groove dfx_groove_mpc(float swing)
{
    dfx_groove g = { DFX_PPQN_MPC, DFX_PPQN_MPC / 4, swing, 1.0f, 0 };
    return g;
}

dfx_groove dfx_groove_sp1200(float swing)
{
    dfx_groove g = { DFX_PPQN_SP1200, DFX_PPQN_SP1200 / 2, swing, 1.0f, 0 };
    return g;
}

static int floor_div(int a, int b)
{
    int q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

/* Position of the off-beat step inside a pair of grid steps. */
static int swing_point(int grid, float swing)
{
    if (swing < 50.0f) swing = 50.0f;
    if (swing > 75.0f) swing = 75.0f;
    return (int)lroundf(2.0f * (float)grid * swing / 100.0f);
}

int dfx_swing_offset(int grid, float swing, int step)
{
    if (grid <= 0 || (step & 1) == 0) return 0;
    return swing_point(grid, swing) - grid;
}

int dfx_groove_target(const dfx_groove *g, int tick)
{
    if (g->grid <= 0) return tick;
    int pair = 2 * g->grid;
    int start = floor_div(tick, pair) * pair;
    int candidates[3] = { start, start + swing_point(g->grid, g->swing), start + pair };
    int best = candidates[0];
    for (int i = 1; i < 3; i++)
        if (abs(candidates[i] - tick) < abs(best - tick)) best = candidates[i];
    return best;
}

void dfx_groove_apply(const dfx_groove *g, dfx_note *notes, int count)
{
    float strength = g->strength < 0 ? 0 : (g->strength > 1 ? 1 : g->strength);
    for (int i = 0; i < count; i++) {
        int t = notes[i].tick;
        int target = dfx_groove_target(g, t);
        int dist = target - t;
        if (g->window > 0 && abs(dist) > g->window) continue;
        t += (int)lroundf(strength * (float)dist);
        notes[i].tick = t < 0 ? 0 : t;
    }
}

static uint32_t next_rand(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

/* Uniform integer in -range..range. */
static int rand_range(uint32_t *s, int range)
{
    if (range <= 0) return 0;
    return (int)(next_rand(s) % (uint32_t)(2 * range + 1)) - range;
}

void dfx_humanize(dfx_note *notes, int count, int timing_ticks, int velocity, uint32_t seed)
{
    uint32_t s = seed;
    for (int i = 0; i < count; i++) {
        int t = notes[i].tick + rand_range(&s, timing_ticks);
        int v = notes[i].velocity + rand_range(&s, velocity);
        notes[i].tick = t < 0 ? 0 : t;
        notes[i].velocity = (uint8_t)(v < 1 ? 1 : (v > 127 ? 127 : v));
    }
}

int dfx_convert_ticks(int tick, int from_ppqn, int to_ppqn)
{
    if (from_ppqn <= 0 || to_ppqn <= 0) return tick;
    return (int)llround((double)tick * to_ppqn / from_ppqn);
}
