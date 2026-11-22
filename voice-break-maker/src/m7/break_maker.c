/* BREAK MAKER: a drum break played as a running loop, locked to the Pērkons sequencer (M7).
 *
 * An extra voice-map engine ("break_maker"): voice-map builds one in an output's arena with
 * pk_bm_build(). The object is cVoice-compatible: the fields the firmware writes (algo, mode,
 * velocity, note, param pointers, mute, gain) sit at the stock offsets, and the vtable has the
 * four methods the firmware calls (trigger, init, update, render).
 *
 * The break (bm_shared.h): one bar in the AXI SRAM, cut into 16 slices, loaded by the M4. Its 16
 * slices are the track's 16 steps: the M4 sends each step of the voice's track ("break-maker-step",
 * before the step's triggers), and each step starts its slice, so the break plays as recorded at
 * whatever tempo (its speed: a slice per step, measured between the steps).
 *
 *   ALGO     1 RUN: the break loops; a trigger jumps: its slice (PARAM2) plays and the break
 *              carries on from there (to the track's first step, which starts it in place again)
 *            2 GENERATE: RUN, and the steps without a trigger may play another slice each bar
 *              (CHOPPAH's generator: anchors on the beats, repeats, fills in the 4th bar)
 *            3 CHOP: only triggers play, each its slice, silent after the gate
 *   TUNE     pitch, semitones (12 o'clock: as recorded); the step's note transposes it (36: none)
 *   DECAY    the gate: how much of each step sounds (RUN) / how long a hit lasts (CHOP);
 *            the top: no gate (the break runs on)
 *   PARAM1   by MODE: 1 ROLL (off, x2, x3, x4, x6, x8 within the step), 2 MANGLE (the chance a step
 *            plays a generated slice; GENERATE: the amount), 3 STUTTER (a window of the slice
 *            looped: off, 1/2 ... 1/32 of a step)
 *   PARAM2   the slice a trigger plays: fully left follows the step, then slices 1-16
 *   velocity how the step plays (bm_vel_kind): plain, ghost (-12 dB), reverse, roll x2 / x3 / x4,
 *            reverse + roll x2
 *
 * Pads (the page's row 2, while held: "break-maker"): rolls at 1/8, 1/16, 1/32 of the slice
 * sounding, reverse, freeze (a 1/4-step window looped), half speed, tape stop (to a halt over a
 * beat), gate (1/32 chops), random slices, filter throw (a low-pass falling over a beat), stutter
 * (a 1/32-step window). Pitch: repitch (speed and pitch together) or keep length (two overlapping
 * grains read at the pitch while the break runs at the tempo). */
#include <stdint.h>
#include "fw_m7.h"
#include "pk_ram.h"                 /* voice-map: PK_ARENA(out) */
#include "bm_shared.h"

#define SR              48000.0f
#define CTL             16          /* samples between control steps */
#define LEVEL           0.55f       /* the break's int16 into the output (peaks ~18k before the clip) */
#define CLIP_KNEE       15200.0f
#define CLIP_CEIL       16800.0f
#define XFADE_S         0.003f      /* a slice ending as another starts: 3 ms */
#define ATT_S           0.0006f     /* a slice's start: 0.6 ms */
#define GATE_S          0.002f      /* the gate closing: 2 ms */
#define STEP_DEF        6000.0f     /* a step's length before any are measured (120 BPM) */
#define GATE_TOP        0.95f       /* DECAY from here: no gate */
#define GHOST_GAIN      0.25f       /* -12 dB */
#define ROLL_DECAY      0.9f        /* each roll repeat this much quieter */
#define GRAIN_S         0.03f       /* keep length: 30 ms grains */
#define PI              3.14159265f
#define LOG2_E          1.4426950f
#define MAGIC           0x524b4d42u /* 'BMKR' */

/* switch values are 1, 0, 2 for panel positions 1, 2, 3 */
enum { POS1 = 1, POS2 = 0, POS3 = 2 };
enum { A_RUN, A_GEN, A_CHOP };
enum { M_ROLL, M_MANGLE, M_STUTTER };

/* ---- from the M4 (kept in the M7 host's RAM) ------------------------------------------- */

typedef struct {
    uint32_t gen;                       /* +1 per new step */
    uint32_t hit_gen;                   /* +1 per hit (a trigger of the sequencer's coming) */
    uint8_t step, first, last, flags;
    uint16_t bpm10, hit_p2;             /* the hit's PARAM2 */
} step_msg_t;
static step_msg_t steps_in[4];
static uint8_t set_flags[4], set_char[4], set_aud[4];
static uint16_t set_pads[4];

void pk_bm_on_step(const uint8_t *buf, uint32_t len)
{
    if (len < BM_STEP_LEN)
        return;
    step_msg_t *m = &steps_in[buf[0] & 3];
    if (buf[4] & BM_STEP_HIT) {
        m->hit_p2 = (uint16_t)(buf[7] | buf[8] << 8);
        m->hit_gen++;
        return;
    }
    m->step = buf[1] & 15;
    m->first = buf[2] & 15;
    m->last = buf[3] & 15;
    m->flags = buf[4];
    m->bpm10 = (uint16_t)(buf[5] | buf[6] << 8);
    m->gen++;
}

void pk_bm_on_settings(const uint8_t *buf, uint32_t len)
{
    if (len < BM_SET_LEN)
        return;
    int o = buf[0] & 3;
    set_flags[o] = buf[1];
    set_pads[o] = (uint16_t)(buf[2] | buf[3] << 8);
    set_char[o] = buf[4] < BM_NCHAR ? buf[4] : 0;
    set_aud[o] = buf[5];
}

/* ---- the object ------------------------------------------------------------------------ */

typedef struct {
    float pos;                          /* the break's frame (fractional) */
    float dir;                          /* +1 forward, -1 reverse */
    float lo, len;                      /* a looped window (stutter, freeze): from lo, len frames; len 0: none */
    float amp, amp_to, amp_k;           /* the level, where it goes, how fast (one pole) */
    float gain;                         /* ghost, roll repeats */
    float ratio;                        /* pitch (frequency ratio) */
    float gph;                          /* keep length: the grains' phase, 0-1 */
    int32_t revpad;                     /* the reverse pad was down when it started (or turned it) */
    int32_t gate;                       /* samples until it closes; -1 none */
    int32_t active;
} pl_t;

typedef struct {
    const cVoice_vtbl *vtbl;            /* +0x00 */
    uint8_t algo, mode, velocity, note; /* +0x04 .. +0x07 (written by the firmware) */
    uint32_t magic;                     /* +0x08 */
    int32_t *param[4];                  /* +0x0c TUNE, DECAY, PARAM1, PARAM2 */
    uint8_t base_pad[0xb8 - 0x1c];
    uint8_t mute;                       /* +0xb8 (written by the firmware) */
    uint8_t pad[3];
    int32_t gain;                       /* +0xbc mute fade, cVoice_tick_mute_fade */
    /* ---- ours (the tests read the first four) ---- */
    int32_t st_slice;                   /* +0xc0 the slice started last */
    int32_t st_count;                   /* +0xc4 slices started */
    int32_t st_step;                    /* +0xc8 the step playing (-1 stopped) */
    int32_t st_kind;                    /* +0xcc the last start's velocity kind */
    int32_t out;
    uint32_t seen_gen, break_gen, hit_seen;
    int32_t playing, step, first, last, bar;
    int32_t offset;                     /* RUN: a jump's slice - its step, until the first step */
    int32_t waiting;                    /* a step that is on: its trigger is coming */
    int32_t last_slice;
    float since;                        /* samples since the step began */
    float step_len;                     /* a step, samples (the last four steps' mean) */
    float iv[4];
    int32_t ivn;
    float speed;                        /* the break's frames per output sample at the tempo */
    /* knobs */
    float semis, gatek, p1;
    int32_t zone, algo_j, mode_j;
    /* settings, pads */
    int32_t keep, chr;
    uint32_t pads, pads_was;
    float tape;                         /* tape stop: 1 running .. 0 stopped */
    float halfk;
    /* rolls: the repeats left in this step, every how many samples, until the next */
    int32_t roll_left, roll_slice, roll_kind;
    float roll_every, roll_at, roll_gain, roll_ratio;
    /* the pad roll: its slice and rate */
    int32_t proll_slice;
    float proll_at;
    /* players: the one playing (cur) and the one fading out */
    pl_t pl[2];
    int32_t cur;
    /* the filter throw: cutoff (Hz, smoothed), TPT state-variable low-pass */
    float fc, f_g, f_k, f_s1, f_s2;
    float gate_amp;                     /* the gate pad's chop level */
    int32_t ctl;
    uint32_t rng;
    uint32_t aud_seen;                  /* the page's last audition */
} bm_t;

_Static_assert(__builtin_offsetof(bm_t, algo) == OFF_cVoice_algo, "algo");
_Static_assert(__builtin_offsetof(bm_t, mode) == OFF_cVoice_mode, "mode");
_Static_assert(__builtin_offsetof(bm_t, velocity) == OFF_cVoice_velocity, "velocity");
_Static_assert(__builtin_offsetof(bm_t, note) == OFF_cVoice_accent, "note (accent)");
_Static_assert(__builtin_offsetof(bm_t, param) == OFF_cVoice_param_ptrs, "param pointers");
_Static_assert(__builtin_offsetof(bm_t, mute) == OFF_cVoice_mute, "mute");
_Static_assert(__builtin_offsetof(bm_t, gain) == OFF_cVoice_gain, "gain");
_Static_assert(__builtin_offsetof(bm_t, st_slice) == 0xc0, "status (the tests read it)");
_Static_assert(sizeof(bm_t) <= PK_ARENA_SIZE, "fits a voice-map arena");

/* ---- small maths (no libm) ------------------------------------------------------------- */

#define INLINE static inline __attribute__((always_inline))

INLINE float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

INLINE float soft_knee(float x, float knee, float ceil)
{
    float m = x < 0.0f ? -x : x;
    if (m <= knee)
        return x;
    float r = ceil - knee, u = (m - knee) / r;
    float y = knee + r * u / (1.0f + u);
    return x < 0.0f ? -y : y;
}

static float exp2f_(float x)
{
    float r = x + 0.5f;
    float fi = (float)(int32_t)r;
    if (fi > r)
        fi -= 1.0f;
    float f = x - fi;
    float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f +
              f * (0.00133336f + f * (0.00015404f + f * 0.00001525f))))));
    union { uint32_t u; float v; } e = {(uint32_t)((int32_t)fi + 127) << 23};
    return p * e.v;
}

static float sin2pi(float p)
{
    p -= (float)(int32_t)p;
    if (p < 0.0f)
        p += 1.0f;
    float x = (p < 0.5f ? p : p - 1.0f) * (2.0f * PI);
    if (x > 0.5f * PI)
        x = PI - x;
    else if (x < -0.5f * PI)
        x = -PI - x;
    float x2 = x * x;
    return x * (1.0f - x2 * (1.0f / 6 - x2 * (1.0f / 120 - x2 * (1.0f / 5040))));
}

static float tan_pi(float f)            /* tan(pi f / SR) */
{
    float p = f * (0.5f / SR);
    return sin2pi(p) / sin2pi(p + 0.25f);
}

static float pole(float t)              /* one-pole coefficient for time constant t seconds */
{
    return 1.0f - exp2f_(-LOG2_E / (t * SR));
}

static uint32_t rnd(bm_t *k)
{
    uint32_t x = k->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    k->rng = x;
    return x;
}

static float rndf(bm_t *k)              /* 0 .. 1 */
{
    return (float)(rnd(k) >> 8) * (1.0f / 16777216.0f);
}

INLINE float knob(const bm_t *k, int i)
{
    return clampf((float)*k->param[i] * (1.0f / 4095.0f), 0.0f, 1.0f);
}

static int zone6(float x)               /* PARAM1's six zones: 0 off, 1-5 */
{
    int z = (int)(x * 6.0f);
    return z > 5 ? 5 : z;
}

/* ---- the break --------------------------------------------------------------------------- */

static int break_ok(void)
{
    return BM->magic == BM_MAGIC && BM->ready && BM->frames > 64;
}

INLINE float sample_at(const int16_t *pcm, uint32_t n, float pos)
{
    int32_t i = (int32_t)pos;
    float f = pos - (float)i;
    uint32_t a = (uint32_t)i, b = a + 1u;
    if (a >= n)
        a -= n;
    if (b >= n)
        b -= n;
    float x = (float)pcm[a];
    return x + ((float)pcm[b] - x) * f;
}

INLINE float wrapf(float p, float n)
{
    if (p >= n)
        p -= n;
    else if (p < 0.0f)
        p += n;
    return p;
}

/* ---- control ------------------------------------------------------------------------------ */

static void read_knobs(bm_t *k)
{
    float t = knob(k, 0) - 0.5f;
    float s = t * 24.0f;
    k->semis = (float)(int32_t)(s + (s < 0.0f ? -0.5f : 0.5f));
    k->gatek = knob(k, 1);
    k->p1 = knob(k, 2);
    k->zone = bm_p2_zone(*k->param[3]);
    k->algo_j = k->algo == POS1 ? A_RUN : (k->algo == POS3 ? A_CHOP : A_GEN);
    k->mode_j = k->mode == POS1 ? M_ROLL : (k->mode == POS3 ? M_STUTTER : M_MANGLE);
    k->keep = set_flags[k->out] & BM_SET_KEEP;
    k->chr = set_char[k->out];
    k->pads = set_pads[k->out];
}

static void update(cVoice *v)
{
    bm_t *k = (bm_t *)v;
    if (k->magic == MAGIC)
        read_knobs(k);
}

/* the gate in samples for a start (RUN: a share of the step; CHOP: up to four steps); -1 none */
static int32_t gate_of(const bm_t *k, float span)
{
    float d = k->gatek;
    if (d >= GATE_TOP)
        return -1;
    float x = d * (1.0f / GATE_TOP);
    if (k->algo_j == A_CHOP)
        return (int32_t)(k->step_len * 0.05f * exp2f_(x * 6.32f));     /* 0.05 to 4 steps */
    return (int32_t)(span * (0.05f + 0.95f * x));
}

/* playing: a slice a step; stopped: the break's own speed */
static void set_speed(bm_t *k)
{
    if (!break_ok())
        k->speed = 1.0f;
    else if (k->playing)
        k->speed = (float)BM->frames * (1.0f / BM_SLICES) / k->step_len;
    else
        k->speed = (float)BM->rate * (1.0f / SR);
}

/* start slice s on a fresh player (the one before fades out). kind: bm_vel_kind; extra semitones */
static void start(bm_t *k, int s, int kind, float extra, float span)
{
    if (!break_ok())
        return;
    s &= BM_SLICES - 1;
    pl_t *old = &k->pl[k->cur];
    if (old->active) {
        old->amp_to = 0.0f;
        old->amp_k = pole(XFADE_S);
        old->gate = -1;
    }
    k->cur ^= 1;
    pl_t *p = &k->pl[k->cur];
    int rev = kind == BM_V_REVERSE || kind == BM_V_REVROLL;
    p->revpad = (int32_t)(k->pads >> BM_PAD_REVERSE & 1);
    if (p->revpad)
        rev = !rev;
    p->dir = rev ? -1.0f : 1.0f;
    p->pos = rev ? (float)BM->slice[s + 1] - 1.0f : (float)BM->slice[s];
    p->amp = 0.0f;
    p->amp_to = 1.0f;
    p->amp_k = pole(ATT_S);
    p->gain = kind == BM_V_GHOST ? GHOST_GAIN : 1.0f;
    p->ratio = exp2f_((k->semis + extra) * (1.0f / 12.0f));
    p->gph = 0.0f;
    p->gate = gate_of(k, span);
    p->len = 0.0f;
    p->active = 1;
    /* stutter (MODE 3): a window of the slice looped */
    if (k->mode_j == M_STUTTER) {
        int z = zone6(k->p1);
        if (z > 0) {
            float w = k->step_len * k->speed / (float)(1 << z);     /* 1/2 .. 1/32 of a step */
            p->len = w < 8.0f ? 8.0f : w;
            p->lo = rev ? p->pos - p->len : p->pos;
        }
    }
    k->last_slice = s;
    k->st_slice = s;
    k->st_kind = kind;
    k->st_count++;
}

/* rolls within the step: n hits, the first now */
static void roll_setup(bm_t *k, int n, int s, int kind, float extra)
{
    k->roll_left = n - 1;
    k->roll_every = k->step_len / (float)n;
    k->roll_at = k->roll_every;
    k->roll_slice = s;
    k->roll_kind = kind;
    k->roll_ratio = extra;
    k->roll_gain = 1.0f;
}

/* The live variation: f is the step's own slice; amount 0..1. Its rules (anchors on the beats, a
 * repeat or the slice next door, a fill in the phrase's fourth bar) follow the generator of CHOPPAH,
 * the breakbeat chopper for the M-VAVE FM-1 by Laurens Profittlich (MPL-2.0). */
static int generate(bm_t *k, int f, float cx)
{
    int in_bar = k->step & 15;
    if (k->bar % 4 == 3)
        cx += 0.5f * (1.0f - cx);       /* the phrase's 4th bar: a fill */
    if (cx <= 0.0f)
        return f;
    if ((in_bar & 3) == 0 && rndf(k) < 0.7f)
        return f;                       /* the beats keep theirs */
    if (rndf(k) >= cx)
        return f;
    if (k->last_slice >= 0 && rndf(k) < 0.3f) {     /* a repeat, or the slice next door */
        float r = rndf(k);
        int s = r < 0.5f ? k->last_slice : k->last_slice + (r < 0.75f ? -1 : 1);
        return s & (BM_SLICES - 1);
    }
    return (int)(rnd(k) % BM_SLICES);
}

static const float CHAR_AMOUNT[BM_NCHAR] = {0.1f, 0.15f, 0.2f, 0.2f, 0.25f, 0.4f, 0.3f, 0.65f};

static int pads_own(const bm_t *k)      /* a pad that owns the voice: the steps stay quiet */
{
    return (k->pads & (1u << BM_PAD_ROLL8 | 1u << BM_PAD_ROLL16 | 1u << BM_PAD_ROLL32 |
                       1u << BM_PAD_FREEZE | 1u << BM_PAD_TAPESTOP | 1u << BM_PAD_STUTTER)) != 0u;
}

/* a step plays its slice: s, how (kind), transposed (extra); rolls by velocity or PARAM1 */
static void play_step(bm_t *k, int s, int kind, float extra)
{
    if (pads_own(k))
        return;
    if (k->pads >> BM_PAD_RANDOM & 1)
        s = (int)(rnd(k) % BM_SLICES);
    int n = kind == BM_V_ROLL2 || kind == BM_V_REVROLL ? 2 : (kind == BM_V_ROLL3 ? 3 : (kind == BM_V_ROLL4 ? 4 : 1));
    if (k->mode_j == M_ROLL) {
        static const uint8_t RN[6] = {1, 2, 3, 4, 6, 8};
        int r = RN[zone6(k->p1)];
        n = r > n ? r : n;
    }
    float span = k->step_len / (float)n;
    start(k, s, kind, extra, span);
    if (n > 1)
        roll_setup(k, n, s, kind, extra);
    else
        k->roll_left = 0;
}

/* the step's own slice for a step without a trigger (RUN, GENERATE) */
static void step_auto(bm_t *k)
{
    int f = (k->step + k->offset) & (BM_SLICES - 1);
    if (k->algo_j == A_GEN)
        f = generate(k, f, k->mode_j == M_MANGLE ? k->p1 : CHAR_AMOUNT[k->chr]);
    else if (k->mode_j == M_MANGLE)
        f = generate(k, f, k->p1);
    play_step(k, f, BM_V_PLAIN, 0.0f);
}

/* a new step message: the tempo, then the step's slice (unless its trigger is coming) */
static void sync_step(bm_t *k)
{
    step_msg_t *m = &steps_in[k->out];
    uint32_t g = m->gen;
    if (g == k->seen_gen)
        return;
    k->seen_gen = g;
    if (!(m->flags & BM_STEP_PLAYING)) {    /* stopped: the break fades out */
        k->playing = 0;
        k->st_step = -1;
        k->waiting = 0;
        k->roll_left = 0;
        for (int i = 0; i < 2; i++) {
            k->pl[i].amp_to = 0.0f;
            k->pl[i].amp_k = pole(GATE_S);
        }
        return;
    }
    if (!k->playing) {                  /* started: its tempo from the message, until measured */
        k->playing = 1;
        k->bar = 0;
        k->ivn = 0;
        float bpm = (float)m->bpm10 * 0.1f;
        if (bpm >= 20.0f && bpm <= 400.0f)
            k->step_len = SR * 15.0f / bpm;
    } else {                            /* the last four steps' mean (shuffle evens out) */
        float d = k->since;
        if (d > 0.25f * k->step_len && d < 4.0f * k->step_len) {
            k->iv[k->ivn & 3] = d;
            k->ivn++;
            int n = k->ivn < 4 ? k->ivn : 4;
            float s = 0.0f;
            for (int i = 0; i < n; i++)
                s += k->iv[i];
            k->step_len = s / (float)n;
        }
    }
    k->since = 0.0f;
    k->step = m->step;
    k->first = m->first;
    k->last = m->last;
    k->st_step = k->step;
    if (k->step == k->first) {
        k->bar++;
        k->offset = 0;                  /* the loop's start: the break in place again */
    }
    set_speed(k);
    k->waiting = (m->flags & BM_STEP_ON) != 0;
    if (!k->waiting && k->algo_j != A_CHOP)
        step_auto(k);
}

/* ---- notes -------------------------------------------------------------------------------- */

static void trigger(cVoice *v)
{
    bm_t *k = (bm_t *)v;
    if (k->magic != MAGIC)
        return;
    read_knobs(k);
    sync_step(k);                       /* the step message comes first */
    int kind = bm_vel_kind(k->velocity);
    float extra = (float)((int)(k->note ? k->note : 36) - 36);
    int s, zone = k->zone;
    step_msg_t *m = &steps_in[k->out];
    if (m->hit_gen != k->hit_seen) {    /* the sequencer's: the step's own PARAM2 */
        k->hit_seen = m->hit_gen;
        zone = bm_p2_zone(m->hit_p2);
    }
    if (zone > 0)
        s = zone - 1;
    else
        s = k->playing ? k->step : (k->last_slice >= 0 ? k->last_slice : 0);
    if (k->algo_j == A_CHOP && k->mode_j == M_MANGLE)
        s = generate(k, s, k->p1);
    if (k->playing && k->algo_j != A_CHOP)
        k->offset = (s - k->step) & (BM_SLICES - 1);
    if (!k->playing)
        set_speed(k);
    k->waiting = 0;
    play_step(k, s, kind, extra);
}

static void init(cVoice *v)
{
    bm_t *k = (bm_t *)v;
    k->magic = MAGIC;
    read_knobs(k);
}

/* ---- audio -------------------------------------------------------------------------------- */

/* every CTL samples: rolls, pads, the filter throw */
static void control(bm_t *k)
{
    uint32_t pads = k->pads, was = k->pads_was;
    k->pads_was = pads;
    /* the pad roll: the slice sounding when it went down, again at its rate */
    uint32_t rolls = pads & 7u;
    if (rolls) {
        float every = k->step_len * (rolls & 4u ? 0.5f : (rolls & 2u ? 1.0f : 2.0f));
        if (!(was & 7u)) {
            k->proll_slice = k->last_slice >= 0 ? k->last_slice : 0;
            k->proll_at = 0.0f;
        }
        k->proll_at -= (float)CTL;
        if (k->proll_at <= 0.0f) {
            k->proll_at += every;
            start(k, k->proll_slice, BM_V_PLAIN, 0.0f, every);
        }
    }
    /* freeze / stutter: the player sounding loops a window from where it is */
    pl_t *p = &k->pl[k->cur];
    uint32_t loops = pads & (1u << BM_PAD_FREEZE | 1u << BM_PAD_STUTTER);
    if (loops && !(was & loops) && p->active) {
        float w = k->step_len * k->speed * (pads >> BM_PAD_STUTTER & 1 ? 1.0f / 32.0f : 0.25f);
        p->len = w < 8.0f ? 8.0f : w;
        p->lo = p->dir > 0.0f ? p->pos : p->pos - p->len;
        p->gate = -1;
    } else if (!loops && (was & (1u << BM_PAD_FREEZE | 1u << BM_PAD_STUTTER))) {
        p->len = 0.0f;
    }
    /* reverse: the ones sounding turn round too (unless they started with it already) */
    int32_t rp = (int32_t)(pads >> BM_PAD_REVERSE & 1);
    for (int i = 0; i < 2; i++)
        if (k->pl[i].active && k->pl[i].revpad != rp) {
            k->pl[i].revpad = rp;
            k->pl[i].dir = -k->pl[i].dir;
        }
    /* tape stop: to a halt over a beat; let go: running at once */
    if (pads >> BM_PAD_TAPESTOP & 1) {
        k->tape -= (float)CTL / (4.0f * k->step_len);
        if (k->tape < 0.0f)
            k->tape = 0.0f;
    } else {
        k->tape = 1.0f;
    }
    k->halfk = pads >> BM_PAD_HALF & 1 ? 0.5f : 1.0f;
    /* the gate pad: 1/32 chops (the first half of each) */
    float ph = k->since / (k->step_len * 0.5f);
    ph -= (float)(int32_t)ph;
    float gt = pads >> BM_PAD_GATE & 1 && ph >= 0.5f ? 0.0f : 1.0f;
    k->gate_amp += (gt - k->gate_amp) * 0.6f;
    /* the filter throw: falling to 250 Hz over a beat; let go: open in ~30 ms */
    float fto = pads >> BM_PAD_FILTER & 1 ? 250.0f : 20000.0f;
    if (fto < k->fc)
        k->fc *= exp2f_(-6.3f * (float)CTL / (4.0f * k->step_len));
    else
        k->fc *= exp2f_(0.03f * (float)CTL);
    k->fc = clampf(k->fc, 250.0f, 20000.0f);
    if (k->fc < 19000.0f) {
        float g = tan_pi(k->fc > 16000.0f ? 16000.0f : k->fc);
        k->f_g = g;
        k->f_k = 1.0f;                  /* Q 1: a little bite */
    }
}

static int16_t render(cVoice *v)
{
    bm_t *k = (bm_t *)v;
    fw_tick_mute_fade(v);
    if (k->magic != MAGIC)
        return 0;
    sync_step(k);
    if (!break_ok()) {
        k->pl[0].active = k->pl[1].active = 0;
        return 0;
    }
    if (++k->ctl >= CTL) {
        k->ctl = 0;
        read_knobs(k);
        if (set_aud[k->out] != k->aud_seen) {   /* the page plays a slice */
            k->aud_seen = set_aud[k->out];
            int a = (int)(k->aud_seen & 31u);
            if (a > 0 && a <= BM_SLICES) {
                if (!k->playing)
                    set_speed(k);
                k->roll_left = 0;
                start(k, a - 1, BM_V_PLAIN, 0.0f, k->step_len);
            }
        }
        control(k);
    }
    k->since += 1.0f;
    /* rolls within the step */
    if (k->roll_left > 0) {
        k->roll_at -= 1.0f;
        if (k->roll_at <= 0.0f) {
            k->roll_at += k->roll_every;
            k->roll_left--;
            start(k, k->roll_slice, k->roll_kind, k->roll_ratio, k->roll_every);
            k->pl[k->cur].gain *= k->roll_gain = k->roll_gain * ROLL_DECAY;
        }
    }
    const int16_t *pcm = (const int16_t *)(uintptr_t)BM->pcm;
    uint32_t n = BM->frames;
    float nf = (float)n, base = k->speed * k->tape * k->halfk;
    float y = 0.0f;
    for (int i = 0; i < 2; i++) {
        pl_t *p = &k->pl[i];
        if (!p->active)
            continue;
        float inc = base * (k->keep ? 1.0f : p->ratio);
        float x;
        if (k->keep && p->ratio != 1.0f && base > 0.0f) {
            /* keep length: two grains half a grain apart, each reading at the pitch from where
             * the break is, faded in and out (triangles) */
            float glen = GRAIN_S * SR, g = p->gph, drift = (p->ratio - 1.0f) * base * glen * p->dir;
            float g2 = g + 0.5f;
            if (g2 >= 1.0f)
                g2 -= 1.0f;
            float w1 = g < 0.5f ? 2.0f * g : 2.0f - 2.0f * g, w2 = 1.0f - w1;
            float a = wrapf(p->pos + drift * (g - 0.5f), nf), b = wrapf(p->pos + drift * (g2 - 0.5f), nf);
            x = sample_at(pcm, n, a) * w1 + sample_at(pcm, n, b) * w2;
            p->gph = g + 1.0f / glen;
            if (p->gph >= 1.0f)
                p->gph -= 1.0f;
        } else {
            x = sample_at(pcm, n, p->pos);
        }
        p->amp += (p->amp_to - p->amp) * p->amp_k;
        y += x * p->amp * p->gain;
        p->pos += inc * p->dir;
        if (p->len > 0.0f) {            /* a looped window */
            if (p->pos >= p->lo + p->len)
                p->pos -= p->len;
            else if (p->pos < p->lo)
                p->pos += p->len;
        }
        p->pos = wrapf(p->pos, nf);     /* runs on round the bar */
        if (p->gate >= 0 && --p->gate < 0) {
            p->amp_to = 0.0f;
            p->amp_k = pole(GATE_S);
        }
        if (p->amp_to == 0.0f && p->amp < 0.5f)
            p->active = 0;
    }
    y *= LEVEL * k->gate_amp * k->tape;     /* a tape stopping fades as it slows */
    if (k->fc < 19000.0f) {             /* the filter throw (TPT state-variable low-pass) */
        float g = k->f_g, kk = k->f_k;
        float hp = (y - (kk + g) * k->f_s1 - k->f_s2) / (1.0f + g * (kk + g));
        float bp = g * hp + k->f_s1;
        k->f_s1 = g * hp + bp;
        float lp = g * bp + k->f_s2;
        k->f_s2 = g * bp + lp;
        y = lp;
    }
    y = soft_knee(y, CLIP_KNEE, CLIP_CEIL);
    return (int16_t)(((int32_t)y * k->gain) >> 16);
}

/* ---- vtable and construction ---------------------------------------------------------- */

static const char bm_name[] = "11cBreakMaker";
static const struct { const void *vptr; const char *name; } bm_typeinfo = {0, bm_name};
static const struct {
    const void *typeinfo;
    cVoice_vtbl vtbl;
} bm_vtable = {&bm_typeinfo, {trigger, init, update, render}};

/* voice-map: build a BREAK MAKER in `obj` (an output's arena) for output `out` */
void pk_bm_build(uint8_t *obj, unsigned out)
{
    uint32_t *w = (uint32_t *)obj;
    for (unsigned i = 0; i < (sizeof(bm_t) + 3) / 4; i++)
        w[i] = 0;
    bm_t *k = (bm_t *)obj;
    k->vtbl = &bm_vtable.vtbl;
    for (int i = 0; i < 4; i++)
        k->param[i] = &FW_PARAMS[out][i];
    k->note = 36;
    k->out = (int32_t)(out & 3);
    k->magic = MAGIC;
    k->seen_gen = steps_in[k->out].gen;
    k->hit_seen = steps_in[k->out].hit_gen;
    k->aud_seen = set_aud[k->out];
    k->st_step = -1;
    k->last_slice = -1;
    k->step_len = STEP_DEF;
    k->tape = k->halfk = k->gate_amp = 1.0f;
    k->fc = 20000.0f;
    k->rng = 0x9e3779b9u ^ (out * 0x85ebca6bu);
    set_speed(k);
    init((cVoice *)k);
}
