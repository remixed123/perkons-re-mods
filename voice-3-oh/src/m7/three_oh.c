/* 3 Oh: an acid bass line voice (M7): one oscillator through the classic bass line's diode
 * ladder filter, with its accent and slide, and four distortions.
 *
 * An extra voice-map engine ("three_oh"): voice-map builds one in an output's arena with
 * pk_oh_build(). The object is cVoice-compatible: the fields the firmware writes (algo, mode,
 * velocity, note, param pointers, mute, gain) sit at the stock offsets, and the vtable has the
 * four methods the firmware calls (trigger, init, update, render).
 *
 *   TUNE (param 0)    filter cutoff, relative to the sound (12 o'clock: the sound's own)
 *   DECAY (param 1)   as the bass line's: the filter envelope's decay only (to 10% in 0.1 s to
 *                     1.3 s, as a1k0n measured; accented notes the fastest). The note's length doesn't change:
 *                     the gate is open 58% of a sixteenth at the tempo, as the bass line's
 *                     sequencer. From 95% of its travel on: drone (the note never releases)
 *   PARAM1 (param 2)  resonance, relative to the sound
 *   PARAM2 (param 3)  ALGO's job, relative to the sound: envelope amount (position 1), accent
 *                     (2), distortion amount (3)
 *   MODE              the wave: saw (position 1), square (2), narrow pulse (3)
 *
 * The notes come from the voice's track: the line generator (the M4, the configuration page)
 * writes it into the Pērkons sequencer. A trigger plays its note (MIDI: 36 is C2) and its
 * velocity says how (oh_sound.h): accented, sliding into the next note, both, or plain (127, a
 * step turned on by hand). Velocity doesn't set the level; only an accent is louder.
 *
 * The filter: the diode ladder, four one-pole stages coupled to their neighbours (Zavalishin,
 * The Art of VA Filter Design, 5.10: stage 1 takes the input and stage 2 at full gain, stages
 * 2-4 their neighbours at half): with the bottom capacitor half the others this is exactly the
 * measured TB-303 core (Stinchcombe: s^4 + 6.727 s^3 + 14.142 s^2 + 9.514 s + 1). The output is
 * fed back through a 100 Hz high-pass (a1k0n's measurement; low notes keep their bass) and a
 * saturator at the ladder's input (Wurtz: the linear loop solved first, then the input
 * saturated), all without a delay (trapezoidal, prewarped: the resonance peaks exactly at the
 * cutoff). Its slope is soft near the cutoff (the poles spread), 24 dB/oct above: the "18 dB"
 * sound. Full resonance sits just under self-oscillation at any cutoff (EDGE): the scream when
 * cutoff and resonance are both high.
 *
 * Cutoff and envelope as the bass line's (Open303's measured mapping, widened as the Devil
 * Fish's): ENV MOD widens the sweep both ways, so it also lowers the base cutoff. Accent: the
 * filter envelope's decay short (200 ms), the envelope added to the level, and the accent
 * sweep: at low resonance a copy of the envelope added to the cutoff, at high resonance a
 * capacitor charged by it (slower, ganged to the resonance) that drains slowly, so accents close
 * together climb ("wow"). Slide: the note before holds its gate and the pitch glides (an RC on
 * the frequency, 60 ms as the bass line's) without the envelopes starting again; a slide into
 * the same note is a tie.
 *
 * The chain: oscillator (PolyBLEP) -> 44 Hz high-pass -> [Filter drive] -> diode ladder ->
 * amp envelope x accent x velocity -> [Muffler / Overdrive / Distortion] -> tone -> DC blocker ->
 * limiter + soft clip -> the level for DRIVE. Envelopes, pitch and the filter's coefficients
 * work every CTL samples. */
#include <stdint.h>
#include "fw_m7.h"
#include "pk_ram.h"                 /* voice-map: PK_ARENA(out) */
#include "oh_sound.h"
#include "oh_presets.h"

#define SR              48000.0f
#define NOTE_DEF        36          /* a trigger without a note: C2 */
#define CTL             16          /* samples between control steps (a block) */
#define LEVEL           14000.0f    /* the scale before the limiter: clean presets peak ~5-8k (stock
                                     * tonal voices ~8000; the ladder takes a lot of a saw's level) */
#define LIM_THR         9000.0f     /* the output limiter: peaks held near this */
#define LIM_ATT         0.067f      /* 0.3 ms attack */
#define LIM_REL         0.999861f   /* 150 ms release */
#define CLIP_KNEE       8500.0f     /* after the limiter, a soft clip rounds what its attack lets */
#define CLIP_CEIL       10000.0f    /*   through: untouched below the knee, never above the ceiling */
#define DC_R            0.9995f     /* DC blocker pole (~4 Hz) */
#define TAIL_FADE       0.98f       /* its offset after the note: gone in ~5 ms */
#define DRONE_AT        0.95f
#define DETENT          0.02f       /* +-2% around 12 o'clock is the sound's own value */
#define MOVED           (82.0f / 4095.0f)   /* knob travel (2%) that hands PARAM2 a new value */
#define DRIVE_DB        3.0f        /* the level for DRIVE: +3 dB at none, -3 dB at full */
#define DRIVE_SMOOTH    (1.0f / 960.0f)
#define PI              3.14159265f
#define LOG2_E          1.4426950f
#define MAGIC           0x33304853u /* '3OHS' */

/* the filter */
#define CUT_LO          160.0f      /* the cutoff knob's range: the bass line's 314-2394 Hz, */
#define CUT_OCT         4.966f      /*   widened as the Devil Fish's: 160 Hz-5 kHz */
#define ENV_MIN         0.3f        /* the envelope's sweep (octaves) at no ENV MOD (the bass line: 0.75) */
#define ENV_MAX         5.0f        /*   and at full (the bass line: ~4.6-5.1) */
#define ENV_OFF_C       0.294f      /* the sweep's share below the cutoff: oC + oF x the knob (Open303) */
#define ENV_OFF_F       0.048f
#define ACC_OCT         2.0f        /* the accent sweep at full accent (octaves) */
#define RES_EDGE        0.985f      /* feedback at full resonance, of the edge of self-oscillation */
#define EDGE_CAP        19.5f       /* ... at most (the edge at ~1.5 kHz; below ~700 Hz the edge rises and
                                     * the resonance falls away: a low cutoff goes dark, not screaming) */
#define BASS_BACK       0.15f       /* at full resonance, the input's low end (BASS_HZ) added back: the
                                     * resonance thins the sound a little less (the scream stays) */
#define BASS_HZ         180.0f
#define FB_HP           100.0f      /* the feedback path's high-pass (a1k0n's x0xb0x measurement) */
#define FB_GH           0.006502519f /*   its G: tan(pi FB_HP / SR) / (1 + tan(...)) */
#define IN_HP           44.0f       /* the input's coupling high-pass */
#define SAT_L           8.0f        /* the ladder's input saturator: L tanh(u / L) (bounds extremes; Filter
                                     * drive pushes into it) */
#define FMIN            30.0f       /* the resonant peak's range */
#define FMAX            14000.0f

/* envelopes (time constants: the time to fall to 1/e) */
#define DEC_T10         0.1f        /* the filter envelope falls to 10% in DEC_T10 + DEC_T10_RANGE x DECAY */
#define DEC_T10_RANGE   1.2f        /*   seconds (a1k0n measured 0.1 + DECAY on a x0xb0x: 0.1-1.1 s) */
#define ACC_DECAY       0.0434f     /* an accented note's: DECAY fully left (time constant, 0.1 s to 10%) */
#define ENV_ATT         0.003f      /* the filter envelope's attack */
#define AMP_ATT         0.003f      /* the amp envelope's attack */
#define AMP_DECAY       1.23f       /* the amp's own decay while the gate is open (Open303) */
#define AMP_REL         0.003f      /* the amp's release at the gate's end (the bass line: ~16 ms gone) */
#define ACC_REL         0.012f      /*   an accented note's */
#define ACC_GAIN        1.5f        /* an accent's level: + the envelope x this x ACCENT */
#define ACC_CHARGE      0.147f      /* the accent sweep's capacitor (1 uF, 147 k) at full resonance */
#define ACC_DRAIN       0.5f        /* its drain */
#define GATE_FRAC       0.58f       /* the gate: 3.5 of a sixteenth's 6 clocks */
#define GATE_MIN        0.02f
#define SLIDE_MIN       0.02f       /* slide time constant: field 0 (field 12: ~60 ms, the bass line's) */
#define SLIDE_OCT       4.0f        /*   field 31: 16x that */
#define SQ_PW           0.47f       /* the square's high share (the bass line's: about 47 / 53) */
#define PULSE_PW        0.25f       /* MODE position 3 */

/* distortion */
#define DIST_SMOOTH     0.25f
#define DRV_AVG         (1.0f / 2400.0f)    /* the levels before and after it: 50 ms averages */
#define MUF_HZ          200.0f      /* Muffler: the bass under this stays clean */
#define OD_HP           720.0f      /* Overdrive: the clipped part is high-passed (the mid hump) */
#define TONE_HZ         900.0f      /* the tone's tilt pivot */
#define TONE_OCT        1.5f        /* +-9 dB tilt at the ends */

/* The feedback at which the ladder (with the high-pass in its loop) oscillates, for resonant peaks
 * from 30 Hz to 14 kHz in 32 equal steps of log frequency (worked out exactly for this discrete
 * filter; docs: kosc.py). Full resonance is RES_EDGE of it, so the filter sits just under
 * self-oscillation at any cutoff, as Open303's fitted k does (a fixed k would leave mid cutoffs
 * far below it: the high-pass raises the edge to ~20 at 1 kHz). */
#define EDGE_N          33
#define EDGE_F0         30.0f
#define EDGE_STEPS      3.6092f     /* (EDGE_N - 1) / log2(14000 / 30) */
static const float EDGE[EDGE_N] = {
    172.2221f, 136.9353f, 109.8822f, 89.1615f, 73.2824f, 61.0902f, 51.6999f, 44.4385f, 38.7971f,
    34.3922f, 30.9348f, 28.2072f, 26.0445f, 24.3216f, 22.9430f, 21.8355f, 20.9424f, 20.2199f,
    19.6336f, 19.1566f, 18.7675f, 18.4494f, 18.1888f, 17.9749f, 17.7988f, 17.6535f, 17.5333f,
    17.4334f, 17.3498f, 17.2794f, 17.2194f, 17.1673f, 17.1210f,
};

/* switch values are 1, 0, 2 for panel positions 1, 2, 3 */
enum { POS1 = 1, POS2 = 0, POS3 = 2 };
enum { R_ENV, R_ACC, R_DIST, NROLE };
enum { W_SAW, W_SQUARE, W_PULSE };
enum { IDLE, ATTACK, DECAY, RELEASE };
enum { C_G, C_C, C_H, C_D23, C_G23, C_D12, C_G12, C_D01, C_G01, C_G04, C_DK, C_K, NCOEF };

#ifndef OH_DEFAULT_PRESETS          /* each output's preset until the M4 sends a sound */
#define OH_DEFAULT_PRESETS {0, 1, 9, 13}
#endif
static const uint8_t DEFAULT_PRESET[4] = OH_DEFAULT_PRESETS;

/* ---- from the M4 ------------------------------------------------------------------------
 * "three-oh-live": the tempo x 10 (2 bytes, little-endian), then each output's DRIVE (0-255).
 * Kept in the M7 host's RAM: 8 bytes. */
static float bpm;                       /* 0: none yet (120 BPM, DRIVE at 0 dB) */
static uint8_t drive[4];

void pk_oh_on_live(const uint8_t *buf, uint32_t len)
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

/* "three-oh": each output's last sound (pk_oh_on_sound) */
#define F_HELD          0x10
#define F_KEEP          0x20
static uint8_t stash[4][OH_PACKED + 2] __attribute__((aligned(4)));    /* [0] flags, [1..] sound */
static uint32_t stashed;                /* outputs (bits) with a new sound; << 4: ever sent */

/* ---- the object ---------------------------------------------------------------------------- */

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
    uint8_t v[OH_NFIELD];               /* +0xc0: the sound playing (the tests read it) */
    float role[NROLE];                  /* PARAM2's three values, knob positions 0-1 */
    int32_t out;
    int32_t cur_role, armed;
    float ref2;
    int32_t held;                       /* the M4 holds PARAM2 (a kit load: positions coming) */
    /* knobs */
    float k_cut, k_res, k_dec;
    /* the decoded sound */
    float u_cut, u_res, u_env, u_dist, accent, slide_a, trim, tone_lo, tone_hi;
    int32_t dtype;
    /* the note */
    int32_t elapsed, gate, released, sounding;
    int32_t slide_next;                 /* this note's gate holds: the next note slides in */
    int32_t acc;                        /* this note is accented */
    float hz, hz_to;                    /* the oscillator's frequency now, and where it glides */
    /* envelopes */
    float fenv, fenv_att;               /* the filter envelope (1 at the attack's end), attack step */
    int32_t fenv_up;
    float acap;                         /* the accent sweep's capacitor */
    float amp, amp_att, amp_dec, amp_rel, acc_rel;
    int32_t amp_stage;
    float lvl, lvl_step;                /* the accent's level, ramped over the block */
    /* the block */
    int32_t bi;                         /* the next sample of buf to play; CTL: a new block */
    int32_t stop;                       /* the amp envelope ended in this block */
    float buf[CTL];
    /* oscillator */
    float ph, dt;
    int32_t wave;
    /* filter: coefficients; the ladder's four states, the feedback high-pass's; output gain */
    float c[NCOEF];
    float s1, s2, s3, s4, shp;
    float og, pre_g, bass, blp;         /* output gain, Filter drive; the low end added back, its state */
    float hp_x, hp_y, hp_r;             /* the input's high-pass */
    /* distortion: drive, bias, wet (smoothed); the clip's last input and antiderivative */
    float d_g, d_b, d_w, d_c, d_m, d_x1, d_F1, d_in, d_out;
    float od_x, od_y, mlp, tone_s;      /* Overdrive's high-pass; Muffler's low band; the tone's state */
    /* out */
    float dc_x, dc_y, lim, dgain, dgain_t;
} oh_t;

_Static_assert(__builtin_offsetof(oh_t, algo) == OFF_cVoice_algo, "algo");
_Static_assert(__builtin_offsetof(oh_t, mode) == OFF_cVoice_mode, "mode");
_Static_assert(__builtin_offsetof(oh_t, velocity) == OFF_cVoice_velocity, "velocity");
_Static_assert(__builtin_offsetof(oh_t, note) == OFF_cVoice_accent, "note (accent)");
_Static_assert(__builtin_offsetof(oh_t, param) == OFF_cVoice_param_ptrs, "param pointers");
_Static_assert(__builtin_offsetof(oh_t, mute) == OFF_cVoice_mute, "mute");
_Static_assert(__builtin_offsetof(oh_t, gain) == OFF_cVoice_gain, "gain");
_Static_assert(__builtin_offsetof(oh_t, v) == 0xc0, "the sound (the tests read it)");
_Static_assert(sizeof(oh_t) <= PK_ARENA_SIZE, "fits a voice-map arena");

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

/* the distortion's cubic soft clip, u - u^3 / 3 up to |u| = 1, then flat at +-2/3, and its
 * antiderivative (first-order antiderivative antialiasing) */
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

/* sin(2 pi p), any p (a 7th-order series on the folded phase) */
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

/* tan(pi f / SR), 0 < f < SR / 2 */
static float tan_pi(float f)
{
    float p = f * (0.5f / SR);
    return sin2pi(p) / sin2pi(p + 0.25f);
}

INLINE float knob(const oh_t *k, int i)
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

/* the factor per n samples of a decay with time constant t (seconds) */
static float tau_c(float t, float n)
{
    return exp2f_(-LOG2_E * n / (t * SR));
}

/* ---- the sound ----------------------------------------------------------------------------- */

static float p2_of(int v)               /* a PARAM2 job's stored value: 16 is 12 o'clock */
{
    return clampf(0.5f + (float)(v - OH_MID) * (1.0f / 30.0f), 0.0f, 1.0f);
}

static void decode(oh_t *k)
{
    const uint8_t *v = k->v;
    k->u_cut = (float)v[OH_CUTOFF] * (1.0f / 31.0f);
    k->u_res = (float)v[OH_RES] * (1.0f / 31.0f);
    k->u_env = (float)v[OH_ENVMOD] * (1.0f / 31.0f);
    k->u_dist = (float)v[OH_DIST] * (1.0f / 31.0f);
    k->accent = (float)v[OH_ACCENT] * (1.0f / 31.0f);
    k->dtype = v[OH_DTYPE] < OH_NDTYPE ? v[OH_DTYPE] : OH_D_MUFFLER;
    float t = SLIDE_MIN * exp2f_((float)v[OH_SLIDE] * (SLIDE_OCT / 31.0f));
    k->slide_a = 1.0f - tau_c(t, (float)CTL);
    k->trim = LEVEL * exp2f_((float)((int)v[OH_TRIM] - 8) * (1.5f / 6.0205999f));
    float tilt = ((float)v[OH_TONE] - 5.5f) * (1.0f / 5.5f) * TONE_OCT;    /* octaves = 6 dB steps */
    k->tone_lo = exp2f_(-tilt);
    k->tone_hi = exp2f_(tilt);
}

/* a new sound: its values, and PARAM2's three jobs at the sound's (the knob takes over once it
 * moves) */
static void use_sound(oh_t *k, const uint8_t *v, int keep)
{
    for (int i = 0; i < OH_NFIELD; i++)
        k->v[i] = v[i];
    oh_clean(k->v);
    if (!keep) {
        for (int r = 0; r < NROLE; r++)
            k->role[r] = p2_of(k->v[OH_P2_ENV + r]);
        k->armed = 0;
        k->ref2 = knob(k, 3);
    }
    decode(k);
}

static void take_stash(oh_t *k)
{
    stashed &= ~(1u << k->out);
    uint8_t v[OH_NFIELD];
    oh_unpack(stash[k->out] + 1, v);
    use_sound(k, v, stash[k->out][0] & F_KEEP);
    k->held = stash[k->out][0] & F_HELD ? 1 : 0;
}

/* "three-oh": [0] the output (bits 0-1), held (bit 4: PARAM2 waits for the kit's knob
 * positions), keep (bit 5: a page edit: PARAM2's values stay as they play), [1..] the packed
 * sound. Kept in the M7 host's RAM: an output switching to 3 Oh at its next note (a kit load)
 * starts with it. */
void pk_oh_on_sound(const uint8_t *buf, uint32_t len)
{
    if (len < OH_PACKED + 1)
        return;
    int out = buf[0] & 3;
    for (int i = 0; i < OH_PACKED + 1; i++)
        stash[out][i] = buf[i];
    stashed |= (1u << out) | (1u << (4 + out));
}

/* ---- control ------------------------------------------------------------------------------- */

static int role_of(int algo)
{
    return algo == POS1 ? R_ENV : (algo == POS3 ? R_DIST : R_ACC);
}

static void read_knobs(oh_t *k)
{
    if (stashed & (1u << k->out))       /* a sound from the M4 */
        take_stash(k);
    k->k_cut = knob(k, 0);
    k->k_dec = knob(k, 1);
    k->k_res = knob(k, 2);
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
    k->wave = k->mode == POS1 ? W_SAW : (k->mode == POS3 ? W_PULSE : W_SQUARE);
    if (bpm > 0.0f) {                   /* the M4 has sent DRIVE (with the tempo); until then 0 dB */
        float d = (float)drive[k->out] * (1.0f / 255.0f);
        float db = d < 0.5f ? DRIVE_DB * (1.0f - 2.0f * d) : -DRIVE_DB * (2.0f * d - 1.0f);
        k->dgain_t = exp2f_(db * (1.0f / 6.0205999f));
    } else {
        k->dgain_t = 1.0f;
    }
}

/* the gate, in samples (-1: drone, DECAY at the top): 58% of a sixteenth at the tempo, as the
 * bass line's sequencer; DECAY doesn't change it (on the bass line it only sets the filter
 * envelope's decay) */
static int32_t gate_samples(const oh_t *k)
{
    float d = k->k_dec;
    if (d >= DRONE_AT)
        return -1;
    float step = 15.0f / (bpm > 0.0f ? bpm : 120.0f);       /* a sixteenth, seconds */
    float g = GATE_FRAC * step;
    if (g < GATE_MIN)
        g = GATE_MIN;
    return (int32_t)(g * SR);
}

/* DECAY: the filter envelope's decay time constant: from 43 ms to 0.56 s (to 10% in 0.1-1.3 s) */
static float fenv_decay(const oh_t *k)
{
    float d = k->k_dec < DRONE_AT ? k->k_dec : DRONE_AT;
    return (DEC_T10 + DEC_T10_RANGE * d * (1.0f / DRONE_AT)) * (1.0f / 2.3025851f);
}

static void set_note(oh_t *k, float midi, int glide)
{
    k->hz_to = exp2f_((midi - 69.0f) * (1.0f / 12.0f) + 8.7813597f);  /* log2(440) */
    if (!glide)
        k->hz = k->hz_to;
}

static void update(cVoice *v)
{
    oh_t *k = (oh_t *)v;
    if (k->magic != MAGIC)
        return;
    read_knobs(k);
}

/* the ladder's coefficients for resonant peak fpk and feedback kk (the report's nested
 * zero-delay solve: each stage y = G in + c s; stage 1 takes u + y2, stages 2-4 half their
 * neighbours') */
static void filter_coefs(oh_t *k, float fpk, float kk)
{
    /* the stages' frequency is sqrt 2 x the peak's; prewarped at the peak (not the stages': that
     * would tune the peak flat at high cutoffs), so it sits exactly at fpk at any cutoff */
    float gt = 1.41421356f * tan_pi(fpk), *c = k->c;
    float G = gt / (1.0f + gt), h = 0.5f * G;
    float d23 = 1.0f / (1.0f - h * h), g23 = h * d23;
    float d12 = 1.0f / (1.0f - h * g23), g12 = h * d12;
    float d01 = 1.0f / (1.0f - G * g12), g01 = G * d01;
    float g04 = g01 * g12 * g23 * h;
    c[C_G] = G;
    c[C_C] = 1.0f - G;                  /* 1 / (1 + g) */
    c[C_H] = h;
    c[C_D23] = d23;
    c[C_G23] = g23;
    c[C_D12] = d12;
    c[C_G12] = g12;
    c[C_D01] = d01;
    c[C_G01] = g01;
    c[C_G04] = g04;
    c[C_DK] = 1.0f / (1.0f + kk * g04 * (1.0f - FB_GH));
    c[C_K] = kk;
}

/* every CTL samples: envelopes, pitch, the oscillator's step, the filter, distortion */
static void control(oh_t *k)
{
    float res = warp(k->k_res, k->u_res), env = warp(k->role[R_ENV], k->u_env);
    float accent = warp(k->role[R_ACC], k->accent);
    /* the filter envelope: a short attack, then its decay (accented: 200 ms) */
    if (k->fenv_up) {
        k->fenv += k->fenv_att;
        if (k->fenv >= 1.0f) {
            k->fenv = 1.0f;
            k->fenv_up = 0;
        }
    } else {
        k->fenv *= tau_c(k->acc ? ACC_DECAY : fenv_decay(k), (float)CTL);
    }
    float fe = k->fenv;
    /* the accent sweep: at low resonance the envelope itself; at high resonance a capacitor it
     * charges (slower the higher the resonance, as the pot's second gang), draining slowly */
    float spike = k->acc ? accent * fe : 0.0f;
    if (spike > k->acap)
        k->acap += (spike - k->acap) * (1.0f - tau_c(0.001f + res * ACC_CHARGE, (float)CTL));
    else
        k->acap *= tau_c(ACC_DRAIN, (float)CTL);
    float sweep = (1.0f - res) * spike + res * 1.5f * k->acap;
    /* the level: an accent adds the envelope */
    float lt = 1.0f + (k->acc ? ACC_GAIN * accent * fe : 0.0f);
    k->lvl_step = (lt - k->lvl) * (1.0f / CTL);
    /* the pitch: a slide glides (an RC on the frequency) */
    if (k->hz != k->hz_to) {
        k->hz += (k->hz_to - k->hz) * k->slide_a;
        if (absf(k->hz_to - k->hz) < 0.01f)
            k->hz = k->hz_to;
    }
    k->dt = clampf(k->hz, 8.0f, 12000.0f) * (1.0f / SR);

    /* the cutoff: PARAM1 around the sound's; the envelope sweeps s octaves, a share of them below
     * the cutoff (ENV MOD also lowers the base); the accent sweep */
    float cn = warp(k->k_cut, k->u_cut);
    float s = ENV_MIN + env * (ENV_MAX - ENV_MIN), o = ENV_OFF_C + ENV_OFF_F * cn;
    float oct = cn * CUT_OCT + s * (fe - o) + sweep * ACC_OCT;
    oct = clampf(oct, -2.415f, 6.451f);  /* the peak within FMIN-FMAX (30 Hz-14 kHz) */
    float fpk = CUT_LO * exp2f_(oct);
    /* resonance: Open303's knob curve (1 - e^-3r) / (1 - e^-3), of the edge at this cutoff */
    float ex = (oct + 2.4150375f) * EDGE_STEPS;          /* log2(CUT_LO / EDGE_F0) = 2.415 */
    int ei = (int)ex;
    ei = ei < 0 ? 0 : (ei > EDGE_N - 2 ? EDGE_N - 2 : ei);
    float ef = clampf(ex - (float)ei, 0.0f, 1.0f);
    float edge = EDGE[ei] + (EDGE[ei + 1] - EDGE[ei]) * ef;
    edge = edge < EDGE_CAP ? edge : EDGE_CAP;
    float rs = (1.0f - exp2f_(-3.0f * LOG2_E * res)) * (1.0f / 0.95021293f);
    float kk = RES_EDGE * edge * rs;
    k->bass = BASS_BACK * rs;
    filter_coefs(k, fpk, kk);
    /* the output's lift: some of what the resonance takes from the passband, back; Filter
     * drive's level back down (its input pushed into the saturator) */
    float r = rsqrtf_(k->pre_g);
    k->og = (1.0f + 0.06f * kk) * r * rsqrtf_(k->pre_g * r);   /* x pre_g^-0.75 */

    /* distortion: the amount (PARAM2's third job around the sound's); the type's drive */
    float d = warp(k->role[R_DIST], k->u_dist);
    float g = 1.0f, b = 0.0f, w = d > 0.002f ? 1.0f : 0.0f, pre = 1.0f;
    switch (k->dtype) {
    case OH_D_MUFFLER:   g = 1.0f + 9.0f * d; w = clampf(d * 4.0f, 0.0f, 1.0f); break;
    case OH_D_FILTER:    pre = 1.0f + 60.0f * d * d; w = clampf(d * 4.0f, 0.0f, 1.0f); break;
    case OH_D_OVERDRIVE: g = 2.0f + 40.0f * d * d; b = 0.18f; break;
    default:             g = 3.0f + 120.0f * d * d; b = 0.05f; break;
    }
    k->d_g += (g - k->d_g) * DIST_SMOOTH;
    k->d_b += (b - k->d_b) * DIST_SMOOTH;
    k->d_w += (w - k->d_w) * DIST_SMOOTH;
    k->pre_g += (pre - k->pre_g) * DIST_SMOOTH;
    k->d_c = clip3(k->d_b);
    if (k->d_w > 0.0005f && k->d_out > 1e-9f) {
        /* make-up: the distorted sound back to the clean one's level, between -16 and +12 dB */
        float t = k->d_in > 1e-9f ? k->d_in * rsqrtf_(k->d_in) * rsqrtf_(k->d_out) : 1.0f;
        k->d_m += (clampf(t, 0.16f, 4.0f) - k->d_m) * DIST_SMOOTH;
    }
    k->dgain += (k->dgain_t - k->dgain) * (DRIVE_SMOOTH * CTL);
}

/* ---- notes --------------------------------------------------------------------------------- */

static void trigger(cVoice *v)
{
    oh_t *k = (oh_t *)v;
    if (k->magic != MAGIC)
        return;
    read_knobs(k);
    /* a slide: the note before holds its gate for it, and it's still open */
    int slide = k->sounding && !k->released && k->slide_next;
    /* the note and how it plays: its velocity (oh_sound.h) */
    int vel = k->velocity;
    k->acc = OH_ACC(vel);
    k->slide_next = OH_SLD(vel);
    set_note(k, (float)(k->note ? k->note : NOTE_DEF), slide);
    if (!k->sounding) {                 /* from silence: the same start every time */
        k->ph = 0.0f;
        k->s1 = k->s2 = k->s3 = k->s4 = k->shp = k->blp = 0.0f;
        k->hp_x = k->hp_y = 0.0f;
        k->amp = 0.0f;
        k->fenv = 0.0f;
        k->lvl = 1.0f;
        k->od_x = k->od_y = k->mlp = k->tone_s = 0.0f;
        k->d_x1 = k->d_F1 = 0.0f;
    }
    if (!slide) {                       /* the envelopes start again, from where they are */
        k->fenv_up = 1;
        k->fenv_att = (float)CTL / (ENV_ATT * SR);
        k->amp_stage = ATTACK;
    }
    k->gate = gate_samples(k);
    k->elapsed = 0;
    k->released = 0;
    k->sounding = 1;
    k->bi = CTL;                        /* a new block (a control step) at once */
    k->stop = 0;
}

static void init(cVoice *v)
{
    oh_t *k = (oh_t *)v;
    k->magic = MAGIC;
    k->cur_role = role_of(k->algo);
    k->ref2 = knob(k, 3);
    k->armed = 0;
    k->d_g = k->d_m = k->pre_g = k->og = 1.0f;
    k->hp_r = 1.0f - 2.0f * PI * IN_HP / SR;
    k->amp_att = 1.0f / (AMP_ATT * SR);
    k->amp_dec = tau_c(AMP_DECAY, 1.0f);
    k->amp_rel = tau_c(AMP_REL, 1.0f);
    k->acc_rel = tau_c(ACC_REL, 1.0f);
    read_knobs(k);
    k->dgain = k->dgain_t;
    set_note(k, (float)NOTE_DEF, 0);
    filter_coefs(k, 1000.0f, 0.0f);
}

/* ---- audio: a block of CTL samples at each control step ------------------------------------ */

/* PolyBLEP: the correction for a unit step at phase 0, t the phase, dt the step */
INLINE float blep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

/* the oscillator over the block, wave w (a constant: each its own loop), through the input's
 * high-pass */
INLINE void osc_block(oh_t *k, int w)
{
    float p = k->ph, dt = k->dt, *buf = k->buf, hx = k->hp_x, hy = k->hp_y, hr = k->hp_r;
    const float pw = w == W_PULSE ? PULSE_PW : SQ_PW;
    for (int i = 0; i < CTL; i++) {
        float y;
        if (w == W_SAW) {
            y = 2.0f * p - 1.0f - blep(p, dt);
        } else {
            float q = p - pw;
            if (q < 0.0f)
                q += 1.0f;
            y = (p < pw ? 1.0f : -1.0f) + blep(p, dt) - blep(q, dt);
        }
        p += dt;
        if (p >= 1.0f)
            p -= 1.0f;
        float h = y - hx + hr * hy;     /* the input's coupling high-pass */
        hx = y;
        hy = h;
        buf[i] = h;
    }
    k->ph = p;
    k->hp_x = hx;
    k->hp_y = hy;
}

static __attribute__((noinline)) void osc_wave(oh_t *k)
{
    switch (k->wave) {
    case W_SQUARE: osc_block(k, W_SQUARE); break;
    case W_PULSE:  osc_block(k, W_PULSE); break;
    default:       osc_block(k, W_SAW); break;
    }
}

/* the ladder over the block. The states' parts S = c s; the loop solved linearly for the fourth
 * stage (with the feedback high-pass in it), then the input u = x - k hp(y4) saturated, and the
 * stages run from it; trapezoidal updates s = 2 y - s. Filter drive: x pushed (pre_g) into the
 * saturator, which also quenches the resonance (as the bass line's overdriven filter) */
static __attribute__((noinline)) void ladder_block(oh_t *k)
{
    const float *c = k->c;
    const float G = c[C_G], cc = c[C_C], h = c[C_H], d23 = c[C_D23], g23 = c[C_G23];
    const float d12 = c[C_D12], g12 = c[C_G12], d01 = c[C_D01], g01 = c[C_G01], g04 = c[C_G04];
    const float dk = c[C_DK], kk = c[C_K];
    const float gh = FB_GH, ch = 1.0f - FB_GH;
    float s1 = k->s1, s2 = k->s2, s3 = k->s3, s4 = k->s4, shp = k->shp, og = k->og, pre = k->pre_g;
    float *buf = k->buf, bass = k->bass, blp = k->blp;
    const float ba = 2.0f * PI * BASS_HZ / SR;
    for (int i = 0; i < CTL; i++) {
        blp += (buf[i] - blp) * ba;     /* the input's low end (clean, before Filter drive) */
        float x = buf[i] * pre;
        float S1 = cc * s1, S2 = cc * s2, S3 = cc * s3, S4 = cc * s4;
        float t23 = (h * S4 + S3) * d23;
        float t12 = (h * t23 + S2) * d12;
        float t01 = (G * t12 + S1) * d01;
        float t04 = h * (g23 * (g12 * t01 + t12) + t23) + S4;
        float Shp = ch * shp;
        float y4e = (g04 * (x + kk * Shp) + t04) * dk;     /* the linear estimate */
        float fb = ch * y4e - Shp;                          /* its high-pass: (1 - gh) y - Shp */
        float u = SAT_L * tanhf_((x - kk * fb) * (1.0f / SAT_L));
        float y1 = g01 * u + t01;
        float y2 = g12 * y1 + t12;
        float y3 = g23 * y2 + t23;
        float y4 = h * y3 + S4;
        s1 = 2.0f * y1 - s1;
        s2 = 2.0f * y2 - s2;
        s3 = 2.0f * y3 - s3;
        s4 = 2.0f * y4 - s4;
        float lp = gh * y4 + Shp;
        shp = 2.0f * lp - shp;
        buf[i] = y4 * og + bass * blp;
    }
    k->blp = blp;
    k->s1 = s1;
    k->s2 = s2;
    k->s3 = s3;
    k->s4 = s4;
    k->shp = shp;
}

/* the amp envelope over the block into a; returns how many samples it sounds (fewer than CTL
 * when it ends) */
static __attribute__((noinline)) int amp_block(oh_t *k, float *a)
{
    float l = k->amp, rel = k->acc ? k->acc_rel : k->amp_rel;
    int hold = k->gate < 0 && !k->released;     /* drone: it holds */
    for (int i = 0; i < CTL; i++) {
        switch (k->amp_stage) {
        case ATTACK:
            l += k->amp_att;
            if (l >= 1.0f) {
                l = 1.0f;
                k->amp_stage = DECAY;
            }
            break;
        case DECAY:
            if (!hold)
                l *= k->amp_dec;
            break;
        case RELEASE:
            l *= rel;
            if (l < 0.0005f) {
                k->amp = 0.0f;
                k->amp_stage = IDLE;
                return i;
            }
            break;
        default:
            k->amp = 0.0f;
            return i;
        }
        a[i] = l;
    }
    k->amp = l;
    return CTL;
}

/* the distortion after the amp (Muffler: the bass under MUF_HZ clean, the rest soft-clipped;
 * Overdrive: the clipped part high-passed at OD_HP, over the clean sound; Distortion: hard;
 * all antialiased; Filter drive: nothing here, it's in the ladder), then the tone (a tilt
 * around TONE_HZ), crossfaded in by d_w; the levels before and after (its make-up) follow the
 * block's mean squares */
static __attribute__((noinline)) void dist_block(oh_t *k, int n)
{
    float *buf = k->buf, g = k->d_g, b = k->d_b, c = k->d_c, m = k->d_m, w = k->d_w;
    float x1 = k->d_x1, F1 = k->d_F1, ox = k->od_x, oy = k->od_y, ml = k->mlp, ts = k->tone_s;
    float tl = k->tone_lo, th = k->tone_hi, sin_ = 0.0f, sout = 0.0f;
    const float tg = 2.0f * PI * TONE_HZ / SR, odr = 1.0f - 2.0f * PI * OD_HP / SR;
    const float mg = 2.0f * PI * MUF_HZ / SR;
    int t = k->dtype;
    for (int i = 0; i < n; i++) {
        float x = buf[i], y;
        if (t == OH_D_FILTER) {
            y = x;
        } else {
            float in = x, lo = 0.0f;
            if (t == OH_D_MUFFLER) {    /* the bass apart */
                ml += (x - ml) * mg;
                lo = ml;
                in = x - ml;
            } else if (t == OH_D_OVERDRIVE) {   /* the clipped part high-passed: the mids lead */
                float hh = x - ox + odr * oy;
                ox = x;
                oy = hh;
                in = hh;
                lo = 0.5f * x;          /* the clean sound under it */
            }
            float u = in * g + b, F = clip3_F(u), du = u - x1;
            y = (du > 1e-4f || du < -1e-4f ? (F - F1) / du : clip3(0.5f * (u + x1))) - c + lo;
            x1 = u;
            F1 = F;
        }
        ts += (y - ts) * tg;
        y = ts * tl + (y - ts) * th;
        sin_ += x * x;
        sout += y * y;
        buf[i] = x + w * (y * m - x);
    }
    k->d_x1 = x1;
    k->d_F1 = F1;
    k->od_x = ox;
    k->od_y = oy;
    k->mlp = ml;
    k->tone_s = ts;
    k->d_in += (sin_ * (1.0f / CTL) - k->d_in) * (DRV_AVG * CTL);
    k->d_out += (sout * (1.0f / CTL) - k->d_out) * (DRV_AVG * CTL);
}

/* the block: control, the gate, then each stage over CTL samples */
static __attribute__((noinline)) void block(oh_t *k)
{
    control(k);
    /* the gate: the note's release (a slide holds it for the next note, up to a beat) */
    if (!k->released && k->gate >= 0) {
        int32_t g = k->gate;
        if (k->slide_next) {
            int32_t beat = (int32_t)(60.0f / (bpm > 0.0f ? bpm : 120.0f) * SR);
            g = g > beat ? g : beat;
        }
        if (k->elapsed >= g) {
            k->released = 1;
            if (k->amp_stage != IDLE)
                k->amp_stage = RELEASE;
        }
    }
    k->elapsed += CTL;

    osc_wave(k);
    ladder_block(k);
    float a[CTL], *buf = k->buf;
    int n = amp_block(k, a);
    float l = k->lvl, ls = k->lvl_step;
    for (int i = 0; i < n; i++) {
        l += ls;
        buf[i] *= a[i] * l;
    }
    k->lvl = l + ls * (float)(CTL - n);
    if (k->d_w > 0.0005f)
        dist_block(k, n);
    /* the DC blocker, the limiter and the soft clip, the level for DRIVE. After the amp
     * envelope ends, the blocker's offset fades out (~5 ms) before the voice stops: no step */
    float tr = k->trim, dx = k->dc_x, dy = k->dc_y, lim = k->lim, dg = k->dgain;
    for (int i = 0; i < CTL; i++) {
        float y = i < n ? buf[i] * tr : 0.0f;
        float d = y - dx + DC_R * dy;
        if (i >= n)
            d *= TAIL_FADE;
        dx = y;
        dy = d;
        float m = absf(d);
        lim = m > lim ? lim + (m - lim) * LIM_ATT : lim * LIM_REL;
        if (lim > LIM_THR)
            d *= LIM_THR / lim;
        d = soft_knee(d, CLIP_KNEE, CLIP_CEIL);
        buf[i] = d * dg;            /* within +-14,200: the clip's ceiling, DRIVE's +3 dB */
    }
    k->dc_x = dx;
    k->dc_y = dy;
    k->lim = lim;
    k->stop = n < CTL && absf(dy) < 0.5f;
}

static int16_t render(cVoice *v)
{
    oh_t *k = (oh_t *)v;
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
static const char oh_name[] = "8cThreeOh";
static const struct { const void *vptr; const char *name; } oh_typeinfo = {0, oh_name};
static const struct {
    const void *typeinfo;
    cVoice_vtbl vtbl;
} oh_vtable = {&oh_typeinfo, {trigger, init, update, render}};

/* voice-map: build a 3 Oh in `obj` (an output's arena) for output `out` */
void pk_oh_build(uint8_t *obj, unsigned out)
{
    uint32_t *w = (uint32_t *)obj;
    for (unsigned i = 0; i < (sizeof(oh_t) + 3) / 4; i++)
        w[i] = 0;
    oh_t *k = (oh_t *)obj;
    k->vtbl = &oh_vtable.vtbl;
    for (int i = 0; i < 4; i++)
        k->param[i] = &FW_PARAMS[out][i];
    k->note = NOTE_DEF;
    k->out = (int32_t)(out & 3);
    k->magic = MAGIC;
    if (stashed & (1u << (4 + k->out)))     /* the M4 has sent this output's sound */
        stashed |= 1u << k->out;
    else
        use_sound(k, PRESETS[DEFAULT_PRESET[k->out] & (OH_NPRESET - 1)], 0);
    init((cVoice *)k);
}
