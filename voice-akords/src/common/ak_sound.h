/* Akords: a voice's sound as small values, shared by the M4 (the kit, Random) and the M7
 * (it plays them). 25 values, packed in 16 bytes (125 bits), as the kit keeps them and the
 * message "akords-voice" carries them.
 *
 *   per oscillator (1, 2): waveform, octave, detune, level, pulse width
 *   the filter: type, cutoff, resonance, envelope amount, key tracking
 *   amp and filter envelopes: attack, decay, sustain, release
 *   glide, level trim
 *
 * The M7 turns them into its units (akords.c, decode). AK_PRESETS are the iOS synth's
 * factory patches, quantised (each value within 7% of the original): what Random starts
 * from; a voice playing a preset's own sound plays the M7's exact float table. */
#pragma once
#include <stdint.h>

enum {
    AK_WAVE, AK_OCT, AK_DET, AK_LEV, AK_PW, AK_OSC_N,           /* an oscillator's values */
    AK_FMODE = 2 * AK_OSC_N, AK_CUT, AK_RES, AK_ENVAMT, AK_KEYTRK,
    AK_AENV, AK_FENV = AK_AENV + 4,                              /* attack, decay, sustain, release */
    AK_GLIDE = AK_FENV + 4, AK_TRIM, AK_NFIELD
};
#define AK_OSC(i)       ((i) * AK_OSC_N)
enum { AK_E_ATT, AK_E_DEC, AK_E_SUS, AK_E_REL };
enum { AK_W_SINE, AK_W_TRI, AK_W_SAW, AK_W_PULSE, AK_NWAVE };
enum { AK_F_LP24, AK_F_HP12, AK_F_FORMANT, AK_NFILT };
#define AK_PACKED       16

/* a voice's options (the page's row 3, steps 12-16), with its sound in "akords-voice" */
#define AK_O_STRUM_UP   0x01        /* the notes start one by one, low to high */
#define AK_O_STRUM_DOWN 0x02        /* high to low */
#define AK_O_SPREAD     0x04        /* the second-highest note an octave down */
#define AK_O_DROP       0x08        /* the lowest note an octave down */
#define AK_O_MUTATE     0x10        /* each chord a small variation of the sound */
#define AK_O_ALL        0x1f
#define AK_O_OWN        0x80        /* (message only) the sound is the preset's own: the M7
                                       plays its exact preset, not these values */

/* Units (the M7's decode):
 *   wave 0-3; octave x - 2 (-2..+3); detune x - 32 cents; level x / 31; pulse width x / 40
 *   (2-20: 5%-50%); filter type 0-2; cutoff 100 Hz * 2^(x / 256 * 7.4919) (100 Hz-18 kHz,
 *   x = 128: the formant's middle vowel, "ah"); resonance x / 100; envelope amount
 *   (x - 20) / 20 (-1..1); key tracking x / 20; times 1 ms * 2^(x / 5) (1 ms-6.2 s);
 *   sustain x / 20; glide x * 10 ms (0: off); level trim 0.5 + x / 62 */
static const uint8_t AK_BITS[AK_NFIELD] = {
    2, 3, 6, 5, 5,  2, 3, 6, 5, 5,
    2, 8, 7, 6, 5,
    6, 6, 5, 6,  6, 6, 5, 6,
    4, 5,
};
static const uint8_t AK_MAX[AK_NFIELD] = {
    3, 5, 63, 31, 20,  3, 5, 63, 31, 20,
    2, 255, 100, 40, 20,
    63, 63, 20, 63,  63, 63, 20, 63,
    15, 31,
};
static const uint8_t AK_MIN[AK_NFIELD] = {
    0, 0, 0, 0, 2,  0, 0, 0, 0, 2,
};
#define AK_MID_DET      32
#define AK_MID_ENV      20

#define AK_NPRESET      16
/* the 16 presets, from gen_sound.py (the original float table, quantised) */
static const uint8_t AK_PRESETS[AK_NPRESET][AK_NFIELD] = {
    {2, 1, 32, 28, 20, 3, 1, 37, 17, 14, 0, 130, 35, 29, 6, 8, 40, 11, 35, 10, 39, 3, 36, 0, 25},    /* Bass */
    {2, 2, 26, 22, 20, 2, 2, 40, 22, 20, 0, 152, 18, 27, 10, 46, 48, 17, 51, 49, 48, 12, 51, 0, 28}, /* Pad */
    {2, 2, 32, 26, 20, 3, 2, 36, 19, 12, 0, 184, 30, 28, 14, 13, 41, 15, 40, 13, 42, 10, 40, 8, 19}, /* Lead */
    {2, 2, 32, 25, 20, 1, 3, 32, 16, 20, 0, 177, 15, 30, 12, 10, 45, 7, 42, 10, 44, 4, 42, 0, 23},   /* Keys */
    {2, 2, 32, 26, 20, 2, 2, 42, 19, 20, 0, 161, 40, 32, 10, 5, 39, 0, 37, 5, 37, 0, 37, 0, 10},     /* Pluck */
    {2, 2, 24, 25, 20, 2, 2, 40, 25, 20, 0, 133, 42, 31, 8, 10, 41, 0, 35, 10, 37, 0, 35, 0, 4},     /* Stab */
    {3, 2, 22, 25, 20, 3, 2, 42, 22, 10, 0, 164, 20, 27, 12, 22, 43, 16, 43, 25, 45, 11, 43, 0, 7},  /* Hollow */
    {2, 1, 10, 28, 20, 2, 1, 54, 28, 20, 0, 108, 25, 26, 5, 12, 43, 18, 38, 17, 45, 12, 40, 4, 6},   /* Reese */
    {1, 2, 32, 25, 20, 0, 3, 32, 22, 20, 0, 222, 0, 20, 6, 12, 38, 19, 30, 12, 38, 20, 30, 0, 31},   /* Organ */
    {0, 3, 32, 26, 20, 1, 4, 37, 16, 20, 0, 202, 0, 20, 12, 5, 49, 0, 45, 5, 46, 0, 45, 0, 29},      /* Bell */
    {2, 2, 25, 23, 20, 3, 3, 39, 16, 16, 1, 96, 30, 12, 10, 17, 46, 14, 45, 12, 43, 6, 43, 0, 19},   /* Air */
    {2, 2, 23, 25, 20, 2, 2, 41, 25, 20, 2, 128, 50, 23, 0, 44, 46, 18, 51, 47, 50, 14, 51, 0, 31},  /* Choir */
    {3, 2, 32, 25, 12, 2, 2, 38, 19, 20, 2, 64, 50, 32, 0, 17, 41, 16, 41, 22, 42, 5, 41, 3, 31},    /* Vox */
    {2, 2, 32, 26, 20, 2, 3, 36, 16, 20, 2, 192, 50, 6, 0, 12, 43, 12, 40, 12, 40, 0, 38, 0, 31},    /* Talk */
    {2, 1, 27, 26, 20, 3, 2, 37, 16, 12, 2, 64, 25, 26, 0, 42, 48, 17, 51, 51, 50, 12, 51, 6, 31},   /* Chant */
    {2, 2, 32, 26, 20, 2, 3, 39, 14, 20, 2, 13, 60, 40, 0, 12, 45, 8, 40, 25, 40, 0, 38, 0, 31},     /* Wah */
};

_Static_assert(AK_NFIELD == 25, "the field list");

/* the values (each within its AK_MAX) into 16 bytes, low bits first */
static inline void ak_pack(const uint8_t *f, uint8_t *out)
{
    for (int i = 0; i < AK_PACKED; i++)
        out[i] = 0;
    unsigned at = 0;
    for (int i = 0; i < AK_NFIELD; i++) {
        unsigned x = f[i];
        for (int b = 0; b < AK_BITS[i]; b++, at++)
            if (x >> b & 1)
                out[at >> 3] |= (uint8_t)(1u << (at & 7));
    }
}

/* 16 bytes into the values, each held within its range */
static inline void ak_unpack(const uint8_t *in, uint8_t *f)
{
    unsigned at = 0;
    for (int i = 0; i < AK_NFIELD; i++) {
        unsigned x = 0;
        for (int b = 0; b < AK_BITS[i]; b++, at++)
            x |= (unsigned)(in[at >> 3] >> (at & 7) & 1) << b;
        unsigned lo = i < (int)sizeof AK_MIN ? AK_MIN[i] : 0;
        f[i] = (uint8_t)(x > AK_MAX[i] ? AK_MAX[i] : (x < lo ? lo : x));
    }
}
