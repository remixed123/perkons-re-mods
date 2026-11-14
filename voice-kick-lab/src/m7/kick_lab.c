/* Kick Lab: a kick drum synth voice (M7), from the analog kick of a drum synth I built on iOS.
 *
 * An extra voice-map engine ("kick_lab"): voice-map builds one in an output's arena with
 * pk_kl_build(). The object is cVoice-compatible: the fields the firmware writes (algo,
 * mode, velocity, note, param pointers, mute, gain) sit at the stock offsets, and the
 * vtable has the four methods the firmware calls (trigger, init, update, render).
 *
 *   TUNE (param 0)    pitch, 20-200 Hz, exponential; the trigger note transposes by
 *                     (note - 36) semitones, as on the stock voices
 *   DECAY (param 1)   amp decay, 30 ms-4 s; from 95% of its travel on: drone
 *   PARAM1 (param 2)  sweep, 0-500 Hz above the pitch at the hit
 *   PARAM2 (param 3)  ALGO's job: attack (position 1), delay mix (2), drive mix (3)
 *   MODE              clean (position 1), white noise (2), pink noise (3)
 *
 * The chain: body (sine or triangle, sweep, drop, tick, click, saturation) -> body peak ->
 * drive (soft, hard, crush, fold; 2x oversampled) -> delay (digital, free or synced) ->
 * + rumble (a small Schroeder reverb) -> + noise -> level -> limiter. The delay and the
 * rumble run in the rest of the arena, at reduced rates. The preset sets everything the
 * panel and the page don't.
 */
#include <stdint.h>
#include "fw_m7.h"

#define SR              48000.0f
#define NOTE_REF        36          /* trigger note meaning "no transposition" */
#define LEVEL           21000.0f    /* a full-scale body at velocity 127, before the preset's TRIM */
#define LIM_THR         16000.0f    /* the output limiter: peaks held at this (the stock kick's highest) */
#define LIM_ATT         0.067f      /* 0.3 ms attack (no instant grab: it would square the first peak) */
#define LIM_REL         0.999583f   /* 50 ms release */
#define CLIP_KNEE       15200.0f    /* after the limiter, a soft clip rounds what its attack lets */
#define CLIP_CEIL       16800.0f    /*   through: untouched below the knee, never above the ceiling */
#define NOISE_LEVEL     0.6f        /* the noise layer against the body */
#define DRONE_AT        0.95f       /* DECAY from here up: drone (the knobs stop short of their ends) */
#define DECAY_MIN       0.03f       /* s */
#define DECAY_OCT       7.0588937f  /* log2(4 s / 30 ms) */
#define DRONE_DROP_S    4.0f        /* in drone, the pitch drop runs over this long */
#define TUNE_MIN        20.0f       /* Hz */
#define SWEEP_HZ        500.0f
#define END_ZONE        0.02f       /* PARAM1 / PARAM2: this much at each end reads as the end */
#define LOG2_10         3.3219281f
#define LOG2_E          1.4426950f
#define PI              3.14159265f
#define MAGIC           0x4b4c4142u /* 'KLAB' */
#define COEF_EVERY      16          /* samples between body-peak coefficient updates */
#define MOVED           82          /* knob travel (0-4095, 2%) that hands a value to the knob */

/* the arena: the object, then the lines (int16) */
#define ARENA           0x4000      /* voice-map's arena per output (PK_ARENA_SIZE) */
#define OBJ_MAX         0x400
#define DL_LEN          6004        /* the delay line, at 48 kHz / DL_DIV[band]: 3 s at 2 kHz */
#define RB_DIV          4           /* the rumble runs at 12 kHz */
#define RB_C0           303         /* its combs and allpasses (the 44.1 kHz set, scaled to 12 kHz) */
#define RB_C1           323
#define RB_C2           347
#define RB_C3           368
#define RB_A0           151
#define RB_A1           120
#define RB_LEN          (RB_C0 + RB_C1 + RB_C2 + RB_C3 + RB_A0 + RB_A1)
#define Q_LINE          8192.0f     /* line sample scale: +-4 full scale */
#define LINE_KNEE       3.0f        /* what goes into a line: untouched below this, then bent */
#define LINE_CEIL       4.0f        /*   smoothly towards full scale (no hard clip in a feedback loop) */
_Static_assert(OBJ_MAX + 2 * (DL_LEN + RB_LEN) <= ARENA, "the lines fit the arena");

/* switch values are 1, 0, 2 for panel positions 1, 2, 3 */
enum { POS1 = 1, POS2 = 0, POS3 = 2 };
enum { ROLE_ATTACK, ROLE_DELAY_MIX, ROLE_DRIVE_MIX, NROLE };
enum { NOISE_OFF, NOISE_WHITE, NOISE_PINK };
enum { DRV_SOFT, DRV_HARD, DRV_CRUSH, DRV_FOLD };

/* ---- the page's values -------------------------------------------------------------- */

#define NTIME           14
static const float TIME_S[NTIME] = {0.015f, 0.025f, 0.04f, 0.06f, 0.08f, 0.1f, 0.125f, 0.15f, 0.19f,
                                    0.25f, 0.375f, 0.5f, 0.75f, 1.0f};
/* beats: 1/64, 1/32, 1/16T, 1/16, 1/16D, 1/8T, 1/8, 1/8D, 1/4T, 1/4, 1/4D, 1/2T, 1/2, 1/2D */
static const float DIV_BEATS[NTIME] = {0.0625f, 0.125f, 1.0f / 6, 0.25f, 0.375f, 1.0f / 3, 0.5f, 0.75f,
                                       2.0f / 3, 1.0f, 1.5f, 4.0f / 3, 2.0f, 3.0f};
#define NFB             10          /* feedback 0 .. 0.9 */
/* the level into the line for each feedback: the echoes pile up between hits as the feedback
 * rises, so the line's input is turned down to keep the delay's level near the kick's own
 * (the repeats, their spacing and their decay unchanged; the line stays below clipping) */
static const float DL_IN_G[NFB] = {1.000f, 0.992f, 0.970f, 0.932f, 0.877f, 0.806f, 0.716f, 0.604f, 0.465f, 0.288f};
#define NDTONE          6           /* delay tone 0 .. 1 */
#define NDAMT           9           /* drive amount 1/9 .. 1 */
static const float DRV_TONE[3] = {0.25f, 0.5f, 0.75f};
static const uint8_t DL_DIV[4] = {3, 6, 12, 24};        /* the delay's rate bands: 16, 8, 4, 2 kHz */

typedef struct {
    uint8_t drv_type, drv_amt, drv_tone;    /* row 4 */
    uint8_t dl_sync, dl_time, dl_fb, dl_tone;   /* rows 2 and 3 */
} kl_page_t;

/* a preset: everything the panel and the page don't set, and their starting values */
typedef struct {
    float tune;             /* Hz (TUNE's starting value) */
    float character;        /* sweep, 0-1 (PARAM1) */
    float decay;            /* s (DECAY) */
    float click;            /* noise click level */
    float sweep_time;       /* s, the pitch envelope's length */
    float drop;             /* 0-1: the slow drop, 0 to -24 semitones (squared) */
    float pitch_curve;      /* 0-1: the pitch envelope's depth by its end */
    float r_tone;           /* 0-1: the rumble's low-pass, 80 Hz-5 kHz */
    float r_mix;            /* 0-1: rumble mix, and the amp's decay shape */
    float r_sus;            /* 0-1: the rumble's length, 0.05-4 s */
    float attack;           /* 0-1 (PARAM2 on ALGO 1): 1 ms .. 100 ms */
    float volume;           /* level trim */
    float body_f, body_q, body_amt, body_mod;   /* body peak: Hz, Q, 0-1 (12 dB; 0 = off), 0-1 */
    float drv_bias;         /* -1..1 */
    float drv_mix, dl_mix;  /* PARAM2 on ALGO 3 and 2 (0 = off) */
    uint8_t triangle, sat, tick;
    kl_page_t page;
} kl_preset_t;

#define NPRESET 16
static const kl_preset_t PRESETS[NPRESET] = {
    /*  1 Basic */
    {50.0f, 0.5f, 0.4f, 0.3f, 0.04f, 0.3f, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 0.8f,
     80.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
     0, 0, 0, {0, 2, 1, 0, 8, 3, 2}},
    /*  2 Tight Techno */
    {55.0f, 0.55f, 0.25f, 0.6f, 0.035f, 0.35f, 0.5f, 0.5f, 0.0f, 0.0f, 0.15f, 0.78f,
     80.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f,
     0, 1, 0, {0, 1, 1, 0, 8, 3, 2}},
    /*  3 9x Punch */
    {60.0f, 0.55f, 0.35f, 0.7f, 0.03f, 0.35f, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 0.85f,
     80.0f, 2.0f, 0.0f, 0.0f, 0.2f, 0.8f, 0.0f,
     0, 1, 0, {0, 2, 1, 0, 8, 3, 2}},
    /*  4 8x Punch */
    {50.0f, 0.45f, 0.7f, 0.4242f, 0.05f, 0.5f, 0.5f, 0.5166f, 0.05687f, 0.05213f, 0.5f, 0.85f,
     137.8f, 1.296f, 0.802f, 0.2622f, 0.0f, 0.6f, 0.0f,
     0, 0, 1, {0, 1, 1, 0, 8, 3, 2}},
    /*  5 Warm House */
    {50.0f, 0.4f, 0.45f, 0.25f, 0.05f, 0.3f, 0.5f, 0.5f, 0.0f, 0.0f, 0.1f, 0.78f,
     70.0f, 1.3f, 0.4f, 0.2f, 0.0f, 0.5f, 0.0f,
     0, 0, 0, {0, 1, 1, 0, 8, 3, 2}},
    /*  6 Boom Bap */
    {60.0f, 0.5f, 0.3f, 0.4f, 0.04f, 0.3f, 0.5f, 0.4f, 0.0f, 0.0f, 0.0f, 0.8f,
     80.0f, 1.5f, 0.45f, 0.2f, 0.15f, 0.6f, 0.0f,
     0, 1, 0, {0, 2, 1, 0, 8, 3, 2}},
    /*  7 Acoustic */
    {55.0f, 0.4f, 0.32f, 0.5f, 0.04f, 0.2f, 0.5f, 0.55f, 0.0f, 0.0f, 0.0f, 0.75f,
     90.0f, 1.4f, 0.4f, 0.2f, 0.0f, 0.0f, 0.0f,
     0, 0, 0, {0, 2, 1, 0, 8, 3, 2}},
    /*  8 Hardstyle Rave */
    {55.0f, 0.6f, 0.6f, 0.5f, 0.03f, 0.7f, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 0.738f,
     100.0f, 1.6f, 0.5f, 0.25f, 0.3f, 0.9f, 0.0f,
     0, 1, 0, {1, 5, 1, 0, 8, 3, 2}},
    /*  9 Drop */
    {70.33f, 0.6215f, 0.8033f, 0.1953f, 0.3482f, 0.8411f, 0.5157f, 0.0f, 0.0f, 1.0f, 0.0f, 0.8524f,
     80.0f, 2.0f, 0.0f, 0.0f, -0.1713f, 0.5594f, 0.0f,
     0, 0, 0, {0, 2, 1, 0, 8, 3, 2}},
    /* 10 8x Melodic Sub */
    {45.0f, 0.15f, 1.8f, 0.1f, 0.05f, 0.15f, 0.5f, 0.4f, 0.0f, 0.0f, 0.0f, 0.8f,
     80.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f,
     0, 0, 0, {0, 1, 1, 0, 8, 3, 2}},
    /* 11 8x Sub Boom */
    {55.9f, 0.09881f, 1.915f, 0.09123f, 0.06f, 0.6f, 0.5f, 0.269f, 0.0f, 0.0f, 0.1457f, 0.85f,
     150.0f, 1.5f, 0.6692f, 0.25f, 0.0f, 0.0f, 0.0f,
     0, 0, 0, {0, 2, 1, 0, 8, 3, 2}},
    /* 12 Sustained */
    {52.0f, 0.4f, 1.6f, 0.15f, 0.05f, 0.12f, 0.5f, 0.4f, 0.15f, 0.3f, 0.0f, 0.63f,
     80.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
     0, 0, 0, {0, 2, 1, 0, 8, 3, 2}},
    /* 13 Hammer */
    {41.89f, 0.5729f, 2.0f, 0.6656f, 0.1947f, 0.6472f, 0.4108f, 0.6922f, 0.3609f, 1.0f, 0.0f, 0.7743f,
     80.0f, 2.0f, 0.0f, 0.0f, -1.0f, 0.3497f, 0.0f,
     0, 0, 0, {3, 3, 2, 0, 8, 3, 2}},
    /* 14 Lo-Fi Crush */
    {55.0f, 0.55f, 0.25f, 0.6f, 0.035f, 0.35f, 0.5f, 0.5f, 0.0f, 0.0f, 0.15f, 0.78f,
     80.0f, 2.0f, 0.0f, 0.0f, 0.15f, 0.6f, 0.1667f,
     0, 1, 0, {2, 3, 1, 0, 5, 3, 3}},
    /* 15 Dub Echo */
    {50.0f, 0.4f, 0.45f, 0.25f, 0.05f, 0.3f, 0.5f, 0.5f, 0.0f, 0.0f, 0.1f, 0.78f,
     70.0f, 1.3f, 0.4f, 0.2f, 0.0f, 0.5f, 0.2692f,
     0, 0, 0, {0, 1, 1, 1, 7, 6, 2}},
    /* 16 Spring Thud */
    {55.0f, 0.4f, 0.32f, 0.5f, 0.04f, 0.2f, 0.5f, 0.55f, 0.0f, 0.0f, 0.0f, 0.75f,
     90.0f, 1.4f, 0.4f, 0.2f, 0.0f, 0.0f, 0.2143f,
     0, 0, 0, {0, 2, 1, 0, 7, 4, 1}},
};

/* each preset's output level: the short, peaky presets are raised towards the stock kick's
 * loudness (the limiter holds their peaks); the same sound, only louder */
static const float TRIM[NPRESET] = {1.567f, 1.365f, 1.000f, 1.000f, 1.000f, 1.000f, 1.660f, 1.035f,
                                    1.000f, 1.000f, 1.000f, 1.109f, 1.084f, 2.570f, 1.000f, 2.239f};

#ifndef KL_DEFAULT_PRESETS          /* each output's preset until the page sets one */
#define KL_DEFAULT_PRESETS {0, 0, 0, 0}
#endif
static const uint8_t DEFAULT_PRESET[4] = KL_DEFAULT_PRESETS;

/* ---- from the M4 (kept in the M7 host's RAM: a voice-map rebuild of the arena finds them) --
 * Message "kick-lab": 4 x a voice's 6 bytes, a byte of voices (bits) to apply them to, and a
 * byte of voices held: PARAM2 doesn't set their values until the M4 sends them free (a kit
 * load, until its knob positions have reached this core).
 *   [0] preset 0-15 | drive type << 4 | drive tone << 6
 *   [1] drive amount 0-8 | time / division 0-13 << 4
 *   [2] feedback 0-9 | delay tone 0-5 << 4 | sync << 7
 *   [3] [4] [5] attack, delay mix, drive mix (0-255)
 * Message "kick-lab-tempo": the tempo x 10, 2 bytes little-endian. */
#define SET_BYTES       6
static uint8_t sets[4][SET_BYTES] __attribute__((aligned(4)));   /* (whole words: the host */
static uint32_t pending;                /* clears its RAM a word at a time) voices (bits) with new
                                         * settings; << 4: ever sent; << 8: held */
static float bpm;                       /* 0: none yet (120) */

void pk_kl_on_settings(const uint8_t *buf, uint32_t len)
{
    if (len < 4 * SET_BYTES + 2)
        return;
    for (int i = 0; i < 4 * SET_BYTES; i++)
        sets[i / SET_BYTES][i % SET_BYTES] = buf[i];
    uint32_t apply = buf[4 * SET_BYTES] & 0x0fu, held = buf[4 * SET_BYTES + 1] & apply;
    pending = (pending & ~(apply << 8)) | apply | apply << 4 | held << 8;
}

void pk_kl_on_tempo(const uint8_t *buf, uint32_t len)
{
    if (len >= 2) {
        float t = (float)(buf[0] | buf[1] << 8) * 0.1f;
        if (t >= 20.0f && t <= 400.0f)
            bpm = t;
    }
}

/* ---- the object ----------------------------------------------------------------------- */

typedef struct {
    float smix, samt, stone, sbias;     /* smoothed parameters */
    float up[16];                       /* the input's history (doubled ring: newest at [pos]) */
    float de[16], dodd[16];             /* the 2x samples' histories (even, odd) */
    int32_t pos;
    float dc_x, dc_y, tilt;
    float hold_n, held;                 /* crush: sample and hold */
} drive_t;

typedef struct {
    float b0, b1, b2, a1, a2, s1, s2;
    float env, env_c;                   /* the mod envelope (per COEF_EVERY samples) */
    float a;                            /* 10^(amount 12 dB / 40) */
    int32_t age;
} peak_t;

typedef struct {
    int16_t *buf;                       /* DL_LEN samples */
    int32_t w;                          /* write index */
    int32_t band;                       /* DL_DIV index, -1: not started */
    int32_t n;                          /* input samples gathered for the next step */
    float acc;                          /* their sum */
    float t, target;                    /* the read delay, in line samples, and where it's going */
    float t_new, xf;                    /* a jump: the new delay, and the crossfade to it (0-1) */
    int32_t xfading;
    float lp, tone_c;                   /* digital tone low-pass at the line's rate */
    float fb;
    float in_g;                         /* DL_IN_G for the feedback */
    float prev, cur;                    /* the last two outputs (linear interpolation up) */
    float gain, gain_to;                /* fades for a band change */
    int32_t quiet;                      /* line steps without signal */
    int32_t pending;                    /* a band waiting for the fade-out */
} delay_t;

typedef struct {
    int16_t *buf;                       /* RB_LEN samples */
    int32_t ci[4], ai[2];
    float cf[4];                        /* the combs' damping states */
    float hp_x, hp_y, hp_a;
    float l1, l2, l_a;
    float fb, sgain, release, release_c;
    int32_t n;
    float acc, prev, cur;
} rumble_t;

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
    const kl_preset_t *pre;
    kl_page_t page;
    float role[NROLE];                  /* PARAM2's values, 0-1 */
    int32_t out;                        /* the output (its settings from the M4) */
    int32_t cur_role;                   /* PARAM2's job now */
    int32_t ref2;                       /* PARAM2 (0-4095) when the job or the value was set */
    int32_t armed;                      /* PARAM2 has moved since: it sets the value */
    int32_t snap;                       /* new settings: the drive's smoothing starts at them */
    float trim;                         /* the preset's output level */
    /* set by update() */
    float tune;                         /* Hz, the note's transposition included */
    float end_f, span;                  /* the drop's floor, and the pitch above it */
    float sweep;                        /* Hz */
    float sat_drive, sat_inv;
    float amp_c, noise_c, drop_dec;     /* per sample (1 in drone) */
    int32_t drone;
    float drv_amt, drv_tone, dl_secs;
    /* the hit */
    float vel;
    float phase;
    float pitch_env, pitch_c;
    float drop_env;
    float tick_env;
    float click_env, click_lp, click_hp, click_soft;
    float atk_env, atk_c;
    float amp;
    int32_t sounding;                   /* the amp hasn't finished */
    int32_t noise;                      /* NOISE_* for this hit */
    float noise_env, pink, noise_hp;
    float lim;                          /* the output limiter's peak follower */
    uint32_t seed;
    /* constants */
    float click_c, click_lp_c, click_hp_c, soft_c, tick_c;
    peak_t peak;
    drive_t drv;
    delay_t dl;
    rumble_t rb;
} kl_t;

_Static_assert(__builtin_offsetof(kl_t, algo) == OFF_cVoice_algo, "algo");
_Static_assert(__builtin_offsetof(kl_t, mode) == OFF_cVoice_mode, "mode");
_Static_assert(__builtin_offsetof(kl_t, velocity) == OFF_cVoice_velocity, "velocity");
_Static_assert(__builtin_offsetof(kl_t, note) == OFF_cVoice_accent, "note (accent)");
_Static_assert(__builtin_offsetof(kl_t, param) == OFF_cVoice_param_ptrs, "param pointers");
_Static_assert(__builtin_offsetof(kl_t, mute) == OFF_cVoice_mute, "mute");
_Static_assert(__builtin_offsetof(kl_t, gain) == OFF_cVoice_gain, "gain");
_Static_assert(sizeof(kl_t) <= OBJ_MAX, "the object's budget in the arena (the lines follow it)");
_Static_assert(__builtin_offsetof(kl_t, role) == 0xcc, "PARAM2's values (the tests read them)");

/* ---- small maths (no libm) --------------------------------------------------------- */

static inline float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float absf(float x)
{
    return x < 0.0f ? -x : x;
}

static inline float floorf_(float x)
{
    float i = (float)(int32_t)x;
    return i > x ? i - 1.0f : i;
}

/* 2^x, |x| < 100: exponent by bits, the fraction by a 7th-order series (~1e-7 relative) */
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

/* identity up to `knee`, then bends smoothly towards `ceil` (never reaches it) */
static inline float soft_knee(float x, float knee, float ceil)
{
    float m = x < 0.0f ? -x : x;
    if (m <= knee)
        return x;
    float r = ceil - knee, u = (m - knee) / r;
    float y = knee + r * u / (1.0f + u);
    return x < 0.0f ? -y : y;
}

static inline float tanh_(float x)
{
    if (x > 3.0f)
        return 1.0f;
    if (x < -3.0f)
        return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* sin(2 pi p), any p */
static float sin2pi(float p)
{
    p -= floorf_(p);
    float x = p < 0.5f ? p : p - 1.0f;          /* -0.5 .. 0.5 */
    x *= 2.0f * PI;
    if (x > 0.5f * PI)
        x = PI - x;
    else if (x < -0.5f * PI)
        x = -PI - x;
    float x2 = x * x;
    return x * (1.0f - x2 * (1.0f / 6 - x2 * (1.0f / 120 - x2 * (1.0f / 5040))));
}

/* the original's sine: a parabola with one correction step, at phase p (0-1) */
static inline float fast_sin(float p)
{
    float x = p * (2.0f * PI) - PI;
    float y = (4.0f / PI) * x - (4.0f / (PI * PI)) * x * absf(x);
    return 0.225f * (y * absf(y) - y) + y;
}

/* a^n for a small whole n */
static float powi(float a, int n)
{
    float r = 1.0f;
    while (n-- > 0)
        r *= a;
    return r;
}

static inline float knob(const kl_t *k, int i)
{
    return clampf((float)*k->param[i] * (1.0f / 4095.0f), 0.0f, 1.0f);
}

/* PARAM1 / PARAM2: the ends of the travel read as the ends */
static inline float knob_ends(const kl_t *k, int i)
{
    return clampf((knob(k, i) - END_ZONE) * (1.0f / (1.0f - 2.0f * END_ZONE)), 0.0f, 1.0f);
}

static inline uint32_t lcg(uint32_t s)
{
    return s * 1664525u + 1013904223u;
}

static inline float noise_of(uint32_t s)
{
    return (float)(int32_t)s * (1.0f / 2147483647.0f);
}

static inline int16_t to_line(float x)
{
    x = soft_knee(x, LINE_KNEE, LINE_CEIL) * Q_LINE;
    return (int16_t)(x > 32767.0f ? 32767.0f : (x < -32767.0f ? -32767.0f : x));
}

/* ---- body peak (RBJ peaking EQ) ----------------------------------------------------- */

static void peak_coefs(kl_t *k)
{
    peak_t *pk = &k->peak;
    float f = clampf(k->pre->body_f * (1.0f + k->pre->body_mod * pk->env), 20.0f, 0.45f * SR);
    float w = f * (1.0f / SR);
    float h = sin2pi(0.5f * w);
    float sn = sin2pi(w), cs = 1.0f - 2.0f * h * h;  /* (a cosine near 1 from the series loses its 1 - cos) */
    float q = k->pre->body_q > 0.1f ? k->pre->body_q : 0.1f;
    float al = sn / (2.0f * q);
    float inv = 1.0f / (1.0f + al / pk->a);
    pk->b0 = (1.0f + al * pk->a) * inv;
    pk->b1 = -2.0f * cs * inv;
    pk->b2 = (1.0f - al * pk->a) * inv;
    pk->a1 = pk->b1;
    pk->a2 = (1.0f - al / pk->a) * inv;
}

static inline float peak_run(kl_t *k, float x)
{
    peak_t *pk = &k->peak;
    if (pk->env > 0.001f && pk->age-- <= 0) {
        pk->age = COEF_EVERY - 1;
        pk->env *= pk->env_c;
        peak_coefs(k);
    }
    float y = pk->b0 * x + pk->s1;
    pk->s1 = pk->b1 * x - pk->a1 * y + pk->s2;
    pk->s2 = pk->b2 * x - pk->a2 * y;
    return y;
}

/* ---- drive ------------------------------------------------------------------------------ */

/* the halfband filter's taps (fc = fs/4): h0 = h14, h2 = h12, h4 = h10, h6 = h8, h7 = 0.5 */
#define HB_A    (-0.00366f)
#define HB_B    0.0161f
#define HB_C    (-0.0683f)
#define HB_D    0.3048f

static inline float hb_even(const float *w)
{
    return HB_A * (w[0] + w[7]) + HB_B * (w[1] + w[6]) + HB_C * (w[2] + w[5]) + HB_D * (w[3] + w[4]);
}

static float shape(drive_t *d, int type, float x, float gain)
{
    float asym = d->sbias * 0.7f * absf(x);
    switch (type) {
    case DRV_HARD:
        return clampf((x + asym) * gain, -1.0f, 1.0f);
    case DRV_CRUSH: {
        float levels = exp2f_(16.0f - d->samt * 14.0f);
        float q = floorf_(x * levels + 0.5f) / levels;
        float rate = 1.0f + absf(d->sbias) * 63.0f;
        d->hold_n += 1.0f;
        if (d->hold_n >= rate) {
            d->hold_n -= rate;
            d->held = q;
        }
        return d->held;
    }
    case DRV_FOLD: {
        float u = 0.25f * (x + asym) * gain;
        return 4.0f * (absf(u + 0.25f - floorf_(u + 0.75f)) - 0.25f);
    }
    default:
        return tanh_((x + asym) * gain);
    }
}

static float drive_run(kl_t *k, float x)
{
    drive_t *d = &k->drv;
    int type = k->page.drv_type;
    const float sc = 0.005f;
    d->smix += sc * (k->role[ROLE_DRIVE_MIX] - d->smix);
    d->samt += sc * (k->drv_amt - d->samt);
    d->stone += sc * (k->drv_tone - d->stone);
    d->sbias += sc * (k->pre->drv_bias - d->sbias);
    /* the input's history runs always: the wet path comes out 7 samples late, and the dry
     * signal is taken 7 samples late too, so they line up in the mix (no comb filtering) and
     * the timing doesn't jump when the drive turns on or off */
    d->pos = (d->pos - 1) & 7;
    d->up[d->pos] = d->up[d->pos + 8] = x;
    const float *w = &d->up[d->pos];
    float dry = w[7];
    if (d->smix < 0.0005f && k->role[ROLE_DRIVE_MIX] <= 0.0f)
        return dry;
    float gain = type == DRV_FOLD ? 1.0f + d->samt * 15.0f : (type == DRV_CRUSH ? 1.0f : 1.0f + d->samt * 20.0f);

    /* 2x up: the even phase through the halfband, the odd phase a 3-sample delay */
    float za = shape(d, type, 2.0f * hb_even(w), gain);
    float zb = shape(d, type, w[3], gain);
    /* 2x down */
    d->de[d->pos] = d->de[d->pos + 8] = za;
    d->dodd[d->pos] = d->dodd[d->pos + 8] = zb;
    float y = hb_even(&d->de[d->pos]) + 0.5f * d->dodd[d->pos + 4];   /* the centre: between de[3] and de[4] */
    y *= type == DRV_FOLD ? 1.0f / (1.0f + d->samt * 2.0f) : (type == DRV_CRUSH ? 1.0f : 1.0f / (1.0f + d->samt * 2.5f));

    float dc = y - d->dc_x + 0.999f * d->dc_y;  /* DC blocker */
    d->dc_x = y;
    d->dc_y = dc;
    d->tilt += 0.12f * (dc - d->tilt);          /* tone: a tilt around ~1 kHz */
    float t = (d->stone - 0.5f) * 2.0f;
    float shaped = d->tilt * (1.0f - t * 0.6f) + (dc - d->tilt) * (1.0f + t * 0.6f);
    return d->smix * shaped + (1.0f - d->smix) * dry;
}

/* ---- delay (digital) ------------------------------------------------------------------ */

static void delay_clear(delay_t *dl)
{
    uint32_t *p = (uint32_t *)dl->buf;
    for (int i = 0; i < DL_LEN / 2; i++)
        p[i] = 0;
    dl->lp = dl->prev = dl->cur = 0.0f;
    dl->acc = 0.0f;
    dl->n = 0;
}

/* the line's time and rate band for the page's values (in update()) */
static void delay_set(kl_t *k)
{
    delay_t *dl = &k->dl;
    float tempo = bpm > 0.0f ? bpm : 120.0f;
    float secs = k->page.dl_sync ? DIV_BEATS[k->page.dl_time] * 60.0f / tempo : TIME_S[k->page.dl_time];
    float max = (float)(DL_LEN - 2) * DL_DIV[3] / SR;
    while (secs > max)                          /* below 60 BPM: half the time, still on the beat */
        secs *= 0.5f;
    k->dl_secs = secs;
    int band = 0;
    while (band < 3 && secs * SR / DL_DIV[band] > (float)(DL_LEN - 2))
        band++;
    int silent = dl->quiet < 0 || dl->quiet >= DL_LEN;
    if (dl->band < 0 || (band != dl->band && silent)) {     /* nothing to fade: switch now */
        if (dl->band >= 0)
            delay_clear(dl);
        dl->band = band;
        dl->t = dl->target = secs * SR / DL_DIV[band];
        dl->xfading = 0;
        dl->gain = dl->gain_to = 1.0f;
    } else if (band != dl->band) {              /* a new rate: fade out, clear, fade in */
        dl->pending = band;
        dl->gain_to = 0.0f;
    }
    if (band == dl->band)                       /* (a change of rate sets it after the fade) */
        dl->target = secs * SR / DL_DIV[dl->band];
    float c = 0.05f + 0.9f * (float)k->page.dl_tone / (NDTONE - 1);
    dl->tone_c = 1.0f - powi(1.0f - c, DL_DIV[dl->band]);     /* the same cutoff at the line's rate */
    if (k->page.dl_tone == NDTONE - 1)
        dl->tone_c = 1.0f;                      /* fully open: no low-pass */
    dl->fb = (float)k->page.dl_fb * 0.1f;
    dl->in_g = DL_IN_G[k->page.dl_fb];
}

/* the line `t` line samples back (linear interpolation) */
static float delay_tap(const delay_t *dl, float t)
{
    t = clampf(t, 1.0f, (float)(DL_LEN - 2));
    float rp = (float)dl->w - t;
    if (rp < 0.0f)
        rp += DL_LEN;
    int i0 = (int)rp;
    float fr = rp - (float)i0;
    int i1 = i0 + 1 >= DL_LEN ? 0 : i0 + 1;
    return ((float)dl->buf[i0] + fr * (float)(dl->buf[i1] - dl->buf[i0])) * (1.0f / Q_LINE);
}

static float delay_run(kl_t *k, float x)
{
    delay_t *dl = &k->dl;
    float mix = k->role[ROLE_DELAY_MIX];
    if (mix <= 0.0f) {
        if (dl->quiet >= 0) {                   /* turned off: forget the line */
            delay_clear(dl);
            dl->quiet = -1;
        }
        return x;
    }
    int div = DL_DIV[dl->band];
    dl->acc += x;
    if (++dl->n >= div) {                       /* a step at the line's rate */
        float in = dl->acc * (1.0f / div);
        dl->acc = 0.0f;
        dl->n = 0;
        /* the time: small changes (a tempo drifting) glide, a jump (a page key, a new tempo)
         * crossfades from the old tap to the new one over ~50 ms, as the original */
        float d = dl->target - dl->t;
        if (dl->xfading) {
            dl->t_new = dl->target;
            dl->xf += 0.02f * div / 48.0f;          /* 50 ms */
            if (dl->xf >= 1.0f) {
                dl->xfading = 0;
                dl->t = dl->t_new;
            }
        } else if (d > 4.0f || d < -4.0f) {
            if (dl->quiet < 0 || dl->quiet >= DL_LEN) {
                dl->t = dl->target;                  /* nothing in the line: jump */
            } else {
                dl->xfading = 1;
                dl->xf = 0.0f;
                dl->t_new = dl->target;
            }
        } else {
            dl->t += d;
        }
        float rd = delay_tap(dl, dl->t);
        if (dl->xfading)
            rd += dl->xf * (delay_tap(dl, dl->t_new) - rd);
        dl->lp += dl->tone_c * (rd - dl->lp);
        float wet = dl->lp * dl->gain;
        dl->buf[dl->w] = to_line(in * dl->in_g + wet * dl->fb);
        if (++dl->w >= DL_LEN)
            dl->w = 0;
        dl->prev = dl->cur;
        dl->cur = wet;
        /* fades for a band change (~5 ms) */
        if (dl->gain > dl->gain_to) {
            dl->gain -= 0.005f * div;
            if (dl->gain <= 0.0f) {
                dl->gain = 0.0f;
                dl->band = dl->pending;
                delay_clear(dl);
                dl->t = dl->target = k->dl_secs * SR / DL_DIV[dl->band];
                dl->xfading = 0;
                float c = 0.05f + 0.9f * (float)k->page.dl_tone / (NDTONE - 1);
                dl->tone_c = k->page.dl_tone == NDTONE - 1 ? 1.0f : 1.0f - powi(1.0f - c, DL_DIV[dl->band]);
                dl->gain_to = 1.0f;
            }
        } else if (dl->gain < dl->gain_to) {
            dl->gain += 0.005f * div;
            if (dl->gain > 1.0f)
                dl->gain = 1.0f;
        }
        dl->quiet = absf(in) > 1e-5f || absf(wet) > 1e-5f ? 0 : dl->quiet + 1;
    }
    float wet = dl->prev + (dl->cur - dl->prev) * ((float)dl->n / div);
    /* up to halfway the echoes come up under the full kick; then the kick fades out */
    return mix < 0.5f ? x + 2.0f * mix * wet : 2.0f * (1.0f - mix) * x + wet;
}

static int delay_ringing(const kl_t *k)
{
    return k->role[ROLE_DELAY_MIX] > 0.0f && k->dl.quiet >= 0 && k->dl.quiet < DL_LEN;
}

/* ---- rumble (Schroeder: 4 combs, 2 allpasses, at 12 kHz) ------------------------------- */

static const int16_t RB_SIZE[6] = {RB_C0, RB_C1, RB_C2, RB_C3, RB_A0, RB_A1};

static void rumble_set(kl_t *k)
{
    rumble_t *r = &k->rb;
    const kl_preset_t *pre = k->pre;
    float sus = clampf(pre->r_sus, 0.0f, 1.0f);
    float t60 = 0.05f + sus * 4.0f;
    r->fb = exp2f_(-6.907755f * LOG2_E * (1234.0f / SR) / t60);    /* as the original */
    if (r->fb > 0.97f)
        r->fb = 0.97f;
    r->sgain = sus < 0.02f ? sus * 50.0f : 1.0f;
    r->release_c = exp2f_(-3.0f * LOG2_10 / (t60 * SR));
    float fc = 80.0f * exp2f_(pre->r_tone * 5.9657843f);            /* 80 Hz * 62.5^tone */
    float om = 2.0f * PI * fc * RB_DIV / SR;
    r->l_a = om / (1.0f + om);
    r->hp_a = exp2f_(-2.0f * PI * 40.0f * RB_DIV / SR * LOG2_E);
}

static float rumble_step(kl_t *k, float in)
{
    rumble_t *r = &k->rb;
    float hp = r->hp_a * (r->hp_y + in - r->hp_x);
    r->hp_x = in;
    r->hp_y = hp;
    int16_t *b = r->buf;
    float sum = 0.0f;
    for (int c = 0; c < 4; c++) {
        float s = (float)b[r->ci[c]] * (1.0f / Q_LINE);
        r->cf[c] = s * 0.8f + r->cf[c] * 0.2f;
        b[r->ci[c]] = to_line(hp + r->cf[c] * r->fb);
        sum += s;
        b += RB_SIZE[c];
    }
    float ap = sum * 0.25f;
    for (int a = 0; a < 2; a++) {
        float s = (float)b[r->ai[a]] * (1.0f / Q_LINE);
        float out = -ap + s;
        b[r->ai[a]] = to_line(ap + s * 0.5f);
        ap = out;
        b += RB_SIZE[4 + a];
    }
    for (int c = 0; c < 4; c++)
        if (++r->ci[c] >= RB_SIZE[c])
            r->ci[c] = 0;
    for (int a = 0; a < 2; a++)
        if (++r->ai[a] >= RB_SIZE[4 + a])
            r->ai[a] = 0;
    r->l1 += r->l_a * (ap - r->l1);
    r->l2 += r->l_a * (r->l1 - r->l2);
    float drv = 1.0f + k->pre->r_mix * 4.0f;
    return tanh_(r->l2 * drv) / drv;
}

static float rumble_run(kl_t *k, float x)
{
    rumble_t *r = &k->rb;
    r->acc += x;
    if (++r->n >= RB_DIV) {
        r->prev = r->cur;
        r->cur = rumble_step(k, r->acc * (1.0f / RB_DIV));
        r->acc = 0.0f;
        r->n = 0;
    }
    if (!k->sounding)
        r->release *= r->release_c;
    float y = r->prev + (r->cur - r->prev) * ((float)r->n / RB_DIV);
    return y * (1.0f - k->amp * 0.85f) * k->pre->r_mix * r->sgain * r->release;
}

static int rumble_ringing(const kl_t *k)
{
    return k->pre->r_mix > 0.001f && k->rb.release > 0.001f;
}

/* ---- control (per block) ------------------------------------------------------------ */

static int role_of(int algo)
{
    return algo == POS1 ? ROLE_ATTACK : (algo == POS3 ? ROLE_DRIVE_MIX : ROLE_DELAY_MIX);
}

static void use_preset(kl_t *k, int i);

/* the M4's settings for this voice: preset, the page, PARAM2's values */
static void take_settings(kl_t *k)
{
    const uint8_t *b = sets[k->out];
    int pr = b[0] & 15;
    if (k->pre != &PRESETS[pr])
        use_preset(k, pr);
    kl_page_t pg;
    pg.drv_type = b[0] >> 4 & 3;
    pg.drv_tone = b[0] >> 6 < 3 ? b[0] >> 6 : 1;
    pg.drv_amt = (b[1] & 15) < NDAMT ? b[1] & 15 : 0;
    pg.dl_time = b[1] >> 4 < NTIME ? b[1] >> 4 : 0;
    pg.dl_fb = (b[2] & 15) < NFB ? b[2] & 15 : 0;
    pg.dl_tone = (b[2] >> 4 & 7) < NDTONE ? b[2] >> 4 & 7 : 0;
    pg.dl_sync = b[2] >> 7;
    k->page = pg;
    for (int i = 0; i < NROLE; i++)
        k->role[i] = (float)b[3 + i] * (1.0f / 255.0f);
    k->ref2 = *k->param[3];             /* the knob takes over once it moves */
    k->armed = 0;
    k->snap = 1;
}

static void update(cVoice *v)
{
    kl_t *k = (kl_t *)v;
    if (k->magic != MAGIC)
        return;
    if (pending & (1u << k->out)) {
        pending &= ~(1u << k->out);
        take_settings(k);
    }
    const kl_preset_t *pre = k->pre;

    /* TUNE: 20-200 Hz, and the note's transposition */
    int note = k->note ? k->note : NOTE_REF;
    k->tune = TUNE_MIN * exp2f_(knob(k, 0) * LOG2_10 + (float)(note - NOTE_REF) * (1.0f / 12.0f));
    float drop_semis = -24.0f * pre->drop * pre->drop;
    k->end_f = k->tune * exp2f_(drop_semis * (1.0f / 12.0f));
    if (k->end_f < TUNE_MIN)
        k->end_f = TUNE_MIN;
    k->span = k->tune - k->end_f;
    if (k->span < 0.0f)
        k->span = 0.0f;

    /* PARAM1: sweep, and the saturation's drive that follows it */
    float c = knob_ends(k, 2);
    k->sweep = c * SWEEP_HZ;
    k->sat_drive = 1.3f + 1.2f * c;
    k->sat_inv = 1.0f / tanh_(k->sat_drive);

    /* PARAM2: ALGO's job. A new job, or a value from the M4, keeps its value until the knob
     * moves (2% of its travel); then the knob sets it. The M4 follows the same rule to save it. */
    int x2 = *k->param[3];
    int rl = role_of(k->algo);
    if (rl != k->cur_role) {
        k->cur_role = rl;
        k->ref2 = x2;
        k->armed = 0;
    }
    if (x2 - k->ref2 > MOVED || k->ref2 - x2 > MOVED)
        k->armed = 1;
    if (pending & 1u << (8 + k->out)) {        /* held: the knob's positions are still arriving */
        k->ref2 = x2;
        k->armed = 0;
    }
    if (k->armed)
        k->role[rl] = knob_ends(k, 3);

    /* DECAY: the amp reaches its floor (-80 dB, raised by R-Mix) at the decay time */
    float d = knob(k, 1);
    k->drone = d >= DRONE_AT;
    if (k->drone) {
        k->amp_c = k->noise_c = 1.0f;
        k->drop_dec = 1.0f / (DRONE_DROP_S * SR);
    } else {
        float t = DECAY_MIN * exp2f_(d * (DECAY_OCT / DRONE_AT));
        k->amp_c = exp2f_((-4.0f + 2.0f * pre->r_mix) * LOG2_10 / (t * SR));
        k->noise_c = exp2f_(-3.0f * LOG2_10 / (t * SR));    /* -60 dB over the decay time */
        k->drop_dec = 1.0f / (t * SR);
    }

    /* the page */
    k->drv_amt = (float)(k->page.drv_amt + 1) * (1.0f / NDAMT);
    k->drv_tone = DRV_TONE[k->page.drv_tone];
    if (k->snap) {                      /* as the original at a preset load: no ramp from the old */
        k->snap = 0;
        k->drv.smix = k->role[ROLE_DRIVE_MIX];
        k->drv.samt = k->drv_amt;
        k->drv.stone = k->drv_tone;
        k->drv.sbias = pre->drv_bias;
    }
    delay_set(k);
}

/* a preset: its sound and the page's values */
static void use_preset(kl_t *k, int i)
{
    k->pre = &PRESETS[i < NPRESET ? i : 0];
    k->trim = TRIM[i < NPRESET ? i : 0];
    k->page = k->pre->page;
    k->role[ROLE_ATTACK] = k->pre->attack;
    k->role[ROLE_DELAY_MIX] = k->pre->dl_mix;
    k->role[ROLE_DRIVE_MIX] = k->pre->drv_mix;
    k->peak.a = exp2f_(k->pre->body_amt * 0.3f * LOG2_10);
    rumble_set(k);
    k->snap = 1;
}

static void trigger(cVoice *v)
{
    kl_t *k = (kl_t *)v;
    if (k->magic != MAGIC)
        return;
    const kl_preset_t *pre = k->pre;
    update(v);
    k->vel = (float)k->velocity * (1.0f / 127.0f);
    k->phase = 0.0f;
    k->pitch_env = 1.0f;
    /* the pitch envelope falls to 10^(-4 + 2 curve) over the sweep time */
    k->pitch_c = exp2f_((-4.0f + 2.0f * pre->pitch_curve) * LOG2_10 / (pre->sweep_time * SR));
    k->drop_env = 1.0f;
    k->tick_env = pre->tick ? 1.0f : 0.0f;
    k->click_env = 1.0f;
    k->click_lp = k->click_hp = k->click_soft = 0.0f;
    /* attack: 1 ms .. 100 ms (the square of the value) */
    float a = 0.1f * k->role[ROLE_ATTACK];
    float at = a * a * 10.0f;
    if (at < 0.001f)
        at = 0.001f;
    k->atk_env = 0.0f;
    k->atk_c = exp2f_(-LOG2_E / (at * SR));
    k->amp = 1.0f;
    k->sounding = 1;
    k->noise = k->mode == POS1 ? NOISE_OFF : (k->mode == POS3 ? NOISE_PINK : NOISE_WHITE);
    k->noise_env = k->noise ? 1.0f : 0.0f;
    k->pink = k->noise_hp = 0.0f;
    k->lim = 0.0f;                      /* (an idle voice's follower doesn't fall) */
    /* body peak: its mod envelope falls over the sweep time */
    k->peak.env = pre->body_amt > 0.001f && pre->body_mod > 0.001f ? 1.0f : 0.0f;
    k->peak.env_c = exp2f_(-LOG2_E * COEF_EVERY / (pre->sweep_time * SR));
    k->peak.age = 0;
    peak_coefs(k);
    k->rb.release = 1.0f;
    if (k->dl.quiet < 0)
        k->dl.quiet = 0;
}

static void init(cVoice *v)
{
    kl_t *k = (kl_t *)v;
    k->magic = MAGIC;
    k->sounding = 0;
    k->noise_env = 0.0f;
    k->seed = 22222u;
    k->click_c = exp2f_(-3.0f * LOG2_10 / (0.008f * SR));          /* -60 dB over 8 ms */
    k->click_lp_c = 1.0f - exp2f_(-2.0f * PI * 6000.0f / SR * LOG2_E);
    k->click_hp_c = 1.0f - exp2f_(-2.0f * PI * 150.0f / SR * LOG2_E);
    k->soft_c = exp2f_(-LOG2_E / (0.0005f * SR));                   /* 0.5 ms */
    k->tick_c = exp2f_(-LOG2_E / (0.002f * SR));                    /* 2 ms */
    k->dl.band = -1;
    k->dl.quiet = -1;
    k->rb.release = 0.0f;
    k->cur_role = role_of(k->algo);
    k->ref2 = *k->param[3];
    k->armed = 0;
    update(v);
}

/* ---- audio (per sample) ---------------------------------------------------------------- */

/* the last stage: peaks above LIM_THR are turned down within ~0.3 ms, and the gain comes back
 * over ~50 ms; then a soft clip rounds what got through before the limiter caught up */
static float limiter(kl_t *k, float x)
{
    float m = absf(x);
    k->lim = m > k->lim ? k->lim + (m - k->lim) * LIM_ATT : k->lim * LIM_REL;
    if (k->lim > LIM_THR)
        x *= LIM_THR / k->lim;
    return soft_knee(x, CLIP_KNEE, CLIP_CEIL);
}

static int16_t render(cVoice *v)
{
    kl_t *k = (kl_t *)v;
    fw_tick_mute_fade(v);
    if (k->magic != MAGIC)
        return 0;
    int tails = delay_ringing(k) || rumble_ringing(k);
    if (!k->sounding && k->noise_env <= 0.0f && !tails)
        return 0;
    const kl_preset_t *pre = k->pre;
    float s = 0.0f;

    if (k->sounding) {
        /* pitch: the drop's floor, the slow drop above it, the fast sweep, the tick's octave */
        float f = (k->end_f + k->span * k->drop_env + k->sweep * k->pitch_env) * (1.0f + k->tick_env);
        k->tick_env *= k->tick_c;
        k->pitch_env *= k->pitch_c;
        k->drop_env -= k->drop_dec;
        if (k->drop_env < 0.0f)
            k->drop_env = 0.0f;
        float p = k->phase + f * (1.0f / SR);
        if (p >= 1.0f)
            p -= 1.0f;
        k->phase = p;
        float osc = pre->triangle ? (p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p) : fast_sin(p);

        /* the click: noise, 0.5 ms soft start, 8 ms decay, band-limited 150 Hz-6 kHz */
        k->seed = lcg(k->seed);
        k->click_soft = 1.0f - (1.0f - k->click_soft) * k->soft_c;
        float raw = noise_of(k->seed) * pre->click * k->click_env * k->click_soft;
        k->click_env *= k->click_c;
        k->click_lp += k->click_lp_c * (raw - k->click_lp);
        k->click_hp += k->click_hp_c * (k->click_lp - k->click_hp);

        k->atk_env = 1.0f - (1.0f - k->atk_env) * k->atk_c;
        if (!k->drone) {
            k->amp *= k->amp_c;
            if (k->amp < 0.001f) {
                k->amp = 0.0f;
                k->sounding = 0;
            }
        }
        float body = osc + (k->click_lp - k->click_hp);
        if (pre->sat)
            body = tanh_(body * k->sat_drive) * k->sat_inv;
        s = body * k->atk_env * k->amp * k->vel * pre->volume;
        if (pre->body_amt > 0.001f)
            s = peak_run(k, s);
        s = drive_run(k, s);
    }
    s = delay_run(k, s);
    if (pre->r_mix > 0.001f)
        s += rumble_run(k, s);

    if (k->noise_env > 0.0f) {          /* the noise layer, decaying with DECAY */
        k->noise_env *= k->noise_c;
        if (k->noise_env < 0.0001f) {
            k->noise_env = 0.0f;
        } else {
            k->seed = lcg(k->seed);
            float n = noise_of(k->seed);
            if (k->noise == NOISE_PINK) {
                k->pink += 0.045f * (n - k->pink);
                n = k->pink * 2.88f;
            } else {
                n *= 0.8f;
            }
            k->noise_hp += 0.02f * (n - k->noise_hp);
            s += (n - k->noise_hp) * k->noise_env * NOISE_LEVEL * k->vel * pre->volume;
        }
    }

    float out = limiter(k, s * (LEVEL * k->trim));
    return (int16_t)(((int32_t)out * k->gain) >> 16);
}

/* ---- vtable and construction --------------------------------------------------------- */

/* RTTI-shaped word before the vtable (tools read class names from vtable[-1]) */
static const char kl_name[] = "8cKickLab";
static const struct { const void *vptr; const char *name; } kl_typeinfo = {0, kl_name};
static const struct {
    const void *typeinfo;
    cVoice_vtbl vtbl;
} kl_vtable = {&kl_typeinfo, {trigger, init, update, render}};

/* voice-map: build a Kick Lab in `obj` (an output's arena) for output `out` */
void pk_kl_build(uint8_t *obj, unsigned out)
{
    uint32_t *w = (uint32_t *)obj;
    for (unsigned i = 0; i < (OBJ_MAX + 2 * (DL_LEN + RB_LEN) + 3) / 4; i++)
        w[i] = 0;
    kl_t *k = (kl_t *)obj;
    k->vtbl = &kl_vtable.vtbl;
    for (int i = 0; i < 4; i++)
        k->param[i] = &FW_PARAMS[out][i];
    k->note = NOTE_REF;
    k->dl.buf = (int16_t *)(obj + OBJ_MAX);
    k->rb.buf = k->dl.buf + DL_LEN;
    k->out = (int32_t)(out & 3);
    use_preset(k, DEFAULT_PRESET[k->out]);
    if (pending & 1u << (4 + k->out))       /* the M4 has sent this output's settings */
        pending |= 1u << k->out;
    init((cVoice *)k);
}
