#include "../include/doomfx.h"

#include <math.h>
#include <string.h>

extern const dfx_effect_def dfx_sp1200, dfx_mpc60, dfx_mpc3000, dfx_bitladder, dfx_busglue;

static const dfx_effect_def *const all_effects[] = {
    &dfx_sp1200, &dfx_mpc60, &dfx_mpc3000, &dfx_bitladder, &dfx_busglue,
};

const dfx_effect_def *const *dfx_effects(int *count)
{
    if (count) *count = (int)(sizeof all_effects / sizeof all_effects[0]);
    return all_effects;
}

const dfx_effect_def *dfx_find(const char *id)
{
    int n;
    const dfx_effect_def *const *fx = dfx_effects(&n);
    for (int i = 0; i < n; i++)
        if (strcmp(fx[i]->id, id) == 0) return fx[i];
    return NULL;
}

int dfx_param_index(const dfx_effect_def *fx, const char *id)
{
    for (int i = 0; i < fx->num_params; i++)
        if (strcmp(fx->params[i].id, id) == 0) return i;
    return -1;
}

float dfx_param_from_knob(const dfx_param *p, float knob)
{
    knob = knob < 0 ? 0 : (knob > 1 ? 1 : knob);
    float v;
    if ((p->flags & DFX_PARAM_LOG) && p->min > 0)
        v = p->min * powf(p->max / p->min, knob);
    else
        v = p->min + (p->max - p->min) * knob;
    return p->steps ? floorf(v + 0.5f) : v;
}

float dfx_param_to_knob(const dfx_param *p, float value)
{
    if (p->max <= p->min) return 0;
    value = value < p->min ? p->min : (value > p->max ? p->max : value);
    if ((p->flags & DFX_PARAM_LOG) && p->min > 0)
        return logf(value / p->min) / logf(p->max / p->min);
    return (value - p->min) / (p->max - p->min);
}
