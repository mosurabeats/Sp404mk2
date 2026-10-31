#ifndef MOS_PNG_H
#define MOS_PNG_H

#include <stdint.h>

/* Writes w x h 8-bit RGB pixels (row by row) as a PNG. Returns 0 on success. */
int png_write_rgb(const char *path, const uint8_t *rgb, int w, int h);

#endif
