/*
 * mosura: runs MOSURA OS on a desktop from a script of button presses, and
 * writes the audio of the whole session plus screenshots of the screen and
 * pad LEDs. See the scripts in examples/ and docs/MOSURA.md for the script commands.
 */
#include "../include/mos.h"
#include "../src/os/mos_internal.h"
#include "../src/os/theme.h"
#include "kit.h"
#include "png.h"
#include "wav.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 48000
#define BLOCK 256

static mos *M;
static float *out_l, *out_r;
static int out_len, out_cap;
static int verbose = 1;

static void run(int frames)
{
    float l[BLOCK], r[BLOCK];
    while (frames > 0) {
        int n = frames > BLOCK ? BLOCK : frames;
        mos_process(M, l, r, n);
        if (out_len + n > out_cap) {
            out_cap = (out_cap + n) * 2;
            out_l = realloc(out_l, (size_t)out_cap * sizeof *out_l);
            out_r = realloc(out_r, (size_t)out_cap * sizeof *out_r);
            if (!out_l || !out_r) { fprintf(stderr, "out of memory\n"); exit(1); }
        }
        memcpy(out_l + out_len, l, (size_t)n * sizeof *l);
        memcpy(out_r + out_len, r, (size_t)n * sizeof *r);
        out_len += n;
        frames -= n;
    }
}

/* "A01" .. "J16" -> slot 0..159 */
static int parse_slot(const char *s)
{
    int bank = toupper((unsigned char)s[0]) - 'A';
    int pad = atoi(s + 1) - 1;
    if (bank < 0 || bank >= MOS_BANKS || pad < 0 || pad > 15) return -1;
    return bank * 16 + pad;
}

/* "500ms", "2s", "1bar", "0.5beat", "24t" (ticks), or plain frames */
static int parse_time(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    double beat = SR * 60.0 / mos_bpm(M);
    if (!strcmp(end, "ms")) return (int)(v * SR / 1000.0);
    if (!strcmp(end, "s")) return (int)(v * SR);
    if (!strcmp(end, "bar") || !strcmp(end, "bars")) return (int)(v * beat * M->seqs[M->seq].beats);
    if (!strcmp(end, "beat") || !strcmp(end, "beats")) return (int)(v * beat);
    if (!strcmp(end, "t")) return (int)(v * beat / MOS_PPQN);
    return (int)v;
}

/* Run until the playhead reaches BAR.BEAT.TICK (1-based bar and beat). */
static int run_until(const char *pos)
{
    int bar = 1, beat = 1, tick = 0;
    if (sscanf(pos, "%d.%d.%d", &bar, &beat, &tick) < 2) return -1;
    int target = ((bar - 1) * M->seqs[M->seq].beats + (beat - 1)) * MOS_PPQN + tick;
    if (!M->playing) return -1;
    if (target >= mos_seq_len(M, M->seq)) return -1;
    int guard = SR * 120;
    while (!(M->playing && M->countin_left == 0 && M->tick == target) && guard-- > 0) run(1);
    return guard > 0 ? 0 : -1;
}

static void print_screen(const uint8_t *fb)
{
    printf("+");
    for (int x = 0; x < MOS_LCD_W; x++) printf("-");
    printf("+\n");
    for (int y = 0; y < MOS_LCD_H; y += 2) {
        printf("|");
        for (int x = 0; x < MOS_LCD_W; x++) {
            int a = fb[y * MOS_LCD_W + x], b = fb[(y + 1) * MOS_LCD_W + x];
            printf("%s", a && b ? "█" : a ? "▀" : b ? "▄" : " ");
        }
        printf("|\n");
    }
    printf("+");
    for (int x = 0; x < MOS_LCD_W; x++) printf("-");
    printf("+\n");
}

static void put(uint8_t *img, int w, int x, int y, uint32_t c)
{
    uint8_t *p = img + ((size_t)y * w + x) * 3;
    p[0] = (uint8_t)(c >> 16); p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)c;
}

static void rect(uint8_t *img, int w, int x, int y, int rw, int rh, uint32_t c)
{
    for (int j = 0; j < rh; j++)
        for (int i = 0; i < rw; i++) put(img, w, x + i, y + j, c);
}

/* The screen at 4x with a pixel grid, and the 16 pad LEDs beside it. */
static int save_panel(const char *path)
{
    const uint8_t *fb = mos_screen(M);
    const int S = 4, pad = 56, gap = 8, margin = 16;
    int sw = MOS_LCD_W * S, sh = MOS_LCD_H * S;
    int w = margin + sw + margin * 2 + 4 * pad + 3 * gap + margin;
    int h = margin + (sh > 4 * pad + 3 * gap ? sh : 4 * pad + 3 * gap) + margin;
    uint8_t *img = malloc((size_t)w * h * 3);
    if (!img) return -1;
    rect(img, w, 0, 0, w, h, 0x15161A);
    rect(img, w, margin - 4, margin - 4, sw + 8, sh + 8, 0x000000);
    for (int y = 0; y < MOS_LCD_H; y++)
        for (int x = 0; x < MOS_LCD_W; x++) {
            uint32_t c = fb[y * MOS_LCD_W + x] ? MOS_THEME_SCREEN_ON : MOS_THEME_SCREEN_OFF;
            rect(img, w, margin + x * S, margin + y * S, S, S, c);
        }
    int px = margin + sw + margin * 2;
    for (int i = 0; i < 16; i++) {
        int x = px + (i % 4) * (pad + gap), y = margin + (i / 4) * (pad + gap);
        uint32_t c = mos_pad_color(M, i);
        rect(img, w, x, y, pad, pad, 0x2A2B30);
        rect(img, w, x + 3, y + 3, pad - 6, pad - 6, c ? c : 0x1C1D21);
    }
    int ret = png_write_rgb(path, img, w, h);
    free(img);
    return ret;
}

static void dump_events(void)
{
    const mos_seq *s = &M->seqs[M->seq];
    printf("sequence %d: %d bars, %.1f BPM, %d events\n", M->seq + 1, s->bars, s->tempo, s->count);
    for (int i = 0; i < s->count; i++) {
        const mos_event *e = &s->ev[i];
        int bar_len = s->beats * MOS_PPQN;
        printf("  %03d.%02d.%02d  track %2d  note %3d  vel %3d  len %d\n", e->tick / bar_len + 1,
               e->tick % bar_len / MOS_PPQN + 1, e->tick % MOS_PPQN, e->track + 1, e->note, e->vel, e->dur);
    }
}

static float *kit_mem[KIT_SOUNDS];

static int command(char **tok, int n, int line)
{
    const char *c = tok[0];
#define NEED(k) do { if (n < (k)) { fprintf(stderr, "line %d: %s needs %d argument(s)\n", line, c, (k) - 1); return -1; } } while (0)
    if (!strcmp(c, "kit")) {
        /* the synthesised kit on pads A01-A08 */
        for (int i = 0; i < KIT_SOUNDS; i++) {
            if (!kit_mem[i]) {
                int len = kit_render(i, SR, &kit_mem[i]);
                mos_set_sample(M, i, kit_mem[i], kit_mem[i], len, SR);
            }
        }
    } else if (!strcmp(c, "load")) {
        NEED(3);
        int slot = parse_slot(tok[1]);
        if (slot < 0) { fprintf(stderr, "line %d: bad pad %s (use A01..J16)\n", line, tok[1]); return -1; }
        wav_audio *a = malloc(sizeof *a);
        if (!a || wav_read(tok[2], a)) return -1;
        mos_set_sample(M, slot, a->left, a->right, a->frames, (float)a->sample_rate);
        /* kept for the whole run on purpose: the OS plays straight from this memory */
    } else if (!strcmp(c, "key") || !strcmp(c, "down") || !strcmp(c, "up")) {
        NEED(2);
        int k = mos_key_from_name(tok[1]);
        if (k < 0) { fprintf(stderr, "line %d: unknown key %s\n", line, tok[1]); return -1; }
        if (strcmp(c, "up")) mos_key(M, (mos_key_id)k, 1);
        if (strcmp(c, "down")) mos_key(M, (mos_key_id)k, 0);
    } else if (!strcmp(c, "push")) {
        mos_key(M, MOS_KEY_ENTER, 1);
        mos_key(M, MOS_KEY_ENTER, 0);
    } else if (!strcmp(c, "pad") || !strcmp(c, "padon")) {
        NEED(2);
        int p = atoi(tok[1]) - 1, v = n > 2 ? atoi(tok[2]) : 100;
        mos_pad(M, p, v);
        if (!strcmp(c, "pad")) {
            run(n > 3 ? parse_time(tok[3]) : SR / 50);
            mos_pad(M, p, 0);
        }
    } else if (!strcmp(c, "padoff")) {
        NEED(2);
        mos_pad(M, atoi(tok[1]) - 1, 0);
    } else if (!strcmp(c, "turn")) {
        NEED(2);
        mos_turn(M, atoi(tok[1]));
    } else if (!strcmp(c, "knob")) {
        NEED(3);
        mos_knob(M, atoi(tok[1]) - 1, (float)atof(tok[2]));
    } else if (!strcmp(c, "wait")) {
        NEED(2);
        run(parse_time(tok[1]));
    } else if (!strcmp(c, "at")) {
        NEED(2);
        if (run_until(tok[1])) { fprintf(stderr, "line %d: can't reach %s (playing? inside the sequence?)\n", line, tok[1]); return -1; }
    } else if (!strcmp(c, "screen")) {
        if (n > 1) {
            if (save_panel(tok[1])) { fprintf(stderr, "line %d: can't write %s\n", line, tok[1]); return -1; }
            if (verbose) printf("wrote %s\n", tok[1]);
        } else {
            print_screen(mos_screen(M));
        }
    } else if (!strcmp(c, "events")) {
        dump_events();
    } else if (!strcmp(c, "say")) {
        for (int i = 1; i < n; i++) printf("%s%s", tok[i], i + 1 < n ? " " : "\n");
    } else {
        fprintf(stderr, "line %d: unknown command %s\n", line, c);
        return -1;
    }
    return 0;
}

static int run_script(FILE *f)
{
    char buf[512];
    int line = 0;
    while (fgets(buf, sizeof buf, f)) {
        line++;
        char *hash = strchr(buf, '#');
        if (hash) *hash = 0;
        char *tok[16];
        int n = 0;
        for (char *t = strtok(buf, " \t\r\n"); t && n < 16; t = strtok(NULL, " \t\r\n")) tok[n++] = t;
        if (n && command(tok, n, line)) return -1;
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: mosura run SCRIPT.mos [--out session.wav] [--quiet]\n"
            "       mosura keys                 list key names\n"
            "\n"
            "Runs " MOS_THEME_NAME " from a script of button presses. Script commands:\n"
            "  kit                       synthesised drums on pads A01-A08\n"
            "  load A01 file.wav         a WAV on a sample pad (A01..J16)\n"
            "  key NAME | down NAME | up NAME   press, hold, release a key (see `mosura keys`)\n"
            "  push                      VALUE push\n"
            "  turn N                    VALUE turn by N detents\n"
            "  knob 1..3 POS             CTRL knob to POS (0..1)\n"
            "  pad N [VEL] [HOLD]        hit pad N (1..16); padon N VEL / padoff N to hold\n"
            "  wait TIME                 500ms, 2s, 1bar, 2beats, 24t\n"
            "  at BAR.BEAT.TICK          run until the playhead gets there\n"
            "  screen [FILE.png]         print the screen, or save screen + pad LEDs\n"
            "  events                    list the sequence's events\n");
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "keys")) {
        for (int i = 0; i < MOS_KEY_COUNT; i++) printf("%s\n", mos_key_name((mos_key_id)i));
        return 0;
    }
    if (argc < 3 || strcmp(argv[1], "run")) { usage(); return 2; }
    const char *out = NULL;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--quiet")) verbose = 0;
    }
    FILE *f = strcmp(argv[2], "-") ? fopen(argv[2], "r") : stdin;
    if (!f) { perror(argv[2]); return 1; }
    M = calloc(1, mos_size());
    if (!M) { fprintf(stderr, "out of memory\n"); return 1; }
    mos_init(M, SR);
    int ret = run_script(f) ? 1 : 0;
    if (f != stdin) fclose(f);
    if (!ret && out) {
        wav_audio a = { SR, out_len, out_l, out_r };
        ret = wav_write(out, &a, 24) ? 1 : 0;
        if (!ret && verbose) printf("wrote %s: %.1f s\n", out, (double)out_len / SR);
    }
    return ret;
}
