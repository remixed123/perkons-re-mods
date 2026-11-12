/* Synth Lab, M4 side: each kit's sounds, the configuration page, Random, Tweak, the tempo and DRIVE.
 *
 * A voice's sound is 88 small values, packed in 40 bytes (sl_sound.h), as the M7 plays it and
 * the kit keeps it (the M4 host's kit sidecar, KITS/PP.KIX, record SLAB: 4 x 40 bytes); a kit
 * without one has the default presets (1, 5, 12, 14 on voices 1-4). Presets, the page, Random
 * and Tweak all write these values: what plays is what's saved.
 *
 * PARAM2: ALGO chooses its job, and it sets three values. The kit keeps only the knob's
 * position, so this core follows the M7's rule on the knob's own position (no locks) and
 * keeps the values: a new job, or a value loaded, holds until the knob moves 2% of its travel;
 * then the knob sets it. Loading a kit sends the sounds held (the M7 doesn't follow the knob)
 * until the kit's positions have reached the M7, then sends them again, free (as Kick Lab).
 *
 * The configuration page: a long press on a voice's Synth Lab step on voice-map's engine page.
 *   row 1   presets 1-16 (a press, even on the one chosen, gives the preset's whole sound and
 *           sets PARAM1 to 12 o'clock; the scale, root and Mutate stay; held: the group's
 *           starter: 1-4 Init Bass, 5-8 Init Lead, 9-11 Init Pad, 12-13 Init Pluck, 14-16 Init)
 *   row 2   cells: oscillator 1 (1-4), 2 (5-8), 3 (9-12): Wave, Tune, Pitch LFO, Volume LFO;
 *           13-16: Filter Env, Amp Env, Cutoff LFO, Fold LFO
 *   row 3   scales (1-10), cells Filter (11) and Fold (12), Tweak light, medium, wild (13-15;
 *           held: undo), Mutate on / off (16)
 *   row 4   roots C-B (1-12), Random Mild, Wild, Recipe, All (13-16; held: undo)
 * A cell: hold it and turn RATE, WAVE, MOD LVL or press SYNC: they set its four values (the
 * stock LFO keeps its own; the knob takes the value once it turns 2%). Bright: chosen or held;
 * medium: a cell in use; dim: the rest. The trigger keys play; SHIFT, KIT or MOD goes back to
 * the engine page; another page button leaves as stock. Leaving keeps a change in the kit.
 *
 * The tempo (LFO sync) and each voice's DRIVE (the level) go to the M7 from the main loop when
 * they change, and again every 250 ms ("synth-lab-live"). */
#include <stdint.h>
#include "pk_host.h"
#include "fw_m4_seq.h"
#include "pk_vm.h"                  /* pk_vm_engine */
#include "pk_sy_engine.h"           /* PK_SY_ENGINE: Synth Lab's engine number */
#include "sl_sound.h"
#include "sl_presets.h"

#define NKITS           64
#define NB              SL_PACKED
#define LED_ON          0x80
#define LED_MID         0x38
#define LED_DIM         0x10
#define MOVED           82          /* knob travel (0-4095, 2%), as the M7's */
#define TURN            0.02f       /* an LFO knob's travel (0-1) that hands it to a held cell */
#define UNDO_MS         500         /* a step held this long: a starter (presets), undo (Tweak, Random) */
#define POS1            1
#define POS3            2
#define F_HELD          0x10        /* message flags, as the M7's */
#define F_KEEP          0x20
enum { CAT_BASS, CAT_LEAD, CAT_PAD, CAT_KEYS, CAT_FX };
enum { R_MILD = 1, R_WILD, R_RECIPE };

extern uint8_t pk_vm_settings_voice;

static const uint8_t DEFAULT_PRESET[4] = {0, 4, 11, 13};

typedef struct {
    uint16_t ref2;                  /* PARAM2 when the job or the value was set (0-4095) */
    uint8_t role, armed, running, pad;
} follow_t;

/* (every variable whole words, word-aligned: the M4 host clears its RAM a word at a time) */
#define WORDS __attribute__((aligned(4)))
static uint8_t kit_snd[NKITS][4][NB] WORDS;     /* each kit's sounds (MAIN): 10,240 B */
static uint8_t sy_cur[4][NB] WORDS;                /* the sounds playing */
static uint8_t undo[4][NB] WORDS;               /* each voice's sound before its last Tweak / Random */
static follow_t fol[4] WORDS;
static uint32_t undo_ok;                        /* voices (bits) with an undo */
static uint32_t editing, ready, active;         /* the page's voice; sy_cur filled; the page open */
static uint32_t kit_ok[NKITS / 32];
static uint32_t rng;
/* a kit loaded: the M7 holds its values until the positions arrive (ms, low 16 bits) */
#define HOLD_SETTLED_MS 20
#define HOLD_MAX_MS     2000
static uint32_t hold_from, hold_matched, holding;
/* tempo and DRIVE */
#define LIVE_EVERY_MS   5
#define LIVE_RESEND_MS  250
static uint8_t live_sent[8] WORDS;
static uint32_t live_checked, live_at, live_any;
/* the page: the cell held (its first field + 1, 0 none), the knobs' anchors */
static int32_t cell1;
#define CELL            (cell1 - 1)
static uint32_t cell_key, turning, tweak_key, tweak_at, tweak_done, all_last;   /* tweak_key: the held step + 1 */
static float anchor[3];

static int current_kit(void)
{
    return *(volatile uint16_t *)(uintptr_t)SYM_current_kit;
}

static uint8_t *vstate(int v)
{
    return fw_voice_state(v);
}

static uint32_t base12(int v, int slot)
{
    return FW_U16(vstate(v) + FW_VS_BASE + 2 * slot) >> 4;
}

static uint8_t role_now(int v)
{
    int algo = FW_U16(vstate(v) + FW_VS_BASE + 2 * FW_SLOT_ALGO);
    return algo == POS1 ? 0 : (algo == POS3 ? 2 : 1);   /* resonance, envelope amount, sync */
}

static void preset_into(uint8_t *packed, int i)
{
    sl_pack(PRESETS[i & (SL_NPRESET - 1)], packed);
}

static void defaults(uint8_t to[4][NB])
{
    for (int v = 0; v < 4; v++)
        preset_into(to[v], DEFAULT_PRESET[v]);
}

static void make_ready(void)
{
    if (!ready) {
        ready = 1;
        defaults(sy_cur);
    }
}

/* ---- to the M7 ------------------------------------------------------------------------ */

static void send(int v, int flags)
{
    make_ready();
    uint8_t msg[NB + 1];
    msg[0] = (uint8_t)(v | flags);
    for (int i = 0; i < NB; i++)
        msg[1 + i] = sy_cur[v][i];
    pk_send_m7(PK_STREAM_SYNTH_LAB, PK_MSG_SYNTH_LAB, msg, sizeof msg);
}

static void send_all(int voices, int flags)
{
    for (int v = 0; v < 4; v++)
        if (voices >> v & 1)
            send(v, flags);
}

/* A sound changed by the page, Random, Tweak or undo: sent from the main loop (flush), never
 * from where it changed. The LFO knob poll runs in an interrupt: a send from there can land
 * inside the sequencer's send of the triggers on the same stream, and two senders interleaved
 * corrupt the ring's reservation: the M7 then stops reading the stream until a reboot. */
static uint32_t dirty;                  /* voices (bits) to send; << 4: with PARAM2's values (no KEEP) */

static void mark(int v, int flags)
{
    uint32_t pm = pk_irq_off();
    dirty |= (1u << v) | (flags & F_KEEP ? 0u : 1u << (4 + v));
    pk_irq_restore(pm);
}

static void flush(void)
{
    uint32_t pm = pk_irq_off();
    uint32_t d = dirty;
    dirty = 0;
    pk_irq_restore(pm);
    for (int v = 0; v < 4; v++)
        if (d >> v & 1)
            send(v, d >> (4 + v) & 1 ? 0 : F_KEEP);
}

static void follow_reset(int v)
{
    fol[v].role = role_now(v);
    fol[v].ref2 = (uint16_t)base12(v, FW_SLOT_PARAM2);
    fol[v].armed = 0;
}

/* PARAM2 of a voice running Synth Lab, by the M7's rule: its value in the sound (5 bits,
 * 16 = 12 o'clock) */
static void follow(int v)
{
    follow_t *f = &fol[v];
    int x2 = (int)base12(v, FW_SLOT_PARAM2);
    uint8_t r = role_now(v);
    if (r != f->role) {
        f->role = r;
        f->ref2 = (uint16_t)x2;
        f->armed = 0;
    }
    int d = x2 - (int)f->ref2;
    if (d > MOVED || d < -MOVED)
        f->armed = 1;
    if (f->armed) {
        int q = SL_MID + (x2 * 30 + 2047) / 4095 - 15;
        q = q < 0 ? 0 : (q > 31 ? 31 : q);
        if (sl_get(sy_cur[v], SL_P2_RES + r) != q)
            sl_put(sy_cur[v], SL_P2_RES + r, q);
    }
}

static int arrived(int v)
{
    static const uint8_t SLOTS[3] = {FW_SLOT_PARAM2, FW_SLOT_ALGO, FW_SLOT_MODE};
    const uint8_t *vs = vstate(v);
    for (int i = 0; i < 3; i++)
        if (FW_U16(vs + FW_VS_VALUE + 2 * SLOTS[i]) != FW_U16(vs + FW_VS_BASE + 2 * SLOTS[i]))
            return 0;
    return 1;
}

static void poll(void)
{
    make_ready();
    int started = 0;
    for (int v = 0; v < 4; v++) {
        int on = pk_vm_engine(v) == PK_SY_ENGINE;
        if (on && !fol[v].running) {
            follow_reset(v);
            started |= 1 << v;
        } else if (on && !holding) {
            follow(v);
        }
        fol[v].running = (uint8_t)on;
    }
    if (started)
        send_all(started, holding ? F_HELD : 0);
    if (holding) {
        uint16_t now = (uint16_t)fw_get_tick();
        for (int v = 0; v < 4; v++)
            if (fol[v].running && !arrived(v))
                hold_matched = now;
        if ((uint16_t)(now - hold_matched) >= HOLD_SETTLED_MS || (uint16_t)(now - hold_from) >= HOLD_MAX_MS) {
            holding = 0;
            for (int v = 0; v < 4; v++)
                follow_reset(v);
            send_all(15, 0);
        }
    }
}

/* ---- kits ------------------------------------------------------------------------------- */

void PK_NEXT(pk_sy_kit_load)(int kit);

void pk_sy_kit_load(int kit)
{
    PK_NEXT(pk_sy_kit_load)(kit);
    if (kit < 0 || kit >= NKITS)
        return;
    if (!(kit_ok[kit / 32] >> (kit % 32) & 1))
        defaults(kit_snd[kit]);
    for (int v = 0; v < 4; v++) {
        for (int b = 0; b < NB; b++)
            sy_cur[v][b] = kit_snd[kit][v][b];
        follow_reset(v);
    }
    undo_ok = 0;
    dirty = 0;                                  /* the page's changes: the kit's sounds instead */
    ready = 1;
    holding = 1;
    hold_from = hold_matched = (uint16_t)fw_get_tick();
    send_all(15, F_HELD);
}

uint32_t pk_sy_kit_save(int bank, int kit, uint8_t *buf, uint32_t max)
{
    (void)bank;
    poll();
    if (kit >= 0 && kit < NKITS) {
        for (int v = 0; v < 4; v++)
            for (int b = 0; b < NB; b++)
                kit_snd[kit][v][b] = sy_cur[v][b];
        kit_ok[kit / 32] |= 1u << (kit % 32);
    }
    if (max < 4 * NB)
        return 0;
    for (int v = 0; v < 4; v++)
        for (int b = 0; b < NB; b++)
            buf[v * NB + b] = sy_cur[v][b];
    return 4 * NB;
}

void pk_sy_kit_restore(int bank, int kit, const uint8_t *rec, uint32_t len)
{
    (void)bank;
    if (kit < 0 || kit >= NKITS)
        return;
    kit_ok[kit / 32] |= 1u << (kit % 32);
    if (len != 4 * NB) {                        /* none (or not ours): the defaults */
        defaults(kit_snd[kit]);
        return;
    }
    for (int v = 0; v < 4; v++) {
        uint8_t x[SL_NFIELD];
        sl_unpack(rec + v * NB, x);
        sl_clean(x);
        sl_pack(x, kit_snd[kit][v]);
    }
}

/* ---- the main loop: PARAM2, the hold, tempo and DRIVE ---------------------------------------- */

void PK_NEXT(pk_sy_main_loop)(void);

void pk_sy_main_loop(void)
{
    PK_NEXT(pk_sy_main_loop)();
    if (dirty)
        flush();
    uint16_t now = (uint16_t)fw_get_tick();
    if ((uint16_t)(now - live_checked) < LIVE_EVERY_MS)
        return;
    live_checked = now;
    poll();
    float bpm = FW_CLOCK_BPM;
    uint16_t t = bpm > 0.0f && bpm < 999.0f ? (uint16_t)(bpm * 10.0f + 0.5f) : 1200;
    uint8_t msg[6] = {(uint8_t)t, (uint8_t)(t >> 8)};
    int changed = !live_any;
    for (int v = 0; v < 4; v++)
        msg[2 + v] = (uint8_t)(FW_U16(vstate(v) + FW_VS_VALUE + 2 * FW_SLOT_DRIVE) >> 8);
    for (int i = 0; i < 6; i++)
        changed |= msg[i] != live_sent[i];
    if (changed || (uint16_t)(now - live_at) >= LIVE_RESEND_MS) {
        live_any = 1;
        live_at = now;
        for (int i = 0; i < 6; i++)
            live_sent[i] = msg[i];
        pk_send_m7(PK_STREAM_SYNTH_LAB_LIVE, PK_MSG_SYNTH_LAB_LIVE, msg, sizeof msg);
    }
}

/* ---- Random and Tweak (on the voice's unpacked values) ------------------------------------- */

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static int rr(int n)                    /* 0 .. n - 1 */
{
    return n > 1 ? (int)(rnd() % (uint32_t)n) : 0;
}

static int between(int lo, int hi)
{
    return lo + rr(hi - lo + 1);
}

static int chance(int pct)
{
    return rr(100) < pct;
}

static void put(uint8_t *v, int f, int x)
{
    int m = sl_max(f);
    v[f] = (uint8_t)(x < 0 ? 0 : (x > m ? m : x));
}

static void nudge(uint8_t *v, int f, int d)
{
    put(v, f, (int)v[f] + between(-d, d));
}

static int category(int preset)
{
    return preset < 4 ? CAT_BASS : preset < 8 ? CAT_LEAD : preset < 11 ? CAT_PAD : preset < 12 ? CAT_KEYS : CAT_FX;
}

static void lfo(uint8_t *v, int at, int speed, int shape, int depth, int sync)
{
    v[at + SL_L_SPEED] = (uint8_t)speed;
    v[at + SL_L_SHAPE] = (uint8_t)shape;
    v[at + SL_L_DEPTH] = (uint8_t)depth;
    v[at + SL_L_SYNC] = (uint8_t)sync;
}

static void osc(uint8_t *v, int i, int wave, int level, int oct, int fine, int sync)
{
    uint8_t *o = v + SL_OSC(i);
    o[SL_WAVE] = (uint8_t)wave;
    o[SL_LEVEL] = (uint8_t)level;
    o[SL_OCT] = (uint8_t)oct;
    o[SL_FINE] = (uint8_t)fine;
    o[SL_SYNC] = (uint8_t)sync;
}

static void env(uint8_t *v, int at, int att, int rel, int sus, int curve)
{
    v[at + SL_E_ATT] = (uint8_t)att;
    v[at + SL_E_REL] = (uint8_t)rel;
    v[at + SL_E_SUS] = (uint8_t)sus;
    v[at + SL_E_CURVE] = (uint8_t)curve;
}

/* a plain start for a recipe: osc 1 a saw, the rest off, filter LP24, no LFOs or fold */
static void blank(uint8_t *v)
{
    for (int i = 0; i < 3; i++) {
        osc(v, i, SL_W_SAW, i ? 0 : 26, 2, SL_MID, 0);
        v[SL_OSC(i) + SL_PW] = 31;
        v[SL_OSC(i) + SL_SWEEP] = i ? 16 + 6 * (2 - i) : 0;
        v[SL_OSC(i) + SL_RATIO] = 1;
        lfo(v, SL_OSC(i) + SL_PLFO, 16, SL_S_SINE, 0, 0);
        lfo(v, SL_OSC(i) + SL_VLFO, 16, SL_S_SINE, 0, 0);
    }
    env(v, SL_FENV, 0, 10, 4, 0);
    env(v, SL_AENV, 0, 12, 24, 0);
    lfo(v, SL_CLFO, 8, SL_S_SINE, 0, 0);
    lfo(v, SL_FLFO, 8, SL_S_SINE, 0, 0);
    v[SL_FOLD_AMT] = 0;
    v[SL_FOLD_SYM] = SL_MID;
    v[SL_FOLD_PUSH] = 0;
    v[SL_FOLD_ON] = 0;
    v[SL_FILT_TYPE] = SL_F_LP24;
    v[SL_KEYTRACK] = 2;
    v[SL_FDECAY] = 14;
    v[SL_FILT_ON] = 1;
    v[SL_CUTOFF] = 18;
    v[SL_RES] = 8;
    v[SL_ENVAMT] = 24;
    v[SL_ADECAY] = 14;
    v[SL_GLIDE] = 0;
    v[SL_LEGATO] = 0;
    v[SL_LFO_FREE] = 1;
    v[SL_TRIM] = 8;
}

/* a vibrato on every oscillator */
static void vibrato(uint8_t *v)
{
    int sp = between(22, 26), d = between(3, 5);
    for (int i = 0; i < 3; i++)
        lfo(v, SL_OSC(i) + SL_PLFO, sp, SL_S_SINE, d, 0);
}

/* Recipe: a coherent sound of the preset's kind */
static void recipe(uint8_t *v, int cat)
{
    blank(v);
    int r = rr(cat == CAT_PAD || cat == CAT_KEYS ? 3 : 4);
    switch (cat) {
    case CAT_BASS:
        v[SL_CUTOFF] = (uint8_t)between(9, 15);
        v[SL_RES] = (uint8_t)between(6, 18);
        v[SL_ENVAMT] = (uint8_t)between(22, 28);
        v[SL_FDECAY] = (uint8_t)between(9, 13);
        env(v, SL_AENV, 0, between(8, 11), between(10, 24), 0);
        v[SL_ADECAY] = (uint8_t)between(11, 14);
        if (r == 0) {                   /* sync stack */
            osc(v, 0, SL_W_SAW, 26, 1, SL_MID, 0);
            osc(v, 1, SL_W_SAW, 22, between(2, 3), between(14, 18), 1);
            osc(v, 2, SL_W_PULSE, 14, 1, SL_MID, 0);
            v[SL_OSC(2) + SL_PW] = (uint8_t)between(10, 28);
        } else if (r == 1) {            /* acid */
            osc(v, 0, chance(50) ? SL_W_SAW : SL_W_PULSE, 28, 2, SL_MID, 0);
            v[SL_FILT_TYPE] = chance(50) ? SL_F_LP18 : SL_F_LP24;
            v[SL_RES] = (uint8_t)between(18, 27);
            v[SL_ENVAMT] = (uint8_t)between(25, 30);
            v[SL_AENV + SL_E_SUS] = (uint8_t)between(0, 8);
            v[SL_GLIDE] = (uint8_t)between(10, 14);
            v[SL_LEGATO] = 1;
        } else if (r == 2) {            /* folded sub */
            osc(v, 0, SL_W_SINE, 28, 1, SL_MID, 0);
            osc(v, 1, SL_W_TRI, 14, 2, SL_MID, 0);
            v[SL_FOLD_ON] = 1;
            v[SL_FOLD_AMT] = (uint8_t)between(6, 16);
            v[SL_FOLD_PUSH] = (uint8_t)between(6, 16);
            v[SL_FILT_TYPE] = chance(50) ? SL_F_LP6 : SL_F_LP12;
            v[SL_CUTOFF] = (uint8_t)between(16, 22);
        } else {                        /* reese */
            osc(v, 0, SL_W_SAW, 24, 2, SL_MID - between(3, 6), 0);
            osc(v, 1, SL_W_SAW, 24, 2, SL_MID + between(3, 6), 0);
            osc(v, 2, SL_W_SINE, 18, 1, SL_MID, 0);
            lfo(v, SL_OSC(1) + SL_VLFO, between(6, 12), SL_S_SINE, between(4, 9), 0);
            v[SL_AENV + SL_E_SUS] = 26;
        }
        break;
    case CAT_LEAD:
        env(v, SL_AENV, between(0, 2), between(10, 13), between(22, 28), 0);
        env(v, SL_FENV, 0, 11, between(6, 14), 0);
        v[SL_FDECAY] = (uint8_t)between(11, 14);
        v[SL_FILT_TYPE] = SL_F_LP12;
        v[SL_CUTOFF] = (uint8_t)between(20, 26);
        v[SL_RES] = (uint8_t)between(6, 14);
        v[SL_ENVAMT] = (uint8_t)between(20, 24);
        if (r == 0) {                   /* sync lead */
            osc(v, 0, SL_W_SAW, 22, 2, SL_MID, 0);
            osc(v, 1, SL_W_SAW, 26, between(4, 6), SL_MID, 1);
            v[SL_OSC(1) + SL_SWEEP] = (uint8_t)between(18, 26);
            if (chance(60))
                osc(v, 2, SL_W_PULSE, 14, 4, SL_MID, 1);
            if (chance(50))
                vibrato(v);
        } else if (r == 1) {            /* detuned */
            osc(v, 0, SL_W_SAW, 24, 2, SL_MID - between(3, 6), 0);
            osc(v, 1, chance(50) ? SL_W_SAW : SL_W_PULSE, 24, 2, SL_MID + between(3, 6), 0);
            osc(v, 2, SL_W_SAW, 16, 4, SL_MID, 0);
            v[SL_FILT_TYPE] = chance(50) ? SL_F_LP12 : SL_F_LP24;
            if (chance(40))
                vibrato(v);
        } else if (r == 2) {            /* fold lead */
            osc(v, 0, SL_W_TRI, 26, 2, SL_MID, 0);
            osc(v, 1, SL_W_SINE, 14, 4, SL_MID, 0);
            v[SL_FOLD_ON] = 1;
            v[SL_FOLD_AMT] = (uint8_t)between(10, 20);
            v[SL_FOLD_PUSH] = (uint8_t)between(6, 14);
            if (chance(40))
                lfo(v, SL_FLFO, between(4, 12), SL_S_SINE, between(6, 14), 0);
            v[SL_CUTOFF] = (uint8_t)between(22, 28);
            v[SL_TRIM] = 6;
        } else {                        /* screamer: two slaves */
            osc(v, 0, SL_W_SAW, 20, 2, SL_MID, 0);
            osc(v, 1, SL_W_SAW, 22, 5, between(14, 18), 1);
            osc(v, 2, SL_W_SAW, 18, 6, between(14, 18), 1);
            v[SL_FOLD_ON] = 1;
            v[SL_FOLD_AMT] = (uint8_t)between(6, 12);
            v[SL_RES] = (uint8_t)between(14, 22);
            v[SL_TRIM] = 6;
        }
        break;
    case CAT_PAD:
        env(v, SL_AENV, between(14, 19), between(18, 23), between(22, 28), 0);
        env(v, SL_FENV, between(14, 18), between(16, 20), between(8, 18), 0);
        v[SL_FDECAY] = (uint8_t)between(16, 20);
        v[SL_ADECAY] = 16;
        v[SL_ENVAMT] = (uint8_t)between(19, 23);
        v[SL_FILT_TYPE] = chance(50) ? SL_F_LP12 : SL_F_LP24;
        if (r == 0) {                   /* warm */
            int w = chance(60) ? SL_W_SAW : SL_W_PULSE;
            osc(v, 0, w, 20, 2, SL_MID - between(2, 4), 0);
            osc(v, 1, w, 20, 2, SL_MID + between(2, 4), 0);
            osc(v, 2, SL_W_SAW, 16, 1, SL_MID, 0);
            v[SL_CUTOFF] = (uint8_t)between(15, 21);
            lfo(v, SL_CLFO, between(0, 5), SL_S_SINE, between(4, 8), 0);
            lfo(v, SL_OSC(1) + SL_VLFO, between(2, 8), SL_S_SINE, between(5, 10), 0);
        } else if (r == 1) {            /* shimmer */
            osc(v, 0, SL_W_TRI, 18, 4, SL_MID, 0);
            osc(v, 1, SL_W_SINE, 20, 6, SL_MID + between(1, 2), 0);
            osc(v, 2, SL_W_TRI, 10, 7, SL_MID - between(1, 2), 0);
            for (int i = 0; i < 3; i++)
                lfo(v, SL_OSC(i) + SL_VLFO, between(0, 8), rr(2), between(10, 18), 0);
            v[SL_CUTOFF] = (uint8_t)between(22, 27);
            v[SL_TRIM] = 12;
        } else {                        /* sync pad: a slow sweep of the slave */
            osc(v, 0, SL_W_TRI, 20, 2, SL_MID, 0);
            osc(v, 1, SL_W_SAW, 22, 4, SL_MID, 1);
            lfo(v, SL_OSC(1) + SL_PLFO, between(2, 6), SL_S_SINE, between(8, 14), 0);
            v[SL_CUTOFF] = 20;
        }
        break;
    case CAT_KEYS:
        env(v, SL_AENV, 0, between(10, 13), 0, 0);
        v[SL_ADECAY] = (uint8_t)between(12, 15);
        env(v, SL_FENV, 0, 10, 0, 0);
        v[SL_FDECAY] = (uint8_t)between(9, 12);
        v[SL_FILT_TYPE] = SL_F_LP12;
        v[SL_CUTOFF] = (uint8_t)between(16, 22);
        v[SL_ENVAMT] = (uint8_t)between(24, 28);
        if (r == 0) {                   /* sync pluck */
            osc(v, 0, SL_W_SAW, 24, 2, SL_MID, 0);
            osc(v, 1, SL_W_SAW, 20, between(2, 4), SL_MID, 1);
            osc(v, 2, SL_W_TRI, 16, 2, SL_MID, 1);
        } else if (r == 1) {            /* bell */
            osc(v, 0, SL_W_SINE, 24, 2, SL_MID, 0);
            osc(v, 1, SL_W_SINE, between(14, 20), between(5, 6), SL_MID, 0);
            osc(v, 2, SL_W_TRI, 6, 7, SL_MID, 0);
            v[SL_FOLD_ON] = 1;
            v[SL_FOLD_AMT] = (uint8_t)between(2, 6);
            v[SL_ADECAY] = (uint8_t)between(15, 18);
            v[SL_FILT_TYPE] = SL_F_LP6;
            v[SL_TRIM] = 10;
        } else {                        /* stab: a fifth */
            osc(v, 0, SL_W_SAW, 22, 2, SL_MID, 0);
            osc(v, 1, SL_W_SAW, 18, 3, SL_MID + 1, 0);
            v[SL_FILT_TYPE] = SL_F_LP24;
            v[SL_RES] = (uint8_t)between(14, 20);
        }
        break;
    default:                            /* FX */
        if (r == 0) {                   /* polyrhythm: tempo-synced gates */
            static const uint8_t DIVS[5] = {17, 12, 10, 6, 4};  /* 1/4, 1/8, 1/8T, 1/16, 1/16T */
            for (int i = 0; i < 3; i++) {
                osc(v, i, rr(5), 20, i == 1 ? 4 : 2, SL_MID + (i == 2 ? -4 : 0), 0);
                lfo(v, SL_OSC(i) + SL_VLFO, DIVS[rr(5)], chance(50) ? SL_S_SQUARE : SL_S_SINE, 31, 1);
            }
            v[SL_LFO_FREE] = 0;
            v[SL_AENV + SL_E_SUS] = 31;
        } else if (r == 1) {            /* siren */
            osc(v, 0, SL_W_SAW, 22, 2, SL_MID, 0);
            osc(v, 1, SL_W_SAW, 24, 4, SL_MID, 1);
            lfo(v, SL_OSC(1) + SL_PLFO, between(12, 22), chance(50) ? SL_S_TRI : SL_S_SAW, between(18, 27), 0);
        } else if (r == 2) {            /* sample-and-hold sync */
            osc(v, 0, SL_W_PULSE, 18, 2, SL_MID, 0);
            osc(v, 1, SL_W_SAW, 24, 4, SL_MID, 1);
            lfo(v, SL_OSC(1) + SL_PLFO, chance(50) ? 6 : 12, SL_S_RANDOM, between(14, 24), 1);
            v[SL_LFO_FREE] = 0;
        } else {                        /* drift */
            osc(v, 0, SL_W_TRI, 22, 2, SL_MID, 0);
            osc(v, 1, SL_W_SAW, 18, 4, SL_MID, 1);
            osc(v, 2, SL_W_SAW, 14, 6, SL_MID, 1);
            for (int i = 1; i < 3; i++)
                lfo(v, SL_OSC(i) + SL_PLFO, between(0, 8), rr(2), between(8, 16), 0);
            v[SL_FOLD_ON] = 1;
            lfo(v, SL_FLFO, between(0, 6), SL_S_SINE, between(8, 14), 0);
            v[SL_TRIM] = 6;
        }
        v[SL_CUTOFF] = (uint8_t)between(20, 26);
        v[SL_RES] = (uint8_t)between(6, 16);
        env(v, SL_AENV, between(0, 3), between(11, 15), between(20, 28), 0);
        break;
    }
}

/* the continuous fields Tweak and Mild move, and how far at most (Mild) */
static void mild(uint8_t *v)
{
    for (int i = 0; i < 3; i++) {
        uint8_t *o = v + SL_OSC(i);
        if (!o[SL_LEVEL])
            continue;
        nudge(v, SL_OSC(i) + SL_LEVEL, 3);
        nudge(v, SL_OSC(i) + SL_PW, 4);
        nudge(v, SL_OSC(i) + SL_FINE, 2);
        if (o[SL_PLFO + SL_L_DEPTH])
            nudge(v, SL_OSC(i) + SL_PLFO + SL_L_SPEED, 2);
        if (o[SL_VLFO + SL_L_DEPTH])
            nudge(v, SL_OSC(i) + SL_VLFO + SL_L_SPEED, 2);
    }
    nudge(v, SL_FENV + SL_E_ATT, 1);
    nudge(v, SL_FENV + SL_E_REL, 2);
    nudge(v, SL_FENV + SL_E_SUS, 3);
    nudge(v, SL_AENV + SL_E_REL, 2);
    nudge(v, SL_AENV + SL_E_SUS, 2);
    nudge(v, SL_CUTOFF, 2);
    nudge(v, SL_RES, 3);
    nudge(v, SL_ENVAMT, 2);
    nudge(v, SL_FDECAY, 2);
    nudge(v, SL_ADECAY, 2);
    if (v[SL_FOLD_ON])
        nudge(v, SL_FOLD_AMT, 2);
    if (v[SL_CLFO + SL_L_DEPTH])
        nudge(v, SL_CLFO + SL_L_SPEED, 2);
    if (v[SL_FLFO + SL_L_DEPTH])
        nudge(v, SL_FLFO + SL_L_SPEED, 2);
}

/* Wild: the preset taken further, and some of its structure drawn anew */
static void wild(uint8_t *v, int cat)
{
    mild(v);
    for (int i = 0; i < 3; i++) {
        uint8_t *o = v + SL_OSC(i);
        if (chance(40))
            o[SL_WAVE] = (uint8_t)(chance(5) ? SL_W_NOISE : rr(5));
        if (chance(30))
            o[SL_OCT] = (uint8_t)(i ? between(2, 6) : between(1, 2));
        if (chance(30))
            o[SL_PW] = (uint8_t)rr(32);
        if (i && chance(20))
            o[SL_LEVEL] = (uint8_t)(o[SL_LEVEL] ? 0 : between(12, 24));
        if (i && chance(15))
            o[SL_SYNC] ^= 1;
        if (chance(15))
            lfo(v, SL_OSC(i) + SL_PLFO, rr(32), rr(SL_NSHAPE), between(3, 12), 0);
        if (chance(15))
            lfo(v, SL_OSC(i) + SL_VLFO, rr(32), rr(SL_NSHAPE), between(8, 31), 0);
    }
    if (chance(30)) {
        v[SL_FOLD_ON] = 1;
        v[SL_FOLD_AMT] = (uint8_t)between(4, 20);
        v[SL_FOLD_SYM] = (uint8_t)between(8, 24);
        v[SL_FOLD_PUSH] = (uint8_t)between(0, 12);
    }
    if (chance(15))
        lfo(v, SL_FLFO, rr(20), rr(SL_NSHAPE), between(4, 16), 0);
    if (chance(15))
        lfo(v, SL_CLFO, rr(24), rr(SL_NSHAPE), between(4, 14), 0);
    if (chance(20))
        v[SL_FILT_TYPE] = (uint8_t)(cat == CAT_BASS ? between(SL_F_LP12, SL_F_LP24) : rr(SL_NFILT - 1));
    if (chance(40))
        v[SL_CUTOFF] = (uint8_t)between(cat == CAT_BASS ? 8 : 14, cat == CAT_BASS ? 20 : 28);
    if (chance(30))
        v[SL_RES] = (uint8_t)rr(25);
    nudge(v, SL_ENVAMT, 6);
    nudge(v, SL_FENV + SL_E_SUS, 6);
    nudge(v, SL_AENV + SL_E_SUS, 6);
    nudge(v, SL_ADECAY, 4);
    nudge(v, SL_FDECAY, 4);
}

/* Tweak: the sound as it is, moved by `amount` (1 light, 2 medium, 3 wild) */
static void tweak(uint8_t *v, int amount)
{
    static const uint8_t PCT[3] = {30, 50, 70}, STEP[3] = {2, 4, 8};
    int pct = PCT[amount - 1], d = STEP[amount - 1];
    for (int f = 0; f < SL_PRESET; f++) {
        int o = f < SL_FENV ? f % SL_OSC_N : -1;
        int lfo_at = f < SL_FENV ? (o >= SL_VLFO ? f - o + SL_VLFO : (o >= SL_PLFO ? f - o + SL_PLFO : -1))
                   : (f >= SL_CLFO && f < SL_FOLD_AMT ? (f < SL_FLFO ? SL_CLFO : SL_FLFO) : -1);
        if (sl_max(f) < 8)              /* a choice or a switch: Tweak's structure part */
            continue;
        if (lfo_at >= 0 && f != lfo_at + SL_L_DEPTH && !v[lfo_at + SL_L_DEPTH])
            continue;                   /* an LFO that's off: its rate doesn't matter */
        if (lfo_at >= 0 && f == lfo_at + SL_L_DEPTH && !v[f] && amount < 3)
            continue;                   /* only wild turns one on */
        if (o == SL_LEVEL && !v[f] && amount < 3)
            continue;                   /* only wild brings an oscillator in */
        if (chance(pct))
            nudge(v, f, d);
    }
    if (amount == 3) {
        for (int i = 0; i < 3; i++) {
            uint8_t *x = v + SL_OSC(i);
            if (chance(15))
                x[SL_WAVE] = (uint8_t)rr(5);
            if (chance(10))
                x[SL_OCT] = (uint8_t)(i ? between(2, 6) : between(1, 2));
            if (i && chance(10))
                x[SL_SYNC] ^= 1;
            if (chance(15))
                x[SL_PLFO + SL_L_SHAPE] = (uint8_t)rr(SL_NSHAPE);
        }
        if (chance(15))
            v[SL_FLFO + SL_L_SHAPE] = (uint8_t)rr(SL_NSHAPE);
    }
}

/* the rules that keep a sound playable */
static void tidy(uint8_t *v, int cat)
{
    sl_clean(v);
    if (v[SL_OSC(0) + SL_LEVEL] < 8)
        v[SL_OSC(0) + SL_LEVEL] = 8;
    if (cat == CAT_LEAD && v[SL_AENV + SL_E_SUS] < 10)
        v[SL_AENV + SL_E_SUS] = 10;
    if (v[SL_AENV + SL_E_ATT] > 20 && cat != CAT_PAD)
        v[SL_AENV + SL_E_ATT] = 20;
}

static void save_undo(int v)
{
    for (int b = 0; b < NB; b++)
        undo[v][b] = sy_cur[v][b];
    undo_ok |= 1u << v;
}

/* a press of Random (style 1-3, or 0: All) or Tweak (amount 1-3) for the voice */
static void change(int v, int style, int amount)
{
    uint8_t x[SL_NFIELD];
    rng ^= (uint32_t)fw_get_tick() * 2654435761u;
    if (!rng)
        rng = 0x9e3779b9u;
    rnd();
    save_undo(v);
    sl_unpack(sy_cur[v], x);
    int cat = category(x[SL_PRESET]);
    if (amount) {
        tweak(x, amount);
    } else {
        all_last = style == 0;
        if (!style)
            style = 1 + rr(3);
        uint8_t keep[SL_NFIELD];
        for (int f = 0; f < SL_NFIELD; f++)
            keep[f] = x[f];
        for (int f = 0; f < SL_NFIELD; f++)
            x[f] = PRESETS[x[SL_PRESET]][f];
        if (style == R_MILD)
            mild(x);
        else if (style == R_WILD)
            wild(x, cat);
        else
            recipe(x, cat);
        for (int f = SL_PRESET; f <= SL_ROOT; f++)
            x[f] = keep[f];
        for (int f = SL_P2_RES; f <= SL_P2_SYNC; f++)
            x[f] = SL_MID;
        x[SL_MUTATE] = keep[SL_MUTATE];
        x[SL_RSTYLE] = (uint8_t)style;
    }
    tidy(x, cat);
    sl_pack(x, sy_cur[v]);
    follow_reset(v);
    mark(v, 0);
}

static void do_undo(int v)
{
    if (!(undo_ok >> v & 1))
        return;
    undo_ok &= ~(1u << v);
    for (int b = 0; b < NB; b++)
        sy_cur[v][b] = undo[v][b];
    follow_reset(v);
    mark(v, 0);
}

/* ---- the configuration page ------------------------------------------------------------------ */

/* a knob's position, as a kit load sets it: its value, and its catch armed as configured */
static void set_knob(int v, int slot, uint32_t x)
{
    uint8_t *vs = vstate(v);
    FW_U16(vs + FW_VS_BASE + 2 * slot) = (uint16_t)(x << 4);
    FW_U16(vs + FW_VS_VALUE + 2 * slot) = (uint16_t)(x << 4);
    FW_U8(vs + FW_VS_CATCH + slot) = FW_U8(SYM_device_config + FW_DC_POT_CATCH);
}

/* the starter a hold on preset step i loads: 1-4, 5-8, 9-11, 12-13, 14-16 */
static int starter_of(int i)
{
    return i < 4 ? 0 : i < 8 ? 1 : i < 11 ? 2 : i < 13 ? 3 : 4;
}

/* a preset (or, held, its group's starter) for the voice */
static void use_preset(int v, int i, int starter)
{
    uint8_t x[SL_NFIELD], old[SL_NFIELD];
    sl_unpack(sy_cur[v], old);
    if (starter)
        save_undo(v);
    const uint8_t *src = starter ? STARTERS[starter_of(i)] : PRESETS[i];
    for (int f = 0; f < SL_NFIELD; f++)
        x[f] = src[f];
    x[SL_PRESET] = (uint8_t)i;
    x[SL_SCALE] = old[SL_SCALE];
    x[SL_ROOT] = old[SL_ROOT];
    x[SL_MUTATE] = old[SL_MUTATE];
    sl_pack(x, sy_cur[v]);
    set_knob(v, FW_SLOT_PARAM1, 2048);
    follow_reset(v);
    mark(v, 0);
}

static void set_value(int f, int x)
{
    if (sl_get(sy_cur[editing], f) != x) {
        sl_put(sy_cur[editing], f, x);
        mark((int)editing, F_KEEP);
    }
}

/* the first field of the cell on step key k (-1: not a cell) */
static int cell_of(int k)
{
    int row = k / 16, col = k % 16;
    if (row == 1)
        return col < 12 ? SL_OSC(col / 4) + 4 * (col % 4) : SL_FENV + 4 * (col - 12);
    if (row == 2 && (col == 10 || col == 11))
        return col == 10 ? SL_FILT_TYPE : SL_FOLD_AMT;
    return -1;
}

int pk_sy_page_key(const uint8_t *evt)
{
    if (evt[1] != FW_EV_DOWN && evt[1] != FW_EV_UP)
        return 0;                       /* not a key (the panel switches): stock */
    int k = evt[0], down = evt[1] == FW_EV_DOWN;
    if (k < 0x40) {                     /* steps (their releases too: the long press's) */
        int row = k / 16, col = k % 16, c = cell_of(k);
        if (c >= 0) {
            if (down) {
                cell1 = c + 1;
                cell_key = (uint32_t)k;
                turning = 0;
                anchor[0] = anchor[1] = anchor[2] = -1.0f;
            } else if ((uint32_t)k == cell_key) {
                cell1 = 0;
            }
            return 1;
        }
        if (row == 0 || (row == 2 && col >= 12 && col <= 14) || (row == 3 && col >= SL_NROOT)) {
            if (down) {                 /* a preset, Tweak or Random: a press (on release); held:
                                           the preset's starter, or undo */
                tweak_key = (uint32_t)k + 1;
                tweak_at = fw_get_tick();
                tweak_done = 0;
            } else if ((uint32_t)k + 1 == tweak_key) {
                if (!tweak_done) {
                    if (row == 0)
                        use_preset((int)editing, col, 0);
                    else if (row == 2)
                        change((int)editing, 0, col - 11);
                    else
                        change((int)editing, col == 15 ? 0 : col - 11, 0);
                }
                tweak_key = 0;
            }
            return 1;
        }
        if (!down)
            return 1;
        if (row == 2 && col < SL_NSCALE)
            set_value(SL_SCALE, col);
        else if (row == 2 && col == 15)
            set_value(SL_MUTATE, !sl_get(sy_cur[editing], SL_MUTATE));
        else if (row == 3 && col < SL_NROOT)
            set_value(SL_ROOT, col);
        return 1;
    }
    if (k == FW_KEY_SYNC && cell1) {    /* SYNC on a held cell: its switch */
        if (down)
            set_value(CELL + 3, !sl_get(sy_cur[editing], CELL + 3));
        return 1;
    }
    if (k == FW_KEY_SHIFT || k == FW_KEY_KIT || k == FW_KEY_MOD) {
        if (down)
            fw_set_mode(PK_PAGE_ENGINES);
        return 1;
    }
    return 0;                           /* the trigger keys play; other page buttons leave */
}

/* the LFO knobs (RATE, WAVE, MOD LVL): a held cell's first three values; else stock */
typedef void (*setter_t)(float value, uint8_t *lfo);

static void knob_poll(int i, float value, uint8_t *main, setter_t next)
{
    if (!active || !cell1) {
        next(value, main);
        return;
    }
    if (anchor[i] < 0.0f)
        anchor[i] = value;
    float d = value - anchor[i];
    if (d > TURN || d < -TURN)
        turning |= 1u << i;
    if (turning >> i & 1) {
        int m = sl_max(CELL + i);
        int x = (int)(value * (float)(m + 1));
        set_value(CELL + i, x > m ? m : (x < 0 ? 0 : x));
    }
    FW_U8(main + FW_LFO_CATCH) |= 1 << i;      /* the stock LFO keeps its value */
}

void PK_NEXT(pk_sy_rate)(float value, uint8_t *lfo);
void PK_NEXT(pk_sy_wave)(float value, uint8_t *lfo);
void PK_NEXT(pk_sy_level)(float value, uint8_t *lfo);
void pk_sy_rate(float value, uint8_t *main) { knob_poll(0, value, main, PK_NEXT(pk_sy_rate)); }
void pk_sy_wave(float value, uint8_t *main) { knob_poll(1, value, main, PK_NEXT(pk_sy_wave)); }
void pk_sy_level(float value, uint8_t *main) { knob_poll(2, value, main, PK_NEXT(pk_sy_level)); }

void pk_sy_page_enter(void)
{
    make_ready();
    if (pk_vm_settings_voice < 4)
        editing = pk_vm_settings_voice;
    active = 1;
    cell1 = 0;
    tweak_key = 0;
}

/* is the cell starting at field c in use (the medium LEDs) */
static int in_use(const uint8_t *x, int c)
{
    if (c < SL_FENV) {
        int o = c % SL_OSC_N, base = c - o;
        if (o == SL_PLFO || o == SL_VLFO)
            return x[c + SL_L_DEPTH] > 0 && x[base + SL_LEVEL] > 0;
        return x[base + SL_LEVEL] > 0;
    }
    switch (c) {
    case SL_FENV: case SL_FILT_TYPE: return x[SL_FILT_ON];
    case SL_CLFO: return x[SL_CLFO + SL_L_DEPTH] > 0 && x[SL_FILT_ON];
    case SL_FLFO: return x[SL_FLFO + SL_L_DEPTH] > 0 && x[SL_FOLD_ON];
    case SL_FOLD_AMT: return x[SL_FOLD_ON];
    default: return 1;
    }
}

void pk_sy_page_draw(void)
{
    uint8_t x[SL_NFIELD];
    sl_unpack(sy_cur[editing], x);
    if (tweak_key && !tweak_done && fw_get_tick() - tweak_at >= UNDO_MS) {
        tweak_done = 1;                 /* held: a preset step its starter; Tweak, Random undo */
        if (tweak_key <= 16)
            use_preset((int)editing, (int)tweak_key - 1, 1);
        else
            do_undo((int)editing);
        sl_unpack(sy_cur[editing], x);
    }
    for (int k = 0; k < 0x40; k++) {
        int row = k / 16, col = k % 16, c = cell_of(k), lv = LED_DIM;
        if (c >= 0)
            lv = c == CELL ? LED_ON : (in_use(x, c) ? LED_MID : LED_DIM);
        else if (row == 0)
            lv = col == x[SL_PRESET] || (uint32_t)k + 1 == tweak_key ? (col == x[SL_PRESET] && x[SL_RSTYLE] ? LED_MID : LED_ON) : LED_DIM;
        else if (row == 2 && col < SL_NSCALE)
            lv = col == x[SL_SCALE] ? LED_ON : LED_DIM;
        else if (row == 2 && col < 15)
            lv = (uint32_t)k + 1 == tweak_key ? LED_ON : LED_DIM;
        else if (row == 2)
            lv = x[SL_MUTATE] ? LED_ON : LED_DIM;
        else if (col < SL_NROOT)
            lv = col == x[SL_ROOT] ? LED_ON : LED_DIM;
        else if ((uint32_t)k + 1 == tweak_key)
            lv = LED_ON;
        else
            lv = x[SL_RSTYLE] && (col == 15 ? all_last : !all_last && col - 11 == x[SL_RSTYLE]) ? LED_ON : LED_DIM;
        fw_set_led(k, (uint8_t)lv);
    }
}

void pk_sy_page_leave(void)
{
    active = 0;
    cell1 = 0;
    int bank = FW_U8(SYM_bank_number), kit = current_kit();
    if (kit < 0 || kit >= NKITS)
        return;
    for (int v = 0; v < 4; v++)
        for (int b = 0; b < NB; b++)
            if (kit_snd[kit][v][b] != sy_cur[v][b]) {
                pk_store_save_kit(bank, kit);
                return;
            }
}
