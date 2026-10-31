/*
 * MOSURA OS user interface: screens, input and LEDs.
 *
 * Every screen has the same three zones:
 *
 *   +--------------------------------+
 *   | TITLE                ▶ 001.01.00 |  header: where you are, transport
 *   | fields...                      |  VALUE turns between fields,
 *   |                                |  VALUE push edits one
 *   | [ CTRL 1 ] [ CTRL 2 ] [ CTRL 3 ] |  knob row: what the knobs do here
 *   +--------------------------------+
 *
 * A knob shows its value in the knob row for a moment after it moves.
 */
#include "mos_internal.h"
#include "gfx.h"
#include "theme.h"
#include "../dsp.h"

#include <stdio.h>
#include <string.h>

#define ROW(r) (10 + 7 * (r))

static const struct { int ticks; const char *name; } tc_table[] = {
    { 48, "1/8" }, { 32, "1/8T" }, { 24, "1/16" }, { 16, "1/16T" }, { 12, "1/32" }, { 8, "1/32T" }, { 1, "OFF" },
};
#define TC_COUNT 7

static const char *const click_names[] = { "OFF", "REC", "ON" };
static const char *const pads_names[] = { "MUTE", "SOLO", "SELECT" };
static const char *const fx_page_names[] = { "INSERT", "SEND A", "SEND B", "MASTER 1", "MASTER 2" };
static const int screen_pages[MOS_SCR_COUNT] = { 1, 1, 3, 5, 2, 1 };

static const dfx_synth_def *juno(void) { return dfx_synth_find("juno106"); }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int blink(const mos *m) { return (int)((m->samples / (int64_t)(m->sr * 0.25f)) & 1); }

void mos_ui_init(mos *m)
{
    m->screen = MOS_SCR_MAIN;
    m->cursor[MOS_SCR_FX] = 1; /* the effect picker */
    m->song[0] = (mos_song_step){ 0, 1 };
    m->song_len = 0;
}

/* --- formatting ------------------------------------------------------------- */

static void fmt_pos(const mos *m, int tick, char *out, size_t n)
{
    int bar_len = m->seqs[m->seq].beats * MOS_PPQN;
    if (bar_len <= 0) bar_len = 4 * MOS_PPQN;
    unsigned bar = (unsigned)(tick / bar_len) % 999u + 1u, beat = (unsigned)(tick % bar_len / MOS_PPQN) % 99u + 1u;
    snprintf(out, n, "%03u.%02u.%02u", bar, beat, (unsigned)(tick % MOS_PPQN) % 96u);
}

static void fmt_note(const mos *m, int track, int note, char *out, size_t n)
{
    static const char *const names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (m->tracks[track].type == MOS_TRACK_SYNTH)
        snprintf(out, n, "%s%d", names[note % 12], note / 12 - 1);
    else
        snprintf(out, n, "%c%02d", 'A' + note / 16, note % 16 + 1);
}

static void fmt_param(const dfx_param *p, float v, char *out, size_t n)
{
    if (p->labels) {
        int i = clampi((int)(v - p->min + 0.5f), 0, p->steps - 1);
        snprintf(out, n, "%s", p->labels[i]);
    } else if (!strcmp(p->unit, "Hz")) {
        if (v >= 1000) snprintf(out, n, "%.1fK", v / 1000);
        else snprintf(out, n, "%.0fHZ", v);
    } else if (!strcmp(p->unit, "dB")) {
        snprintf(out, n, "%+.1fDB", v);
    } else if (!strcmp(p->unit, "ms")) {
        snprintf(out, n, v < 10 ? "%.1fMS" : "%.0fMS", v);
    } else if (!strcmp(p->unit, "%")) {
        snprintf(out, n, "%.0f%%", v);
    } else if (!strcmp(p->unit, "st")) {
        snprintf(out, n, "%+.0fST", v);
    } else if (p->steps || p->max - p->min > 10) {
        snprintf(out, n, "%.0f", v);
    } else {
        snprintf(out, n, "%.2f", v);
    }
}

static void fmt_pan(float pan, char *out, size_t n)
{
    int p = (int)lrintf(pan * 50);
    if (p == 0) snprintf(out, n, "C");
    else snprintf(out, n, "%c%d", p < 0 ? 'L' : 'R', p < 0 ? -p : p);
}

static const char *track_name(const mos *m, int t) { return mos_theme_track_names[m->tracks[t].name]; }

/* --- drawing helpers ---------------------------------------------------------- */

static void field(mos *m, int id, int x, int y, const char *text)
{
    int w = g_text_width(text, 1);
    g_text(m->fb, x, y, text, 1, 1);
    if (id == m->cursor[m->screen]) {
        if (m->editing) g_invert(m->fb, x - 1, y - 1, w + 2, 7);
        else g_fill(m->fb, x, y + 5, w, 1, 1);
    }
    if (id + 1 > m->nfields) m->nfields = id + 1;
}

static void label(mos *m, int x, int y, const char *text) { g_text(m->fb, x, y, text, 1, 1); }

static void header(mos *m, const char *title)
{
    g_fill(m->fb, 0, 0, MOS_LCD_W, 7, 1);
    g_text(m->fb, 1, 1, title, 0, 1);
    char right[40], pos[24];
    if (m->playing && m->countin_left > 0) {
        snprintf(right, sizeof right, G_REC " COUNT %d", (m->countin_left + MOS_PPQN - 1) / MOS_PPQN);
    } else {
        fmt_pos(m, m->tick, pos, sizeof pos);
        const char *g = G_STOP;
        if (m->playing) g = m->rec ? G_REC : G_PLAY;
        else if (m->rec_armed) g = blink(m) ? G_REC : " ";
        snprintf(right, sizeof right, "%s %s", g, pos);
    }
    g_text(m->fb, MOS_LCD_W - g_text_width(right, 1) - 1, 1, right, 0, 1);
}

static void knob_row(mos *m, const char *const labels[3], char values[3][16])
{
    for (int k = 0; k < 3; k++) {
        int x = k * 43, w = 42;
        const char *t = labels[k];
        int touched = values[k][0] && m->samples < m->knob_touch[k];
        if (touched) t = values[k];
        if (touched) g_fill(m->fb, x, 57, w, 7, 1);
        else g_frame(m->fb, x, 57, w, 7, 1);
        g_text(m->fb, x + (w - g_text_width(t, 1)) / 2, 58, t, touched ? 0 : 1, 1);
    }
}

/* --- MAIN ------------------------------------------------------------------- */

static int tc_index(int ticks)
{
    for (int i = 0; i < TC_COUNT; i++)
        if (tc_table[i].ticks == ticks) return i;
    return 2;
}

static void draw_main(mos *m)
{
    char b[40];
    const mos_seq *s = &m->seqs[m->seq];
    const mos_track *t = &m->tracks[m->active_track];
    header(m, MOS_THEME_NAME);

    label(m, 0, ROW(0), "SEQ");
    snprintf(b, sizeof b, "%02d", m->seq + 1);
    field(m, 0, 16, ROW(0), b);
    if (m->next_seq >= 0) {
        snprintf(b, sizeof b, G_ARROW "NEXT %02d", m->next_seq + 1);
        label(m, 28, ROW(0), b);
    }
    snprintf(b, sizeof b, "EVENTS %d", s->count);
    label(m, MOS_LCD_W - g_text_width(b, 1), ROW(0), b);

    label(m, 0, ROW(1), "TRK");
    snprintf(b, sizeof b, "%02d", m->active_track + 1);
    field(m, 1, 16, ROW(1), b);
    snprintf(b, sizeof b, "%s %s%s", track_name(m, m->active_track), t->type == MOS_TRACK_SYNTH ? "SYNTH" : "DRUM",
             t->mute ? " MUTED" : "");
    label(m, 28, ROW(1), b);

    label(m, 0, ROW(2), "BPM");
    snprintf(b, sizeof b, "%05.1f", s->tempo);
    field(m, 2, 16, ROW(2), b);
    label(m, 48, ROW(2), "BARS");
    snprintf(b, sizeof b, "%02d", s->bars);
    field(m, 3, 68, ROW(2), b);
    label(m, 84, ROW(2), "LOOP");
    field(m, 4, 104, ROW(2), s->loop ? "ON" : "OFF");

    label(m, 0, ROW(3), "TC");
    field(m, 5, 16, ROW(3), tc_table[tc_index(m->tc)].name);
    label(m, 48, ROW(3), "SWING");
    snprintf(b, sizeof b, "%.0f%%", m->swing);
    field(m, 6, 72, ROW(3), b);

    label(m, 0, ROW(4), "COUNT");
    field(m, 7, 24, ROW(4), m->countin ? "ON" : "OFF");
    label(m, 48, ROW(4), "CLICK");
    field(m, 8, 72, ROW(4), click_names[m->click]);

    const char *st = "STOPPED";
    if (m->playing && m->countin_left > 0) st = "COUNT IN...";
    else if (m->playing) st = m->rec == MOS_REC_REPLACE ? "RECORDING" : m->rec == MOS_REC_OVERDUB ? "OVERDUB" : "PLAYING";
    else if (m->rec_armed) st = m->rec_armed == MOS_REC_REPLACE ? "REC READY: PRESS PLAY" : "DUB READY: PRESS PLAY";
    snprintf(b, sizeof b, "%s%s", st, m->full_level ? "  FULL LV" : "");
    label(m, 0, ROW(5), b);

    static const char *const kl[3] = { "BPM", "SWING", "TRK VOL" };
    char kv[3][16];
    snprintf(kv[0], 16, "%.1f", s->tempo);
    snprintf(kv[1], 16, "%.0f%%", m->swing);
    snprintf(kv[2], 16, "%.0f", t->vol * 100);
    knob_row(m, kl, kv);
}

static void select_seq(mos *m, int seq)
{
    if (m->seqs[seq].bars == 0) {
        m->seqs[seq].bars = 2;
        m->seqs[seq].beats = 4;
        m->seqs[seq].tempo = mos_bpm(m);
        m->seqs[seq].loop = 1;
        m->seqs[seq].count = 0;
    }
    m->seq = seq;
    m->tick = 0;
    m->ev_idx = 0;
    m->step_tick = 0;
    m->step_sel = 0;
}

static void truncate_seq(mos *m)
{
    int len = mos_seq_len(m, m->seq);
    mos_seq *s = &m->seqs[m->seq];
    while (s->count > 0 && s->ev[s->count - 1].tick >= len) mos_delete_event(m, m->seq, s->count - 1);
}

static void edit_main(mos *m, int id, int d)
{
    mos_seq *s = &m->seqs[m->seq];
    switch (id) {
    case 0: {
        int cur = m->next_seq >= 0 ? m->next_seq : m->seq;
        int t = clampi(cur + d, 0, MOS_SEQS - 1);
        if (m->playing) {
            m->next_seq = t == m->seq ? -1 : t;
        } else {
            select_seq(m, t);
        }
        break;
    }
    case 1: m->active_track = clampi(m->active_track + d, 0, MOS_TRACKS - 1); break;
    case 2: s->tempo = dfx_clampf(s->tempo + d * (m->shift ? 0.1f : 1.0f), 30, 300); break;
    case 3: {
        int bars = clampi(s->bars + d, 1, 99);
        if (bars < s->bars) mos_snapshot(m);
        s->bars = bars;
        truncate_seq(m);
        break;
    }
    case 4: s->loop = !s->loop; break;
    case 5: m->tc = tc_table[clampi(tc_index(m->tc) + d, 0, TC_COUNT - 1)].ticks; break;
    case 6: m->swing = dfx_clampf(m->swing + d, 50, 75); break;
    case 7: m->countin = !m->countin; break;
    case 8: m->click = clampi(m->click + d, 0, 2); break;
    }
}

/* --- STEP ------------------------------------------------------------------- */

static int step_grid(const mos *m) { return m->tc > 1 ? m->tc : 1; }

/* Indexes of the active track's events at the step cursor. */
static int step_events(const mos *m, int *idx, int max)
{
    const mos_seq *s = &m->seqs[m->seq];
    int n = 0;
    for (int i = 0; i < s->count && n < max; i++)
        if (s->ev[i].tick == m->step_tick && s->ev[i].track == m->active_track) idx[n++] = i;
    return n;
}

static void draw_step(mos *m)
{
    char b[40];
    const mos_seq *s = &m->seqs[m->seq];
    snprintf(b, sizeof b, "STEP TR%02d %s", m->active_track + 1, track_name(m, m->active_track));
    header(m, b);

    /* this bar of the active track, as 16 cells */
    int bar_len = s->beats * MOS_PPQN, cell = bar_len / 16;
    int bar_start = m->step_tick / bar_len * bar_len;
    for (int c = 0; c < 16; c++) {
        int from = bar_start + c * cell, hit = 0;
        for (int i = 0; i < s->count && !hit; i++)
            if (s->ev[i].track == m->active_track && s->ev[i].tick >= from && s->ev[i].tick < from + cell) hit = 1;
        int x = c * 8;
        if (hit) g_fill(m->fb, x, 9, 7, 6, 1);
        else g_frame(m->fb, x, 9, 7, 6, 1);
        if (c % 4 == 0) g_pixel(m->fb, x, 8, 1);
        if (m->playing && m->tick >= from && m->tick < from + cell) g_invert(m->fb, x + 2, 11, 3, 2);
        if (m->step_tick >= from && m->step_tick < from + cell) g_fill(m->fb, x, 16, 7, 2, 1);
    }

    char pos[24];
    fmt_pos(m, m->step_tick, pos, sizeof pos);
    snprintf(b, sizeof b, "AT %s  STEP %s", pos, tc_table[tc_index(m->tc)].name);
    label(m, 0, 20, b);

    int idx[8];
    int n = step_events(m, idx, 8);
    if (m->step_sel >= n) m->step_sel = n ? n - 1 : 0;
    if (n == 0) label(m, 0, 28, "NO EVENTS. HIT A PAD TO ADD ONE");
    int first = m->step_sel > 3 ? m->step_sel - 3 : 0;
    for (int k = first; k < n && k < first + 4; k++) {
        const mos_event *e = &s->ev[idx[k]];
        char note[8];
        fmt_note(m, e->track, e->note, note, sizeof note);
        snprintf(b, sizeof b, "%s %-4s VEL %3d  LEN %3d", k == m->step_sel ? G_ARROW : " ", note, e->vel, e->dur);
        label(m, 0, 28 + (k - first) * 7, b);
        if (k == m->step_sel) g_invert(m->fb, 4, 27 + (k - first) * 7, 124, 7);
    }

    static const char *const kl[3] = { "VELO", "LENGTH", "NOTE" };
    char kv[3][16] = { "", "", "" };
    if (n) {
        const mos_event *e = &s->ev[idx[m->step_sel]];
        snprintf(kv[0], 16, "VEL %d", e->vel);
        snprintf(kv[1], 16, "LEN %d", e->dur);
        fmt_note(m, e->track, e->note, kv[2], 16);
    }
    knob_row(m, kl, kv);
}

/*
 * The step cursor walks the swung grid, the same grid recording snaps to,
 * so a swung note recorded live shows up on its step. SHIFT jumps a bar
 * and keeps the position in the bar.
 */
static void step_move(mos *m, int d)
{
    int len = mos_seq_len(m, m->seq);
    int g = step_grid(m);
    int steps = len / g;
    if (steps < 1) steps = 1;
    int k = (m->step_tick + g / 2) / g; /* nearest grid step; swing never moves a step by half a step */
    if (m->shift) d *= m->seqs[m->seq].beats * MOS_PPQN / g;
    k = ((k + d) % steps + steps) % steps;
    m->step_tick = k * g + (g > 1 ? dfx_swing_offset(g, m->swing, k) : 0);
    m->step_sel = 0;
}

static void step_add(mos *m, int pad, int vel)
{
    int track = m->active_track;
    int note = mos_note_for_pad(m, track, pad);
    mos_snapshot(m);
    int dur = step_grid(m) > 1 ? step_grid(m) : MOS_PPQN / 4;
    int i = mos_insert_event(m, m->seq, (mos_event){ m->step_tick, (uint16_t)dur, (uint8_t)track, (uint8_t)note, (uint8_t)vel, 0 });
    if (i < 0) { mos_message(m, "SEQUENCE FULL"); return; }
    int idx[8], n = step_events(m, idx, 8);
    for (int k = 0; k < n; k++) if (idx[k] == i) m->step_sel = k;
}

static void step_delete(mos *m)
{
    int idx[8], n = step_events(m, idx, 8);
    if (!n) return;
    mos_snapshot(m);
    mos_delete_event(m, m->seq, idx[m->step_sel]);
    if (m->step_sel > 0) m->step_sel--;
}

/* --- TRACK ------------------------------------------------------------------ */

static void draw_track(mos *m)
{
    char b[40];
    const mos_track *t = &m->tracks[m->active_track];
    snprintf(b, sizeof b, "TRACK %02d", m->active_track + 1);
    header(m, b);

    label(m, 0, ROW(0), "TRACK");
    snprintf(b, sizeof b, "%02d", m->active_track + 1);
    field(m, 0, 28, ROW(0), b);
    label(m, 0, ROW(1), "NAME");
    field(m, 1, 28, ROW(1), track_name(m, m->active_track));
    label(m, 0, ROW(2), "TYPE");
    field(m, 2, 28, ROW(2), t->type == MOS_TRACK_SYNTH ? "SYNTH" : "DRUM");
    label(m, 0, ROW(3), "PADS");
    field(m, 3, 28, ROW(3), pads_names[m->pads_mode]);
    if (t->type == MOS_TRACK_SYNTH) {
        label(m, 0, ROW(4), "PATCH");
        field(m, 4, 28, ROW(4), juno()->presets[t->preset].name);
        label(m, 0, ROW(5), "OCT");
        snprintf(b, sizeof b, "%d", t->octave);
        field(m, 5, 28, ROW(5), b);
    } else {
        label(m, 0, ROW(4), "PLAY");
        field(m, 4, 28, ROW(4), t->gate ? "GATE" : "ONESHOT");
    }

    /* the 16 tracks, as the pads see them */
    int any_solo = 0;
    for (int i = 0; i < MOS_TRACKS; i++) any_solo |= m->tracks[i].solo;
    for (int i = 0; i < MOS_TRACKS; i++) {
        int x = 84 + (i % 4) * 11, y = 9 + (i / 4) * 11;
        const mos_track *ti = &m->tracks[i];
        int on = any_solo ? ti->solo : !ti->mute;
        snprintf(b, sizeof b, "%d", i + 1);
        if (on) {
            g_fill(m->fb, x, y, 10, 10, 1);
            g_text(m->fb, x + (11 - g_text_width(b, 1)) / 2, y + 3, b, 0, 1);
        } else {
            g_frame(m->fb, x, y, 10, 10, 1);
            g_text(m->fb, x + (11 - g_text_width(b, 1)) / 2, y + 3, b, 1, 1);
        }
        if (ti->solo) g_invert(m->fb, x + 7, y + 1, 2, 2);
        if (i == m->active_track) g_frame(m->fb, x - 1, y - 1, 12, 12, 1);
    }

    static const char *const kl_d[3] = { "VOLUME", "PAN", "SEND A" };
    static const char *const kl_s[3] = { "VOLUME", "PAN", "OCTAVE" };
    char kv[3][16];
    snprintf(kv[0], 16, "VOL %.0f", t->vol * 100);
    fmt_pan(t->pan, kv[1], 16);
    if (t->type == MOS_TRACK_SYNTH) snprintf(kv[2], 16, "OCT %d", t->octave);
    else snprintf(kv[2], 16, "A %.0f", t->send[0] * 100);
    knob_row(m, t->type == MOS_TRACK_SYNTH ? kl_s : kl_d, kv);
}

static void edit_track(mos *m, int id, int d)
{
    mos_track *t = &m->tracks[m->active_track];
    switch (id) {
    case 0: m->active_track = clampi(m->active_track + d, 0, MOS_TRACKS - 1); break;
    case 1: t->name = ((t->name + d) % MOS_THEME_TRACK_NAME_COUNT + MOS_THEME_TRACK_NAME_COUNT) % MOS_THEME_TRACK_NAME_COUNT; break;
    case 2: mos_set_track_type(m, m->active_track, t->type == MOS_TRACK_SYNTH ? MOS_TRACK_DRUM : MOS_TRACK_SYNTH); break;
    case 3: m->pads_mode = ((m->pads_mode + d) % 3 + 3) % 3; break;
    case 4:
        if (t->type == MOS_TRACK_SYNTH) mos_load_synth_preset(m, m->active_track, t->preset + d);
        else t->gate = !t->gate;
        break;
    case 5: t->octave = clampi(t->octave + d, 0, 8); break;
    }
}

/* --- FX --------------------------------------------------------------------- */

static int fx_unit(const mos *m)
{
    switch (m->page[MOS_SCR_FX]) {
    case 0: return m->tracks[m->active_track].unit;
    case 1: return MOS_UNIT_SEND_A;
    case 2: return MOS_UNIT_SEND_B;
    case 3: return MOS_UNIT_MASTER;
    default: return MOS_UNIT_MASTER + 1;
    }
}

static const dfx_effect_def *fx_of(const mos *m)
{
    int u = fx_unit(m);
    return u >= 0 ? m->units[u].fx : NULL;
}

static void draw_fx(mos *m)
{
    char b[40];
    int page = m->page[MOS_SCR_FX];
    int base = page == 0 ? 1 : 0;
    if (page == 0) snprintf(b, sizeof b, "FX INSERT TR%02d %s", m->active_track + 1, track_name(m, m->active_track));
    else snprintf(b, sizeof b, "FX %s", fx_page_names[page]);
    header(m, b);

    const dfx_effect_def *fx = fx_of(m);
    int x = 0;
    if (page == 0) {
        label(m, 0, ROW(0), "TRK");
        snprintf(b, sizeof b, "%02d", m->active_track + 1);
        field(m, 0, 16, ROW(0), b);
        x = 32;
    }
    label(m, x, ROW(0), "FX");
    field(m, base, x + 12, ROW(0), fx ? fx->name : "OFF");

    int np = fx ? fx->num_params : 0;
    for (int p = 0; p < np; p++) {
        const dfx_param *pp = &fx->params[p];
        int cx = (p % 2) * 64, y = ROW(1 + p / 2);
        char name[8], val[16];
        snprintf(name, sizeof name, "%s", pp->name);
        label(m, cx, y, name);
        fmt_param(pp, fx->get_param(m->units[fx_unit(m)].mem, p), val, sizeof val);
        field(m, base + 1 + p, cx + 32, y, val);
    }
    if (page == 0) {
        const mos_track *t = &m->tracks[m->active_track];
        label(m, 0, ROW(5), "SEND A");
        snprintf(b, sizeof b, "%.0f%%", t->send[0] * 100);
        field(m, base + 1 + np, 28, ROW(5), b);
        label(m, 64, ROW(5), "SEND B");
        snprintf(b, sizeof b, "%.0f%%", t->send[1] * 100);
        field(m, base + 2 + np, 92, ROW(5), b);
    } else if (!fx) {
        label(m, 0, ROW(2), "PUSH VALUE, TURN TO PICK AN EFFECT");
    }

    const char *kl[3] = { "-", "-", "-" };
    char kv[3][16] = { "", "", "" };
    for (int k = 0; k < 3 && k < np; k++) {
        kl[k] = fx->params[k].name;
        fmt_param(&fx->params[k], fx->get_param(m->units[fx_unit(m)].mem, k), kv[k], 16);
    }
    knob_row(m, kl, kv);
}

static void set_param_knob(mos *m, int p, float knob)
{
    const dfx_effect_def *fx = fx_of(m);
    if (!fx || p >= fx->num_params) return;
    fx->set_param(m->units[fx_unit(m)].mem, p, dfx_param_from_knob(&fx->params[p], knob));
}

static void edit_fx(mos *m, int id, int d)
{
    int page = m->page[MOS_SCR_FX];
    int base = page == 0 ? 1 : 0;
    const dfx_effect_def *fx = fx_of(m);
    int np = fx ? fx->num_params : 0;
    if (page == 0 && id == 0) {
        m->active_track = clampi(m->active_track + d, 0, MOS_TRACKS - 1);
        return;
    }
    if (id == base) {
        int count, cur = -1;
        const dfx_effect_def *const *all = dfx_effects(&count);
        for (int i = 0; i < count; i++) if (all[i] == fx) cur = i;
        int next = clampi(cur + d, -1, count - 1);
        const dfx_effect_def *nf = next < 0 ? NULL : all[next];
        if (page == 0) mos_set_track_fx(m, m->active_track, nf);
        else mos_set_unit_fx(m, fx_unit(m), nf);
        return;
    }
    int p = id - base - 1;
    if (p < np) {
        const dfx_param *pp = &fx->params[p];
        void *mem = m->units[fx_unit(m)].mem;
        float v = fx->get_param(mem, p);
        if (pp->steps) {
            v += d;
        } else {
            float k = dfx_param_to_knob(pp, v) + d * (m->shift ? 0.002f : 0.01f);
            v = dfx_param_from_knob(pp, k);
        }
        fx->set_param(mem, p, dfx_clampf(v, pp->min, pp->max));
        return;
    }
    mos_track *t = &m->tracks[m->active_track];
    int b = p - np;
    if (page == 0 && b >= 0 && b < 2) t->send[b] = dfx_clampf(t->send[b] + d * 0.01f, 0, 1);
}

/* --- MIX -------------------------------------------------------------------- */

static void draw_mix(mos *m)
{
    char b[40];
    snprintf(b, sizeof b, "MIX  MASTER %.0f", m->master * 100);
    header(m, b);
    int any_solo = 0;
    for (int i = 0; i < MOS_TRACKS; i++) any_solo |= m->tracks[i].solo;
    for (int i = 0; i < MOS_TRACKS; i++) {
        const mos_track *t = &m->tracks[i];
        int x = i * 8;
        int h = (int)(t->vol / 1.25f * 30 + 0.5f);
        int on = any_solo ? t->solo : !t->mute;
        if (on) g_fill(m->fb, x + 1, 40 - h, 3, h, 1);
        else g_frame(m->fb, x + 1, 40 - h, 3, h > 1 ? h : 1, 1);
        float db = t->meter > 1e-6f ? 20.0f * log10f(t->meter) : -96.0f;
        int mh = clampi((int)((db + 48.0f) / 48.0f * 30.0f), 0, 30);
        if (mh) g_fill(m->fb, x + 5, 40 - mh, 2, mh, 1);
        /* numbers on every fourth strip, like a console's group marks; the selected one is boxed */
        snprintf(b, sizeof b, "%d", i + 1);
        if (i % 4 == 0 || i == m->active_track) g_text(m->fb, x + (i >= 9 ? 0 : 2), 43, b, 1, 1);
        if (i == m->active_track) g_invert(m->fb, x - (i >= 9 ? 1 : 0), 42, i >= 9 ? 9 : 8, 7);
    }
    const mos_track *t = &m->tracks[m->active_track];
    char pan[8];
    fmt_pan(t->pan, pan, sizeof pan);
    snprintf(b, sizeof b, "%s VOL %.0f PAN %s A%.0f B%.0f", track_name(m, m->active_track), t->vol * 100, pan,
             t->send[0] * 100, t->send[1] * 100);
    label(m, 0, 50, b);

    static const char *const kl0[3] = { "VOLUME", "PAN", "SEND A" };
    static const char *const kl1[3] = { "SEND B", "MASTER", "-" };
    char kv[3][16] = { "", "", "" };
    if (m->page[MOS_SCR_MIX] == 0) {
        snprintf(kv[0], 16, "VOL %.0f", t->vol * 100);
        fmt_pan(t->pan, kv[1], 16);
        snprintf(kv[2], 16, "A %.0f", t->send[0] * 100);
    } else {
        snprintf(kv[0], 16, "B %.0f", t->send[1] * 100);
        snprintf(kv[1], 16, "MST %.0f", m->master * 100);
    }
    knob_row(m, m->page[MOS_SCR_MIX] == 0 ? kl0 : kl1, kv);
}

/* --- SONG ------------------------------------------------------------------- */

static void draw_song(mos *m)
{
    char b[64];
    if (m->song_mode) snprintf(b, sizeof b, "SONG  STEP %d/%d", m->song_step + 1, m->song_len);
    else snprintf(b, sizeof b, "SONG  %d STEPS", m->song_len);
    header(m, b);
    int first = m->song_cursor > 2 ? m->song_cursor - 2 : 0;
    for (int r = 0; r < 5; r++) {
        int i = first + r;
        if (i > m->song_len) break;
        if (i == m->song_len) {
            snprintf(b, sizeof b, "   END   PUSH VALUE: ADD STEP");
        } else {
            const mos_song_step *st = &m->song[i];
            snprintf(b, sizeof b, "%s %02d  SEQ %02d  X%d  %d BARS", m->song_mode && m->song_step == i ? G_PLAY : " ",
                     i + 1, st->seq + 1, st->reps, m->seqs[st->seq].bars ? m->seqs[st->seq].bars : 2);
        }
        label(m, 0, ROW(r), b);
        if (i == m->song_cursor) g_invert(m->fb, 0, ROW(r) - 1, MOS_LCD_W, 7);
    }
    snprintf(b, sizeof b, "LOOP SONG: %s", m->song_loop ? "ON" : "OFF");
    label(m, 0, ROW(5), b);

    static const char *const kl[3] = { "SEQ", "REPEATS", "LOOP" };
    char kv[3][16] = { "", "", "" };
    if (m->song_cursor < m->song_len) {
        snprintf(kv[0], 16, "SEQ %02d", m->song[m->song_cursor].seq + 1);
        snprintf(kv[1], 16, "X%d", m->song[m->song_cursor].reps);
    }
    snprintf(kv[2], 16, "%s", m->song_loop ? "ON" : "OFF");
    knob_row(m, kl, kv);
}

static void song_add(mos *m)
{
    if (m->song_len >= MOS_SONG_STEPS) return;
    int at = m->song_cursor;
    mos_song_step st = at > 0 ? m->song[at - 1] : (mos_song_step){ m->seq, 1 };
    memmove(&m->song[at + 1], &m->song[at], (size_t)(m->song_len - at) * sizeof st);
    m->song[at] = st;
    m->song_len++;
}

static void song_delete(mos *m)
{
    int at = m->song_cursor;
    if (at >= m->song_len) return;
    memmove(&m->song[at], &m->song[at + 1], (size_t)(m->song_len - at - 1) * sizeof m->song[0]);
    m->song_len--;
}

/* --- boot and messages --------------------------------------------------------- */

static void draw_boot(mos *m)
{
    const char *name = MOS_THEME_NAME;
    g_text(m->fb, (MOS_LCD_W - g_text_width(name, 2)) / 2, 10, name, 1, 2);
    g_text(m->fb, (MOS_LCD_W - g_text_width(MOS_THEME_TAGLINE, 1)) / 2, 28, MOS_THEME_TAGLINE, 1, 1);
    const char *v = "V" MOS_THEME_VERSION;
    g_text(m->fb, (MOS_LCD_W - g_text_width(v, 1)) / 2, 36, v, 1, 1);
    float t = (float)m->samples / m->sr;
    for (int i = 0; i < 16; i++) {
        int h = 2 + (int)(9.0f * fabsf(sinf(t * 5.0f + i * 0.7f) * sinf(t * 1.3f + i * 0.3f)));
        g_fill(m->fb, 8 + i * 7, 62 - h, 5, h, 1);
    }
}

static void draw_message(mos *m)
{
    int w = g_text_width(m->msg, 1) + 10;
    int x = (MOS_LCD_W - w) / 2;
    g_fill(m->fb, x, 23, w, 13, 0);
    g_frame(m->fb, x, 23, w, 13, 1);
    g_frame(m->fb, x + 1, 24, w - 2, 11, 1);
    g_text(m->fb, x + 5, 27, m->msg, 1, 1);
}

void mos_ui_draw(mos *m)
{
    g_clear(m->fb);
    m->nfields = 0;
    if (!m->booted && m->samples < (int64_t)(m->sr * MOS_THEME_BOOT_MS / 1000)) {
        draw_boot(m);
        return;
    }
    m->booted = 1;
    switch (m->screen) {
    case MOS_SCR_MAIN: draw_main(m); break;
    case MOS_SCR_STEP: draw_step(m); break;
    case MOS_SCR_TRACK: draw_track(m); break;
    case MOS_SCR_FX: draw_fx(m); break;
    case MOS_SCR_MIX: draw_mix(m); break;
    case MOS_SCR_SONG: draw_song(m); break;
    }
    if (m->cursor[m->screen] >= m->nfields && m->nfields > 0) m->cursor[m->screen] = m->nfields - 1;
    if (m->samples < m->msg_until) draw_message(m);
}

const uint8_t *mos_screen(mos *m)
{
    mos_ui_draw(m);
    return m->fb;
}

/* --- input -------------------------------------------------------------------- */

static void edit(mos *m, int id, int d)
{
    switch (m->screen) {
    case MOS_SCR_MAIN: edit_main(m, id, d); break;
    case MOS_SCR_TRACK: edit_track(m, id, d); break;
    case MOS_SCR_FX: edit_fx(m, id, d); break;
    }
}

void mos_turn(mos *m, int delta)
{
    m->booted = 1;
    switch (m->screen) {
    case MOS_SCR_STEP: step_move(m, delta); return;
    case MOS_SCR_MIX: m->active_track = clampi(m->active_track + delta, 0, MOS_TRACKS - 1); return;
    case MOS_SCR_SONG: m->song_cursor = clampi(m->song_cursor + delta, 0, m->song_len); return;
    }
    if (m->editing) {
        edit(m, m->cursor[m->screen], delta);
    } else {
        mos_ui_draw(m); /* counts the fields */
        m->cursor[m->screen] = clampi(m->cursor[m->screen] + delta, 0, m->nfields > 0 ? m->nfields - 1 : 0);
    }
}

static void show_screen(mos *m, int screen)
{
    if (m->screen == screen) {
        if (screen == MOS_SCR_TRACK) m->pads_mode = (m->pads_mode + 1) % 3;
        else m->page[screen] = (m->page[screen] + 1) % screen_pages[screen];
        if (screen == MOS_SCR_FX) m->cursor[screen] = m->page[screen] == 0 ? 1 : 0; /* on the effect picker */
    } else {
        m->screen = screen;
        for (int k = 0; k < 3; k++) m->knob_touch[k] = 0; /* the knob row shows this screen's labels */
    }
    m->editing = 0;
}

static void tap(mos *m)
{
    int64_t now = m->samples;
    double interval = (double)(now - m->tap_last);
    m->tap_last = now;
    if (m->tap_count == 0 || interval > 2.0 * m->sr || interval < 0.15 * m->sr) {
        m->tap_count = 1;
        mos_message(m, "TAP");
        return;
    }
    m->tap_avg = m->tap_count == 1 ? interval : m->tap_avg * 0.6 + interval * 0.4;
    m->tap_count++;
    float bpm = (float)(60.0 * m->sr / m->tap_avg);
    m->seqs[m->seq].tempo = dfx_clampf(floorf(bpm * 10 + 0.5f) / 10, 30, 300);
    char b[24];
    snprintf(b, sizeof b, "TAP %.1f BPM", m->seqs[m->seq].tempo);
    mos_message(m, b);
}

static void arm(mos *m, int mode)
{
    if (m->playing) {
        if (m->rec == mode) {
            m->rec = MOS_REC_OFF; /* punch out */
        } else {
            if (!m->undo_taken) { mos_snapshot(m); m->undo_taken = 1; }
            m->rec = mode;
        }
    } else {
        m->rec_armed = m->rec_armed == mode ? MOS_REC_OFF : mode;
    }
}

void mos_key(mos *m, mos_key_id key, int down)
{
    m->booted = 1;
    if (!down) {
        if (key == MOS_KEY_SHIFT) m->shift = 0;
        if (key == MOS_KEY_NOTE_REPEAT) m->repeat_held = 0;
        if (key == MOS_KEY_ERASE) { m->erase_held = 0; m->erase_snap = 0; }
        return;
    }
    switch (key) {
    case MOS_KEY_SHIFT: m->shift = 1; break;
    case MOS_KEY_MAIN: show_screen(m, MOS_SCR_MAIN); break;
    case MOS_KEY_STEP: show_screen(m, MOS_SCR_STEP); break;
    case MOS_KEY_TRACK: show_screen(m, MOS_SCR_TRACK); break;
    case MOS_KEY_FX: show_screen(m, MOS_SCR_FX); break;
    case MOS_KEY_MIX: show_screen(m, MOS_SCR_MIX); break;
    case MOS_KEY_SONG: show_screen(m, MOS_SCR_SONG); break;
    case MOS_KEY_PLAY:
        if (m->playing) {
            mos_stop(m);
        } else if (m->screen == MOS_SCR_SONG && m->song_len > 0) {
            m->song_mode = 1;
            m->song_step = m->song_cursor < m->song_len ? m->song_cursor : 0;
            m->song_rep = 0;
            select_seq(m, m->song[m->song_step].seq);
            mos_start(m, 0);
        } else {
            mos_start(m, m->shift && m->screen == MOS_SCR_STEP ? m->step_tick : 0);
        }
        break;
    case MOS_KEY_REC: arm(m, MOS_REC_REPLACE); break;
    case MOS_KEY_OVERDUB: arm(m, MOS_REC_OVERDUB); break;
    case MOS_KEY_NOTE_REPEAT: m->repeat_held = 1; break;
    case MOS_KEY_ERASE:
        if (m->screen == MOS_SCR_STEP) { step_delete(m); break; }
        if (m->screen == MOS_SCR_SONG) { song_delete(m); break; }
        m->erase_held = 1;
        if (m->playing && !m->erase_snap) { mos_snapshot(m); m->erase_snap = 1; }
        if (m->playing) mos_message(m, "ERASE: HOLD PADS");
        break;
    case MOS_KEY_UNDO: mos_undo(m); break;
    case MOS_KEY_TAP: tap(m); break;
    case MOS_KEY_FULL_LEVEL:
        m->full_level = !m->full_level;
        mos_message(m, m->full_level ? "FULL LEVEL ON" : "FULL LEVEL OFF");
        break;
    case MOS_KEY_BANK_A: case MOS_KEY_BANK_B: case MOS_KEY_BANK_C: case MOS_KEY_BANK_D: case MOS_KEY_BANK_E: {
        int b = key - MOS_KEY_BANK_A;
        m->bank = m->bank == b ? b + 5 : b;
        char t[16];
        snprintf(t, sizeof t, "BANK %c", 'A' + m->bank);
        mos_message(m, t);
        break;
    }
    case MOS_KEY_ENTER:
        if (m->screen == MOS_SCR_STEP) {
            int idx[8], n = step_events(m, idx, 8);
            m->step_sel = n ? (m->step_sel + 1) % n : 0;
        } else if (m->screen == MOS_SCR_MIX) {
            m->tracks[m->active_track].mute = !m->tracks[m->active_track].mute;
        } else if (m->screen == MOS_SCR_SONG) {
            if (m->song_cursor >= m->song_len) song_add(m);
        } else {
            m->editing = !m->editing;
        }
        break;
    case MOS_KEY_EXIT:
        if (m->editing) m->editing = 0;
        else if (m->screen != MOS_SCR_MAIN) show_screen(m, MOS_SCR_MAIN);
        break;
    default: break;
    }
}

void mos_pad(mos *m, int pad, int velocity)
{
    if (pad < 0 || pad > 15) return;
    m->booted = 1;
    if (velocity <= 0) {
        if (!m->held_vel[pad]) return;
        m->held_vel[pad] = 0;
        mos_release(m, m->held_track[pad], m->held_note[pad]);
        mos_pad_release(m, pad);
        return;
    }
    int vel = m->full_level ? 127 : (velocity > 127 ? 127 : velocity);

    if (m->screen == MOS_SCR_TRACK) {
        mos_track *t = &m->tracks[pad];
        if (m->pads_mode == MOS_PADS_MUTE) t->mute = !t->mute;
        else if (m->pads_mode == MOS_PADS_SOLO) t->solo = !t->solo;
        else m->active_track = pad;
        return;
    }

    int track = m->active_track;
    int note = mos_note_for_pad(m, track, pad);
    m->held_vel[pad] = vel;
    m->held_track[pad] = track;
    m->held_note[pad] = note;
    m->held_recorded[pad] = 0;
    m->pad_flash[pad] = 1.0f;

    if (m->screen == MOS_SCR_STEP) {
        mos_trigger(m, track, note, vel, -1);
        step_add(m, pad, vel);
        return;
    }
    if (m->erase_held) return;                   /* ERASE + pad: silence it while the playhead passes */
    if (m->repeat_held && m->playing) return;    /* note repeat plays it on the next grid point */
    mos_trigger(m, track, note, vel, -1);
    mos_record_hit(m, pad, track, note, vel);
}

void mos_knob(mos *m, int ctrl, float pos)
{
    if (ctrl < 0 || ctrl > 2) return;
    m->booted = 1;
    pos = dfx_clampf(pos, 0, 1);
    m->knob[ctrl] = pos;
    m->knob_touch[ctrl] = m->samples + (int64_t)(m->sr * 1.2f);
    mos_track *t = &m->tracks[m->active_track];
    switch (m->screen) {
    case MOS_SCR_MAIN:
        if (ctrl == 0) m->seqs[m->seq].tempo = floorf((40.0f + 200.0f * pos) * 2 + 0.5f) / 2;
        if (ctrl == 1) m->swing = floorf(50.0f + 25.0f * pos + 0.5f);
        if (ctrl == 2) t->vol = pos * 1.25f;
        break;
    case MOS_SCR_STEP: {
        int idx[8], n = step_events(m, idx, 8);
        if (!n) break;
        mos_event *e = &m->seqs[m->seq].ev[idx[m->step_sel]];
        if (ctrl == 0) e->vel = (uint8_t)clampi((int)(1 + pos * 126 + 0.5f), 1, 127);
        if (ctrl == 1) e->dur = (uint16_t)clampi((int)(dfx_expmap(1.0f, 4.0f * MOS_PPQN * m->seqs[m->seq].beats, pos) + 0.5f), 1, 65535);
        if (ctrl == 2) {
            int max = t->type == MOS_TRACK_SYNTH ? 127 : MOS_PADS - 1;
            e->note = (uint8_t)clampi((int)(pos * max + 0.5f), 0, max);
        }
        break;
    }
    case MOS_SCR_TRACK:
        if (ctrl == 0) t->vol = pos * 1.25f;
        if (ctrl == 1) t->pan = floorf((pos * 2 - 1) * 50 + 0.5f) / 50;
        if (ctrl == 2) {
            if (t->type == MOS_TRACK_SYNTH) t->octave = clampi((int)(pos * 8 + 0.5f), 0, 8);
            else t->send[0] = pos;
        }
        break;
    case MOS_SCR_FX: set_param_knob(m, ctrl, pos); break;
    case MOS_SCR_MIX:
        if (m->page[MOS_SCR_MIX] == 0) {
            if (ctrl == 0) t->vol = pos * 1.25f;
            if (ctrl == 1) t->pan = floorf((pos * 2 - 1) * 50 + 0.5f) / 50;
            if (ctrl == 2) t->send[0] = pos;
        } else {
            if (ctrl == 0) t->send[1] = pos;
            if (ctrl == 1) m->master = pos * 1.25f;
        }
        break;
    case MOS_SCR_SONG:
        if (ctrl == 2) { m->song_loop = pos > 0.5f; break; }
        if (m->song_cursor >= m->song_len) break;
        if (ctrl == 0) m->song[m->song_cursor].seq = clampi((int)(pos * (MOS_SEQS - 1) + 0.5f), 0, MOS_SEQS - 1);
        if (ctrl == 1) m->song[m->song_cursor].reps = clampi((int)(1 + pos * 15 + 0.5f), 1, 16);
        break;
    }
}

/* --- LEDs ---------------------------------------------------------------------- */

static uint32_t blend(uint32_t a, uint32_t b, float t)
{
    t = dfx_clampf(t, 0, 1);
    uint32_t out = 0;
    for (int s = 0; s < 24; s += 8) {
        float ca = (float)((a >> s) & 255), cb = (float)((b >> s) & 255);
        out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << s;
    }
    return out;
}

uint32_t mos_pad_color(const mos *m, int pad)
{
    if (pad < 0 || pad > 15) return 0;
    if (m->screen == MOS_SCR_TRACK) {
        const mos_track *t = &m->tracks[pad];
        int any_solo = 0;
        for (int i = 0; i < MOS_TRACKS; i++) any_solo |= m->tracks[i].solo;
        if (pad == m->active_track) return MOS_COLOR_SELECTED;
        if (t->solo) return MOS_COLOR_SOLO;
        return (any_solo ? 0 : !t->mute) ? MOS_COLOR_TRACK_ON : MOS_COLOR_TRACK_OFF;
    }
    int track = m->active_track;
    int note = mos_note_for_pad(m, track, pad);
    uint32_t base;
    if (m->tracks[track].type == MOS_TRACK_SYNTH) base = note % 12 == 0 ? MOS_COLOR_ROOT : MOS_COLOR_SYNTH;
    else base = m->pads[note].l ? MOS_COLOR_LOADED : MOS_COLOR_IDLE;
    if (m->screen == MOS_SCR_STEP) {
        const mos_seq *s = &m->seqs[m->seq];
        for (int i = 0; i < s->count; i++)
            if (s->ev[i].tick == m->step_tick && s->ev[i].track == track && s->ev[i].note == note) base = MOS_COLOR_STEP;
    }
    return blend(base, MOS_COLOR_HIT, m->pad_flash[pad]);
}

int mos_key_led(const mos *m, mos_key_id key)
{
    static const int screen_of[] = { [MOS_KEY_MAIN] = MOS_SCR_MAIN, [MOS_KEY_STEP] = MOS_SCR_STEP, [MOS_KEY_TRACK] = MOS_SCR_TRACK,
                                     [MOS_KEY_FX] = MOS_SCR_FX, [MOS_KEY_MIX] = MOS_SCR_MIX, [MOS_KEY_SONG] = MOS_SCR_SONG };
    switch (key) {
    case MOS_KEY_SHIFT: return m->shift;
    case MOS_KEY_MAIN: case MOS_KEY_STEP: case MOS_KEY_TRACK: case MOS_KEY_FX: case MOS_KEY_MIX: case MOS_KEY_SONG:
        return m->screen == screen_of[key];
    case MOS_KEY_PLAY: return m->playing;
    case MOS_KEY_REC:
        if (m->rec == MOS_REC_REPLACE) return MOS_LED_ON;
        return m->rec_armed == MOS_REC_REPLACE ? MOS_LED_BLINK : MOS_LED_OFF;
    case MOS_KEY_OVERDUB:
        if (m->rec == MOS_REC_OVERDUB) return MOS_LED_ON;
        return m->rec_armed == MOS_REC_OVERDUB ? MOS_LED_BLINK : MOS_LED_OFF;
    case MOS_KEY_NOTE_REPEAT: return m->repeat_held;
    case MOS_KEY_ERASE: return m->erase_held;
    case MOS_KEY_UNDO: return m->undo_valid;
    case MOS_KEY_FULL_LEVEL: return m->full_level;
    case MOS_KEY_BANK_A: case MOS_KEY_BANK_B: case MOS_KEY_BANK_C: case MOS_KEY_BANK_D: case MOS_KEY_BANK_E: {
        int b = key - MOS_KEY_BANK_A;
        if (m->bank == b) return MOS_LED_ON;
        return m->bank == b + 5 ? MOS_LED_BLINK : MOS_LED_OFF;
    }
    default: return MOS_LED_OFF;
    }
}

/* --- names --------------------------------------------------------------------- */

static const char *const key_names[MOS_KEY_COUNT] = {
    "SHIFT", "MAIN", "STEP", "TRACK", "FX", "MIX", "SONG", "PLAY", "REC", "OVERDUB",
    "REPEAT", "ERASE", "UNDO", "TAP", "FULL", "BANK_A", "BANK_B", "BANK_C", "BANK_D", "BANK_E",
    "ENTER", "EXIT",
};

const char *mos_key_name(mos_key_id key) { return (unsigned)key < MOS_KEY_COUNT ? key_names[key] : "?"; }

int mos_key_from_name(const char *name)
{
    for (int i = 0; i < MOS_KEY_COUNT; i++) {
        const char *a = key_names[i], *b = name;
        while (*a && *b && (*a == *b || *a == *b - 32)) a++, b++;
        if (!*a && !*b) return i;
    }
    return -1;
}
