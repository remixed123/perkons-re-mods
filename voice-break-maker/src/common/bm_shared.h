/* BREAK MAKER: what the M4 and the M7 share.
 *
 * The break: one 1-bar drum loop, mono 16-bit, in the AXI SRAM that held the stock custom
 * samples (both cores address it alike; the card's DMA reaches it). The M4 loads it from the card
 * (BREAKS/n.wav), finds its 16 slices and what each one is (kick, snare, hat, ghost), and fills
 * the control block; the M7 plays it. While the M4 rewrites it, `ready` is 0 and the M7 is silent.
 *
 * Messages (ordered: on the trigger stream, handled in the audio callback in order with the
 * triggers):
 *   "break-maker-step"  each step of a track running BREAK MAKER, before the step's triggers (from
 *                       the sequencer's tick), and once when the sequencer stops: BM_STEP_*.
 *   "break-maker"       a voice's page settings: BM_SET_*. */
#pragma once
#include <stdint.h>

#define BM_SLICES           16
#define BM_BASE             0x24020100u             /* the stock sample data starts at 0x240200e4 */
#define BM_CTRL_SIZE        0x100u
#define BM_PCM              (BM_BASE + BM_CTRL_SIZE)
#define BM_POOL_END         0x24080000u             /* the stock sample data ends here */
#define BM_WORK             (BM_POOL_END - 0x400u)  /* the M4's file objects (the card's DMA fills their buffers) */
#define BM_FIL              BM_WORK                 /* FatFs FIL (0x230) */
#define BM_DIR              (BM_WORK + 0x240u)      /* DIR (FW_DIR_MAX) */
#define BM_FNO              (BM_WORK + 0x2c0u)      /* FILINFO (0x116) */
#define BM_PCM_BYTES        (BM_WORK - BM_PCM)      /* 391,680 B: ~4.44 s at 44.1 kHz */
#define BM_MAGIC            0x524b4d42u             /* 'BMKR' */

/* what each slice is (found by the M4 at load) */
enum { BM_T_KICK, BM_T_SNARE, BM_T_HAT, BM_T_GHOST, BM_NTYPE };

typedef struct {
    volatile uint32_t magic;        /* BM_MAGIC once the M4 host has booted */
    volatile uint32_t ready;        /* 1: pcm and slices valid (0 while loading, or no break) */
    volatile uint32_t gen;          /* +1 per load */
    uint32_t frames;                /* the break's length */
    uint32_t rate;                  /* Hz */
    uint32_t pcm;                   /* address of frame 0 (BM_PCM + 0..3) */
    uint32_t slice[BM_SLICES + 1];  /* frame where each slice starts; slice[16] = frames */
    uint8_t type[BM_SLICES];        /* BM_T_* */
    uint8_t number;                 /* the break loaded, 0-15 (BREAKS/<number + 1>.wav) */
    uint8_t bars;                   /* how many bars the file is (1, 2, 4): its first one is played */
    uint8_t pad[2];
} bm_ctrl_t;

_Static_assert(sizeof(bm_ctrl_t) <= BM_CTRL_SIZE, "control fits");
_Static_assert(__builtin_offsetof(bm_ctrl_t, slice) == 24 && __builtin_offsetof(bm_ctrl_t, type) == 92,
               "bm_rig.py: CTRL fields");

#define BM                  ((bm_ctrl_t *)(uintptr_t)BM_BASE)

/* "break-maker-step": [0] output, [1] step (0-15), [2] first, [3] last, [4] flags, [5..6] tempo x 10,
 * [7..8] PARAM2 (0-4095) for the step's trigger: its lock, else the knob's (the voice parameters can
 * reach the M7 after the trigger) */
#define BM_STEP_LEN         9
#define BM_STEP_PLAYING     0x01    /* the sequencer plays (0: it stopped; the step bytes mean nothing) */
#define BM_STEP_ON          0x02    /* the step is on: its trigger follows */
#define BM_STEP_HIT         0x04    /* not a new step: the step's trigger goes out next (its PARAM2) */

/* "break-maker": [0] output, [1] flags, [2..3] pads held (bits, BM_PAD_*), [4] character,
 * [5] audition: bits 0-4 slice + 1 (0 none), bits 5-7 a count (a new value: play it) */
#define BM_SET_LEN          6
#define BM_SET_KEEP         0x01    /* pitch: keep length (else repitch) */

/* the pads (page row 2, while held) */
enum { BM_PAD_ROLL8, BM_PAD_ROLL16, BM_PAD_ROLL32, BM_PAD_REVERSE, BM_PAD_FREEZE, BM_PAD_HALF,
       BM_PAD_TAPESTOP, BM_PAD_GATE, BM_PAD_RANDOM, BM_PAD_FILTER, BM_PAD_STUTTER, BM_NPAD };

/* the generator's characters (page row 4, 1-8) */
enum { BM_C_STRAIGHT, BM_C_SPARSE, BM_C_BOOMBAP, BM_C_HALFTIME, BM_C_TWOSTEP, BM_C_JUNGLE, BM_C_FILLS,
       BM_C_BREAKCORE, BM_NCHAR };

/* A step's velocity says how it plays (a step turned on by hand gets 127: plain). The generator
 * and the page write the middle values; MIDI velocity picks them too. */
enum { BM_V_PLAIN, BM_V_GHOST, BM_V_REVERSE, BM_V_ROLL2, BM_V_ROLL3, BM_V_ROLL4, BM_V_REVROLL, BM_NVEL };
static inline int bm_vel_kind(int vel)
{
    if (vel >= 112 || vel <= 0)
        return BM_V_PLAIN;
    if (vel >= 96)
        return BM_V_GHOST;
    if (vel >= 80)
        return BM_V_REVERSE;
    if (vel >= 64)
        return BM_V_ROLL2;
    if (vel >= 48)
        return BM_V_ROLL3;
    if (vel >= 32)
        return BM_V_ROLL4;
    return BM_V_REVROLL;
}
static const uint8_t BM_VEL_OF[BM_NVEL] = {127, 104, 88, 72, 56, 40, 16};

/* PARAM2 (0-4095): fully left (zone 0) follows the step; zones 1-16 are slices 1-16 */
#define BM_P2_ZONES         17
static inline int bm_p2_zone(int x)
{
    int z = x * BM_P2_ZONES / 4096;
    return z < 0 ? 0 : (z >= BM_P2_ZONES ? BM_P2_ZONES - 1 : z);
}
static inline int bm_p2_of_slice(int s)         /* the middle of slice s's zone (a lock's value) */
{
    return ((2 * (s + 1) + 1) * 4096) / (2 * BM_P2_ZONES);
}
