/*
 * Stutter (new): tempo-locked chop and repeat, with MPC swing.
 *
 * Mode picks what happens on each slice of the beat:
 *   Off    - straight through.
 *   Gate   - the live input is chopped: each slice opens for Gate percent
 *            of its length. With Swing above 50 the chops land on the same
 *            swung grid as an MPC sequence.
 *   Repeat - waits for the next slice, records one slice, then loops it on
 *            every slice after that until Mode leaves Repeat. Each repeat
 *            can fall in pitch (Fall) and fade (Decay), for tape-stop style
 *            rolls. Turn CTRL 3 to Repeat and back to throw it.
 *
 * Rate and Swing follow the host's tempo and beat position (set_tempo), so
 * the slices line up with the sequence.
 */
#include "../../include/doomfx.h"
#include "../dsp.h"

enum { P_RATE, P_GATE, P_MODE, P_SWING, P_DECAY, P_FALL, P_MIX, P_LEVEL, P_COUNT };
enum { MODE_OFF, MODE_GATE, MODE_REPEAT };

static const char *const mode_labels[] = { "Off", "Gate", "Repeat" };

static const dfx_param params[P_COUNT] = {
    { "rate",  "Rate",  0, DFX_DIV_COUNT - 1, 2, "", DFX_DIV_COUNT, dfx_div_labels, 0 },
    { "gate",  "Gate",  5, 100, 50, "%", 0, 0, 0 },
    { "mode",  "Mode",  0, 2, 1, "", 3, mode_labels, 0 },
    { "swing", "Swing", 50, 75, 50, "%", 0, 0, 0 },
    { "decay", "Decay", 0, 100, 0, "%", 0, 0, 0 },
    { "fall",  "Fall",  -12, 0, 0, "st", 0, 0, 0 },
    { "mix",   "Mix",   0, 100, 100, "%", 0, 0, 0 },
    { "level", "Level", -24, 12, 0, "dB", 0, 0, 0 },
};

#define BUF 65536 /* 1.36 s at 48 kHz: one beat down to 44 BPM */
#define REC_MAX (BUF - 4)

typedef struct {
    float sr;
    float p[P_COUNT];
    dfx_smooth mix, level;
    int fresh;
    float bpm;
    double beat;           /* transport position in beats */
    int synced;            /* the host is giving us positions */
    long seg;              /* index of the current (swung) slice */
    double seg_start;      /* beat where it started */
    double seg_len;        /* its length in beats */
    int state;             /* repeat state machine */
    int rec_len, rec_pos, repeats;
    float play_pos;
    float env;             /* smoothed gate, removes clicks */
    float wet;             /* smoothed crossfade between dry and the repeat */
    float buf[2][BUF];
} stutter;

enum { R_IDLE, R_ARMED, R_RECORDING, R_REPEATING };

static void reset(void *state)
{
    stutter *s = state;
    s->state = R_IDLE;
    s->rec_len = s->rec_pos = s->repeats = 0;
    s->play_pos = 0;
    s->env = 1;
    s->wet = 0;
    s->seg = -1;
}

static void set_param(void *state, int i, float v)
{
    stutter *s = state;
    if (i < 0 || i >= P_COUNT) return;
    v = dfx_clampf(v, params[i].min, params[i].max);
    if (params[i].steps) v = floorf(v + 0.5f);
    s->p[i] = v;
}

static float get_param(const void *state, int i)
{
    const stutter *s = state;
    return (i >= 0 && i < P_COUNT) ? s->p[i] : 0.0f;
}

static void set_tempo(void *state, float bpm, double beat)
{
    stutter *s = state;
    s->bpm = dfx_clampf(bpm, 30.0f, 300.0f);
    if (beat >= 0) {
        s->beat = beat;
        s->synced = 1;
    } else {
        s->synced = 0;
    }
}

static void init(void *state, float sr)
{
    stutter *s = state;
    *s = (stutter){ 0 };
    s->sr = sr;
    s->bpm = 120;
    for (int i = 0; i < P_COUNT; i++) s->p[i] = params[i].def;
    dfx_smooth_init(&s->mix, 0, 20, sr);
    dfx_smooth_init(&s->level, 1, 20, sr);
    s->fresh = 1;
    reset(s);
}

/*
 * Which swung slice holds `beat`: slices come in pairs, and the first of
 * each pair takes `swing` percent of the pair, like MPC sixteenths.
 */
static long find_seg(double beat, double slice, float swing, double *start, double *len)
{
    double pair = 2.0 * slice;
    double pair_idx = floor(beat / pair);
    double in_pair = beat - pair_idx * pair;
    double split = pair * swing * 0.01;
    if (in_pair < split) {
        *start = pair_idx * pair;
        *len = split;
        return (long)pair_idx * 2;
    }
    *start = pair_idx * pair + split;
    *len = pair - split;
    return (long)pair_idx * 2 + 1;
}

static void process(void *state, float *l, float *r, int frames)
{
    stutter *s = state;
    float mix_t = s->p[P_MIX] * 0.01f, level_t = dfx_db(s->p[P_LEVEL]);
    if (s->fresh) {
        s->mix.y = mix_t; s->level.y = level_t;
        s->fresh = 0;
    }
    int mode = (int)s->p[P_MODE];
    double slice = dfx_div_to_beats(s->p[P_RATE]);
    float swing = s->p[P_SWING];
    float gate = s->p[P_GATE] * 0.01f;
    double beat_inc = s->bpm / 60.0 / s->sr;
    float env_a = 1.0f - expf(-1.0f / (0.0015f * s->sr));
    float decay = 1.0f - s->p[P_DECAY] * 0.01f;
    float fall = s->p[P_FALL];

    if (mode != MODE_REPEAT) s->state = R_IDLE;
    else if (s->state == R_IDLE) s->state = R_ARMED;

    for (int n = 0; n < frames; n++) {
        float mix = dfx_smooth_step(&s->mix, mix_t);
        float level = dfx_smooth_step(&s->level, level_t);
        double start, len;
        long seg = find_seg(s->beat, slice, swing, &start, &len);
        int boundary = seg != s->seg;
        s->seg = seg;
        double pos_in_seg = (s->beat - start) / len; /* 0..1 */

        if (boundary) {
            if (s->state == R_ARMED) {
                s->state = R_RECORDING;
                s->rec_pos = 0;
            } else if (s->state == R_RECORDING || s->state == R_REPEATING) {
                if (s->state == R_RECORDING) {
                    s->rec_len = s->rec_pos;
                    s->repeats = 0;
                    s->state = s->rec_len > 0 ? R_REPEATING : R_ARMED;
                } else {
                    s->repeats++;
                }
                s->play_pos = 0;
            }
        }

        float dry[2] = { l[n], r[n] };
        float y[2] = { dry[0], dry[1] };
        float target_wet = 0;

        if (s->state == R_RECORDING) {
            if (s->rec_pos < REC_MAX) {
                s->buf[0][s->rec_pos] = dry[0];
                s->buf[1][s->rec_pos] = dry[1];
                s->rec_pos++;
            }
        } else if (s->state == R_REPEATING) {
            target_wet = 1;
            float speed = exp2f(fmaxf(fall * (float)s->repeats, -36.0f) / 12.0f);
            float gain = powf(decay, (float)s->repeats);
            int i0 = (int)s->play_pos;
            float t = s->play_pos - (float)i0;
            for (int c = 0; c < 2; c++) {
                float v = 0;
                if (i0 + 1 < s->rec_len) v = s->buf[c][i0] + (s->buf[c][i0 + 1] - s->buf[c][i0]) * t;
                y[c] = v * gain;
            }
            s->play_pos += speed;
        }
        s->wet += (target_wet - s->wet) * env_a;

        float g = 1.0f;
        if (mode == MODE_GATE || s->state == R_REPEATING) g = pos_in_seg < gate ? 1.0f : 0.0f;
        s->env += (g - s->env) * env_a;

        for (int c = 0; c < 2; c++) {
            float wet = dfx_lerp(dry[c], y[c], s->wet) * s->env;
            float out = dry[c] + (wet - dry[c]) * mix;
            if (c == 0) l[n] = out * level; else r[n] = out * level;
        }
        s->beat += beat_inc;
    }
}

const dfx_effect_def dfx_stutter = {
    "stutter", "Stutter",
    "Tempo-locked gate and repeat with MPC swing, pitch fall and decay",
    P_COUNT, params, sizeof(stutter),
    init, reset, set_param, get_param, process, set_tempo,
};
