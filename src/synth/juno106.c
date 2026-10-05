/*
 * Juno-106: a six voice polysynth in the style of Roland's 1984 classic.
 *
 * Per voice, as on the original:
 *   DCO (saw, pulse with PWM, square sub one octave down, noise)
 *   -> 24 dB/oct resonant lowpass (IR3109 style, modelled as a ladder)
 *   -> VCA (envelope or gate)
 * and then, for the whole synth: HPF (4 positions, 0 is a bass boost)
 *   -> BBD chorus I / II / I+II.
 * One LFO (triangle, with a delayed fade-in per voice) and one ADSR per voice,
 * shared by the VCF and the VCA, like the 106.
 *
 * The DCOs are digitally controlled on the real thing, so they are stable
 * here too: no drift. Oscillators are band-limited with PolyBLEP.
 * Envelope time ranges follow the 106's published specifications. The HPF
 * corner frequencies and filter voicing are approximations, not measured.
 *
 * Not on the original: velocity sensitivity (Velo, off by default), because
 * the SP's pads are velocity sensitive.
 */
#include "../../include/doomfx_synth.h"
#include "../dsp.h"
#include "../juno_chorus.h"

#define VOICES 6

enum {
    P_CUTOFF, P_RESO, P_ENV, P_ENVPOL, P_LFOVCF, P_KYBD, P_HPF,
    P_RANGE, P_LFODCO, P_PWM, P_PWMMODE, P_PULSE, P_SAW, P_SUB, P_NOISE,
    P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE, P_VCA, P_LEVEL,
    P_LFORATE, P_LFODELAY, P_CHORUS, P_VELO, P_COUNT
};

static const char *const pol_labels[] = { "+", "-" };
static const char *const hpf_labels[] = { "0", "1", "2", "3" };
static const char *const range_labels[] = { "16'", "8'", "4'" };
static const char *const pwmmode_labels[] = { "LFO", "MAN" };
static const char *const onoff_labels[] = { "Off", "On" };
static const char *const vca_labels[] = { "Env", "Gate" };
static const char *const chorus_labels[] = { "Off", "I", "II", "I+II" };

static const dfx_param params[P_COUNT] = {
    { "cutoff",   "Cutoff",   20, 18000, 3000, "Hz", 0, 0, DFX_PARAM_LOG },
    { "reso",     "Reso",     0, 100, 0, "%", 0, 0, 0 },
    { "env",      "Env",      0, 100, 30, "%", 0, 0, 0 },
    { "envpol",   "EnvPol",   0, 1, 0, "", 2, pol_labels, 0 },
    { "lfovcf",   "LFO>VCF",  0, 100, 0, "%", 0, 0, 0 },
    { "kybd",     "Kybd",     0, 100, 50, "%", 0, 0, 0 },
    { "hpf",      "HPF",      0, 3, 1, "", 4, hpf_labels, 0 },
    { "range",    "Range",    0, 2, 1, "", 3, range_labels, 0 },
    { "lfodco",   "LFO>DCO",  0, 100, 0, "%", 0, 0, 0 },
    { "pwm",      "PWM",      0, 100, 0, "%", 0, 0, 0 },
    { "pwmmode",  "PWMMode",  0, 1, 0, "", 2, pwmmode_labels, 0 },
    { "pulse",    "Pulse",    0, 1, 0, "", 2, onoff_labels, 0 },
    { "saw",      "Saw",      0, 1, 1, "", 2, onoff_labels, 0 },
    { "sub",      "Sub",      0, 100, 0, "%", 0, 0, 0 },
    { "noise",    "Noise",    0, 100, 0, "%", 0, 0, 0 },
    { "attack",   "Attack",   1.5f, 3000, 1.5f, "ms", 0, 0, DFX_PARAM_LOG },
    { "decay",    "Decay",    1.5f, 12000, 600, "ms", 0, 0, DFX_PARAM_LOG },
    { "sustain",  "Sustain",  0, 100, 70, "%", 0, 0, 0 },
    { "release",  "Release",  1.5f, 12000, 300, "ms", 0, 0, DFX_PARAM_LOG },
    { "vca",      "VCA",      0, 1, 0, "", 2, vca_labels, 0 },
    { "level",    "Level",    -24, 12, 0, "dB", 0, 0, 0 },
    { "lforate",  "LFORate",  0.1f, 30, 3, "Hz", 0, 0, DFX_PARAM_LOG },
    { "lfodelay", "LFODelay", 0, 3000, 0, "ms", 0, 0, 0 },
    { "chorus",   "Chorus",   0, 3, 1, "", 4, chorus_labels, 0 },
    { "velocity", "Velo",     0, 100, 0, "%", 0, 0, 0 },
};

#define PV(id, v) { id, v }
static const dfx_preset_value pad[] = {
    PV("saw", 1), PV("pulse", 1), PV("pwm", 45), PV("lforate", 0.8f), PV("sub", 20),
    PV("cutoff", 1500), PV("reso", 15), PV("env", 25), PV("kybd", 40),
    PV("attack", 600), PV("decay", 2500), PV("sustain", 80), PV("release", 1800), PV("chorus", 2),
};
static const dfx_preset_value bass[] = {
    PV("saw", 0), PV("pulse", 1), PV("sub", 100), PV("range", 0), PV("hpf", 0),
    PV("cutoff", 250), PV("reso", 35), PV("env", 55), PV("kybd", 60),
    PV("attack", 1.5f), PV("decay", 350), PV("sustain", 20), PV("release", 80), PV("chorus", 0),
};
static const dfx_preset_value brass[] = {
    PV("saw", 1), PV("cutoff", 500), PV("reso", 10), PV("env", 55), PV("kybd", 60),
    PV("attack", 60), PV("decay", 700), PV("sustain", 60), PV("release", 250),
    PV("lfodco", 8), PV("lforate", 5.5f), PV("lfodelay", 600), PV("chorus", 1),
};
static const dfx_preset_value strings[] = {
    PV("saw", 1), PV("pulse", 1), PV("pwm", 60), PV("lforate", 0.5f), PV("hpf", 2),
    PV("cutoff", 4000), PV("env", 0), PV("attack", 400), PV("decay", 1000), PV("sustain", 100),
    PV("release", 900), PV("lfodco", 6), PV("lfodelay", 800), PV("chorus", 1),
};
static const dfx_preset_value pluck[] = {
    PV("saw", 0), PV("pulse", 1), PV("pwm", 35), PV("pwmmode", 1),
    PV("cutoff", 350), PV("reso", 30), PV("env", 65), PV("kybd", 70),
    PV("attack", 1.5f), PV("decay", 300), PV("sustain", 0), PV("release", 300), PV("chorus", 1),
};
static const dfx_preset_value keys[] = {
    PV("saw", 1), PV("pulse", 1), PV("cutoff", 900), PV("reso", 5), PV("env", 40),
    PV("attack", 1.5f), PV("decay", 1400), PV("sustain", 30), PV("release", 400),
    PV("chorus", 2), PV("velocity", 60),
};
#define PRESET(name, arr) { name, (int)(sizeof arr / sizeof arr[0]), arr }
static const dfx_preset presets[] = {
    PRESET("pad", pad), PRESET("bass", bass), PRESET("brass", brass),
    PRESET("strings", strings), PRESET("pluck", pluck), PRESET("keys", keys),
};

enum { ST_IDLE, ST_ATTACK, ST_DECAY, ST_RELEASE };

typedef struct {
    int note, gate, stage;
    unsigned age;
    float vel_gain;
    float phase2;      /* half-frequency phase: drives the sub, and the main phase is twice it */
    float dt2;         /* phase increment of phase2 */
    float env, gate_env, lfo_fade;
    dfx_ladder lad;
    dfx_ladder_coef lc;
    uint32_t rng;
} voice;

typedef struct {
    float sr;
    float p[P_COUNT];
    voice v[VOICES];
    unsigned age;
    int ctrl;
    int fresh;
    float lfo_phase;
    dfx_smooth cutoff, reso, level;
    float a_coef, d_coef, r_coef, gate_coef, fade_inc, k, makeup;
    dfx_bq_coef hpf;
    dfx_bq_state hpf_s;
    int hpf_on;
    float dc_x, dc_y, dc_coef; /* output DC blocker */
    juno_chorus ch;
} juno;

static float blep(float t, float dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

/* one-pole coefficient so that an exponential segment covers ~60 dB in time_ms */
static float seg_coef(float time_ms, float sr, float span)
{
    return 1.0f - expf(-span / (time_ms * 0.001f * sr));
}

static void update_global(juno *s)
{
    s->a_coef = seg_coef(s->p[P_ATTACK], s->sr, 1.466f); /* attack aims at 1.3 and stops at 1 */
    s->d_coef = seg_coef(s->p[P_DECAY], s->sr, 6.9f);
    s->r_coef = seg_coef(s->p[P_RELEASE], s->sr, 6.9f);
    s->gate_coef = seg_coef(2.0f, s->sr, 6.9f);
    float delay = s->p[P_LFODELAY] * 0.001f;
    s->fade_inc = delay > 0 ? 1.0f / (delay * s->sr) : 1.0f;
    s->k = s->reso.y * 4.1f;
    s->makeup = 1.0f + 0.3f * s->k;
    int hpf = (int)s->p[P_HPF];
    s->hpf_on = hpf != 1;
    if (hpf == 0) dfx_bq_design(&s->hpf, DFX_BQ_LOWSHELF, 100.0f, 0.7f, 6.0f, s->sr);
    else if (hpf == 2) dfx_bq_design(&s->hpf, DFX_BQ_HP, 240.0f, 0.6f, 0, s->sr);
    else if (hpf == 3) dfx_bq_design(&s->hpf, DFX_BQ_HP, 720.0f, 0.6f, 0, s->sr);
    juno_chorus_set(&s->ch, (int)s->p[P_CHORUS], 1.0f, s->sr);
}

static void update_voice(juno *s, voice *v, float lfo)
{
    float lfo_v = lfo * v->lfo_fade;
    float octave = s->p[P_RANGE] - 1.0f;
    float semis = (float)(v->note - 69) + 12.0f * octave + lfo_v * s->p[P_LFODCO] * 0.03f;
    float freq = 440.0f * exp2f(semis / 12.0f);
    v->dt2 = dfx_clampf(0.5f * freq / s->sr, 0.0f, 0.24f);

    float pol = s->p[P_ENVPOL] > 0.5f ? -1.0f : 1.0f;
    float oct = pol * v->env * s->p[P_ENV] * 0.08f           /* up to 8 octaves */
              + lfo_v * s->p[P_LFOVCF] * 0.03f               /* up to 3 octaves */
              + s->p[P_KYBD] * 0.01f * (float)(v->note - 60) / 12.0f;
    dfx_ladder_design(&v->lc, s->cutoff.y * exp2f(oct), s->sr);
}

static void reset(void *state)
{
    juno *s = state;
    for (int i = 0; i < VOICES; i++) {
        voice *v = &s->v[i];
        *v = (voice){ 0 };
        v->rng = 0x51D0u + (uint32_t)i * 7919u;
        v->phase2 = (float)i / VOICES; /* free running DCOs don't start in phase */
    }
    s->age = 0;
    s->lfo_phase = 0.0f;
    s->hpf_s = (dfx_bq_state){ 0 };
    s->dc_x = s->dc_y = 0.0f;
    juno_chorus_reset(&s->ch);
    s->ctrl = 0;
}

static void set_param(void *state, int i, float v)
{
    juno *s = state;
    if (i < 0 || i >= P_COUNT) return;
    v = dfx_clampf(v, params[i].min, params[i].max);
    if (params[i].steps) v = floorf(v + 0.5f);
    s->p[i] = v;
}

static float get_param(const void *state, int i)
{
    const juno *s = state;
    return (i >= 0 && i < P_COUNT) ? s->p[i] : 0.0f;
}

static void init(void *state, float sr)
{
    juno *s = state;
    *s = (juno){ 0 };
    s->sr = sr;
    for (int i = 0; i < P_COUNT; i++) s->p[i] = params[i].def;
    dfx_smooth_init(&s->cutoff, s->p[P_CUTOFF], 20, sr);
    dfx_smooth_init(&s->reso, s->p[P_RESO] * 0.01f, 20, sr);
    dfx_smooth_init(&s->level, dfx_db(s->p[P_LEVEL]), 20, sr);
    s->dc_coef = 1.0f - 2.0f * (float)M_PI * 8.0f / sr; /* 8 Hz */
    s->fresh = 1;
    reset(s);
}

/* The first note or block after init starts at the set values instead of gliding from the defaults. */
static void snap_if_fresh(juno *s)
{
    if (!s->fresh) return;
    s->cutoff.y = s->p[P_CUTOFF];
    s->reso.y = s->p[P_RESO] * 0.01f;
    s->level.y = dfx_db(s->p[P_LEVEL]);
    update_global(s);
    s->fresh = 0;
}

static int voice_done(const juno *s, const voice *v)
{
    float out = s->p[P_VCA] > 0.5f ? v->gate_env : v->env;
    return !v->gate && out < 1e-5f;
}

static void note_on(void *state, int note, int velocity)
{
    juno *s = state;
    voice *pick = NULL;
    snap_if_fresh(s);
    /* 1: the voice already on this note, 2: the longest idle voice,
       3: the oldest released voice, 4: the oldest voice */
    for (int i = 0; i < VOICES && !pick; i++)
        if (s->v[i].stage != ST_IDLE && s->v[i].note == note) pick = &s->v[i];
    for (int pass = 0; pass < 3 && !pick; pass++)
        for (int i = 0; i < VOICES; i++) {
            voice *v = &s->v[i];
            int ok = pass == 0 ? v->stage == ST_IDLE : pass == 1 ? !v->gate : 1;
            if (ok && (!pick || v->age < pick->age)) pick = v;
        }
    float velo = s->p[P_VELO] * 0.01f;
    pick->note = note & 127;
    pick->gate = 1;
    pick->stage = ST_ATTACK;
    pick->age = ++s->age;
    pick->vel_gain = 1.0f - velo * (1.0f - dfx_clampf((float)velocity, 1, 127) / 127.0f);
    pick->lfo_fade = 0.0f;
    /* the envelope continues from where it is, like an analog one */
    update_voice(s, pick, 4.0f * fabsf(s->lfo_phase - 0.5f) - 1.0f);
}

static void note_off(void *state, int note)
{
    juno *s = state;
    for (int i = 0; i < VOICES; i++)
        if (s->v[i].gate && s->v[i].note == note) {
            s->v[i].gate = 0;
            s->v[i].stage = ST_RELEASE;
        }
}

static void all_notes_off(void *state)
{
    juno *s = state;
    for (int i = 0; i < VOICES; i++)
        if (s->v[i].gate) {
            s->v[i].gate = 0;
            s->v[i].stage = ST_RELEASE;
        }
}

static void render(void *state, float *l, float *r, int frames)
{
    juno *s = state;
    float level_t = dfx_db(s->p[P_LEVEL]), reso_t = s->p[P_RESO] * 0.01f;
    float saw_on = s->p[P_SAW], pulse_on = s->p[P_PULSE];
    float sub = s->p[P_SUB] * 0.01f, noise = s->p[P_NOISE] * 0.01f * 0.7f;
    float sustain = s->p[P_SUSTAIN] * 0.01f, pwm = s->p[P_PWM] * 0.01f;
    int pwm_lfo = s->p[P_PWMMODE] < 0.5f, gate_vca = s->p[P_VCA] > 0.5f;
    int chorus = (int)s->p[P_CHORUS];
    float lfo_inc = s->p[P_LFORATE] / s->sr;

    snap_if_fresh(s);

    for (int n = 0; n < frames; n++) {
        s->lfo_phase += lfo_inc;
        if (s->lfo_phase >= 1.0f) s->lfo_phase -= 1.0f;
        float lfo = 4.0f * fabsf(s->lfo_phase - 0.5f) - 1.0f;
        dfx_smooth_step(&s->cutoff, s->p[P_CUTOFF]);
        dfx_smooth_step(&s->reso, reso_t);
        float level = dfx_smooth_step(&s->level, level_t);
        int ctrl = s->ctrl-- <= 0;
        if (ctrl) {
            update_global(s);
            s->ctrl = DFX_CTRL_INTERVAL - 1;
        }
        float pw = 0.5f - 0.45f * (pwm_lfo ? pwm * (0.5f + 0.5f * lfo) : pwm);

        float mix = 0.0f;
        for (int i = 0; i < VOICES; i++) {
            voice *v = &s->v[i];
            if (v->stage == ST_IDLE) continue;
            if (ctrl) update_voice(s, v, lfo);
            v->lfo_fade = fminf(1.0f, v->lfo_fade + s->fade_inc);

            /* envelope */
            if (v->stage == ST_ATTACK) {
                v->env += (1.3f - v->env) * s->a_coef;
                if (v->env >= 1.0f) { v->env = 1.0f; v->stage = ST_DECAY; }
            } else if (v->stage == ST_DECAY) {
                v->env += (sustain - v->env) * s->d_coef;
            } else {
                v->env = dfx_undenormal(v->env - v->env * s->r_coef);
            }
            v->gate_env += ((float)v->gate - v->gate_env) * s->gate_coef;

            /* DCO: one accumulator at half the pitch keeps the sub locked to the main wave */
            float dt2 = v->dt2, dt = 2.0f * dt2;
            v->phase2 += dt2;
            if (v->phase2 >= 1.0f) v->phase2 -= 1.0f;
            float ph = 2.0f * v->phase2;
            if (ph >= 1.0f) ph -= 1.0f;
            float osc = 0.0f;
            if (saw_on > 0.5f) osc += 2.0f * ph - 1.0f - blep(ph, dt);
            if (pulse_on > 0.5f) {
                float t2 = ph + 1.0f - pw;
                if (t2 >= 1.0f) t2 -= 1.0f;
                /* minus the pulse's average (2pw - 1): AC coupling, as in the circuit */
                osc += (ph < pw ? 1.0f : -1.0f) + blep(ph, dt) - blep(t2, dt) - (2.0f * pw - 1.0f);
            }
            if (sub > 0.0f) {
                float t2 = v->phase2 + 0.5f;
                if (t2 >= 1.0f) t2 -= 1.0f;
                osc += sub * ((v->phase2 < 0.5f ? 1.0f : -1.0f) + blep(v->phase2, dt2) - blep(t2, dt2));
            }
            if (noise > 0.0f) osc += noise * dfx_randf(&v->rng);

            float y = dfx_ladder_run(&v->lad, &v->lc, osc * 0.4f, s->k) * s->makeup;
            mix += y * (gate_vca ? v->gate_env : v->env) * v->vel_gain;

            if (voice_done(s, v)) {
                v->stage = ST_IDLE;
                v->env = v->gate_env = 0.0f;
                v->lad = (dfx_ladder){ { 0 } };
            }
        }

        mix *= 0.5f;
        s->dc_y = dfx_undenormal(mix - s->dc_x + s->dc_coef * s->dc_y);
        s->dc_x = mix;
        mix = s->dc_y;
        if (s->hpf_on) mix = dfx_bq_run(&s->hpf, &s->hpf_s, mix);
        /* the delay line keeps running when the chorus is off, so switching it on is clean */
        float yl = mix, yr = mix, wl, wr;
        juno_chorus_run(&s->ch, mix, 0.0f, s->sr, &wl, &wr);
        if (chorus != JC_OFF) {
            yl = 0.7f * (mix + wl);
            yr = 0.7f * (mix + wr);
        }
        l[n] = yl * level;
        r[n] = yr * level;
    }
}

const dfx_synth_def dfx_juno106 = {
    "juno106", "Juno-106",
    "Six voice DCO polysynth: saw, PWM pulse, sub, noise, 24 dB resonant filter, ADSR, HPF and BBD chorus",
    P_COUNT, params,
    (int)(sizeof presets / sizeof presets[0]), presets,
    sizeof(juno),
    init, reset, set_param, get_param, note_on, note_off, all_notes_off, render,
};
