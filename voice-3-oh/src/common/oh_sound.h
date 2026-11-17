/* 3 Oh: a sound, as the kit saves it, with the voice's line generator settings. Every value is
 * a small whole number; presets, the page and New sound all write these, so what plays is what's
 * saved. Shared by the M4 (kit table, page, the line generator) and the M7 (the engine, which
 * reads only the sound's part). Packed, LSB first, in field order: OH_BITS bits, OH_PACKED
 * bytes. */
#pragma once
#include <stdint.h>

enum {
    OH_CUTOFF, OH_RES, OH_ENVMOD, OH_ACCENT,       /* the filter: the sound's own positions */
    OH_DIST, OH_DTYPE, OH_TONE,                     /* distortion: amount, type, tone */
    OH_SLIDE, OH_TRIM,                              /* slide time; the level (1.5 dB steps, 8 = 0 dB) */
    OH_PRESET,                                      /* the preset it came from */
    OH_SCALE, OH_ROOT, OH_OCTAVE,                   /* the line: key and base octave (C1-C4) */
    OH_NOTES, OH_RANGE,                             /*   how many pitches, how wide (0-3 each) */
    OH_D_ACC, OH_D_SLIDE, OH_D_OCT, OH_D_REST,      /*   densities: 0 off, 1 low, 2 high */
    OH_P2_ENV, OH_P2_ACC, OH_P2_DIST,               /* PARAM2's three jobs (16 = the sound's own) */
    OH_RSTYLE,                                      /* 1: New sound changed the preset's sound */
    OH_NFIELD
};
_Static_assert(OH_NFIELD == 23, "the field list");

/* each field's width in bits */
static const uint8_t OH_WIDTH[OH_NFIELD] = {
    5, 5, 5, 5,                                     /* cutoff, resonance, env mod, accent */
    5, 2, 4,                                        /* distortion amount, type, tone */
    5, 4,                                           /* slide time, trim */
    4,                                              /* preset */
    4, 4, 2,                                        /* scale, root, octave */
    2, 2,                                           /* notes, range */
    2, 2, 2, 2,                                     /* densities */
    5, 5, 5,                                        /* PARAM2's jobs */
    2,                                              /* random style */
};
#define OH_BITS         83
#define OH_PACKED       11

/* the choices */
enum { OH_D_MUFFLER, OH_D_FILTER, OH_D_OVERDRIVE, OH_D_DIST, OH_NDTYPE };
#define OH_NPRESET      16
#define OH_NSCALE       12
#define OH_NROOT        12
#define OH_NTONE        12
#define OH_MID          16              /* 5-bit "centre" (PARAM2's jobs) */

/* a step's velocity: what the line's step plays. 127 (a step turned on by hand) is a plain note */
#define OH_VEL_NORMAL   127
#define OH_VEL_ACCENT   104
#define OH_VEL_SLIDE    80
#define OH_VEL_BOTH     48
#define OH_ACC(v)       (((v) >= 96 && (v) < 112) || ((v) > 0 && (v) < 64))    /* accented */
#define OH_SLD(v)       ((v) > 0 && (v) < 96)                                   /* slides into the next */

static inline void oh_pack(const uint8_t *v, uint8_t *out)
{
    for (int i = 0; i < OH_PACKED; i++)
        out[i] = 0;
    unsigned bit = 0;
    for (int f = 0; f < OH_NFIELD; f++) {
        unsigned x = v[f] & ((1u << OH_WIDTH[f]) - 1u);
        for (unsigned b = 0; b < OH_WIDTH[f]; b++, bit++)
            if (x >> b & 1u)
                out[bit >> 3] |= (uint8_t)(1u << (bit & 7));
    }
}

static inline void oh_unpack(const uint8_t *in, uint8_t *v)
{
    unsigned bit = 0;
    for (int f = 0; f < OH_NFIELD; f++) {
        unsigned x = 0;
        for (unsigned b = 0; b < OH_WIDTH[f]; b++, bit++)
            x |= (unsigned)(in[bit >> 3] >> (bit & 7) & 1u) << b;
        v[f] = (uint8_t)x;
    }
}

/* the largest value a field takes (the choices have fewer than their bits allow) */
static inline int oh_max(int f)
{
    switch (f) {
    case OH_TONE:   return OH_NTONE - 1;
    case OH_PRESET: return OH_NPRESET - 1;
    case OH_SCALE:  return OH_NSCALE - 1;
    case OH_ROOT:   return OH_NROOT - 1;
    case OH_D_ACC: case OH_D_SLIDE: case OH_D_OCT: case OH_D_REST: return 2;
    case OH_RSTYLE: return 1;
    default:        return (1 << OH_WIDTH[f]) - 1;
    }
}

/* every field in its range (a record from a later version can't play nonsense) */
static inline void oh_clean(uint8_t *v)
{
    for (int f = 0; f < OH_NFIELD; f++)
        if (v[f] > oh_max(f))
            v[f] = (uint8_t)oh_max(f);
}

/* one field of a packed sound */
static inline unsigned oh_bit(int f)
{
    unsigned b = 0;
    for (int i = 0; i < f; i++)
        b += OH_WIDTH[i];
    return b;
}

static inline int oh_get(const uint8_t *p, int f)
{
    unsigned bit = oh_bit(f), x = 0;
    for (unsigned b = 0; b < OH_WIDTH[f]; b++, bit++)
        x |= (unsigned)(p[bit >> 3] >> (bit & 7) & 1u) << b;
    return (int)x;
}

static inline void oh_put(uint8_t *p, int f, int x)
{
    unsigned bit = oh_bit(f);
    for (unsigned b = 0; b < OH_WIDTH[f]; b++, bit++) {
        uint8_t m = (uint8_t)(1u << (bit & 7));
        p[bit >> 3] = (uint8_t)((x >> b & 1) ? p[bit >> 3] | m : p[bit >> 3] & ~m);
    }
}
