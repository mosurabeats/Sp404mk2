#ifndef MOS_KIT_H
#define MOS_KIT_H

#include <stdint.h>

#define KIT_SOUNDS 8
extern const char *const kit_names[KIT_SOUNDS];

/* Synthesises one mono sound; *out is malloc'd. Returns its length in frames, 0 on failure. */
int kit_render(int sound, float sample_rate, float **out);

#endif
