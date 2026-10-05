#include "../include/doomfx.h"
#include "../include/doomfx_synth.h"

#include <math.h>
#include <string.h>

extern const dfx_effect_def dfx_sp1200, dfx_mpc60, dfx_mpc3000, dfx_bitladder, dfx_busglue, dfx_junochorus;
extern const dfx_synth_def dfx_juno106;

static const dfx_effect_def *const all_effects[] = {
    &dfx_sp1200, &dfx_mpc60, &dfx_mpc3000, &dfx_bitladder, &dfx_busglue, &dfx_junochorus,
};

static const dfx_synth_def *const all_synths[] = {
    &dfx_juno106,
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

const dfx_synth_def *const *dfx_synths(int *count)
{
    if (count) *count = (int)(sizeof all_synths / sizeof all_synths[0]);
    return all_synths;
}

const dfx_synth_def *dfx_synth_find(const char *id)
{
    int n;
    const dfx_synth_def *const *s = dfx_synths(&n);
    for (int i = 0; i < n; i++)
        if (strcmp(s[i]->id, id) == 0) return s[i];
    return NULL;
}

int dfx_synth_param_index(const dfx_synth_def *synth, const char *id)
{
    for (int i = 0; i < synth->num_params; i++)
        if (strcmp(synth->params[i].id, id) == 0) return i;
    return -1;
}

int dfx_synth_preset_index(const dfx_synth_def *synth, const char *name)
{
    for (int i = 0; i < synth->num_presets; i++)
        if (strcmp(synth->presets[i].name, name) == 0) return i;
    return -1;
}

int dfx_synth_load_preset(const dfx_synth_def *synth, void *state, int index)
{
    if (index < 0 || index >= synth->num_presets) return -1;
    for (int i = 0; i < synth->num_params; i++) synth->set_param(state, i, synth->params[i].def);
    const dfx_preset *p = &synth->presets[index];
    for (int i = 0; i < p->count; i++) {
        int idx = dfx_synth_param_index(synth, p->values[i].param);
        if (idx >= 0) synth->set_param(state, idx, p->values[i].value);
    }
    return 0;
}
