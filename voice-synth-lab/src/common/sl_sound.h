/* Synth Lab: a sound, as the kit saves it. Every value is a small whole number (5 bits for
 * amounts, 1-3 for choices); presets, the page, Random and Tweak all write these, so what
 * plays is what's saved. Shared by the M4 (kit table, page) and the M7 (the engine).
 *
 * The fields come in fours: each four is one cell of the configuration page, in the order of the
 * panel controls that edit it (RATE, WAVE, MOD LVL, SYNC). Packed, LSB first, in field order:
 * 316 bits, SL_PACKED bytes. */
#pragma once
#include <stdint.h>

/* per oscillator: at SL_OSC(i) + */
enum {
    SL_WAVE, SL_PW, SL_LEVEL, SL_SYNC,              /* Wave cell */
    SL_OCT, SL_FINE, SL_SWEEP, SL_RATIO,            /* Tune cell */
    SL_PLFO = 8,                                    /* Pitch LFO cell (an LFO's four) */
    SL_VLFO = 12,                                   /* Volume LFO cell */
    SL_OSC_N = 16
};
#define SL_OSC(i)       ((i) * SL_OSC_N)

/* an LFO's four, and an envelope's four */
enum { SL_L_SPEED, SL_L_SHAPE, SL_L_DEPTH, SL_L_SYNC };
enum { SL_E_ATT, SL_E_REL, SL_E_SUS, SL_E_CURVE };

enum {
    SL_FENV = 3 * SL_OSC_N,                         /* Filter Env cell (48) */
    SL_AENV = SL_FENV + 4,                          /* Amp Env cell */
    SL_CLFO = SL_AENV + 4,                          /* Cutoff LFO cell */
    SL_FLFO = SL_CLFO + 4,                          /* Fold LFO cell */
    SL_FOLD_AMT = SL_FLFO + 4, SL_FOLD_SYM, SL_FOLD_PUSH, SL_FOLD_ON,          /* Fold cell (64) */
    SL_FILT_TYPE, SL_KEYTRACK, SL_FDECAY, SL_FILT_ON,                           /* Filter cell (68) */
    SL_CUTOFF, SL_RES, SL_ENVAMT, SL_ADECAY, SL_GLIDE, SL_LEGATO,               /* the sound's own */
    SL_PRESET, SL_SCALE, SL_ROOT,                                               /* the page */
    SL_P2_RES, SL_P2_ENV, SL_P2_SYNC,               /* PARAM2's three jobs (16 = the sound's own) */
    SL_MUTATE, SL_RSTYLE, SL_LFO_FREE, SL_TRIM,     /* trim: the level, 1.5 dB steps, 8 = 0 dB */
    SL_NFIELD
};
_Static_assert(SL_NFIELD == 88, "the field list");

/* each field's width in bits */
static const uint8_t SL_WIDTH[SL_NFIELD] = {
#define SL_W_OSC 3, 5, 5, 1, 3, 5, 5, 1, 5, 3, 5, 1, 5, 3, 5, 1
    SL_W_OSC, SL_W_OSC, SL_W_OSC,
#undef SL_W_OSC
    5, 5, 5, 1,  5, 5, 5, 1,                        /* filter env, amp env */
    5, 3, 5, 1,  5, 3, 5, 1,                        /* cutoff LFO, fold LFO */
    5, 5, 5, 1,                                     /* fold */
    3, 2, 5, 1,                                     /* filter cell */
    5, 5, 5, 5, 5, 1,                               /* cutoff, res, env amount, amp decay, glide, legato */
    4, 4, 4,                                        /* preset, scale, root */
    5, 5, 5,                                        /* PARAM2's jobs */
    1, 2, 1, 4,                                     /* mutate, random style, LFOs free-running, trim */
};
#define SL_BITS         316
#define SL_PACKED       40

/* the choices */
enum { SL_W_SINE, SL_W_TRI, SL_W_SAW, SL_W_RAMP, SL_W_PULSE, SL_W_NOISE, SL_NWAVE };
enum { SL_S_SINE, SL_S_TRI, SL_S_SAW, SL_S_RAMP, SL_S_SQUARE, SL_S_RANDOM, SL_NSHAPE };
enum { SL_F_LP6, SL_F_LP12, SL_F_LP18, SL_F_LP24, SL_F_BP, SL_F_HP, SL_F_NOTCH, SL_NFILT };
#define SL_NPRESET      16
#define SL_NSTARTER     5           /* starters: a hold on a preset step */
#define SL_NSCALE       10
#define SL_NROOT        12
#define SL_MID          16              /* 5-bit "centre" (fine tune, env amount, PARAM2's jobs) */

static inline void sl_pack(const uint8_t *v, uint8_t *out)
{
    for (int i = 0; i < SL_PACKED; i++)
        out[i] = 0;
    unsigned bit = 0;
    for (int f = 0; f < SL_NFIELD; f++) {
        unsigned x = v[f] & ((1u << SL_WIDTH[f]) - 1u);
        for (unsigned b = 0; b < SL_WIDTH[f]; b++, bit++)
            if (x >> b & 1u)
                out[bit >> 3] |= (uint8_t)(1u << (bit & 7));
    }
}

static inline void sl_unpack(const uint8_t *in, uint8_t *v)
{
    unsigned bit = 0;
    for (int f = 0; f < SL_NFIELD; f++) {
        unsigned x = 0;
        for (unsigned b = 0; b < SL_WIDTH[f]; b++, bit++)
            x |= (unsigned)(in[bit >> 3] >> (bit & 7) & 1u) << b;
        v[f] = (uint8_t)x;
    }
}

/* the largest value a field takes (the choices have fewer than their bits allow) */
static inline int sl_max(int f)
{
    if (f < SL_FENV) {
        int o = f % SL_OSC_N;
        if (o == SL_WAVE)
            return SL_NWAVE - 1;
        if (o == SL_PLFO + SL_L_SHAPE || o == SL_VLFO + SL_L_SHAPE)
            return SL_NSHAPE - 1;
    }
    if (f == SL_CLFO + SL_L_SHAPE || f == SL_FLFO + SL_L_SHAPE)
        return SL_NSHAPE - 1;
    if (f == SL_FILT_TYPE)
        return SL_NFILT - 1;
    if (f == SL_PRESET)
        return SL_NPRESET - 1;
    if (f == SL_SCALE)
        return SL_NSCALE - 1;
    if (f == SL_ROOT)
        return SL_NROOT - 1;
    return (1 << SL_WIDTH[f]) - 1;
}

/* every field in its range (a record from a later version can't play nonsense) */
static inline void sl_clean(uint8_t *v)
{
    for (int f = 0; f < SL_NFIELD; f++)
        if (v[f] > sl_max(f))
            v[f] = (uint8_t)sl_max(f);
}

/* one field of a packed sound */
static inline unsigned sl_bit(int f)
{
    unsigned b = 0;
    for (int i = 0; i < f; i++)
        b += SL_WIDTH[i];
    return b;
}

static inline int sl_get(const uint8_t *p, int f)
{
    unsigned bit = sl_bit(f), x = 0;
    for (unsigned b = 0; b < SL_WIDTH[f]; b++, bit++)
        x |= (unsigned)(p[bit >> 3] >> (bit & 7) & 1u) << b;
    return (int)x;
}

static inline void sl_put(uint8_t *p, int f, int x)
{
    unsigned bit = sl_bit(f);
    for (unsigned b = 0; b < SL_WIDTH[f]; b++, bit++) {
        uint8_t m = (uint8_t)(1u << (bit & 7));
        p[bit >> 3] = (uint8_t)((x >> b & 1) ? p[bit >> 3] | m : p[bit >> 3] & ~m);
    }
}
