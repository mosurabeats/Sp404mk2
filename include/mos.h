/*
 * MOSURA OS: an MPC 2000XL / 2500 style workflow for the SP-404MK2.
 *
 * One button, one screen. No button combos for anything you do every day.
 *
 *   MAIN   sequence, track, tempo, bars, loop, timing correct, swing
 *   STEP   step editor: the events at each grid step of the active track
 *   TRACK  track type, name, synth preset; pads mute / solo / select tracks
 *   FX     insert effect of the active track, send buses A and B, master
 *   MIX    16 channel strips: level, pan, sends, meters
 *   SONG   a list of sequences and repeats
 *
 * Pressing a screen key again flips through that screen's pages. On every
 * screen VALUE turns to pick a field, VALUE push edits it, and CTRL 1-3
 * always set the three things in the knob row at the bottom of the screen.
 *
 * Like doomfx, the core is portable C with no allocation and no I/O: the
 * host allocates mos_size() bytes, owns the sample memory, feeds input
 * events and pulls audio and the screen. All mos_* calls must come from
 * one thread (or be serialised by the host).
 */
#ifndef MOS_H
#define MOS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOS_PPQN        96
#define MOS_TRACKS      16
#define MOS_SEQS        32
#define MOS_SEQ_EVENTS  2048
#define MOS_BANKS       10
#define MOS_PADS        (MOS_BANKS * 16)
#define MOS_SONG_STEPS  64
#define MOS_LCD_W       128
#define MOS_LCD_H       64

typedef struct mos mos;

/* Logical keys. docs/MOSURA.md suggests a mapping onto the SP's buttons. */
typedef enum {
    MOS_KEY_SHIFT,
    /* screens */
    MOS_KEY_MAIN, MOS_KEY_STEP, MOS_KEY_TRACK, MOS_KEY_FX, MOS_KEY_MIX, MOS_KEY_SONG,
    /* transport */
    MOS_KEY_PLAY,        /* stopped: play from the top (SHIFT: from the step cursor). playing: stop */
    MOS_KEY_REC,         /* record, replacing the active track's events as the playhead passes */
    MOS_KEY_OVERDUB,     /* record on top */
    /* performance and editing */
    MOS_KEY_NOTE_REPEAT, /* hold with pads: repeat at the timing correct value, swung */
    MOS_KEY_ERASE,       /* hold with a pad while playing: erase that pad. STEP: delete the event */
    MOS_KEY_UNDO,        /* undo / redo the last recording or edit of the sequence */
    MOS_KEY_TAP,         /* tap tempo */
    MOS_KEY_FULL_LEVEL,  /* pads play at velocity 127 */
    MOS_KEY_BANK_A, MOS_KEY_BANK_B, MOS_KEY_BANK_C, MOS_KEY_BANK_D, MOS_KEY_BANK_E, /* again: F-J */
    MOS_KEY_ENTER,       /* VALUE push */
    MOS_KEY_EXIT,
    MOS_KEY_COUNT
} mos_key_id;

/* Key LED states for mos_key_led(). */
enum { MOS_LED_OFF, MOS_LED_ON, MOS_LED_BLINK };

size_t mos_size(void);

/* Sets up an empty project: sequence 1, two bars at 90 BPM, track 1 a drum track. */
void mos_init(mos *m, float sample_rate);

/*
 * Assign audio to sample pad `slot` (bank * 16 + pad, 0..159). The memory
 * stays owned by the host and must outlive its use; NULL clears the pad.
 * `right` may equal `left` for mono. Samples at another rate are resampled
 * on playback.
 */
void mos_set_sample(mos *m, int slot, const float *left, const float *right, int frames, float sample_rate);

/* Render `frames` of stereo output. Overwrites the buffers. Any block size. */
void mos_process(mos *m, float *left, float *right, int frames);

/* Input. pad is 0..15 (pad 1 = 0), velocity 1..127 on press and 0 on release. */
void mos_key(mos *m, mos_key_id key, int down);
void mos_pad(mos *m, int pad, int velocity);
void mos_turn(mos *m, int delta);           /* VALUE encoder, in detents */
void mos_knob(mos *m, int ctrl, float pos); /* CTRL 1-3 (ctrl 0..2), absolute position 0..1 */

/* Output. The screen is MOS_LCD_W x MOS_LCD_H, one byte per pixel (0 or 1), row by row. */
const uint8_t *mos_screen(mos *m);
uint32_t mos_pad_color(const mos *m, int pad); /* 0xRRGGBB for the pad's RGB LED */
int mos_key_led(const mos *m, mos_key_id key);    /* MOS_LED_* */

/* Names for the simulator and for remapping. */
const char *mos_key_name(mos_key_id key);
int mos_key_from_name(const char *name);       /* -1 if unknown */

#ifdef __cplusplus
}
#endif

#endif
