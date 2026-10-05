/*
 * doomfx: SP-1200 / MPC60 / MPC3000 flavoured effects and groove tools,
 * written to be dropped into an SP-404MK2 custom OS (DOOM OS) once it has
 * an effects SDK, and usable on a desktop in the meantime.
 *
 * Design rules, so the code ports to a firmware audio callback:
 *   - The host owns all memory. An effect's state is a plain block of
 *     `state_size` bytes; nothing is allocated, nothing is freed.
 *   - process() works in place on stereo float buffers of any block size,
 *     and the output does not depend on how the audio is split into blocks.
 *   - Parameters are set in their real units (Hz, dB, semitones, %), and
 *     the first three map to CTRL 1, CTRL 2 and CTRL 3 on the SP.
 */
#ifndef DOOMFX_H
#define DOOMFX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFX_MAX_PARAMS 8

/* dfx_param.flags */
#define DFX_PARAM_LOG 1 /* a knob should sweep this exponentially (frequencies, times) */

typedef struct dfx_param {
    const char *id;     /* short id for scripts: "cutoff" */
    const char *name;   /* display name, 8 chars or less fits the SP screen */
    float min, max, def;
    const char *unit;   /* "Hz", "dB", "st", "%", "" */
    int steps;          /* 0 = continuous, otherwise the number of whole-number choices */
    const char *const *labels; /* optional names for the choices of a stepped parameter */
    int flags;          /* DFX_PARAM_* */
} dfx_param;

typedef struct dfx_effect_def {
    const char *id;
    const char *name;
    const char *desc;
    int num_params;           /* params[0..2] are CTRL 1..3, the rest are SHIFT pages */
    const dfx_param *params;
    size_t state_size;
    void (*init)(void *state, float sample_rate); /* sets every parameter to its default */
    void (*reset)(void *state);                    /* clears audio history, keeps parameters */
    void (*set_param)(void *state, int index, float value);
    float (*get_param)(const void *state, int index);
    void (*process)(void *state, float *left, float *right, int frames);
} dfx_effect_def;

/* All effects in this library, in display order. */
const dfx_effect_def *const *dfx_effects(int *count);

/* Look an effect up by id; NULL if there is no such effect. */
const dfx_effect_def *dfx_find(const char *id);

/* Index of a parameter by id, or -1. */
int dfx_param_index(const dfx_effect_def *fx, const char *id);

/*
 * Knob position (0..1) to parameter value and back, following the
 * parameter's range, steps and DFX_PARAM_LOG. Use these to drive a
 * parameter from a CTRL knob or MIDI CC.
 */
float dfx_param_from_knob(const dfx_param *p, float knob);
float dfx_param_to_knob(const dfx_param *p, float value);

#ifdef __cplusplus
}
#endif

#endif
