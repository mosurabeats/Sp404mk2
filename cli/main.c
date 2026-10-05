/*
 * doomfx: run the effects and the groove engine on a desktop.
 *
 *   doomfx list
 *   doomfx render <effect> <in.wav> <out.wav> [param=value ...] [--block N] [--bits 16|24]
 *   doomfx demo <out.wav> [--bpm N] [--swing P] [--machine mpc|sp1200] [--humanize T]
 *   doomfx groove [--swing P] [--machine mpc|sp1200] [--bpm N]
 *   doomfx synth <synth> <out.wav> [--preset NAME] [--seq "C3+E3+G3:2 -:1 ..."] [--bpm N] [param=value ...]
 */
#include "../include/doomfx.h"
#include "../include/doomfx_groove.h"
#include "../include/doomfx_synth.h"
#include "wav.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  doomfx list\n"
        "  doomfx render <effect> <in.wav> <out.wav> [param=value ...] [--block N] [--bits 16|24]\n"
        "  doomfx demo <out.wav> [--bpm N] [--swing P] [--machine mpc|sp1200] [--humanize TICKS]\n"
        "  doomfx groove [--swing P] [--machine mpc|sp1200] [--bpm N]\n"
        "  doomfx synth <synth> <out.wav> [--preset NAME] [--seq SEQ] [--bpm N] [--gate F] [param=value ...]\n"
        "      SEQ: steps separated by spaces, NOTES:BEATS, notes joined by '+', '-' for a rest,\n"
        "      e.g. \"D3+F3+A3+C4:4 G2+F3+B3:4 -:1\" (C4 = MIDI 60)\n");
}

static const char *opt(int argc, char **argv, const char *name, const char *def)
{
    for (int i = 0; i + 1 < argc; i++)
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return def;
}

static void print_param(const dfx_param *p, int index)
{
    const char *slot = index < 3 ? (index == 0 ? "CTRL 1" : index == 1 ? "CTRL 2" : "CTRL 3") : "SHIFT ";
    printf("    %s  %-8s %-8s ", slot, p->id, p->name);
    if (p->labels) {
        for (int j = 0; j < p->steps; j++)
            printf("%s%s", j ? " | " : "", p->labels[j]);
        printf("  (default %s)\n", p->labels[(int)p->def - (int)p->min]);
    } else {
        printf("%g..%g %s  (default %g)\n", p->min, p->max, p->unit, p->def);
    }
}

static int cmd_list(void)
{
    int n;
    const dfx_effect_def *const *fx = dfx_effects(&n);
    for (int i = 0; i < n; i++) {
        printf("%s  -  %s\n  %s\n", fx[i]->id, fx[i]->name, fx[i]->desc);
        for (int p = 0; p < fx[i]->num_params; p++) print_param(&fx[i]->params[p], p);
        printf("\n");
    }
    const dfx_synth_def *const *syn = dfx_synths(&n);
    for (int i = 0; i < n; i++) {
        printf("%s  -  %s (synth)\n  %s\n  presets:", syn[i]->id, syn[i]->name, syn[i]->desc);
        for (int p = 0; p < syn[i]->num_presets; p++) printf(" %s", syn[i]->presets[p].name);
        printf("\n");
        for (int p = 0; p < syn[i]->num_params; p++) print_param(&syn[i]->params[p], p);
        printf("\n");
    }
    return 0;
}

/* Parse "value" for a parameter: a number, or one of its labels. */
static int parse_value(const dfx_param *p, const char *text, float *out)
{
    if (p->labels)
        for (int j = 0; j < p->steps; j++)
            if (strcmp(text, p->labels[j]) == 0) { *out = p->min + j; return 0; }
    char *end;
    *out = strtof(text, &end);
    return (end == text || *end) ? -1 : 0;
}

static int cmd_render(int argc, char **argv)
{
    if (argc < 5) { usage(); return 2; }
    const dfx_effect_def *fx = dfx_find(argv[2]);
    if (!fx) { fprintf(stderr, "error: unknown effect '%s' (see: doomfx list)\n", argv[2]); return 2; }
    /* parse everything before touching any files */
    float values[DFX_MAX_PARAMS];
    int set[DFX_MAX_PARAMS] = { 0 };
    int block = 256, bits = 24;
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--block") && i + 1 < argc) { block = atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "--bits") && i + 1 < argc) { bits = atoi(argv[++i]); continue; }
        char key[64];
        const char *eq = strchr(argv[i], '=');
        if (!eq || eq - argv[i] >= (long)sizeof key) { fprintf(stderr, "error: expected param=value, got '%s'\n", argv[i]); return 2; }
        memcpy(key, argv[i], (size_t)(eq - argv[i]));
        key[eq - argv[i]] = 0;
        int idx = dfx_param_index(fx, key);
        if (idx < 0) { fprintf(stderr, "error: %s has no parameter '%s'\n", fx->id, key); return 2; }
        const dfx_param *p = &fx->params[idx];
        if (parse_value(p, eq + 1, &values[idx]) || values[idx] < p->min || values[idx] > p->max) {
            fprintf(stderr, "error: bad value '%s' for %s (", eq + 1, key);
            if (p->labels)
                for (int j = 0; j < p->steps; j++) fprintf(stderr, "%s%s", j ? " | " : "", p->labels[j]);
            else
                fprintf(stderr, "%g..%g %s", p->min, p->max, p->unit);
            fprintf(stderr, ")\n");
            return 2;
        }
        set[idx] = 1;
    }
    if (bits != 16 && bits != 24) { fprintf(stderr, "error: --bits must be 16 or 24\n"); return 2; }

    wav_audio audio;
    if (wav_read(argv[3], &audio)) return 1;
    void *state = calloc(1, fx->state_size);
    if (!state) { wav_free(&audio); return 1; }
    fx->init(state, (float)audio.sample_rate);
    for (int i = 0; i < fx->num_params; i++)
        if (set[i]) fx->set_param(state, i, values[i]);
    if (block < 1) block = 1;

    for (int pos = 0; pos < audio.frames; pos += block) {
        int n = audio.frames - pos < block ? audio.frames - pos : block;
        fx->process(state, audio.left + pos, audio.right + pos, n);
    }
    int ret = wav_write(argv[4], &audio, bits) ? 1 : 0;
    if (!ret) {
        printf("%s:", fx->name);
        for (int p = 0; p < fx->num_params; p++) printf(" %s=%g", fx->params[p].id, fx->get_param(state, p));
        printf("\nwrote %s (%d frames, %d Hz)\n", argv[4], audio.frames, audio.sample_rate);
    }
    free(state);
    wav_free(&audio);
    return ret;
}

static int parse_groove(int argc, char **argv, dfx_groove *g, float *bpm)
{
    const char *machine = opt(argc, argv, "--machine", "mpc");
    int sp = strcmp(machine, "sp1200") == 0;
    if (!sp && strcmp(machine, "mpc") != 0) { fprintf(stderr, "error: --machine must be mpc or sp1200\n"); return -1; }
    float swing = (float)atof(opt(argc, argv, "--swing", sp ? "63" : "62"));
    *g = sp ? dfx_groove_sp1200(swing) : dfx_groove_mpc(swing);
    *bpm = (float)atof(opt(argc, argv, "--bpm", "90"));
    if (*bpm <= 0) { fprintf(stderr, "error: bad --bpm\n"); return -1; }
    return 0;
}

static int cmd_groove(int argc, char **argv)
{
    dfx_groove g;
    float bpm;
    if (parse_groove(argc, argv, &g, &bpm)) return 2;
    double ms_per_tick = 60000.0 / bpm / g.ppqn;
    printf("%d PPQN, grid %d ticks, swing %g%%, %g BPM\n", g.ppqn, g.grid, g.swing, bpm);
    printf("step  straight  swung  late by\n");
    for (int step = 0; step < 8; step++) {
        int t = step * g.grid, s = t + dfx_swing_offset(g.grid, g.swing, step);
        printf("%4d  %8d  %5d  %5.1f ms\n", step + 1, t, s, (s - t) * ms_per_tick);
    }
    return 0;
}

/* --- demo beat ---------------------------------------------------------- */

enum { KICK, SNARE, HAT };

static void add_hit(wav_audio *a, int start, int voice, float vel, uint32_t *rng)
{
    float sr = (float)a->sample_rate;
    float dur = voice == KICK ? 0.45f : voice == SNARE ? 0.25f : 0.06f;
    int len = (int)(dur * sr);
    float phase = 0, hp = 0, prev = 0;
    for (int i = 0; i < len && start + i < a->frames; i++) {
        float t = i / sr, y;
        *rng ^= *rng << 13; *rng ^= *rng >> 17; *rng ^= *rng << 5;
        float noise = (float)(*rng >> 8) / 8388608.0f - 1.0f;
        if (voice == KICK) {
            float f = 45.0f + 75.0f * expf(-t * 30.0f);
            phase += 2.0f * (float)M_PI * f / sr;
            y = sinf(phase) * expf(-t * 7.0f) * 1.2f;
        } else if (voice == SNARE) {
            phase += 2.0f * (float)M_PI * 185.0f / sr;
            y = (0.5f * sinf(phase) * expf(-t * 25.0f) + 0.6f * noise * expf(-t * 14.0f));
        } else {
            hp = 0.6f * (hp + noise - prev); /* crude highpass */
            prev = noise;
            y = hp * expf(-t * 70.0f) * 0.5f;
        }
        y = tanhf(y) * vel;
        a->left[start + i] += y * (voice == HAT ? 0.8f : 1.0f);
        a->right[start + i] += y;
    }
}

static int cmd_demo(int argc, char **argv)
{
    if (argc < 3) { usage(); return 2; }
    dfx_groove g;
    float bpm;
    if (parse_groove(argc, argv, &g, &bpm)) return 2;
    int humanize = atoi(opt(argc, argv, "--humanize", "0"));

    /* a two bar boom bap pattern on a sixteenth grid */
    static const char *pattern[3] = {
        "x......x..x.....x.....x...x..x..", /* kick */
        "....x.......x.......x.......x..g", /* snare, g = ghost */
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", /* hat */
    };
    int sixteenth = g.ppqn / 4;
    dfx_note notes[96];
    int count = 0;
    for (int v = 0; v < 3; v++)
        for (int s = 0; s < 32; s++) {
            char c = pattern[v][s];
            if (c == '.') continue;
            int vel = c == 'g' ? 45 : (v == HAT ? (s % 2 ? 70 : 100) : 115);
            notes[count++] = (dfx_note){ s * sixteenth, sixteenth, (uint8_t)v, (uint8_t)vel };
        }
    /*
     * Swing on playback, the way a step sequencer does it: notes on every
     * second grid step move late. The SP-1200 swings eighths, so there the
     * sixteenths that fall between eighths stay where they are.
     */
    for (int i = 0; i < count; i++)
        if (notes[i].tick % g.grid == 0)
            notes[i].tick += dfx_swing_offset(g.grid, g.swing, notes[i].tick / g.grid);
    if (humanize > 0) dfx_humanize(notes, count, humanize, 10, 404);

    float sr = 48000.0f;
    double samples_per_tick = sr * 60.0 / bpm / g.ppqn;
    int bars_ticks = 2 * 4 * g.ppqn;
    wav_audio a;
    if (wav_alloc(&a, (int)sr, (int)(bars_ticks * samples_per_tick + sr * 0.5f))) return 1;
    uint32_t rng = 1;
    for (int i = 0; i < count; i++)
        add_hit(&a, (int)lround(notes[i].tick * samples_per_tick), notes[i].note, notes[i].velocity / 127.0f, &rng);
    for (int i = 0; i < a.frames; i++) { a.left[i] *= 0.6f; a.right[i] *= 0.6f; }
    int ret = wav_write(argv[2], &a, 24) ? 1 : 0;
    if (!ret) printf("wrote %s: 2 bars at %g BPM, %d PPQN, swing %g%%\n", argv[2], bpm, g.ppqn, g.swing);
    wav_free(&a);
    return ret;
}

/* --- synth -------------------------------------------------------------- */

/* "C3", "F#2", "Bb-1" -> MIDI note (C4 = 60), or -1 */
static int parse_note(const char *t, const char **end)
{
    static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 }; /* A..G */
    char c = *t;
    if (c >= 'a' && c <= 'g') c = (char)(c - 32);
    if (c < 'A' || c > 'G') return -1;
    int note = base[c - 'A'];
    t++;
    if (*t == '#') { note++; t++; } else if (*t == 'b') { note--; t++; }
    char *e;
    long oct = strtol(t, &e, 10);
    if (e == t) return -1;
    *end = e;
    note += (int)(oct + 1) * 12;
    return note >= 0 && note <= 127 ? note : -1;
}

typedef struct { int frame, note, on; } synth_event;

static int cmp_event(const void *a, const void *b)
{
    const synth_event *x = a, *y = b;
    if (x->frame != y->frame) return x->frame < y->frame ? -1 : 1;
    return x->on - y->on; /* offs before ons at the same frame */
}

static int cmd_synth(int argc, char **argv)
{
    if (argc < 4) { usage(); return 2; }
    const dfx_synth_def *syn = dfx_synth_find(argv[2]);
    if (!syn) { fprintf(stderr, "error: unknown synth '%s' (see: doomfx list)\n", argv[2]); return 2; }
    const char *preset = opt(argc, argv, "--preset", NULL);
    int is_bass = preset && strcmp(preset, "bass") == 0;
    const char *seq = opt(argc, argv, "--seq", is_bass
        ? "C2:0.75 C2:0.25 -:0.5 Eb2:0.5 F2:1 G2:0.5 Bb1:0.5 C2:0.75 C2:0.25 -:0.5 G1:0.5 Bb1:1 C2:1"
        : "D3+F3+A3+C4:4 G2+F3+B3+D4:4 C3+E3+G3+B3:4 A2+E3+G3+C4:4");
    float bpm = (float)atof(opt(argc, argv, "--bpm", "90"));
    float gate = (float)atof(opt(argc, argv, "--gate", "0.9"));
    if (bpm <= 0 || gate <= 0 || gate > 1) { fprintf(stderr, "error: bad --bpm or --gate\n"); return 2; }

    float sr = 48000.0f;
    void *state = calloc(1, syn->state_size);
    synth_event *ev = NULL;
    wav_audio a = { 0 };
    int ret = 2;
    if (!state) return 1;
    syn->init(state, sr);
    if (preset && dfx_synth_load_preset(syn, state, dfx_synth_preset_index(syn, preset))) {
        fprintf(stderr, "error: %s has no preset '%s'\n", syn->id, preset);
        goto done;
    }
    for (int i = 4; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == '-') { i++; continue; }
        const char *eq = strchr(argv[i], '=');
        char key[64];
        if (!eq || eq - argv[i] >= (long)sizeof key) { fprintf(stderr, "error: expected param=value, got '%s'\n", argv[i]); goto done; }
        memcpy(key, argv[i], (size_t)(eq - argv[i]));
        key[eq - argv[i]] = 0;
        int idx = dfx_synth_param_index(syn, key);
        float v;
        if (idx < 0) { fprintf(stderr, "error: %s has no parameter '%s'\n", syn->id, key); goto done; }
        const dfx_param *p = &syn->params[idx];
        if (parse_value(p, eq + 1, &v) || v < p->min || v > p->max) {
            fprintf(stderr, "error: bad value '%s' for %s\n", eq + 1, key);
            goto done;
        }
        syn->set_param(state, idx, v);
    }

    /* parse the sequence into note on / off events */
    int cap = 64, count = 0;
    ev = malloc(sizeof *ev * (size_t)cap);
    double beat_frames = sr * 60.0 / bpm, pos = 0;
    for (const char *t = seq; *t;) {
        while (*t == ' ') t++;
        if (!*t) break;
        int notes[16], nn = 0;
        if (*t == '-') {
            t++;
        } else {
            for (;;) {
                const char *e;
                int note = parse_note(t, &e);
                if (note < 0 || nn == 16) { fprintf(stderr, "error: bad note in --seq near '%s'\n", t); goto done; }
                notes[nn++] = note;
                t = e;
                if (*t != '+') break;
                t++;
            }
        }
        double beats = 1;
        if (*t == ':') {
            char *e;
            beats = strtod(t + 1, &e);
            if (e == t + 1 || beats <= 0) { fprintf(stderr, "error: bad length in --seq near '%s'\n", t); goto done; }
            t = e;
        }
        if (*t && *t != ' ') { fprintf(stderr, "error: unexpected '%c' in --seq\n", *t); goto done; }
        for (int i = 0; i < nn; i++) {
            if (count + 2 > cap) { cap *= 2; ev = realloc(ev, sizeof *ev * (size_t)cap); }
            ev[count++] = (synth_event){ (int)lround(pos), notes[i], 1 };
            ev[count++] = (synth_event){ (int)lround(pos + beats * beat_frames * gate), notes[i], 0 };
        }
        pos += beats * beat_frames;
    }
    qsort(ev, (size_t)count, sizeof *ev, cmp_event);

    /* render, splitting the audio at every event; then let the release ring out */
    float release_ms = syn->get_param(state, dfx_synth_param_index(syn, "release"));
    int total = (int)pos + (int)(sr * (release_ms > 0 ? release_ms * 0.0012f + 0.3f : 2.0f));
    if (wav_alloc(&a, (int)sr, total)) { ret = 1; goto done; }
    int frame = 0;
    for (int e = 0; e <= count; e++) {
        int until = e < count ? ev[e].frame : total;
        if (until > total) until = total;
        if (until > frame) {
            syn->render(state, a.left + frame, a.right + frame, until - frame);
            frame = until;
        }
        if (e < count) {
            if (ev[e].on) syn->note_on(state, ev[e].note, 100);
            else syn->note_off(state, ev[e].note);
        }
    }
    ret = wav_write(argv[3], &a, 24) ? 1 : 0;
    if (!ret) printf("%s%s%s: wrote %s (%.2f s)\n", syn->name, preset ? " / " : "", preset ? preset : "",
                     argv[3], total / sr);
done:
    free(ev);
    free(state);
    wav_free(&a);
    return ret;
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }
    if (!strcmp(argv[1], "list")) return cmd_list();
    if (!strcmp(argv[1], "render")) return cmd_render(argc, argv);
    if (!strcmp(argv[1], "demo")) return cmd_demo(argc, argv);
    if (!strcmp(argv[1], "groove")) return cmd_groove(argc, argv);
    if (!strcmp(argv[1], "synth")) return cmd_synth(argc, argv);
    usage();
    return 2;
}
