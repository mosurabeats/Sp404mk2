/*
 * MOSURA OS engine: the sequencer clock, recording, sample voices, synth
 * tracks and the effect routing.
 *
 * Signal flow, MPC 2500 style:
 *
 *   track 1..16 --> [insert FX] --> level / pan --+--> master --> [master FX 1] --> [master FX 2] --> out
 *                                                 +--> send A --> [send FX A] --> master
 *                                                 +--> send B --> [send FX B] --> master
 *
 * Effects belong to tracks and buses, not to pads or to whatever was last
 * touched, so a recorded sequence always plays back through the same
 * effects. Sends are post-fader.
 *
 * Timing: 96 ticks per quarter note. Audio is rendered in chunks that end
 * on tick boundaries, so events land on the same sample whatever block
 * size the host uses.
 */
#include "mos_internal.h"
#include "theme.h"
#include "../dsp.h"

#include <string.h>

size_t mos_size(void) { return sizeof(mos); }

static const dfx_synth_def *juno(void)
{
    static const dfx_synth_def *j;
    if (!j) j = dfx_synth_find("juno106");
    return j;
}

float mos_bpm(const mos *m) { return m->seqs[m->seq].tempo; }

int mos_seq_len(const mos *m, int seq) { return m->seqs[seq].bars * m->seqs[seq].beats * MOS_PPQN; }

static double samples_per_tick(const mos *m) { return m->sr * 60.0 / (mos_bpm(m) * MOS_PPQN); }

static void seq_create(mos_seq *s, float tempo)
{
    s->bars = 2;
    s->beats = 4;
    s->tempo = tempo;
    s->loop = 1;
    s->count = 0;
}

void mos_message(mos *m, const char *text)
{
    strncpy(m->msg, text, sizeof m->msg - 1);
    m->msg[sizeof m->msg - 1] = 0;
    m->msg_until = m->samples + (int64_t)(m->sr * 1.2f);
}

int mos_note_for_pad(const mos *m, int track, int pad)
{
    const mos_track *t = &m->tracks[track];
    if (t->type == MOS_TRACK_SYNTH) {
        int n = 12 * (t->octave + 1) + pad;
        return n < 0 ? 0 : (n > 127 ? 127 : n);
    }
    return m->bank * 16 + pad;
}

/* --- setup ---------------------------------------------------------------- */

void mos_load_synth_preset(mos *m, int track, int preset)
{
    const dfx_synth_def *j = juno();
    if (preset < 0) preset = j->num_presets - 1;
    if (preset >= j->num_presets) preset = 0;
    m->tracks[track].preset = preset;
    dfx_synth_load_preset(j, m->tracks[track].synth_mem, preset);
}

void mos_set_track_type(mos *m, int track, int type)
{
    mos_track *t = &m->tracks[track];
    if (t->type == type) return;
    juno()->all_notes_off(t->synth_mem);
    for (int i = 0; i < MOS_VOICES; i++)
        if (m->voices[i].track == track) m->voices[i].active = 0;
    t->type = type;
    if (type == MOS_TRACK_SYNTH) mos_load_synth_preset(m, track, t->preset);
}

void mos_init(mos *m, float sample_rate)
{
    memset(m, 0, sizeof *m);
    m->sr = sample_rate;
    seq_create(&m->seqs[0], 90.0f);
    m->next_seq = -1;
    m->master = 0.9f;
    m->tc = 24;         /* 1/16 */
    m->swing = 50.0f;
    m->countin = 1;
    m->click = 1;       /* while recording */
    m->song_loop = 1;

    const dfx_synth_def *j = juno();
    for (int i = 0; i < MOS_TRACKS; i++) {
        mos_track *t = &m->tracks[i];
        t->name = mos_theme_default_tracks[i];
        t->type = MOS_TRACK_DRUM;
        t->octave = 3;
        t->vol = 0.8f;
        t->unit = -1;
        t->gain_l = t->gain_r = t->vol;
        if (j && j->state_size <= sizeof t->synth_mem) j->init(t->synth_mem, sample_rate);
    }
    for (int u = 0; u < MOS_UNITS; u++) m->units[u].owner = -1;
    mos_ui_init(m);
}

void mos_set_sample(mos *m, int slot, const float *left, const float *right, int frames, float sample_rate)
{
    if (slot < 0 || slot >= MOS_PADS) return;
    for (int i = 0; i < MOS_VOICES; i++)
        if (m->voices[i].active && m->voices[i].slot == slot) m->voices[i].active = 0;
    mos_sample *s = &m->pads[slot];
    s->l = left;
    s->r = right ? right : left;
    s->frames = left ? frames : 0;
    s->sr = sample_rate > 0 ? sample_rate : m->sr;
}

/* --- effects ---------------------------------------------------------------- */

void mos_set_unit_fx(mos *m, int unit, const dfx_effect_def *fx)
{
    mos_unit *u = &m->units[unit];
    if (fx && fx->state_size > sizeof u->mem) fx = NULL; /* never happens with this library; checked by the tests */
    u->fx = fx;
    if (!fx) return;
    fx->init(u->mem, m->sr);
    /* a send bus returns only the effect */
    if (unit == MOS_UNIT_SEND_A || unit == MOS_UNIT_SEND_B) {
        int mix = dfx_param_index(fx, "mix");
        if (mix >= 0) fx->set_param(u->mem, mix, 100.0f);
    }
}

void mos_set_track_fx(mos *m, int track, const dfx_effect_def *fx)
{
    mos_track *t = &m->tracks[track];
    if (!fx) {
        if (t->unit >= 0) {
            m->units[t->unit].fx = NULL;
            m->units[t->unit].owner = -1;
        }
        t->unit = -1;
        return;
    }
    if (t->unit < 0) {
        for (int u = 0; u < MOS_INSERTS; u++)
            if (m->units[u].owner < 0) { t->unit = u; break; }
        if (t->unit < 0) {
            mos_message(m, "ALL 8 INSERTS IN USE");
            return;
        }
        m->units[t->unit].owner = track;
    }
    mos_set_unit_fx(m, t->unit, fx);
}

/* --- events ----------------------------------------------------------------- */

int mos_insert_event(mos *m, int seq, mos_event e)
{
    mos_seq *s = &m->seqs[seq];
    if (s->count >= MOS_SEQ_EVENTS) return -1;
    int i = s->count;
    while (i > 0 && s->ev[i - 1].tick > e.tick) i--;
    memmove(&s->ev[i + 1], &s->ev[i], (size_t)(s->count - i) * sizeof e);
    s->ev[i] = e;
    s->count++;
    if (seq == m->seq && i < m->ev_idx) m->ev_idx++;
    return i;
}

void mos_delete_event(mos *m, int seq, int index)
{
    mos_seq *s = &m->seqs[seq];
    if (index < 0 || index >= s->count) return;
    memmove(&s->ev[index], &s->ev[index + 1], (size_t)(s->count - index - 1) * sizeof s->ev[0]);
    s->count--;
    if (seq == m->seq && index < m->ev_idx) m->ev_idx--;
}

static void clear_flags(mos_seq *s, int mask)
{
    for (int i = 0; i < s->count; i++) s->ev[i].flags &= (uint8_t)~mask;
}

static int first_event_at(const mos_seq *s, int tick)
{
    int i = 0;
    while (i < s->count && s->ev[i].tick < tick) i++;
    return i;
}

/* --- undo ------------------------------------------------------------------ */

void mos_snapshot(mos *m)
{
    m->undo = m->seqs[m->seq];
    clear_flags(&m->undo, MOS_EV_NEW | MOS_EV_HELD);
    m->undo_seq = m->seq;
    m->undo_valid = 1;
}

void mos_undo(mos *m)
{
    if (!m->undo_valid) {
        mos_message(m, "NOTHING TO UNDO");
        return;
    }
    /* swap, so pressing UNDO again is redo */
    unsigned char *a = (unsigned char *)&m->undo, *b = (unsigned char *)&m->seqs[m->undo_seq];
    for (size_t i = 0; i < sizeof(mos_seq); i++) {
        unsigned char t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
    clear_flags(&m->seqs[m->undo_seq], MOS_EV_NEW | MOS_EV_HELD);
    if (m->undo_seq == m->seq) m->ev_idx = first_event_at(&m->seqs[m->seq], m->tick + (m->playing ? 1 : 0));
    m->step_sel = 0;
    mos_message(m, "UNDO SEQ");
}

/* --- voices ------------------------------------------------------------------ */

static void voice_start(mos *m, int track, int slot, int vel)
{
    const mos_sample *smp = &m->pads[slot];
    if (!smp->l || smp->frames < 2) return;
    /* a pad retriggering cuts its own previous hit, like the SP and the MPC's mono pads */
    for (int i = 0; i < MOS_VOICES; i++) {
        mos_voice *v = &m->voices[i];
        if (v->active && v->slot == slot && !v->releasing) v->releasing = 1;
    }
    mos_voice *v = NULL;
    for (int i = 0; i < MOS_VOICES && !v; i++)
        if (!m->voices[i].active) v = &m->voices[i];
    if (!v) { /* steal the one that has played longest */
        v = &m->voices[0];
        for (int i = 1; i < MOS_VOICES; i++)
            if (m->voices[i].pos / m->voices[i].inc > v->pos / v->inc) v = &m->voices[i];
    }
    float g = vel / 127.0f;
    *v = (mos_voice){ 1, slot, track, slot, 0.0, smp->sr / m->sr, g * g * 0.6f + g * 0.4f, 1.0f, 0 };
}

static void schedule_off(mos *m, int track, int note, int64_t at)
{
    if (m->pending_count >= MOS_PENDING) mos_release(m, m->pending[0].track, m->pending[0].note), m->pending[0] = m->pending[--m->pending_count];
    m->pending[m->pending_count++] = (mos_pending){ track, note, at };
}

void mos_trigger(mos *m, int track, int note, int vel, int64_t off_at)
{
    mos_track *t = &m->tracks[track];
    if (t->type == MOS_TRACK_SYNTH) {
        juno()->note_on(t->synth_mem, note, vel);
        if (off_at >= 0) schedule_off(m, track, note, off_at);
    } else {
        if (note < 0 || note >= MOS_PADS) return;
        voice_start(m, track, note, vel);
        if (off_at >= 0 && t->gate) schedule_off(m, track, note, off_at);
    }
}

void mos_release(mos *m, int track, int note)
{
    mos_track *t = &m->tracks[track];
    if (t->type == MOS_TRACK_SYNTH) {
        juno()->note_off(t->synth_mem, note);
        return;
    }
    if (!t->gate) return;
    for (int i = 0; i < MOS_VOICES; i++) {
        mos_voice *v = &m->voices[i];
        if (v->active && v->track == track && v->note == note) v->releasing = 1;
    }
}

static void render_voices(mos *m, int n)
{
    float fade_step = 1.0f / (0.004f * m->sr); /* 4 ms release, no clicks */
    for (int i = 0; i < MOS_VOICES; i++) {
        mos_voice *v = &m->voices[i];
        if (!v->active) continue;
        const mos_sample *smp = &m->pads[v->slot];
        float *ol = m->tb[v->track][0], *orr = m->tb[v->track][1];
        for (int k = 0; k < n; k++) {
            int idx = (int)v->pos;
            if (idx + 1 >= smp->frames) { v->active = 0; break; }
            float t = (float)(v->pos - idx);
            float l = smp->l[idx] + (smp->l[idx + 1] - smp->l[idx]) * t;
            float r = smp->r[idx] + (smp->r[idx + 1] - smp->r[idx]) * t;
            if (v->releasing) {
                v->fade -= fade_step;
                if (v->fade <= 0) { v->active = 0; break; }
            }
            float g = v->gain * v->fade;
            ol[k] += l * g;
            orr[k] += r * g;
            v->pos += v->inc;
        }
    }
}

/* --- metronome ---------------------------------------------------------------- */

static void click(mos *m, int accent)
{
    m->click_env = accent ? 0.22f : 0.13f;
    m->click_freq = accent ? 1760.0f : 1320.0f;
    m->click_ph = 0;
}

static int click_wanted(const mos *m)
{
    return m->click == 2 || (m->click == 1 && (m->rec || m->countin_left > 0));
}

/* --- transport ---------------------------------------------------------------- */

static int is_grid_point(int tick, int grid, float swing)
{
    int k = tick / grid;
    for (int kk = k; kk >= k - 1 && kk >= 0; kk--)
        if (kk * grid + dfx_swing_offset(grid, swing, kk) == tick) return 1;
    return 0;
}

static int track_audible(const mos *m, int track)
{
    int any_solo = 0;
    for (int i = 0; i < MOS_TRACKS; i++) any_solo |= m->tracks[i].solo;
    const mos_track *t = &m->tracks[track];
    return any_solo ? t->solo : !t->mute;
}

/* Close the length of notes recorded while their pad was held. */
static void close_held(mos *m, int pad, double now)
{
    if (!m->held_recorded[pad]) return;
    m->held_recorded[pad] = 0;
    mos_seq *s = &m->seqs[m->seq];
    for (int i = 0; i < s->count; i++) {
        mos_event *e = &s->ev[i];
        if ((e->flags & MOS_EV_HELD) && e->track == m->held_track[pad] && e->note == m->held_note[pad]) {
            double d = now - m->held_start[pad];
            int dur = (int)(d + 0.5);
            int len = mos_seq_len(m, m->seq);
            if (dur < 1) dur = 1;
            if (dur > len) dur = len;
            if (dur > 65535) dur = 65535;
            e->dur = (uint16_t)dur;
            e->flags &= (uint8_t)~MOS_EV_HELD;
            break;
        }
    }
}

static double clock_now(const mos *m)
{
    double frac = m->playing ? 1.0 - m->to_next / samples_per_tick(m) : 0.0;
    return (double)m->clock + (frac < 0 ? 0 : frac);
}

static void begin_recording(mos *m)
{
    if (!m->rec_armed) return;
    if (!m->undo_taken) {
        mos_snapshot(m);
        m->undo_taken = 1;
    }
    m->rec = m->rec_armed;
    m->rec_armed = MOS_REC_OFF;
}

static void fire_events(mos *m)
{
    mos_seq *s = &m->seqs[m->seq];
    while (m->ev_idx < s->count && s->ev[m->ev_idx].tick <= m->tick) {
        mos_event *e = &s->ev[m->ev_idx];
        if (e->tick < m->tick || (e->flags & MOS_EV_NEW)) {
            m->ev_idx++;
            continue;
        }
        if (m->rec == MOS_REC_REPLACE && e->track == m->active_track) {
            mos_delete_event(m, m->seq, m->ev_idx);
            continue;
        }
        if (m->erase_held && e->track == m->active_track) {
            int erase = 0;
            for (int p = 0; p < 16; p++)
                if (m->held_vel[p] && mos_note_for_pad(m, e->track, p) == e->note) erase = 1;
            if (erase) {
                mos_delete_event(m, m->seq, m->ev_idx);
                continue;
            }
        }
        if (track_audible(m, e->track)) {
            mos_trigger(m, e->track, e->note, e->vel, m->clock + (e->dur ? e->dur : 1));
            if (e->track == m->active_track) {
                for (int p = 0; p < 16; p++)
                    if (mos_note_for_pad(m, e->track, p) == e->note) m->pad_flash[p] = 1.0f;
            }
        }
        m->ev_idx++;
    }
}

static void note_repeat(mos *m)
{
    if (!m->repeat_held) return;
    if (!is_grid_point(m->tick, m->tc, m->swing)) return;
    int gate = m->tc / 2 > 0 ? m->tc / 2 : 1;
    for (int p = 0; p < 16; p++) {
        if (!m->held_vel[p]) continue;
        int track = m->active_track;
        int note = mos_note_for_pad(m, track, p);
        mos_trigger(m, track, note, m->held_vel[p], m->clock + gate);
        m->pad_flash[p] = 1.0f;
        if (m->rec) {
            int i = mos_insert_event(m, m->seq, (mos_event){ m->tick, (uint16_t)gate, (uint8_t)track, (uint8_t)note, (uint8_t)m->held_vel[p], MOS_EV_NEW });
            if (i < 0) mos_message(m, "SEQUENCE FULL");
        }
    }
}

/* Called on entering a tick, including the first one. */
static void on_tick(mos *m)
{
    const mos_seq *s = &m->seqs[m->seq];
    int bar = s->beats * MOS_PPQN;
    if (m->countin_left > 0) {
        int pos = bar - m->countin_left;
        if (pos % MOS_PPQN == 0) click(m, pos == 0);
        return;
    }
    if (click_wanted(m) && m->tick % MOS_PPQN == 0) click(m, m->tick % bar == 0);
    fire_events(m);
    note_repeat(m);
}

static void seq_end(mos *m)
{
    clear_flags(&m->seqs[m->seq], MOS_EV_NEW);
    int next = -1;
    if (m->song_mode) {
        if (++m->song_rep >= m->song[m->song_step].reps) {
            m->song_rep = 0;
            if (++m->song_step >= m->song_len) {
                if (!m->song_loop) { mos_stop(m); return; }
                m->song_step = 0;
            }
        }
        next = m->song[m->song_step].seq;
    } else if (m->next_seq >= 0) {
        next = m->next_seq;
        m->next_seq = -1;
    } else if (!m->seqs[m->seq].loop) {
        mos_stop(m);
        return;
    }
    if (next >= 0 && next != m->seq) {
        if (m->seqs[next].bars == 0) seq_create(&m->seqs[next], mos_bpm(m));
        m->seq = next;
    }
    m->tick = 0;
    m->ev_idx = 0;
    /* like the MPC: after one pass, REC carries on as OVERDUB */
    if (m->rec == MOS_REC_REPLACE) m->rec = MOS_REC_OVERDUB;
}

static void advance(mos *m)
{
    m->clock++;
    for (int i = 0; i < m->pending_count;) {
        if (m->pending[i].at <= m->clock) {
            mos_release(m, m->pending[i].track, m->pending[i].note);
            m->pending[i] = m->pending[--m->pending_count];
        } else {
            i++;
        }
    }
    if (m->countin_left > 0) {
        if (--m->countin_left == 0) begin_recording(m);
    } else if (++m->tick >= mos_seq_len(m, m->seq)) {
        seq_end(m);
    }
    if (m->playing) on_tick(m);
}

void mos_start(mos *m, int from_tick)
{
    mos_seq *s = &m->seqs[m->seq];
    if (s->bars == 0) seq_create(s, 90.0f);
    if (from_tick < 0 || from_tick >= mos_seq_len(m, m->seq)) from_tick = 0;
    clear_flags(s, MOS_EV_NEW | MOS_EV_HELD);
    m->playing = 1;
    m->clock = 0;
    m->tick = from_tick;
    m->ev_idx = first_event_at(s, from_tick);
    m->to_next = samples_per_tick(m);
    m->undo_taken = 0;
    m->countin_left = (m->rec_armed && m->countin) ? s->beats * MOS_PPQN : 0;
    if (m->countin_left == 0) begin_recording(m);
    on_tick(m);
}

void mos_stop(mos *m)
{
    if (!m->playing) {
        m->rec_armed = MOS_REC_OFF;
        return;
    }
    double now = clock_now(m);
    for (int p = 0; p < 16; p++) close_held(m, p, now);
    for (int i = 0; i < m->pending_count; i++) mos_release(m, m->pending[i].track, m->pending[i].note);
    m->pending_count = 0;
    m->playing = 0;
    m->rec = m->rec_armed = MOS_REC_OFF;
    m->countin_left = 0;
    m->song_mode = 0;
    m->next_seq = -1;
    clear_flags(&m->seqs[m->seq], MOS_EV_NEW | MOS_EV_HELD);
}

/* Record a live pad hit, timing corrected and swung onto the grid. */
void mos_record_hit(mos *m, int pad, int track, int note, int vel)
{
    if (!m->playing || m->countin_left > 0 || !m->rec) return;
    double now = clock_now(m);
    double frac = now - (double)m->clock;
    int t = m->tick + (int)(frac + 0.5);
    dfx_groove g = { MOS_PPQN, m->tc, m->swing, 1.0f, 0 };
    if (m->tc > 1) t = dfx_groove_target(&g, t);
    int len = mos_seq_len(m, m->seq);
    while (t >= len) t -= len;
    int i = mos_insert_event(m, m->seq, (mos_event){ t, 1, (uint8_t)track, (uint8_t)note, (uint8_t)vel, MOS_EV_NEW | MOS_EV_HELD });
    if (i < 0) {
        mos_message(m, "SEQUENCE FULL");
        return;
    }
    m->held_recorded[pad] = 1;
    m->held_start[pad] = now;
}

void mos_pad_release(mos *m, int pad)
{
    close_held(m, pad, clock_now(m));
}

/* --- audio ------------------------------------------------------------------- */

/* Gains glide per sample (5 ms), so a level change sounds the same whatever the block size. */
static inline float glide(float *g, float target, float a) { *g += (target - *g) * a; return *g; }

static void render_chunk(mos *m, float *out_l, float *out_r, int n)
{
    memset(m->tb, 0, sizeof m->tb);
    memset(m->bus, 0, sizeof m->bus);
    render_voices(m, n);

    const dfx_synth_def *j = juno();
    for (int t = 0; t < MOS_TRACKS; t++) {
        mos_track *tr = &m->tracks[t];
        if (tr->type != MOS_TRACK_SYNTH) continue;
        j->render(tr->synth_mem, m->tmp[0], m->tmp[1], n);
        for (int i = 0; i < n; i++) {
            m->tb[t][0][i] += m->tmp[0][i];
            m->tb[t][1][i] += m->tmp[1][i];
        }
    }

    float bpm = mos_bpm(m);
    double beat = -1.0;
    if (m->playing && m->countin_left == 0)
        beat = ((double)m->tick + 1.0 - m->to_next / samples_per_tick(m)) / MOS_PPQN;
    for (int u = 0; u < MOS_UNITS; u++)
        if (m->units[u].fx && m->units[u].fx->set_tempo) m->units[u].fx->set_tempo(m->units[u].mem, bpm, beat);

    float ga = 1.0f - expf(-1.0f / (0.005f * m->sr));
    for (int t = 0; t < MOS_TRACKS; t++) {
        mos_track *tr = &m->tracks[t];
        float *l = m->tb[t][0], *r = m->tb[t][1];
        if (tr->unit >= 0 && m->units[tr->unit].fx) m->units[tr->unit].fx->process(m->units[tr->unit].mem, l, r, n);
        float gl = tr->vol * (tr->pan > 0 ? 1.0f - tr->pan : 1.0f);
        float gr = tr->vol * (tr->pan < 0 ? 1.0f + tr->pan : 1.0f);
        float peak = 0;
        for (int i = 0; i < n; i++) peak = fmaxf(peak, fmaxf(fabsf(l[i] * gl), fabsf(r[i] * gr)));
        tr->meter = fmaxf(peak, tr->meter * expf(-(float)n / (0.3f * m->sr)));
        for (int i = 0; i < n; i++) {
            float al = glide(&tr->gain_l, gl, ga), ar = glide(&tr->gain_r, gr, ga);
            float sa = glide(&tr->send_g[0], tr->send[0], ga), sb = glide(&tr->send_g[1], tr->send[1], ga);
            float x = l[i], y = r[i];
            m->bus[0][0][i] += x * al;
            m->bus[0][1][i] += y * ar;
            m->bus[1][0][i] += x * al * sa;
            m->bus[1][1][i] += y * ar * sa;
            m->bus[2][0][i] += x * al * sb;
            m->bus[2][1][i] += y * ar * sb;
        }
    }

    for (int b = 0; b < 2; b++) {
        mos_unit *u = &m->units[MOS_UNIT_SEND_A + b];
        if (!u->fx) continue;
        u->fx->process(u->mem, m->bus[1 + b][0], m->bus[1 + b][1], n);
        for (int i = 0; i < n; i++) {
            m->bus[0][0][i] += m->bus[1 + b][0][i];
            m->bus[0][1][i] += m->bus[1 + b][1][i];
        }
    }
    for (int k = 0; k < 2; k++) {
        mos_unit *u = &m->units[MOS_UNIT_MASTER + k];
        if (u->fx) u->fx->process(u->mem, m->bus[0][0], m->bus[0][1], n);
    }

    float click_decay = expf(-1.0f / (0.012f * m->sr));
    float click_inc = 2.0f * (float)M_PI * m->click_freq / m->sr;
    for (int i = 0; i < n; i++) {
        float c = 0;
        if (m->click_env > 1e-4f) {
            c = sinf(m->click_ph) * m->click_env;
            m->click_ph += click_inc;
            m->click_env *= click_decay;
        }
        out_l[i] = m->bus[0][0][i] * m->master + c;
        out_r[i] = m->bus[0][1][i] * m->master + c;
    }

    float flash_decay = expf(-(float)n / (0.12f * m->sr));
    for (int p = 0; p < 16; p++) m->pad_flash[p] *= flash_decay;
}

void mos_process(mos *m, float *left, float *right, int frames)
{
    int pos = 0;
    while (pos < frames) {
        int n = frames - pos;
        if (n > MOS_CHUNK) n = MOS_CHUNK;
        if (m->playing) {
            int k = (int)ceil(m->to_next);
            if (k < 1) k = 1;
            if (n > k) n = k;
        }
        render_chunk(m, left + pos, right + pos, n);
        m->samples += n;
        if (m->playing) {
            m->to_next -= n;
            if (m->to_next <= 1e-9) {
                m->to_next += samples_per_tick(m);
                advance(m);
            }
        }
        pos += n;
    }
}
