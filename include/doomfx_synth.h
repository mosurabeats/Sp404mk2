/*
 * Synth engines. Same rules as the effects in doomfx.h: the host owns the
 * memory, nothing allocates, any block size gives the same output.
 *
 * Notes are sample accurate when the host splits its audio block at each
 * event: render up to the event, call note_on / note_off, render the rest.
 * On the SP that maps to pads in chromatic mode, or the DOOM OS piano roll.
 */
#ifndef DOOMFX_SYNTH_H
#define DOOMFX_SYNTH_H

#include "doomfx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dfx_preset_value {
    const char *param; /* parameter id */
    float value;
} dfx_preset_value;

/* A preset is the parameter defaults plus these changes. */
typedef struct dfx_preset {
    const char *name;
    int count;
    const dfx_preset_value *values;
} dfx_preset;

typedef struct dfx_synth_def {
    const char *id;
    const char *name;
    const char *desc;
    int num_params;           /* params[0..2] are CTRL 1..3 */
    const dfx_param *params;
    int num_presets;
    const dfx_preset *presets;
    size_t state_size;
    void (*init)(void *state, float sample_rate); /* parameters to defaults, all voices silent */
    void (*reset)(void *state);                    /* silence every voice, keep parameters */
    void (*set_param)(void *state, int index, float value);
    float (*get_param)(const void *state, int index);
    void (*note_on)(void *state, int note, int velocity); /* MIDI note 0..127, velocity 1..127 */
    void (*note_off)(void *state, int note);
    void (*all_notes_off)(void *state);           /* release every voice */
    void (*render)(void *state, float *left, float *right, int frames); /* overwrites the buffers */
} dfx_synth_def;

const dfx_synth_def *const *dfx_synths(int *count);
const dfx_synth_def *dfx_synth_find(const char *id);
int dfx_synth_param_index(const dfx_synth_def *synth, const char *id);

/* Set every parameter to its default, then apply preset `index`. Returns -1 if there is no such preset. */
int dfx_synth_load_preset(const dfx_synth_def *synth, void *state, int index);
int dfx_synth_preset_index(const dfx_synth_def *synth, const char *name);

#ifdef __cplusplus
}
#endif

#endif
