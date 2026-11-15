/* Synth Lab: a mono synth voice (M7), from a synth I built on iOS: three oscillators with hard
 * sync, a wavefolder after the mix, a filter with its own envelope, eight LFOs.
 *
 * An extra voice-map engine ("synth_lab"): voice-map builds one in an output's arena with
 * pk_sy_build(). The object is cVoice-compatible: the fields the firmware writes (algo,
 * mode, velocity, note, param pointers, mute, gain) sit at the stock offsets, and the
 * vtable has the four methods the firmware calls (trigger, init, update, render).
 *
 *   TUNE (param 0)    the note, in the sound's scale and root, C2 to C6; the trigger note
 *                     transposes by (note - 36) semitones. The lowest oscillator plays it
 *   DECAY (param 1)   GATE: how long the note holds before its release, 0.05-2 s; from 95%
 *                     of its travel on: drone
 *   PARAM1 (param 2)  filter cutoff, relative to the sound (12 o'clock: the sound's own)
 *   PARAM2 (param 3)  ALGO's job, relative to the sound: resonance (position 1), filter
 *                     envelope amount (2), sync: the slaves' pitch (3)
 *   MODE              drive after the filter: clean (position 1), warm (2), hot (3)
 *
 * Sync and sweep play as the sound sets them: an oscillator with its sync on is reset by
 * oscillator 1, and its sweep depth (if any) has the filter envelope sweep its pitch.
 *
 * The sound is 88 small values (sl_sound.h), as the kit saves it: a preset, or what the M4
 * sends ("synth-lab"). The chain: oscillators (minBLEP-corrected steps; slaves reset by the
 * master; a silent one only keeps its phase) -> mix -> fold -> filter -> drive (MODE) (trapezoidal state
 * variable, saturating; or a ladder) -> amp envelope -> DC blocker -> limiter -> the level for DRIVE. LFOs,
 * envelopes' filter side, glide, pitch and the filter's coefficients work every CTL samples.
 */
#include <stdint.h>
#include "fw_m7.h"
#include "pk_ram.h"                 /* voice-map: PK_ARENA(out) */
#include "sl_sound.h"
#include "sl_presets.h"
#include "sl_minblep.h"
#include "sl_sine.h"

#define SR              48000.0f
#define CTL             16          /* samples between control steps (LFOs, filter, pitch) */
#define NOTE_REF        36          /* trigger note meaning "no transposition" */
#define BASE_NOTE       36          /* TUNE at minimum, root C: C2 */
#define OCTAVES         4
#define HYST            0.2f        /* TUNE: hysteresis while a note sounds, of a step each side */
#define FOLLOW_SAMPLES  1200        /* 25 ms: a new note follows TUNE at once (as Akords) */
#define SETTLE_SAMPLES  960         /* later, a new TUNE step must hold 20 ms */
#define LEVEL           6750.0f     /* peak scale: 1.5 dB under the stock tonal voices' 8000, so a
                                     * saw's peaks (its band-limited overshoot) stay under CLIP_KNEE */
#define LIM_THR         9000.0f     /* the output limiter: peaks held near this */
#define LIM_ATT         0.067f      /* 0.3 ms attack (no instant grab: it would square the peaks) */
#define LIM_REL         0.999861f   /* 150 ms release */
#define CLIP_KNEE       8500.0f     /* after the limiter, a soft clip rounds what its attack lets */
#define CLIP_CEIL       10000.0f    /*   through: untouched below the knee, never above the ceiling */
#define SAT_KNEE        3.0f        /* the band state: resonance pushed hard saturates (an analog */
#define SAT_CEIL        6.0f        /*   filter's growl) instead of growing on */
#define LP2_K           1.0f        /* LP24's second stage: damping 1, no resonance of its own */
#define LAD_K           4.4f        /* the ladder: feedback at full resonance (4: it rings on its own) */
#define LAD_COMP        0.5f        /* its input lifted by 1 + LAD_COMP x feedback: the bass thins
                                     * as the resonance rises (the ladder's sound), but not all of it */
#define LAD_DRIVE       0.5f        /* its saturation: 2 tanh(x / 2), so +-2 at most */
#define VEL_CUT_OCT     1.0f        /* velocity: 127 as the sound, 0 an octave darker */
#define DC_R            0.9995f     /* DC blocker pole (~4 Hz) */
#define DRONE_AT        0.95f
#define DETENT          0.02f       /* +-2% around 12 o'clock is the sound's own value */
#define MOVED           (82.0f / 4095.0f)   /* knob travel (2%) that hands PARAM2 a new value */
#define DRIVE_DB        3.0f        /* the level for DRIVE: +3 dB at none, -3 dB at full */
#define DRV_SMOOTH      0.25f       /* MODE's drive glides over ~4 control steps (no click) */
#define DRIVE_SMOOTH    (1.0f / 960.0f)
#define PI              3.14159265f
#define LOG2_E          1.4426950f
#define MAGIC           0x534c4142u /* 'SLAB' */
#define NOSC            3

/* the 5-bit scales (the engine's side of sl_sound.h) */
#define T_OCT           12.965784f  /* envelope times: 1 ms .. 8 s */
#define LFO_OCT         8.643856f   /* LFO rates: 0.05 .. 20 Hz; glide: 5 ms .. 2 s */
#define CUT_MIN         20.0f       /* cutoff: 20 Hz .. 20 kHz */
#define CUT_OCT         10.0f
#define CUT_FIELD       0.9f        /* the sound's cutoff (0-31) spans the first 9 octaves, 20 Hz to
                                     * 10 kHz, as the earlier filter played it: PARAM1 opens the rest */

/* switch values are 1, 0, 2 for panel positions 1, 2, 3 */
enum { POS1 = 1, POS2 = 0, POS3 = 2 };
enum { R_RES, R_ENV, R_SYNC, NROLE };
enum { IDLE, ATTACK, DECAY, SUSTAIN, RELEASE };
enum { L_PITCH0, L_VOL0 = 3, L_CUT = 6, L_FOLD, NLFO };
enum { C_A1, C_A2, C_A3, C_B1, C_B2, C_B3, C_G1, NCOEF };     /* the filter's coefficients */
enum { C_LG = C_A1, C_LD, C_LK };       /* the ladder's, in the resonant stage's places: pole G, 1 / (1 + k G^4), k */

/* MODE's drive, by switch value (POS2 = 0 warm, POS1 = 1 clean, POS3 = 2 hot): the gain into a
 * cubic soft clip, its bias (asymmetry: even harmonics, a tube's warmth) and the wet share. Its
 * level follows the dry sound's (DRV_AVG). */
static const float DRV_GAIN[3] = {3.5f, 1.0f, 9.0f};
static const float DRV_BIAS[3] = {0.3f, 0.0f, 0.15f};
static const float DRV_WET[3] = {1.0f, 0.0f, 1.0f};
#define DRV_AVG         (1.0f / 2400.0f)    /* the levels before and after it: 50 ms averages */

static const float OCT_RATIO[8] = {0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 8.0f};
/* tempo-synced LFOs: beats per cycle, longest to shortest (RATE up = faster) */
static const float DIV_BEATS[17] = {16.0f, 8.0f, 4.0f, 3.0f, 2.0f, 4.0f / 3, 1.5f, 1.0f, 2.0f / 3, 0.75f,
                                    0.5f, 1.0f / 3, 0.375f, 0.25f, 1.0f / 6, 0.125f, 0.0625f};

/* the scales (as Akords' list, without Major Pentatonic) */
static const int8_t SC_MAJOR[] = {0, 2, 4, 5, 7, 9, 11};
static const int8_t SC_MINOR[] = {0, 2, 3, 5, 7, 8, 10};
static const int8_t SC_MIN_PENT[] = {0, 3, 5, 7, 10};
static const int8_t SC_HARM_MINOR[] = {0, 2, 3, 5, 7, 8, 11};
static const int8_t SC_DORIAN[] = {0, 2, 3, 5, 7, 9, 10};
static const int8_t SC_PHRYGIAN[] = {0, 1, 3, 5, 7, 8, 10};
static const int8_t SC_MIXOLYDIAN[] = {0, 2, 4, 5, 7, 9, 10};
static const int8_t SC_BLUES[] = {0, 3, 5, 6, 7, 10};
static const int8_t SC_PHRYG_DOM[] = {0, 1, 4, 5, 7, 8, 10};
static const int8_t SC_CHROMATIC[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const struct { const int8_t *n; uint8_t len; } SCALES[SL_NSCALE] = {
    {SC_MAJOR, 7}, {SC_MINOR, 7}, {SC_MIN_PENT, 5}, {SC_HARM_MINOR, 7}, {SC_DORIAN, 7},
    {SC_PHRYGIAN, 7}, {SC_MIXOLYDIAN, 7}, {SC_BLUES, 6}, {SC_PHRYG_DOM, 7}, {SC_CHROMATIC, 12},
};

#ifndef SY_DEFAULT_PRESETS          /* each output's preset until the M4 sends a sound */
#define SY_DEFAULT_PRESETS {0, 4, 11, 13}
#endif
static const uint8_t DEFAULT_PRESET[4] = SY_DEFAULT_PRESETS;

/* ---- from the M4 ------------------------------------------------------------------------
 * "synth-lab-live": the tempo x 10 (2 bytes, little-endian), then each output's DRIVE (0-255).
 * Kept in the M7 host's RAM: 8 bytes. */
static float bpm;                       /* 0: none yet (120 BPM, DRIVE at 0 dB) */
static uint8_t drive[4];

void pk_sy_on_live(const uint8_t *buf, uint32_t len)
{
    if (len >= 2) {
        float t = (float)(buf[0] | buf[1] << 8) * 0.1f;
        if (t >= 20.0f && t <= 400.0f)
            bpm = t;
    }
    if (len >= 6)
        for (int i = 0; i < 4; i++)
            drive[i] = buf[2 + i];
}

/* "synth-lab": each output's last sound (pk_sy_on_sound) */
#define F_HELD          0x10
#define F_KEEP          0x20
static uint8_t stash[4][SL_PACKED + 1] __attribute__((aligned(4)));    /* [0] flags, [1..] sound */
static uint32_t stashed;                /* outputs (bits) with a new sound; << 4: ever sent */

/* ---- the object ---------------------------------------------------------------------------- */

typedef struct {
    float ph, dt;                       /* phase 0-1, step per sample */
    float g, g_step;                    /* gain (level x volume LFO), ramped over a control step */
    float pw;
    int32_t wave;
} osc_t;

typedef struct {
    float lvl;
    float a_inc, d_dec, r_dec;          /* linear */
    float a_c, d_c, r_c;                /* exponential */
    float sus;
    int32_t stage, exp;
} env_t;

typedef struct {
    float ph, val, held;                /* phase, output 0-1, S&H value */
} lfo_t;

typedef struct {
    const cVoice_vtbl *vtbl;            /* +0x00 */
    uint8_t algo, mode, velocity, note; /* +0x04 .. +0x07 (written by the firmware) */
    uint32_t magic;                     /* +0x08 */
    int32_t *param[4];                  /* +0x0c TUNE, DECAY, PARAM1, PARAM2 */
    uint8_t base_pad[0xb8 - 0x1c];
    uint8_t mute;                       /* +0xb8 (written by the firmware) */
    uint8_t pad[3];
    int32_t gain;                       /* +0xbc mute fade, cVoice_tick_mute_fade */
    /* ---- ours ---- */
    uint8_t v[SL_NFIELD];               /* +0xc0: the sound playing (the tests read it) */
    uint8_t base[SL_NFIELD];            /* the sound as saved (Mutate plays nudges of it) */
    float role[NROLE];                  /* PARAM2's three values, knob positions 0-1 */
    int32_t out;
    int32_t cur_role, armed;
    float ref2;
    int32_t held;                       /* the M4 holds PARAM2 (a kit load: positions coming) */
    /* knobs */
    float k_cut;
    int32_t gate_samples;               /* -1: drone */
    /* the decoded sound (decode()) */
    float ratio[NOSC], level[NOSC], sweep[NOSC];
    int32_t synced[NOSC], ratio_mode[NOSC];
    float reg;                          /* register: 1 / the lowest oscillator's ratio */
    float norm;                         /* mix scale: 1 / the levels' sum (at least 1) */
    float trim;                         /* the sound's level (after the fold: it doesn't drive it) */
    float lfo_rate[NLFO], lfo_depth[NLFO];
    int32_t lfo_shape[NLFO], lfo_sync[NLFO];
    float u_cut, u_res, u_env;          /* the sound's own positions for PARAM1 / PARAM2 */
    float keytrack;
    float fold_amt, fold_bias, fold_push;
    int32_t fold_on, filt_on, ftype;
    float glide_a;                      /* per control step; 0: no glide */
    int32_t legato, lfo_free;
    /* the note */
    int32_t tune_step, tune_scale, tune_since, elapsed, released, sounding;
    float note_midi;                    /* the note's pitch (MIDI, may be fractional while gliding) */
    float pitch, pitch_to;              /* log2 Hz of the note now, and where it glides */
    float vel;
    /* the block: CTL samples worked out together at each control step, then played one by one */
    int32_t bi;                         /* the next sample of buf to play; CTL: a new block */
    int32_t stop;                       /* the amp envelope ended in this block */
    float buf[CTL];                     /* the block's signal, stage by stage; at the end its output */
    float syn[CTL];                     /* the master's wraps: the time from each to the next sample, or -1 */
    float ring[CTL + MB_TAPS];          /* minBLEP residuals (all oscillators', x their gains) */
    int32_t live;                       /* oscillators (bits) with a level: the others only keep their phase */
    lfo_t lfo[NLFO];
    uint32_t rng;
    osc_t o[NOSC];
    env_t aenv, fenv;
    float fenv_lvl;
    /* filter */
    float low, band, low2, band2, one;  /* trapezoidal integrators' states; the one-pole's */
    float lad[4];                       /* the ladder's four poles */
    float c[NCOEF], c_step[NCOEF];      /* coefficients (the resonant stage's ramped over a control step) */
    float q, q_step, in_gain;           /* damping (1 / Q) */
    /* fold */
    float fold, fold_step, prev;
    /* MODE's drive: gain, make-up, wet (smoothed); the soft clip's last input and antiderivative */
    float drv_g, drv_m, drv_w, drv_x1, drv_F1;
    float drv_in, drv_out;              /* mean squares before and after the clip (its make-up) */
    float drv_b, drv_c;                 /* the bias, and the clip's output at it (taken off: no DC) */
    /* out */
    float dc_x, dc_y, lim, dgain, dgain_t;
} sy_t;

_Static_assert(__builtin_offsetof(sy_t, algo) == OFF_cVoice_algo, "algo");
_Static_assert(__builtin_offsetof(sy_t, mode) == OFF_cVoice_mode, "mode");
_Static_assert(__builtin_offsetof(sy_t, velocity) == OFF_cVoice_velocity, "velocity");
_Static_assert(__builtin_offsetof(sy_t, note) == OFF_cVoice_accent, "note (accent)");
_Static_assert(__builtin_offsetof(sy_t, param) == OFF_cVoice_param_ptrs, "param pointers");
_Static_assert(__builtin_offsetof(sy_t, mute) == OFF_cVoice_mute, "mute");
_Static_assert(__builtin_offsetof(sy_t, gain) == OFF_cVoice_gain, "gain");
_Static_assert(__builtin_offsetof(sy_t, v) == 0xc0, "the sound (the tests read it)");
_Static_assert(sizeof(sy_t) <= PK_ARENA_SIZE, "fits a voice-map arena");

/* ---- small maths (no libm) --------------------------------------------------------- */

#define INLINE static inline __attribute__((always_inline))

INLINE float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

INLINE float absf(float x)
{
    return x < 0.0f ? -x : x;
}

/* identity up to `knee`, then bends smoothly towards `ceil` (never reaches it) */
INLINE float soft_knee(float x, float knee, float ceil)
{
    float m = x < 0.0f ? -x : x;
    if (m <= knee)
        return x;
    float r = ceil - knee, u = (m - knee) / r;
    float y = knee + r * u / (1.0f + u);
    return x < 0.0f ? -y : y;
}

/* the drive's cubic soft clip, u - u^3 / 3 up to |u| = 1, then flat at +-2/3, and its
 * antiderivative (for first-order antiderivative antialiasing) */
INLINE float clip3(float u)
{
    if (u > 1.0f)
        return 2.0f / 3.0f;
    if (u < -1.0f)
        return -2.0f / 3.0f;
    return u - u * u * u * (1.0f / 3.0f);
}

INLINE float clip3_F(float u)
{
    float a = absf(u);
    if (a > 1.0f)
        return (2.0f / 3.0f) * a - 0.25f;
    float u2 = u * u;
    return u2 * (0.5f - u2 * (1.0f / 12.0f));
}

/* 2^x, |x| < 100 (as voice-zibens) */
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

/* 1 / sqrt(x), x > 0 */
INLINE float rsqrtf_(float x)
{
    union { float f; uint32_t u; } v = {x};
    v.u = 0x5f3759dfu - (v.u >> 1);
    float y = v.f;
    y *= 1.5f - 0.5f * x * y * y;
    return y * (1.5f - 0.5f * x * y * y);
}

/* tan(x) for 0 <= x <= ~1.2 (Pade) */
INLINE float tanf_(float x)
{
    float x2 = x * x;
    return x * (15.0f - x2) / (15.0f - 6.0f * x2);
}

/* sin(2 pi p), any p (a 7th-order series on the folded phase) */
INLINE float sin2pi(float p)
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

/* sin(2 pi p), any p: the table, interpolated */
INLINE float sin_t(float p)
{
    float x = p * (float)SINE_N;
    int32_t i = (int32_t)x;
    if ((float)i > x)
        i--;
    float f = x - (float)i;
    const float *t = SINE + (i & (SINE_N - 1));
    return t[0] + (t[1] - t[0]) * f;
}

INLINE uint32_t lcg(uint32_t s)
{
    return s * 1664525u + 1013904223u;
}

INLINE float knob(const sy_t *k, int i)
{
    return clampf((float)*k->param[i] * (1.0f / 4095.0f), 0.0f, 1.0f);
}

/* A knob relative to the sound: 12 o'clock (+-DETENT) is the sound's own position p, fully
 * left / right the ends of the range */
static float warp(float x, float p)
{
    float d = x - 0.5f;
    if (d > -DETENT && d < DETENT)
        return p;
    if (d < 0.0f)
        return p * (x / (0.5f - DETENT));
    return p + (1.0f - p) * ((d - DETENT) / (0.5f - DETENT));
}

/* ---- the sound: 5-bit values -> the engine's ------------------------------------------ */

static float time_of(int v)             /* envelope times */
{
    return 0.001f * exp2f_((float)v * (T_OCT / 31.0f));
}

static float sq31(int v)                /* (v / 31)^2 */
{
    float x = (float)v * (1.0f / 31.0f);
    return x * x;
}

/* a PARAM2 job's stored value: 16 is 12 o'clock */
static float p2_of(int v)
{
    return clampf(0.5f + (float)(v - SL_MID) * (1.0f / 30.0f), 0.0f, 1.0f);
}

static void env_set(env_t *e, const uint8_t *c, int decay_v, float n)
{
    float att = time_of(c[SL_E_ATT]) * SR / n, dec = time_of(decay_v) * SR / n;
    float rel = time_of(c[SL_E_REL]) * SR / n;
    e->sus = (float)c[SL_E_SUS] * (1.0f / 31.0f);
    e->exp = c[SL_E_CURVE] & 1;
    e->a_inc = 1.0f / (att > 1.0f ? att : 1.0f);
    e->d_dec = (1.0f - e->sus) / (dec > 1.0f ? dec : 1.0f);
    e->r_dec = 1.0f / (rel > 1.0f ? rel : 1.0f);    /* times the level at the release */
    /* the iOS synth's exponential segments: e^-5 over the segment's time */
    e->a_c = exp2f_(-5.0f * LOG2_E / (att > 1.0f ? att : 1.0f));
    e->d_c = exp2f_(-5.0f * LOG2_E / (dec > 1.0f ? dec : 1.0f));
    e->r_c = exp2f_(-5.0f * LOG2_E / (rel > 1.0f ? rel : 1.0f));
}

static void decode(sy_t *k)
{
    const uint8_t *v = k->v;
    float sum = 0.0f, low = 100.0f;
    for (int i = 0; i < NOSC; i++) {
        const uint8_t *o = v + SL_OSC(i);
        k->o[i].wave = o[SL_WAVE] < SL_NWAVE ? o[SL_WAVE] : SL_W_SAW;
        k->o[i].pw = 0.05f + (float)o[SL_PW] * (0.45f / 31.0f);
        k->level[i] = (float)o[SL_LEVEL] * (1.0f / 31.0f);
        k->synced[i] = i > 0 && o[SL_SYNC];
        k->ratio[i] = OCT_RATIO[o[SL_OCT] & 7] * exp2f_((float)(o[SL_FINE] - SL_MID) * (50.0f / 15.0f / 1200.0f));
        k->sweep[i] = 48.0f * sq31(o[SL_SWEEP]);
        k->ratio_mode[i] = o[SL_RATIO] & 1;
        sum += k->level[i];
        /* the register: the lowest oscillator that sets a pitch (the master always: its
         * slaves follow it even when it's silent) */
        if (!k->synced[i] && (k->level[i] > 0.0f || i == 0) && OCT_RATIO[o[SL_OCT] & 7] < low)
            low = OCT_RATIO[o[SL_OCT] & 7];
        for (int t = 0; t < 2; t++) {
            const uint8_t *l = o + (t ? SL_VLFO : SL_PLFO);
            int n = t ? L_VOL0 + i : L_PITCH0 + i;
            k->lfo_rate[n] = 0.05f * exp2f_((float)l[SL_L_SPEED] * (LFO_OCT / 31.0f));
            k->lfo_shape[n] = l[SL_L_SHAPE];
            k->lfo_sync[n] = l[SL_L_SYNC] ? 16 - ((l[SL_L_SPEED] * 17) >> 5) + 1 : 0;   /* index + 1 */
            k->lfo_depth[n] = t ? (float)l[SL_L_DEPTH] * (1.0f / 31.0f) : 24.0f * sq31(l[SL_L_DEPTH]);
        }
    }
    k->reg = 1.0f / low;
    k->norm = 1.0f / (sum > 1.0f ? sum : 1.0f);
    k->trim = LEVEL * exp2f_((float)((int)v[SL_TRIM] - 8) * (1.5f / 6.0205999f));
    for (int t = 0; t < 2; t++) {
        const uint8_t *l = v + (t ? SL_FLFO : SL_CLFO);
        int n = t ? L_FOLD : L_CUT;
        k->lfo_rate[n] = 0.05f * exp2f_((float)l[SL_L_SPEED] * (LFO_OCT / 31.0f));
        k->lfo_shape[n] = l[SL_L_SHAPE];
        k->lfo_sync[n] = l[SL_L_SYNC] ? 16 - ((l[SL_L_SPEED] * 17) >> 5) + 1 : 0;
        k->lfo_depth[n] = t ? (float)l[SL_L_DEPTH] * (1.0f / 31.0f) : 4.0f * sq31(l[SL_L_DEPTH]);
    }
    env_set(&k->aenv, v + SL_AENV, v[SL_ADECAY], 1.0f);
    env_set(&k->fenv, v + SL_FENV, v[SL_FDECAY], (float)CTL);
    k->u_cut = (float)v[SL_CUTOFF] * (CUT_FIELD / 31.0f);
    k->u_res = (float)v[SL_RES] * (1.0f / 31.0f);
    k->u_env = clampf(0.5f + (float)(v[SL_ENVAMT] - SL_MID) * (1.0f / 30.0f), 0.0f, 1.0f);
    k->keytrack = (float)(v[SL_KEYTRACK] & 3) * (1.0f / 3.0f);
    k->filt_on = v[SL_FILT_ON] & 1;
    k->ftype = v[SL_FILT_TYPE] < SL_NFILT ? v[SL_FILT_TYPE] : SL_F_LP24;
    k->fold_on = v[SL_FOLD_ON] & 1;
    k->fold_amt = (float)v[SL_FOLD_AMT] * (1.0f / 31.0f);
    k->fold_bias = (float)(v[SL_FOLD_SYM] - SL_MID) * (1.0f / 32.0f);
    k->fold_push = (float)v[SL_FOLD_PUSH] * (1.0f / 31.0f);
    if (v[SL_GLIDE]) {                  /* ~95% of the way in the glide time */
        float t = 0.005f * exp2f_((float)(v[SL_GLIDE] - 1) * (LFO_OCT / 30.0f));
        k->glide_a = 1.0f - exp2f_(-3.0f * LOG2_E * (float)CTL / (t * SR));
    } else {
        k->glide_a = 0.0f;
    }
    k->legato = v[SL_LEGATO] & 1;
    k->lfo_free = v[SL_LFO_FREE] & 1;
}

/* a new sound: its values, and PARAM2's three jobs back at the sound's (the knob takes
 * over once it moves) */
static void use_sound(sy_t *k, const uint8_t *v, int keep)
{
    for (int i = 0; i < SL_NFIELD; i++)
        k->v[i] = k->base[i] = v[i];
    sl_clean(k->base);
    sl_clean(k->v);
    if (!keep) {
        for (int r = 0; r < NROLE; r++)
            k->role[r] = p2_of(k->v[SL_P2_RES + r]);
        k->armed = 0;
        k->ref2 = knob(k, 3);
    }
    decode(k);
}

/* a sound from the M4 for this output */
static void take_stash(sy_t *k)
{
    stashed &= ~(1u << k->out);
    uint8_t v[SL_NFIELD];
    sl_unpack(stash[k->out] + 1, v);
    use_sound(k, v, stash[k->out][0] & F_KEEP);
    k->held = stash[k->out][0] & F_HELD ? 1 : 0;
}

/* "synth-lab": [0] the output (bits 0-1), held (bit 4: PARAM2 waits for the kit's knob
 * positions), keep (bit 5: a page edit: PARAM2's values stay as they play), [1..] the packed
 * sound. Kept in the M7 host's RAM (164 B): an output switching to Synth Lab at its next note
 * (a kit load) starts with it. */

void pk_sy_on_sound(const uint8_t *buf, uint32_t len)
{
    if (len < SL_PACKED + 1)
        return;
    int out = buf[0] & 3;
    for (int i = 0; i < SL_PACKED + 1; i++)
        stash[out][i] = buf[i];
    stashed |= (1u << out) | (1u << (4 + out));
}

/* ---- control ------------------------------------------------------------------------------- */

static float gate_time(float d)         /* DECAY: GATE seconds, 0 = drone */
{
    if (d >= DRONE_AT)
        return 0.0f;
    return 0.05f * exp2f_(d * (5.3219281f / DRONE_AT));    /* 0.05 * 40^(d / 0.95) */
}

static int role_of(int algo)
{
    return algo == POS1 ? R_RES : (algo == POS3 ? R_SYNC : R_ENV);
}

static void read_knobs(sy_t *k)
{
    if (stashed & (1u << k->out))       /* a sound from the M4 */
        take_stash(k);
    k->k_cut = knob(k, 2);
    float g = gate_time(knob(k, 1));
    k->gate_samples = g > 0.0f ? (int32_t)(g * SR) : -1;
    /* PARAM2: ALGO's job. A new job, a new sound or a held kit load keeps its value until
     * the knob moves (2% of its travel); then the knob sets it. */
    float x = knob(k, 3);
    int r = role_of(k->algo);
    if (r != k->cur_role) {
        k->cur_role = r;
        k->ref2 = x;
        k->armed = 0;
    }
    if (k->held) {
        k->ref2 = x;
        k->armed = 0;
    }
    if (!k->armed && absf(x - k->ref2) > MOVED)
        k->armed = 1;
    if (k->armed)
        k->role[r] = x;
    if (bpm > 0.0f) {                   /* the M4 has sent DRIVE (with the tempo); until then 0 dB */
        float d = (float)drive[k->out] * (1.0f / 255.0f);
        float db = d < 0.5f ? DRIVE_DB * (1.0f - 2.0f * d) : -DRIVE_DB * (2.0f * d - 1.0f);
        k->dgain_t = exp2f_(db * (1.0f / 6.0205999f));
    } else {
        k->dgain_t = 1.0f;
    }
}

/* TUNE's scale step for knob position x: without memory (a trigger), or with hysteresis
 * around `cur` (a note sounding) */
static int tune_step(const sy_t *k, int cur)
{
    int len = SCALES[k->v[SL_SCALE]].len;
    float pos = knob(k, 0) * (float)(OCTAVES * len);
    if (cur >= 0 && pos <= (float)cur + 0.5f + HYST && pos >= (float)cur - 0.5f - HYST)
        return cur;
    return (int)(pos + 0.5f);
}

static float midi_of(const sy_t *k, int step)
{
    int sc = k->v[SL_SCALE], len = SCALES[sc].len;
    int semis = (step / len) * 12 + SCALES[sc].n[step % len];
    int note = k->note ? k->note : NOTE_REF;
    return (float)(BASE_NOTE + k->v[SL_ROOT] + semis + note - NOTE_REF);
}

static void set_note(sy_t *k, float midi, int glide)
{
    k->note_midi = midi;
    k->pitch_to = (midi - 69.0f) * (1.0f / 12.0f) + 8.7813597f;    /* log2(440) */
    if (!glide)
        k->pitch = k->pitch_to;
}

/* As Akords: a note follows TUNE at once in its first FOLLOW_SAMPLES (the M4 can send a step's
 * TUNE just after its trigger); later a new step must hold SETTLE_SAMPLES */
static void follow_tune(sy_t *k)
{
    if (k->released || k->tune_scale != k->v[SL_SCALE])
        return;
    int s = tune_step(k, k->tune_step);
    if (s == k->tune_step) {
        k->tune_since = -1;
        return;
    }
    if (k->elapsed >= FOLLOW_SAMPLES) {
        if (k->tune_since < 0)
            k->tune_since = k->elapsed;
        if (k->elapsed - k->tune_since < SETTLE_SAMPLES)
            return;
    }
    k->tune_since = -1;
    k->tune_step = s;
    set_note(k, midi_of(k, s), k->glide_a > 0.0f);
}

static void update(cVoice *v)
{
    sy_t *k = (sy_t *)v;
    if (k->magic != MAGIC)
        return;
    read_knobs(k);
    follow_tune(k);
}

static float lfo_hz(const sy_t *k, int n)
{
    if (!k->lfo_sync[n])
        return k->lfo_rate[n];
    float t = bpm > 0.0f ? bpm : 120.0f;
    return t * (1.0f / 60.0f) / DIV_BEATS[k->lfo_sync[n] - 1];
}

/* the LFOs, one control step on: each output 0-1 */
static void lfo_step(sy_t *k)
{
    for (int n = 0; n < NLFO; n++) {
        lfo_t *l = &k->lfo[n];
        if (k->lfo_depth[n] <= 0.0f) {
            l->val = 0.0f;
            continue;
        }
        float p = l->ph + lfo_hz(k, n) * ((float)CTL / SR);
        if (p >= 1.0f) {
            p -= (float)(int32_t)p;
            k->rng = lcg(k->rng);
            l->held = (float)(k->rng >> 8) * (1.0f / 16777216.0f);
        }
        l->ph = p;
        float y;
        switch (k->lfo_shape[n]) {
        case SL_S_TRI:    y = p < 0.5f ? 2.0f * p : 2.0f - 2.0f * p; break;
        case SL_S_SAW:    y = p; break;
        case SL_S_RAMP:   y = 1.0f - p; break;
        case SL_S_SQUARE: y = p < 0.5f ? 1.0f : 0.0f; break;
        case SL_S_RANDOM: y = l->held; break;
        default:          y = 0.5f + 0.5f * sin_t(p); break;
        }
        l->val = y;
    }
}

INLINE float env_step(env_t *e)
{
    switch (e->stage) {
    case ATTACK:
        if (e->exp)
            e->lvl = 1.0f - (1.0f - e->lvl) * e->a_c;
        else
            e->lvl += e->a_inc;
        if (e->lvl >= 0.999f) {
            e->lvl = 1.0f;
            e->stage = DECAY;
        }
        break;
    case DECAY:
        if (e->exp)
            e->lvl = e->sus + (e->lvl - e->sus) * e->d_c;
        else
            e->lvl -= e->d_dec;
        if (e->lvl <= e->sus + (e->exp ? 0.001f : 0.0f)) {
            e->lvl = e->sus;
            e->stage = SUSTAIN;
        }
        break;
    case RELEASE:
        if (e->exp)
            e->lvl *= e->r_c;
        else
            e->lvl -= e->r_dec;
        if (e->lvl <= 0.001f) {
            e->lvl = 0.0f;
            e->stage = IDLE;
        }
        break;
    default:
        break;
    }
    return e->lvl;
}

static void env_release(env_t *e)
{
    if (e->stage == IDLE)
        return;
    e->r_dec *= e->lvl > 0.0f ? e->lvl : 0.0f;  /* linear: from here to 0 in the release time */
    e->stage = RELEASE;
}

/* every CTL samples: LFOs, filter envelope, pitch, oscillator steps and gains, filter, fold */
static void control(sy_t *k)
{
    lfo_step(k);
    float fe = env_step(&k->fenv);
    k->fenv_lvl = fe;
    if (k->glide_a > 0.0f && k->pitch != k->pitch_to) {
        k->pitch += (k->pitch_to - k->pitch) * k->glide_a;
        if (absf(k->pitch_to - k->pitch) < 0.0005f)
            k->pitch = k->pitch_to;
    }
    const int sync = 1, sweep = 1;     /* as the sound sets them (its sync flags, sweep depths) */
    float base = k->pitch;              /* log2 Hz */
    float sync_semis = k->role[R_SYNC] < 0.5f - DETENT || k->role[R_SYNC] > 0.5f + DETENT
                     ? 36.0f * warp(k->role[R_SYNC], 1.0f / 3.0f) - 12.0f : 0.0f;
    float master = 0.0f;
    int live = 0;
    for (int i = 0; i < NOSC; i++) {
        osc_t *o = &k->o[i];
        float semis = 0.0f;
        if (k->lfo_depth[L_PITCH0 + i] > 0.0f)
            semis += (2.0f * k->lfo[L_PITCH0 + i].val - 1.0f) * k->lfo_depth[L_PITCH0 + i];
        if (k->synced[i]) {
            semis += sync_semis;
            if (sweep)
                semis += k->sweep[i] * fe;
        }
        float hz = exp2f_(base + semis * (1.0f / 12.0f)) * k->ratio[i] * k->reg;
        if (i == 0)
            master = hz;
        else if (sync && k->synced[i] && k->ratio_mode[i] && master > 0.0f) {
            /* the iOS synth's sync detune: ratios near a whole number are pushed to n.3, where
             * sync sounds (at a whole ratio a slave just doubles the master) */
            float r = hz / master, fr = r - (float)(int32_t)r;
            float near = fr < 1.0f - fr ? fr : 1.0f - fr;
            if (near < 0.3f)
                hz += (0.3f - near) * master;
        }
        o->dt = clampf(hz, 1.0f, 18000.0f) * (1.0f / SR);
        float g = k->level[i] * k->norm;
        if (k->lfo_depth[L_VOL0 + i] > 0.0f)
            g *= 1.0f - k->lfo_depth[L_VOL0 + i] * (1.0f - k->lfo[L_VOL0 + i].val);
        if (g == 0.0f && o->g < 1e-4f) {   /* silent: it stops (render keeps only its phase) */
            o->g = o->g_step = 0.0f;
        } else {
            o->g_step = (g - o->g) * (1.0f / CTL);
            live |= 1 << i;
        }
    }
    k->live = live;

    /* filter: cutoff (PARAM1, key tracking, LFO, envelope) and resonance (PARAM2) */
    if (k->filt_on) {
        float oct = warp(k->k_cut, k->u_cut) * CUT_OCT;
        oct += (k->note_midi - 60.0f) * k->keytrack * (1.0f / 12.0f);
        oct += (k->vel - 1.0f) * VEL_CUT_OCT;
        if (k->lfo_depth[L_CUT] > 0.0f)
            oct += (2.0f * k->lfo[L_CUT].val - 1.0f) * k->lfo_depth[L_CUT];
        float ep = fe * (2.0f * warp(k->role[R_ENV], k->u_env) - 1.0f);
        float scale = ep >= 0.0f ? 1.0f + ep * 10.0f : (1.0f + ep > 0.05f ? 1.0f + ep : 0.05f);
        float fc = clampf(CUT_MIN * exp2f_(oct) * scale, 20.0f, 0.45f * SR);
        float res = warp(k->role[R_RES], k->u_res);
        float tq = 1.0f - res * 0.9f;
        /* the trapezoidal SVF (Zavalishin, Simper): g = tan(pi fc / SR), any fc up to 0.45 SR;
         * damping tq (the iOS synth's q) */
        float p = fc * (0.5f / SR);
        float g = sin2pi(p) / sin2pi(p + 0.25f);
        float t[NCOEF];
        float tg = 0.4f + 0.6f * tq;
        if (k->ftype == SL_F_LADDER) {
            /* four trapezoidal one-poles at fc, their output fed back to the input
             * (Zavalishin's zero-delay ladder; the loop solved linearly, then saturated) */
            float G = g / (1.0f + g), G4 = G * G, kk = res * LAD_K;
            G4 *= G4;
            t[C_LG] = G;
            t[C_LD] = 1.0f / (1.0f + kk * G4);
            t[C_LK] = kk;
            tg = 1.0f + LAD_COMP * kk;
        } else {
            t[C_A1] = 1.0f / (1.0f + g * (g + tq));
            t[C_A2] = g * t[C_A1];
            t[C_A3] = g * t[C_A2];
            t[C_B1] = 1.0f / (1.0f + g * (g + LP2_K));
            t[C_B2] = g * t[C_B1];
            t[C_B3] = g * t[C_B2];
            /* the one-pole (LP6, LP18's third pole): an octave under fc, as the iOS synth's */
            float gh = tanf_(PI * p);
            t[C_G1] = gh / (1.0f + gh);
            for (int i = C_B1; i < NCOEF; i++)   /* these follow at once (no resonance: no zipper) */
                k->c[i] = t[i];
        }
        for (int i = C_A1; i <= C_A3; i++)       /* the resonant stage glides over the step */
            k->c_step[i] = (t[i] - k->c[i]) * (1.0f / CTL);
        k->q_step = (tq - k->q) * (1.0f / CTL);
        /* resonance compensation: the SVF's peak grows about as 1 / q (as Akords); LP24 has one
         * resonant stage, so half of it in dB; the ladder's set above */
        if (k->ftype == SL_F_LP24)
            tg *= rsqrtf_(tg);
        k->in_gain += (tg - k->in_gain) * 0.25f;
    }

    /* fold: the amount, pushed by the filter envelope, moved by its LFO */
    float fa = 0.0f;
    if (k->fold_on) {
        fa = k->fold_amt + k->fold_push * fe;
        if (k->lfo_depth[L_FOLD] > 0.0f)
            fa += k->lfo_depth[L_FOLD] * k->lfo[L_FOLD].val;
        fa = clampf(fa, 0.0f, 1.0f);
    }
    k->fold_step = (fa - k->fold) * (1.0f / CTL);

    k->dgain += (k->dgain_t - k->dgain) * (DRIVE_SMOOTH * CTL);

    /* MODE: the drive */
    int m = k->mode < 3 ? k->mode : POS1;
    k->drv_g += (DRV_GAIN[m] - k->drv_g) * DRV_SMOOTH;
    k->drv_w += (DRV_WET[m] - k->drv_w) * DRV_SMOOTH;
    k->drv_b += (DRV_BIAS[m] - k->drv_b) * DRV_SMOOTH;
    k->drv_c = clip3(k->drv_b);
    if (k->drv_w > 0.0005f && k->drv_out > 1e-9f) {
        /* make-up: the clip's output back to the dry sound's level, between -6 and +12 dB */
        float t = k->drv_in > 1e-9f ? k->drv_in * rsqrtf_(k->drv_in) * rsqrtf_(k->drv_out) : 1.0f;
        k->drv_m += (clampf(t, 0.5f, 4.0f) - k->drv_m) * DRV_SMOOTH;
    }
}

/* Mutate: each note a few small nudges of the saved sound, worked out from it afresh (they
 * never add up): the filter, the pulse widths, levels, detune, envelopes, fold, LFO rates */
static const uint8_t MUT_FIELDS[] = {
    SL_CUTOFF, SL_RES, SL_ENVAMT, SL_FDECAY, SL_ADECAY, SL_FOLD_AMT, SL_FOLD_SYM,
    SL_OSC(0) + SL_PW, SL_OSC(1) + SL_PW, SL_OSC(2) + SL_PW,
    SL_OSC(1) + SL_LEVEL, SL_OSC(2) + SL_LEVEL, SL_OSC(1) + SL_FINE, SL_OSC(2) + SL_FINE,
    SL_OSC(1) + SL_SWEEP, SL_OSC(2) + SL_SWEEP,
    SL_CLFO + SL_L_SPEED, SL_FLFO + SL_L_SPEED, SL_OSC(1) + SL_PLFO + SL_L_SPEED,
};
static const int8_t MUT_STEP[] = {1, 2, 2, 1, 1, 2, 2, 3, 3, 3, 2, 2, 1, 1, 2, 2, 1, 1, 1};

static void mutate(sy_t *k)
{
    for (int i = 0; i < SL_NFIELD; i++)
        k->v[i] = k->base[i];
    if (k->base[SL_MUTATE]) {
        for (unsigned i = 0; i < sizeof MUT_FIELDS; i++) {
            k->rng = lcg(k->rng);
            uint32_t r = k->rng >> 24;
            if (r & 1)                  /* about half of them each note */
                continue;
            int f = MUT_FIELDS[i], st = MUT_STEP[i];
            int d = (int)((r >> 1) % (unsigned)(2 * st + 1)) - st;
            int x = (int)k->base[f] + d, m = sl_max(f);
            k->v[f] = (uint8_t)(x < 0 ? 0 : (x > m ? m : x));
        }
    }
    decode(k);
}

static void trigger(cVoice *v)
{
    sy_t *k = (sy_t *)v;
    if (k->magic != MAGIC)
        return;
    read_knobs(k);
    if (k->base[SL_MUTATE] || k->v[SL_MUTATE])
        mutate(k);
    int was = k->sounding && !k->released;     /* the last note's gate still open: legato */
    k->tune_scale = k->v[SL_SCALE];
    k->tune_step = tune_step(k, -1);
    int glide = k->glide_a > 0.0f && k->sounding && (!k->legato || was);
    set_note(k, midi_of(k, k->tune_step), glide);
    k->tune_since = -1;
    k->vel = (float)k->velocity * (1.0f / 127.0f);
    if (!k->sounding) {                 /* from silence: the same start every time */
        for (int i = 0; i < NOSC; i++) {
            k->o[i].ph = 0.0f;
            k->o[i].g = 0.0f;
        }
        for (int t = 0; t < CTL + MB_TAPS; t++)
            k->ring[t] = 0.0f;
        k->low = k->band = k->low2 = k->band2 = k->one = 0.0f;
        k->lad[0] = k->lad[1] = k->lad[2] = k->lad[3] = 0.0f;
        k->fold = 0.0f;
        k->prev = 0.0f;
    }
    if (!k->lfo_free || !k->sounding)   /* the LFOs restart with the note */
        for (int n = 0; n < NLFO; n++)
            k->lfo[n].ph = 0.0f;
    /* the envelopes restart from where they are (no click) */
    k->aenv.stage = ATTACK;
    k->fenv.stage = ATTACK;
    env_set(&k->aenv, k->v + SL_AENV, k->v[SL_ADECAY], 1.0f);
    env_set(&k->fenv, k->v + SL_FENV, k->v[SL_FDECAY], (float)CTL);
    k->elapsed = 0;
    k->released = 0;
    k->sounding = 1;
    k->bi = CTL;                        /* a new block (a control step) at once */
    k->stop = 0;
}

static void init(cVoice *v)
{
    sy_t *k = (sy_t *)v;
    k->magic = MAGIC;
    k->cur_role = role_of(k->algo);
    k->ref2 = knob(k, 3);
    k->armed = 0;
    k->tune_step = -1;
    k->tune_since = -1;
    k->q = 1.0f;
    k->in_gain = 1.0f;
    k->drv_g = k->drv_m = 1.0f;
    k->rng = 0x2545f491u + (uint32_t)k->out;
    read_knobs(k);
    k->dgain = k->dgain_t;
    set_note(k, midi_of(k, tune_step(k, -1)), 0);
}

/* ---- audio: a block of CTL samples at each control step ------------------------------------ */

/* The firmware asks for one sample at a time; the engine works out CTL of them together, stage
 * by stage (oscillators, fold, filter, drive, amp envelope and output), each stage one tight
 * loop over the block with its choices (the wave, the filter type) made once. CTL divides the
 * firmware's 32-sample half, and a note starts a new block, so every half holds whole blocks. */

/* add the band-limited correction for a jump h (already x the oscillator's gain), `t` samples
 * (0-1) before the sample r[0] */
static __attribute__((noinline)) void blep_add(float *r, float h, float t)
{
    float x = t * (float)MB_PHASES;
    int i = (int)x;
    if (i >= MB_PHASES)
        i = MB_PHASES - 1;
    float a = x - (float)i, h1 = h * a, h0 = h - h1;
    const float *r0 = MINBLEP[i], *r1 = MINBLEP[i + 1];
#pragma GCC unroll 16
    for (int j = 0; j < MB_TAPS; j++)
        r[j] += h0 * r0[j] + h1 * r1[j];
}

INLINE float wave_at(int w, float p, float pw, uint32_t *rng)
{
    switch (w) {
    case SL_W_SINE:  return sin_t(p);
    case SL_W_TRI:   return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
    case SL_W_RAMP:  return 1.0f - 2.0f * p;
    case SL_W_PULSE: return p < pw ? 1.0f : -1.0f;
    case SL_W_NOISE: *rng = lcg(*rng); return (float)(int32_t)*rng * (1.0f / 2147483648.0f);
    default:         return 2.0f * p - 1.0f;
    }
}

/* the triangle's corners: the 2-point BLAMP (as Akords) */
INLINE float blamp(float t, float dt, float idt)
{
    if (t < dt) {
        float n = t * idt;
        return dt * (n * n - n * n * n * (1.0f / 3) - n);
    }
    if (t > 1.0f - dt) {
        float n = (t - 1.0f) * idt;
        return -dt * (n * n * n * (1.0f / 3) + n * n + n);
    }
    return 0.0f;
}

enum { MASTER, FREE, SYNCED };          /* an oscillator's part in the block */

/* one oscillator over the block, wave w (a constant: each wave its own loop): its samples x
 * its gain into buf (the master sets it, the others add), its jumps' corrections (x its gain)
 * into the ring. The master notes its wraps in syn; a synced slave is reset at them */
INLINE void osc_block(sy_t *k, osc_t *o, int role, int w)
{
    float p = o->ph, dt = o->dt, idt = 1.0f / dt, pw = o->pw, g = o->g, gs = o->g_step;
    float *buf = k->buf, *syn = k->syn, *ring = k->ring;
    uint32_t rng = k->rng;
    for (int i = 0; i < CTL; i++) {
        g += gs;
        float y = wave_at(w, p, pw, &rng);
        if (w == SL_W_TRI) {
            float q = p - 0.5f;
            if (q < 0.0f)
                q += 1.0f;
            y += 8.0f * (blamp(q, dt, idt) - blamp(p, dt, idt));
        }
        if (role == MASTER)
            buf[i] = y * g;
        else
            buf[i] += y * g;
        if (role == SYNCED && syn[i] >= 0.0f) {
            /* reset by the master `frac` samples before the next sample */
            float frac = syn[i], at = p + dt * (1.0f - frac);   /* its phase at the master's wrap */
            if (at >= 1.0f) {                                   /* its own wrap first */
                at -= 1.0f;
                float t = frac + at * idt;
                t = t > 1.0f ? 1.0f : t;
                if (w == SL_W_SAW)
                    blep_add(ring + i + 1, -2.0f * g, t);
                else if (w == SL_W_RAMP || w == SL_W_PULSE)
                    blep_add(ring + i + 1, 2.0f * g, t);
            }
            if (w != SL_W_NOISE) {
                float h = wave_at(w, 0.0f, pw, &rng) - wave_at(w, at, pw, &rng);
                if (h > 1e-4f || h < -1e-4f)
                    blep_add(ring + i + 1, h * g, frac);
            }
            p = frac * dt;
            continue;
        }
        float n = p + dt;
        if (w == SL_W_PULSE && p < pw && n >= pw)
            blep_add(ring + i + 1, -2.0f * g, (n - pw) * idt);
        if (n >= 1.0f) {
            n -= 1.0f;
            float frac = n * idt;
            if (role == MASTER)
                syn[i] = frac;
            if (w == SL_W_SAW)
                blep_add(ring + i + 1, -2.0f * g, frac);
            else if (w == SL_W_RAMP || w == SL_W_PULSE)
                blep_add(ring + i + 1, 2.0f * g, frac);
        } else if (role == MASTER) {
            syn[i] = -1.0f;
        }
        p = n;
    }
    o->ph = p;
    o->g = g;
    k->rng = rng;
}

static __attribute__((noinline)) void osc_wave(sy_t *k, osc_t *o, int role)
{
    switch (o->wave) {
    case SL_W_SINE:  osc_block(k, o, role, SL_W_SINE); break;
    case SL_W_TRI:   osc_block(k, o, role, SL_W_TRI); break;
    case SL_W_RAMP:  osc_block(k, o, role, SL_W_RAMP); break;
    case SL_W_PULSE: osc_block(k, o, role, SL_W_PULSE); break;
    case SL_W_NOISE: osc_block(k, o, role, SL_W_NOISE); break;
    default:         osc_block(k, o, role, SL_W_SAW); break;
    }
}

/* a silent oscillator: only its phase (a silent master still syncs its slaves) */
static __attribute__((noinline)) void osc_idle(sy_t *k, osc_t *o, int role)
{
    float p = o->ph, dt = o->dt, idt = 1.0f / dt, *syn = k->syn;
    for (int i = 0; i < CTL; i++) {
        if (role == MASTER)
            k->buf[i] = 0.0f;
        if (role == SYNCED && syn[i] >= 0.0f) {
            p = syn[i] * dt;
            continue;
        }
        float n = p + dt;
        if (n >= 1.0f) {
            n -= 1.0f;
            if (role == MASTER)
                syn[i] = n * idt;
        } else if (role == MASTER) {
            syn[i] = -1.0f;
        }
        p = n;
    }
    o->ph = p;
}

/* the oscillators' residuals added in, then the fold (2x: the midpoint with the last sample
 * too, as the iOS synth); the ring moves on a block */
static __attribute__((noinline)) void mix_fold(sy_t *k)
{
    float *buf = k->buf, *ring = k->ring;
    float f = k->fold, fs = k->fold_step, bias = k->fold_bias, prev = k->prev;
    if (f > 0.001f || f + fs * CTL > 0.001f) {
        for (int i = 0; i < CTL; i++) {
            float x = buf[i] + ring[i] + bias;
            f += fs;
            if (f > 0.001f) {
                float g = (1.0f + f * 9.0f) * 0.25f;
                float mid = (prev + x) * 0.5f;
                buf[i] = (sin_t(mid * g) + sin_t(x * g)) * 0.5f;
            } else {
                buf[i] = x - bias;
            }
            prev = x;
        }
    } else {
        for (int i = 0; i < CTL; i++)
            buf[i] += ring[i];
        f += fs * CTL;
        prev = buf[CTL - 1] + bias;
    }
    k->fold = f;
    k->prev = prev;
    for (int j = 0; j < MB_TAPS; j++)
        ring[j] = ring[j + CTL];
    for (int j = MB_TAPS; j < CTL + MB_TAPS; j++)
        ring[j] = 0.0f;
}

/* tanh(x) (Pade, exact at +-3, flat beyond) */
INLINE float tanhf_(float x)
{
    if (x > 3.0f)
        return 1.0f;
    if (x < -3.0f)
        return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* the trapezoidal state-variable filter over the block, type t (a constant: each its own loop);
 * its band state saturates when the resonance is pushed. LP24: a second stage, no resonance;
 * LP18: the SVF's low pass, then the one-pole */
INLINE void svf_block(sy_t *k, int t)
{
    float *buf = k->buf, *c = k->c, *cs = k->c_step, ig = k->in_gain;
    float a1 = c[C_A1], a2 = c[C_A2], a3 = c[C_A3], s1 = cs[C_A1], s2 = cs[C_A2], s3 = cs[C_A3];
    float q = k->q, qs = k->q_step, low = k->low, band = k->band;
    float b1 = c[C_B1], b2 = c[C_B2], b3 = c[C_B3], g1 = c[C_G1];
    float low2 = k->low2, band2 = k->band2, one = k->one;
    for (int i = 0; i < CTL; i++) {
        float x = buf[i] * ig, y;
        a1 += s1;
        a2 += s2;
        a3 += s3;
        q += qs;
        float v3 = x - low;
        float v1 = a1 * band + a2 * v3;
        float v2 = low + a2 * band + a3 * v3;
        band = soft_knee(2.0f * v1 - band, SAT_KNEE, SAT_CEIL);
        low = 2.0f * v2 - low;
        switch (t) {
        case SL_F_LP12:  y = v2; break;
        case SL_F_BP:    y = v1; break;
        case SL_F_HP:    y = x - q * v1 - v2; break;
        case SL_F_NOTCH: y = x - q * v1; break;
        case SL_F_LP18: {
            float u = (v2 - one) * g1;
            y = u + one;
            one = y + u;
            break;
        }
        default: {
            float w3 = v2 - low2;
            float w1 = b1 * band2 + b2 * w3;
            float w2 = low2 + b2 * band2 + b3 * w3;
            band2 = 2.0f * w1 - band2;
            low2 = 2.0f * w2 - low2;
            y = w2;
            break;
        }
        }
        buf[i] = y;
    }
    c[C_A1] = a1;
    c[C_A2] = a2;
    c[C_A3] = a3;
    k->q = q;
    k->low = low;
    k->band = band;
    k->low2 = low2;
    k->band2 = band2;
    k->one = one;
}

/* the ladder: four one-poles G, the fourth's output fed back (k); its states' sum S, so the loop's
 * input is solved without a delay, u = (x - k S) / (1 + k G^4), then saturated */
static __attribute__((noinline)) void ladder_block(sy_t *k)
{
    float *buf = k->buf, *c = k->c, *cs = k->c_step, ig = k->in_gain;
    float G = c[C_LG], D = c[C_LD], K = c[C_LK], sg = cs[C_LG], sd = cs[C_LD], sk = cs[C_LK];
    float s0 = k->lad[0], s1 = k->lad[1], s2 = k->lad[2], s3 = k->lad[3];
    for (int i = 0; i < CTL; i++) {
        G += sg;
        D += sd;
        K += sk;
        float S = (1.0f - G) * (((s0 * G + s1) * G + s2) * G + s3);
        float u = (buf[i] * ig - K * S) * D, v;
        u = tanhf_(u * LAD_DRIVE) * (1.0f / LAD_DRIVE);
        v = (u - s0) * G; u = v + s0; s0 = u + v;
        v = (u - s1) * G; u = v + s1; s1 = u + v;
        v = (u - s2) * G; u = v + s2; s2 = u + v;
        v = (u - s3) * G; u = v + s3; s3 = u + v;
        buf[i] = u;
    }
    c[C_LG] = G;
    c[C_LD] = D;
    c[C_LK] = K;
    k->lad[0] = s0;
    k->lad[1] = s1;
    k->lad[2] = s2;
    k->lad[3] = s3;
}

static __attribute__((noinline)) void filter_block(sy_t *k)
{
    switch (k->ftype) {
    case SL_F_LP6: {
        float *buf = k->buf, g1 = k->c[C_G1], ig = k->in_gain, one = k->one;
        for (int i = 0; i < CTL; i++) {
            float u = (buf[i] * ig - one) * g1;
            buf[i] = u + one;
            one = buf[i] + u;
        }
        k->one = one;
        break;
    }
    case SL_F_LADDER: ladder_block(k); break;
    case SL_F_LP12:   svf_block(k, SL_F_LP12); break;
    case SL_F_LP18:   svf_block(k, SL_F_LP18); break;
    case SL_F_BP:     svf_block(k, SL_F_BP); break;
    case SL_F_HP:     svf_block(k, SL_F_HP); break;
    case SL_F_NOTCH:  svf_block(k, SL_F_NOTCH); break;
    default:          svf_block(k, SL_F_LP24); break;
    }
}

/* MODE's drive: the soft clip, antialiased (the antiderivative's difference over the step;
 * the midpoint when the step is tiny), crossfaded in by drv_w; the levels before and after it
 * (its make-up) follow the block's mean squares */
static __attribute__((noinline)) void drive_block(sy_t *k)
{
    float *buf = k->buf, dg = k->drv_g, db = k->drv_b, dc = k->drv_c, dm = k->drv_m, dw = k->drv_w;
    float x1 = k->drv_x1, F1 = k->drv_F1, sin_ = 0.0f, sout = 0.0f;
    for (int i = 0; i < CTL; i++) {
        float x = buf[i];
        float u = x * dg + db, F = clip3_F(u), du = u - x1;
        float y = (du > 1e-4f || du < -1e-4f ? (F - F1) / du : clip3(0.5f * (u + x1))) - dc;
        x1 = u;
        F1 = F;
        sin_ += x * x;
        sout += y * y;
        buf[i] = x + dw * (y * dm - x);
    }
    k->drv_x1 = x1;
    k->drv_F1 = F1;
    k->drv_in += (sin_ * (1.0f / CTL) - k->drv_in) * (DRV_AVG * CTL);
    k->drv_out += (sout * (1.0f / CTL) - k->drv_out) * (DRV_AVG * CTL);
}

/* the amp envelope over the block into a (each stage one loop); returns how many samples it
 * sounds: fewer than CTL when it ends (those after are silent) */
static __attribute__((noinline)) int env_block(env_t *e, float *a)
{
    float l = e->lvl;
    int i = 0;
    while (i < CTL) {
        float m = 1.0f, b = 0.0f, thr = 0.0f, end = 0.0f;
        int up = 0, next = IDLE;
        switch (e->stage) {
        case ATTACK:
            m = e->exp ? e->a_c : 1.0f;
            b = e->exp ? 1.0f - e->a_c : e->a_inc;
            thr = 0.999f, end = 1.0f, up = 1, next = DECAY;
            break;
        case DECAY:
            m = e->exp ? e->d_c : 1.0f;
            b = e->exp ? e->sus * (1.0f - e->d_c) : -e->d_dec;
            thr = e->sus + (e->exp ? 0.001f : 0.0f), end = e->sus, next = SUSTAIN;
            break;
        case RELEASE:
            m = e->exp ? e->r_c : 1.0f;
            b = e->exp ? 0.0f : -e->r_dec;
            thr = 0.001f;
            break;
        case IDLE:
            e->lvl = l;
            return i;
        default:                        /* sustain */
            for (; i < CTL; i++)
                a[i] = l;
            e->lvl = l;
            return CTL;
        }
        for (; i < CTL; i++) {
            l = l * m + b;
            if (up ? l >= thr : l <= thr) {
                l = end;
                e->stage = next;
                if (next == IDLE) {
                    e->lvl = l;
                    return i;
                }
                a[i++] = l;
                break;
            }
            a[i] = l;
        }
    }
    e->lvl = l;
    return CTL;
}

/* the block: control, the gate, then each stage over CTL samples */
static __attribute__((noinline)) void block(sy_t *k)
{
    control(k);
    /* the gate: the note's release */
    if (!k->released && k->gate_samples >= 0 && k->elapsed >= k->gate_samples) {
        k->released = 1;
        env_release(&k->aenv);
        env_release(&k->fenv);
    }
    k->elapsed += CTL;

    /* oscillators: the master, then the slaves (those with sync on: reset at its wraps) */
    osc_t *o = k->o;
    for (int i = 0; i < NOSC; i++) {
        int role = i == 0 ? MASTER : (k->synced[i] ? SYNCED : FREE);
        if (k->live >> i & 1)
            osc_wave(k, &o[i], role);
        else
            osc_idle(k, &o[i], role);
    }
    mix_fold(k);
    if (k->filt_on)
        filter_block(k);
    /* MODE: drive, after the filter (the filtered tone gets its harmonics back: an overdriven
     * filter's growl), before the amp envelope (the same grit at any level of the note) */
    if (k->drv_w > 0.0005f)
        drive_block(k);

    /* the amp envelope, the DC blocker (the pulse's and the fold's offsets), the limiter and
     * the soft clip, the level for DRIVE */
    float a[CTL], *buf = k->buf;
    int n = env_block(&k->aenv, a);
    float vt = k->vel * k->trim, dx = k->dc_x, dy = k->dc_y, lim = k->lim, dg = k->dgain;
    for (int i = 0; i < n; i++) {
        float y = buf[i] * a[i] * vt;
        float d = y - dx + DC_R * dy;
        dx = y;
        dy = d;
        float m = absf(d);
        lim = m > lim ? lim + (m - lim) * LIM_ATT : lim * LIM_REL;
        if (lim > LIM_THR)
            d *= LIM_THR / lim;
        d = soft_knee(d, CLIP_KNEE, CLIP_CEIL);
        buf[i] = d * dg;            /* within +-14,200: the clip's ceiling, DRIVE's +3 dB */
    }
    for (int i = n; i < CTL; i++)
        buf[i] = 0.0f;
    k->dc_x = dx;
    k->dc_y = dy;
    k->lim = lim;
    k->stop = n < CTL;
}

static int16_t render(cVoice *v)
{
    sy_t *k = (sy_t *)v;
    fw_tick_mute_fade(v);
    if (k->magic != MAGIC || !k->sounding)
        return 0;
    if (k->bi >= CTL) {
        if (k->stop) {
            k->sounding = 0;
            return 0;
        }
        block(k);
        k->bi = 0;
    }
    return (int16_t)(((int32_t)k->buf[k->bi++] * k->gain) >> 16);
}

/* ---- vtable and construction --------------------------------------------------------- */

/* RTTI-shaped word before the vtable (tools read class names from vtable[-1]) */
static const char sy_name[] = "9cSynthLab";
static const struct { const void *vptr; const char *name; } sy_typeinfo = {0, sy_name};
static const struct {
    const void *typeinfo;
    cVoice_vtbl vtbl;
} sy_vtable = {&sy_typeinfo, {trigger, init, update, render}};

/* voice-map: build a Synth Lab in `obj` (an output's arena) for output `out` */
void pk_sy_build(uint8_t *obj, unsigned out)
{
    uint32_t *w = (uint32_t *)obj;
    for (unsigned i = 0; i < (sizeof(sy_t) + 3) / 4; i++)
        w[i] = 0;
    sy_t *k = (sy_t *)obj;
    k->vtbl = &sy_vtable.vtbl;
    for (int i = 0; i < 4; i++)
        k->param[i] = &FW_PARAMS[out][i];
    k->note = NOTE_REF;
    k->out = (int32_t)(out & 3);
    k->magic = MAGIC;
    if (stashed & (1u << (4 + k->out)))     /* the M4 has sent this output's sound */
        stashed |= 1u << k->out;
    else
        use_sound(k, PRESETS[DEFAULT_PRESET[k->out] & (SL_NPRESET - 1)], 0);
    init((cVoice *)k);
}
