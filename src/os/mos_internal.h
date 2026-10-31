/* MOSURA OS internals, shared by the engine, the UI and the tests. */
#ifndef MOS_INTERNAL_H
#define MOS_INTERNAL_H

#include "../../include/mos.h"
#include "../../include/doomfx.h"
#include "../../include/doomfx_synth.h"
#include "../../include/doomfx_groove.h"

#define MOS_CHUNK       64           /* the engine renders at most this many frames at a time */
#define MOS_VOICES      32           /* sample voices */
#define MOS_PENDING     128          /* scheduled note offs */
#define MOS_INSERTS     8            /* insert effects shared by the 16 tracks */
#define MOS_FX_BYTES    (528 * 1024) /* memory per effect unit: the largest effect fits */
#define MOS_SYNTH_BYTES (12 * 1024)  /* memory per synth track */

/* effect units: inserts first, then the two sends, then the two master slots */
#define MOS_UNIT_SEND_A (MOS_INSERTS)
#define MOS_UNIT_SEND_B (MOS_INSERTS + 1)
#define MOS_UNIT_MASTER (MOS_INSERTS + 2)
#define MOS_UNITS       (MOS_INSERTS + 4)

/* event flags */
#define MOS_EV_NEW  1 /* recorded during this pass: don't play it again until the loop comes round */
#define MOS_EV_HELD 2 /* the pad is still down: the length is not known yet */

typedef struct {
    int32_t tick;
    uint16_t dur;
    uint8_t track, note, vel, flags;
} mos_event;

typedef struct {
    int bars;     /* 0 = empty sequence */
    int beats;    /* beats per bar */
    float tempo;
    int loop;
    int count;
    mos_event ev[MOS_SEQ_EVENTS]; /* sorted by tick */
} mos_seq;

enum { MOS_TRACK_DRUM, MOS_TRACK_SYNTH };
enum { MOS_PADS_MUTE, MOS_PADS_SOLO, MOS_PADS_SELECT };

typedef struct {
    int name;      /* index into mos_theme_track_names */
    int type;      /* MOS_TRACK_* */
    int preset;    /* synth preset */
    int octave;    /* synth: pad 1 plays C of this octave (C4 = 60) */
    float vol;     /* 0..1.25 */
    float pan;     /* -1..1 */
    float send[2]; /* 0..1 */
    int mute, solo;
    int gate;      /* drum: pads play while held (chops) instead of one-shot */
    int unit;      /* insert effect unit, or -1 */
    float gain_l, gain_r, send_g[2]; /* gains as they glide towards vol / pan / send */
    float meter;
    double synth_mem[MOS_SYNTH_BYTES / sizeof(double)];
} mos_track;

typedef struct {
    const float *l, *r;
    int frames;
    float sr;
} mos_sample;

typedef struct {
    int active;
    int slot, track, note;
    double pos, inc;
    float gain;
    float fade;     /* 1 while playing, ramps to 0 on release or choke */
    int releasing;
} mos_voice;

typedef struct {
    int track, note;
    int64_t at;     /* clock tick */
} mos_pending;

typedef struct {
    const dfx_effect_def *fx; /* NULL = empty */
    int owner;                /* track for inserts, -1 otherwise */
    double mem[MOS_FX_BYTES / sizeof(double)];
} mos_unit;

typedef struct { int seq, reps; } mos_song_step;

enum { MOS_SCR_MAIN, MOS_SCR_STEP, MOS_SCR_TRACK, MOS_SCR_FX, MOS_SCR_MIX, MOS_SCR_SONG, MOS_SCR_COUNT };
enum { MOS_REC_OFF, MOS_REC_REPLACE, MOS_REC_OVERDUB };

struct mos {
    float sr;
    int64_t samples;     /* since init: drives the boot screen, messages, tap tempo */

    /* project */
    mos_seq seqs[MOS_SEQS];
    mos_track tracks[MOS_TRACKS];
    mos_sample pads[MOS_PADS];
    mos_song_step song[MOS_SONG_STEPS];
    int song_len, song_loop;
    float master;
    int tc;              /* timing correct grid in ticks (0 = off) */
    float swing;         /* 50..75 */
    int countin;         /* count in before recording */
    int click;           /* 0 off, 1 while recording, 2 always */

    /* transport */
    int seq;             /* current sequence */
    int next_seq;        /* queued, or -1 */
    int playing;
    int rec, rec_armed;  /* MOS_REC_* */
    int song_mode, song_step, song_rep;
    int countin_left;    /* ticks of count in still to play */
    int tick;            /* position in the sequence */
    int64_t clock;       /* ticks since play started */
    double to_next;      /* samples until the next tick */
    int ev_idx;          /* playback cursor into the sequence's events */
    mos_pending pending[MOS_PENDING];
    int pending_count;

    /* voices and effects */
    mos_voice voices[MOS_VOICES];
    mos_unit units[MOS_UNITS];
    float click_env, click_ph, click_freq;

    /* performance state */
    int active_track;
    int bank;            /* 0..9 */
    int held_vel[16];    /* velocity of each pad while held, 0 = up */
    int shift, repeat_held, erase_held, full_level;
    float pad_flash[16];
    int tap_count;
    int64_t tap_last;
    double tap_avg;

    /* undo: one level, swap to redo */
    mos_seq undo;
    int undo_seq, undo_valid;
    int undo_taken;      /* a snapshot was taken for the recording in progress */

    /* UI */
    int screen;
    int page[MOS_SCR_COUNT];
    int cursor[MOS_SCR_COUNT];
    int editing;
    int step_tick;       /* STEP: cursor position */
    int step_sel;        /* STEP: selected event among those at the cursor */
    int song_cursor;
    int pads_mode;       /* TRACK: MOS_PADS_* */
    char msg[33];
    int64_t msg_until;
    float knob[3];
    int64_t knob_touch[3];
    int knob_valid[3];
    int nfields;         /* fields on the current screen, counted while drawing */
    int booted;

    /* live pads: what each held pad started, so the release matches */
    int held_track[16], held_note[16];
    double held_start[16]; /* clock tick (with fraction) of the press, for recorded lengths */
    int held_recorded[16];
    int erase_snap;

    /* work buffers */
    float tb[MOS_TRACKS][2][MOS_CHUNK];
    float bus[3][2][MOS_CHUNK]; /* master, send A, send B */
    float tmp[2][MOS_CHUNK];

    uint8_t fb[MOS_LCD_W * MOS_LCD_H];
};

/* engine.c */
int mos_seq_len(const mos *m, int seq);                 /* in ticks */
int mos_insert_event(mos *m, int seq, mos_event e);     /* returns its index, or -1 if full */
void mos_delete_event(mos *m, int seq, int index);
void mos_trigger(mos *m, int track, int note, int vel, int64_t off_at); /* off_at < 0: held until mos_release */
void mos_release(mos *m, int track, int note);
void mos_start(mos *m, int from_tick);
void mos_stop(mos *m);
void mos_set_track_fx(mos *m, int track, const dfx_effect_def *fx);
void mos_set_unit_fx(mos *m, int unit, const dfx_effect_def *fx);
void mos_set_track_type(mos *m, int track, int type);
void mos_load_synth_preset(mos *m, int track, int preset);
void mos_snapshot(mos *m);                              /* save the current sequence for undo */
void mos_undo(mos *m);
void mos_message(mos *m, const char *text);
int mos_note_for_pad(const mos *m, int track, int pad); /* event note for a pad on a track */
float mos_bpm(const mos *m);
void mos_record_hit(mos *m, int pad, int track, int note, int vel); /* live hit while recording */
void mos_pad_release(mos *m, int pad);                             /* closes a recorded note's length */

/* ui.c */
void mos_ui_draw(mos *m);
void mos_ui_init(mos *m);

#endif
