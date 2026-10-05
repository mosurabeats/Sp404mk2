/*
 * Groove engine: MPC60 / MPC3000 / SP-1200 style swing, timing correct
 * (quantise) and humanise for note events.
 *
 * Timing is in sequencer ticks. The MPC60 and MPC3000 run at 96 ticks per
 * quarter note and the SP-1200 at 24, so both feels can be reproduced by
 * choosing the PPQN. Swing works like Linn's: notes on every second grid
 * step are pushed late, so that the on-beat step takes `swing` percent of
 * each pair of steps. 50% is straight and 66% is close to a triplet feel.
 */
#ifndef DOOMFX_GROOVE_H
#define DOOMFX_GROOVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFX_PPQN_MPC    96
#define DFX_PPQN_SP1200 24

/*
 * SP-1200 swing settings in percent. The SP-1200 swings eighth notes at
 * 24 PPQN, so a pair of eighths is 24 ticks and each setting moves the
 * off-beat eighth one more tick late: 12, 13, 14, 15, 16, 17 ticks.
 * (For sixteenth swing on an SP-1200 you program at double tempo.)
 * The MPC60 and MPC3000 go from 50 to 75 in 1% steps at 96 PPQN.
 */
#define DFX_SP1200_SWING_COUNT 6
extern const float DFX_SP1200_SWING[DFX_SP1200_SWING_COUNT];

typedef struct dfx_note {
    int32_t tick;    /* start, in ticks from the start of the pattern */
    int32_t length;  /* in ticks */
    uint8_t note;    /* pad or MIDI note */
    uint8_t velocity;/* 1..127 */
} dfx_note;

typedef struct dfx_groove {
    int ppqn;        /* ticks per quarter note: 96 for MPC, 24 for SP-1200 */
    int grid;        /* grid step in ticks, e.g. ppqn / 4 for sixteenths */
    float swing;     /* 50..75 percent */
    float strength;  /* 0..1: how far notes move towards the grid; 1 = hard quantise */
    int window;      /* only move notes within this many ticks of a grid point; 0 = all notes */
} dfx_groove;

/* A groove with MPC defaults: 96 PPQN, sixteenth grid, the given swing, full strength. */
dfx_groove dfx_groove_mpc(float swing);

/* A groove with SP-1200 defaults: 24 PPQN, eighth grid, the given swing, full strength. */
dfx_groove dfx_groove_sp1200(float swing);

/* Tick offset added to grid step `step` (0-based) by swing; 0 for even steps. */
int dfx_swing_offset(int grid, float swing, int step);

/* The swung grid point nearest to `tick`. */
int dfx_groove_target(const dfx_groove *g, int tick);

/* Quantise and swing notes in place. Lengths are kept. */
void dfx_groove_apply(const dfx_groove *g, dfx_note *notes, int count);

/*
 * Humanise notes in place: random timing shift of up to +/- timing_ticks
 * and random velocity change of up to +/- velocity. The same seed always
 * gives the same result, so an undo can replay it. Ticks never go below 0
 * and velocities stay in 1..127.
 */
void dfx_humanize(dfx_note *notes, int count, int timing_ticks, int velocity, uint32_t seed);

/*
 * Convert a tick from one resolution to another, rounding to the nearest
 * tick of the new resolution. Going from 96 to 24 PPQN and back gives the
 * coarser timing of the SP-1200.
 */
int dfx_convert_ticks(int tick, int from_ppqn, int to_ppqn);

#ifdef __cplusplus
}
#endif

#endif
