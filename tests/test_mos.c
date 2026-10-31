/* Tests for MOSURA OS: sequencer timing, recording, editing, routing and the UI. */
#include "../include/mos.h"
#include "../src/os/mos_internal.h"
#include "../src/dsp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define SR 48000

static mos *M;
static float impulse[64], impulse2[64];

static void fresh(void)
{
    mos_init(M, SR);
    /* a click on A01 and a softer one on A02, so onsets can be found to the sample */
    mos_set_sample(M, 0, impulse, impulse, 64, SR);
    mos_set_sample(M, 1, impulse2, impulse2, 64, SR);
    M->booted = 1;
}

static void run(int frames)
{
    static float l[4096], r[4096];
    while (frames > 0) {
        int n = frames > 4096 ? 4096 : frames;
        mos_process(M, l, r, n);
        frames -= n;
    }
}

static double spt(void) { return SR * 60.0 / (mos_bpm(M) * MOS_PPQN); }

/* Run until the playhead is at `tick` (and any count in is over). */
static void run_to_tick(int tick)
{
    for (int guard = 0; guard < SR * 60; guard++) {
        if (M->playing && M->countin_left == 0 && M->tick == tick) return;
        run(1);
    }
}

static void press(int pad, int vel) { mos_pad(M, pad, vel); run(32); mos_pad(M, pad, 0); }
static void key(mos_key_id k) { mos_key(M, k, 1); mos_key(M, k, 0); }

static int count_track(int track)
{
    int n = 0;
    const mos_seq *s = &M->seqs[M->seq];
    for (int i = 0; i < s->count; i++) n += s->ev[i].track == track;
    return n;
}

static void test_setup(void)
{
    CHECK(mos_size() >= sizeof(mos), "mos_size covers the state");
    int n;
    const dfx_effect_def *const *fx = dfx_effects(&n);
    for (int i = 0; i < n; i++)
        CHECK(fx[i]->state_size <= MOS_FX_BYTES, "%s fits an effect unit (%zu bytes)", fx[i]->id, fx[i]->state_size);
    CHECK(dfx_synth_find("juno106")->state_size <= MOS_SYNTH_BYTES, "juno106 fits a synth track");
    fresh();
    CHECK(M->seqs[0].bars == 2 && M->seqs[0].tempo == 90.0f, "a fresh project is two bars at 90");
    for (int k = 0; k < MOS_KEY_COUNT; k++)
        CHECK(mos_key_from_name(mos_key_name((mos_key_id)k)) == k, "key name %s round trips", mos_key_name((mos_key_id)k));
    CHECK(mos_key_from_name("overdub") == MOS_KEY_OVERDUB, "key names ignore case");
}

static void test_playback_timing(void)
{
    fresh();
    mos_event e = { 96 * 3 + 24, 10, 0, 0, 100, 0 }; /* 1.4.24 */
    mos_insert_event(M, 0, e);
    mos_start(M, 0);
    static float l[SR * 3], r[SR * 3];
    mos_process(M, l, r, SR * 3);
    int first = -1;
    for (int i = 0; i < SR * 3 && first < 0; i++) if (fabsf(l[i]) > 1e-6f) first = i;
    double want = e.tick * spt();
    CHECK(first >= 0 && fabs(first - want) <= 1.0, "event at tick %d sounds at sample %d (want %.1f)", e.tick, first, want);
}

static void test_block_independence(void)
{
    static float a_l[SR * 2], a_r[SR * 2], b_l[SR * 2], b_r[SR * 2];
    for (int pass = 0; pass < 2; pass++) {
        fresh();
        mos_set_track_fx(M, 0, dfx_find("stutter"));     /* tempo synced, position aware */
        mos_set_unit_fx(M, MOS_UNIT_SEND_A, dfx_find("dubecho"));
        M->tracks[0].send[0] = 0.5f;
        mos_set_track_type(M, 1, MOS_TRACK_SYNTH);
        for (int t = 0; t < 768; t += 48) mos_insert_event(M, 0, (mos_event){ t, 20, 0, (uint8_t)(t / 48 % 2), 100, 0 });
        for (int t = 0; t < 768; t += 192) mos_insert_event(M, 0, (mos_event){ t, 150, 1, 48, 100, 0 });
        mos_start(M, 0);
        float *l = pass ? b_l : a_l, *r = pass ? b_r : a_r;
        if (pass == 0) {
            mos_process(M, l, r, SR * 2);
        } else {
            for (int pos = 0, n = 1; pos < SR * 2; pos += n, n = n % 97 + 7)
                mos_process(M, l + pos, r + pos, pos + n > SR * 2 ? SR * 2 - pos : n);
        }
    }
    CHECK(!memcmp(a_l, b_l, sizeof a_l) && !memcmp(a_r, b_r, sizeof a_r), "output does not depend on the host block size");
    float peak = 0;
    for (int i = 0; i < SR * 2; i++) peak = fmaxf(peak, fabsf(a_l[i]));
    CHECK(peak > 0.01f && peak < 4.0f, "that session makes sound (peak %.3f)", peak);
}

static void test_record_timing_correct(void)
{
    fresh();
    M->countin = 0;
    M->swing = 66;
    key(MOS_KEY_OVERDUB);
    CHECK(M->rec_armed == MOS_REC_OVERDUB, "OVERDUB arms when stopped");
    key(MOS_KEY_PLAY);
    CHECK(M->playing && M->rec == MOS_REC_OVERDUB, "PLAY starts recording straight away without count in");
    run_to_tick(22);              /* a little early for the swung 16th at 32 */
    press(0, 100);
    run_to_tick(98);              /* a little late for beat 2 */
    press(1, 90);
    run_to_tick(300);
    const mos_seq *s = &M->seqs[0];
    CHECK(s->count == 2, "two events recorded (%d)", s->count);
    if (s->count == 2) {
        CHECK(s->ev[0].tick == 32, "66%% swing pulls the off 16th to tick 32 (got %d)", s->ev[0].tick);
        CHECK(s->ev[1].tick == 96, "a late hit snaps back to the beat (got %d)", s->ev[1].tick);
        CHECK(s->ev[0].note == 0 && s->ev[1].note == 1 && s->ev[1].vel == 90, "pads and velocity recorded");
        CHECK(s->ev[0].dur >= 1 && s->ev[0].dur <= 2, "the length is how long the pad was held (%d ticks)", s->ev[0].dur);
        CHECK(!(s->ev[0].flags & MOS_EV_HELD), "the length is closed when the pad is released");
    }
    /* a recorded note isn't played twice in the pass it was recorded in, then plays normally */
    key(MOS_KEY_PLAY);
    CHECK(!M->playing && M->rec == MOS_REC_OFF, "PLAY while playing stops and ends recording");
}

static void test_countin_and_replace(void)
{
    fresh();
    for (int t = 0; t < 768; t += 96) mos_insert_event(M, 0, (mos_event){ t, 10, 0, 0, 100, 0 });
    mos_insert_event(M, 0, (mos_event){ 48, 10, 1, 0, 100, 0 }); /* another track */
    key(MOS_KEY_REC);
    key(MOS_KEY_PLAY);
    CHECK(M->countin_left == 4 * MOS_PPQN && M->rec == MOS_REC_OFF, "count in: one bar before recording starts");
    run((int)(4 * MOS_PPQN * spt()) + 2);
    CHECK(M->countin_left == 0 && M->rec == MOS_REC_REPLACE, "recording starts after the count in");
    run_to_tick(400);
    press(2, 100);
    run_to_tick(767);
    run((int)spt() + 2);          /* past the loop point */
    CHECK(M->rec == MOS_REC_OVERDUB, "REC turns into OVERDUB after one pass, like the MPC");
    CHECK(count_track(0) == 1, "REC replaced the active track's events (left %d)", count_track(0));
    CHECK(count_track(1) == 1, "other tracks are untouched");
    key(MOS_KEY_PLAY);
    key(MOS_KEY_UNDO);
    CHECK(count_track(0) == 8, "UNDO brings the replaced events back (%d)", count_track(0));
    key(MOS_KEY_UNDO);
    CHECK(count_track(0) == 1, "UNDO again is redo (%d)", count_track(0));
}

static void test_note_repeat(void)
{
    fresh();
    M->countin = 0;
    M->tc = 24;
    M->swing = 62;
    key(MOS_KEY_OVERDUB);
    key(MOS_KEY_PLAY);
    run_to_tick(10);
    mos_key(M, MOS_KEY_NOTE_REPEAT, 1);
    mos_pad(M, 0, 100);
    run_to_tick(110);
    mos_pad(M, 0, 0);
    mos_key(M, MOS_KEY_NOTE_REPEAT, 0);
    run_to_tick(200);
    const mos_seq *s = &M->seqs[0];
    /* 16ths from 24 to 96 with 62% swing on the odd ones: 24+5.76~>30? check against the groove engine */
    int want[4], n = 0;
    for (int k = 1; k <= 4; k++) want[n++] = k * 24 + dfx_swing_offset(24, 62, k);
    CHECK(s->count == 4, "note repeat recorded 4 hits between ticks 10 and 110 (%d)", s->count);
    for (int i = 0; i < 4 && i < s->count; i++)
        CHECK(s->ev[i].tick == want[i], "repeat %d on the swung grid: %d (want %d)", i, s->ev[i].tick, want[i]);
}

static void test_mute_solo(void)
{
    static float l[SR], r[SR];
    fresh();
    mos_insert_event(M, 0, (mos_event){ 0, 10, 0, 0, 100, 0 });
    M->tracks[0].mute = 1;
    mos_start(M, 0);
    mos_process(M, l, r, SR / 2);
    float peak = 0;
    for (int i = 0; i < SR / 2; i++) peak = fmaxf(peak, fabsf(l[i]));
    CHECK(peak == 0, "a muted track plays nothing");
    fresh();
    mos_insert_event(M, 0, (mos_event){ 0, 10, 0, 0, 100, 0 });
    mos_insert_event(M, 0, (mos_event){ 0, 10, 1, 1, 100, 0 });
    M->tracks[1].solo = 1;
    mos_start(M, 0);
    mos_process(M, l, r, 200);
    float solo = l[0];
    fresh();
    mos_insert_event(M, 0, (mos_event){ 0, 10, 1, 1, 100, 0 });
    mos_start(M, 0);
    mos_process(M, l, r, 200);
    CHECK(solo != 0 && solo == l[0], "solo: only the soloed track sounds (%.4f vs %.4f alone)", solo, l[0]);
}

static void test_next_seq_and_song(void)
{
    fresh();
    M->seqs[0].bars = 1;
    mos_start(M, 0);
    M->screen = MOS_SCR_MAIN;
    M->cursor[MOS_SCR_MAIN] = 0;
    key(MOS_KEY_ENTER);
    mos_turn(M, 2);               /* seq 3, while playing: queued */
    CHECK(M->next_seq == 2 && M->seq == 0, "turning SEQ while playing queues the next sequence");
    run((int)(4 * MOS_PPQN * spt()) + 4);
    CHECK(M->seq == 2 && M->tick < 10 && M->seqs[2].bars == 2, "it switches at the end of the bar (seq %d)", M->seq + 1);
    mos_stop(M);

    fresh();
    M->seqs[0].bars = 1;
    M->seqs[1] = M->seqs[0];
    M->song[0] = (mos_song_step){ 1, 2 };
    M->song[1] = (mos_song_step){ 0, 1 };
    M->song_len = 2;
    M->song_loop = 0;
    M->screen = MOS_SCR_SONG;
    key(MOS_KEY_PLAY);
    CHECK(M->song_mode && M->seq == 1, "PLAY on the SONG screen plays the song from its first step");
    int bar = (int)(4 * MOS_PPQN * spt());
    run(bar + 4);
    CHECK(M->seq == 1 && M->song_rep == 1, "first step repeats");
    run(bar);
    CHECK(M->seq == 0 && M->song_step == 1, "then the second step");
    run(bar);
    CHECK(!M->playing, "a song without loop stops at the end");
}

static void test_step_edit(void)
{
    fresh();
    key(MOS_KEY_STEP);
    mos_turn(M, 3);               /* 1/16 grid: tick 72 */
    CHECK(M->step_tick == 72, "VALUE moves the step cursor by the timing correct grid (%d)", M->step_tick);
    mos_pad(M, 4, 77);
    mos_pad(M, 4, 0);
    mos_pad(M, 5, 66);
    mos_pad(M, 5, 0);
    CHECK(M->seqs[0].count == 2 && M->seqs[0].ev[0].tick == 72 && M->seqs[0].ev[0].vel == 77, "pads add events at the cursor");
    mos_knob(M, 0, 1.0f);
    int idx = -1;
    for (int i = 0; i < M->seqs[0].count; i++) if (M->seqs[0].ev[i].note == 5) idx = i;
    CHECK(idx >= 0 && M->seqs[0].ev[idx].vel == 127, "CTRL 1 sets the selected event's velocity");
    key(MOS_KEY_ERASE);
    CHECK(M->seqs[0].count == 1, "ERASE on the STEP screen deletes the selected event");
    mos_key(M, MOS_KEY_SHIFT, 1);
    mos_turn(M, 1);
    mos_key(M, MOS_KEY_SHIFT, 0);
    CHECK(M->step_tick == 384 + 72, "SHIFT + VALUE jumps a bar and keeps the step (%d)", M->step_tick);
    M->swing = 66;
    mos_turn(M, -2);              /* step 17 of the sequence: an odd 16th, swung to 24 + 8 */
    CHECK(M->step_tick == 384 + 32, "with swing the cursor walks the swung grid (%d)", M->step_tick);
}

static void test_fx_routing(void)
{
    static float l[SR], r[SR];
    fresh();
    /* an insert on track 2 must not touch track 1 */
    mos_insert_event(M, 0, (mos_event){ 0, 10, 0, 0, 100, 0 });
    mos_set_track_fx(M, 1, dfx_find("bitladder"));
    mos_start(M, 0);
    mos_process(M, l, r, 300);
    float a = l[0];
    fresh();
    mos_insert_event(M, 0, (mos_event){ 0, 10, 0, 0, 100, 0 });
    mos_start(M, 0);
    mos_process(M, l, r, 300);
    CHECK(a == l[0], "an insert on another track doesn't change this one");

    fresh();
    mos_set_unit_fx(M, MOS_UNIT_SEND_A, dfx_find("dubecho"));
    const dfx_effect_def *de = dfx_find("dubecho");
    CHECK(de->get_param(M->units[MOS_UNIT_SEND_A].mem, dfx_param_index(de, "mix")) == 100.0f, "effects on a send bus come up 100%% wet");
    mos_insert_event(M, 0, (mos_event){ 48, 10, 0, 0, 100, 0 });
    M->tracks[0].send[0] = 1.0f;
    mos_start(M, 0);
    mos_process(M, l, r, SR);
    /* the default echo is a dotted 8th at 90 BPM: 0.5 s after the hit, which is at 48 ticks = 1/3 s */
    int hit = SR / 3, echo_at = hit + SR / 2;
    float before = 0, echo = 0;
    for (int i = hit + 2000; i < echo_at - 500; i++) before += fabsf(l[i]) + fabsf(r[i]);
    for (int i = echo_at - 500; i < echo_at + 2000; i++) echo += fabsf(l[i]) + fabsf(r[i]);
    CHECK(echo > 0.05f && echo > 20 * before, "send A returns the echo half a second later (%.4f vs %.4f before)", echo, before);

    fresh();
    for (int t = 0; t < 8; t++) mos_set_track_fx(M, t, dfx_find("mpc60"));
    mos_set_track_fx(M, 8, dfx_find("mpc60"));
    CHECK(M->tracks[8].unit < 0 && strstr(M->msg, "INSERTS"), "a 9th insert is refused with a message");
    mos_set_track_fx(M, 3, NULL);
    mos_set_track_fx(M, 8, dfx_find("mpc60"));
    CHECK(M->tracks[8].unit >= 0, "turning an insert off frees its unit");
}

static void test_synth_track(void)
{
    static float l[SR], r[SR];
    fresh();
    M->screen = MOS_SCR_TRACK;
    M->cursor[MOS_SCR_TRACK] = 2;
    key(MOS_KEY_ENTER);
    mos_turn(M, 1);
    CHECK(M->tracks[0].type == MOS_TRACK_SYNTH, "TYPE turns track 1 into a synth track");
    key(MOS_KEY_EXIT);
    key(MOS_KEY_MAIN);
    CHECK(mos_note_for_pad(M, 0, 0) == 48 && mos_note_for_pad(M, 0, 12) == 60, "pads are chromatic from C3");
    mos_pad(M, 0, 100);
    mos_process(M, l, r, SR / 4);
    mos_pad(M, 0, 0);
    float peak = 0;
    for (int i = 0; i < SR / 4; i++) peak = fmaxf(peak, fabsf(l[i]));
    CHECK(peak > 0.01f, "the synth track plays (peak %.3f)", peak);
}

static void test_ui(void)
{
    fresh();
    M->booted = 0;
    const uint8_t *fb = mos_screen(M);
    int lit = 0;
    for (int i = 0; i < MOS_LCD_W * MOS_LCD_H; i++) lit += fb[i];
    CHECK(lit > 100, "boot screen draws");
    static const mos_key_id screens[] = { MOS_KEY_MAIN, MOS_KEY_STEP, MOS_KEY_TRACK, MOS_KEY_FX, MOS_KEY_MIX, MOS_KEY_SONG };
    mos_set_track_fx(M, 0, dfx_find("dust"));
    for (int s = 0; s < 6; s++)
        for (int p = 0; p < 6; p++) {
            key(screens[s]);
            fb = mos_screen(M);
            int ok = 1;
            for (int i = 0; i < MOS_LCD_W * MOS_LCD_H; i++) ok &= fb[i] <= 1;
            CHECK(ok, "screen %d page %d is 0/1 pixels", s, p);
            mos_turn(M, 3);
            key(MOS_KEY_ENTER);
            mos_turn(M, -1);
            key(MOS_KEY_EXIT);
        }
    key(MOS_KEY_FX);
    CHECK(M->screen == MOS_SCR_FX, "FX key opens the FX screen");
    CHECK(mos_key_led(M, MOS_KEY_FX) == MOS_LED_ON && mos_key_led(M, MOS_KEY_MAIN) == MOS_LED_OFF, "the current screen's key is lit");
    key(MOS_KEY_EXIT);
    CHECK(M->screen == MOS_SCR_MAIN, "EXIT goes back to MAIN");

    /* the FX screen: pick an effect, the knobs move its first three parameters */
    fresh();
    key(MOS_KEY_FX);
    key(MOS_KEY_ENTER);
    mos_turn(M, 1);
    CHECK(M->tracks[0].unit >= 0 && M->units[M->tracks[0].unit].fx == dfx_effects(NULL)[0], "VALUE picks the insert effect");
    mos_knob(M, 0, 0.0f);
    const dfx_effect_def *fx = M->units[M->tracks[0].unit].fx;
    CHECK(fx->get_param(M->units[M->tracks[0].unit].mem, 0) == fx->params[0].min, "CTRL 1 sets the effect's first parameter");

    /* TRACK screen pads mute tracks; pressing TRACK again cycles mute / solo / select */
    fresh();
    key(MOS_KEY_TRACK);
    mos_pad(M, 3, 100);
    mos_pad(M, 3, 0);
    CHECK(M->tracks[3].mute, "a pad mutes its track");
    key(MOS_KEY_TRACK);
    key(MOS_KEY_TRACK);
    mos_pad(M, 5, 100);
    CHECK(M->active_track == 5 && M->pads_mode == MOS_PADS_SELECT, "third press: pads select the track");
    CHECK(mos_pad_color(M, 5) == 0xFFFFFF, "the active track's pad is white");

    /* banks */
    fresh();
    key(MOS_KEY_BANK_B);
    CHECK(M->bank == 1, "BANK B");
    key(MOS_KEY_BANK_B);
    CHECK(M->bank == 6, "BANK B again is G");
    CHECK(mos_note_for_pad(M, 0, 2) == 6 * 16 + 2, "drum pads follow the bank");

    /* full level */
    fresh();
    key(MOS_KEY_FULL_LEVEL);
    M->countin = 0;
    key(MOS_KEY_OVERDUB);
    key(MOS_KEY_PLAY);
    press(0, 20);
    CHECK(M->seqs[0].count == 1 && M->seqs[0].ev[0].vel == 127, "FULL LEVEL records velocity 127");

    /* tap tempo */
    fresh();
    for (int i = 0; i < 5; i++) { key(MOS_KEY_TAP); run(SR / 2); }
    CHECK(fabsf(mos_bpm(M) - 120.0f) < 0.6f, "tapping every 0.5 s gives 120 BPM (%.1f)", mos_bpm(M));
}

int main(void)
{
    for (int i = 0; i < 64; i++) {
        impulse[i] = i == 0 ? 1.0f : 0.0f;
        impulse2[i] = i == 0 ? 0.5f : 0.0f;
    }
    M = calloc(1, mos_size());
    if (!M) return 1;
    test_setup();
    test_playback_timing();
    test_block_independence();
    test_record_timing_correct();
    test_countin_and_replace();
    test_note_repeat();
    test_mute_solo();
    test_next_seq_and_song();
    test_step_edit();
    test_fx_routing();
    test_synth_track();
    test_ui();
    printf("%d checks, %d failed\n", checks, failures);
    free(M);
    return failures ? 1 : 0;
}
