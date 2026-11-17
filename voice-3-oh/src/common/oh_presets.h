/* 3 Oh: the 16 presets, as the kit saves a sound (oh_sound.h). A preset press keeps the
 * voice's line settings; these columns hold the defaults: Natural Minor on C, octave C2, 3 notes
 * over an octave, every density low. */
#pragma once
#include "oh_sound.h"

#define OH_P(cut, res, env, acc, dist, dtype, tone, slide, trim, n) \
    {cut, res, env, acc, dist, dtype, tone, slide, trim, n, 1, 0, 1, 1, 1, 1, 1, 1, 1, OH_MID, OH_MID, OH_MID, 0}

static const uint8_t PRESETS[OH_NPRESET][OH_NFIELD] = {
    /*    cut res env acc dist type            tone slide trim */
    OH_P( 10, 22, 18, 20,  0, OH_D_MUFFLER,    6, 12, 8,  0),   /* Classic Acid */
    OH_P(  6, 28, 26, 26,  4, OH_D_MUFFLER,    7, 12, 8,  1),   /* Squelch */
    OH_P(  8, 12, 14, 14,  0, OH_D_MUFFLER,    6, 12, 8,  2),   /* Rubber Bass */
    OH_P(  4, 17, 10, 12,  8, OH_D_MUFFLER,    4, 12, 8,  3),   /* Deep Roller */
    OH_P( 12, 26, 30, 18,  0, OH_D_MUFFLER,    8, 10, 8,  4),   /* Chirp */
    OH_P(  7, 30, 20, 31,  0, OH_D_MUFFLER,    6, 12, 8,  5),   /* Wow */
    OH_P( 14, 20, 12, 10,  0, OH_D_MUFFLER,    6, 18, 8,  6),   /* Liquid */
    OH_P( 15, 14, 28, 14,  0, OH_D_MUFFLER,    6,  8, 8,  7),   /* Pluck */
    OH_P(  9, 20, 18, 20, 16, OH_D_MUFFLER,    5, 12, 8,  8),   /* Muffled */
    OH_P( 10, 24, 20, 22, 18, OH_D_FILTER,     7, 12, 8,  9),   /* Hot Filter */
    OH_P(  3,  9,  8, 10, 22, OH_D_FILTER,     3, 12, 8, 10),   /* Gritty Sub */
    OH_P( 12, 26, 22, 24, 20, OH_D_OVERDRIVE,  8, 12, 8, 11),   /* Screamer */
    OH_P( 18, 22, 16, 18, 13, OH_D_OVERDRIVE,  9, 15, 8, 12),   /* Acid Lead */
    OH_P( 13, 27, 24, 26, 25, OH_D_DIST,       8, 12, 8, 13),   /* Rave */
    OH_P(  8, 18, 16, 16, 30, OH_D_DIST,       4, 12, 8, 14),   /* Fuzz Line */
    OH_P( 11, 29, 28, 28, 31, OH_D_DIST,      10, 10, 8, 15),   /* Industrial */
};
#undef OH_P
