/* 3 Oh, M4 side: each kit's sounds, the configuration page, the line generator, New sound, the
 * tempo and DRIVE.
 *
 * A voice's sound is 23 small values, packed in 11 bytes (oh_sound.h): the sound the M7 plays
 * and the voice's line settings, as the kit keeps them (the M4 host's kit sidecar,
 * KITS/PP.KIX, record 3OHS: 4 x 11 bytes); a kit without one has the default presets (1, 2,
 * 10, 14 on voices 1-4). Presets, the page and New sound all write these values.
 *
 * The line: the generator writes it into the voice's own track of the pattern playing (the
 * steps' on / off, note and velocity; their other settings and the locks stay), as a hand edit
 * would: it plays from FIRST to LAST and is saved with the pattern. The velocity says how a step
 * plays (oh_sound.h): 127 plain (as a step turned on by hand), 104 accent, 80 slide into the next
 * note, 48 both. Slides only where the next step that plays is within two steps; a slide into
 * the same note is a tie.
 *
 * PARAM2: ALGO chooses its job, and it sets three values (envelope amount, accent,
 * distortion). The kit keeps only the knob's position, so this core follows the M7's rule on
 * the knob's own position (no locks) and keeps the values: a new job, or a value loaded, holds
 * until the knob moves 2% of its travel; then the knob sets it. Loading a kit sends the sounds
 * held (the M7 doesn't follow the knob) until the kit's positions have reached the M7, then sends
 * them again, free (as Synth Lab).
 *
 * The configuration page: a long press on a voice's 3 Oh step on voice-map's engine page.
 *   row 1   presets 1-16 (a press, even on the one chosen, gives the preset's sound and sets TUNE
 *           (cutoff) and PARAM1 (resonance) to 12 o'clock; the line settings stay)
 *   row 2   scale 1-12, the line's octave 13-16 (C1-C4)
 *   row 3   root C-B (1-12), distortion type 13-16: Muffler, Filter drive, Overdrive, Distortion
 *   row 4   Notes 2, 3, 5, 7 (1-4), Range a fifth, an octave, an octave and a half, two octaves
 *           (5-8), Accent, Slide, Octave jumps, Rests (9-12: each press off, low, high), New line,
 *           Keep rhythm, Vary, New sound (13-16: on release; held 0.5 s: undo the last of them)
 * Bright: chosen (densities: dim off, medium low, bright high). The trigger keys play; SHIFT,
 * KIT or MOD goes back to the engine page; another page button leaves as stock. Leaving keeps a
 * change in the kit.
 *
 * The tempo (the gate length) and each voice's DRIVE (the level) go to the M7 from the main
 * loop when they change, and again every 250 ms ("three-oh-live"). */
#include <stdint.h>
#include "pk_host.h"
#include "fw_m4_seq.h"
#include "pk_vm.h"                  /* pk_vm_engine */
#include "pk_oh_engine.h"           /* PK_OH_ENGINE: 3 Oh's engine number */
#include "oh_sound.h"
#include "oh_presets.h"

#define NKITS           64
#define NB              OH_PACKED
#define LED_ON          0x80
#define LED_MID         0x38
#define LED_DIM         0x10
#define MOVED           82          /* knob travel (0-4095, 2%), as the M7's */
#define UNDO_MS         500         /* a Random step held this long: undo */
#define POS1            1
#define POS3            2
#define F_HELD          0x10        /* message flags, as the M7's */
#define F_KEEP          0x20
enum { A_LINE, A_RHYTHM, A_VARY, A_SOUND, A_UNDO };      /* row 4, 13-16; held: undo */

extern uint8_t pk_vm_settings_voice;

static const uint8_t DEFAULT_PRESET[4] = {0, 1, 9, 13};

typedef struct {
    uint16_t ref2;                  /* PARAM2 when the job or the value was set (0-4095) */
    uint8_t role, armed, running, pad;
} follow_t;

/* (every variable whole words, word-aligned: the M4 host clears its RAM a word at a time) */
#define WORDS __attribute__((aligned(4)))
static uint8_t kit_snd[NKITS][4][NB] WORDS;     /* each kit's sounds (MAIN): 2,560 B */
static uint8_t oh_cur[4][NB] WORDS;             /* the sounds playing */
static follow_t fol[4] WORDS;
/* undo: the last New line / Keep rhythm / Vary (the steps it wrote) or New sound, of one voice */
typedef struct {
    uint8_t *blk;                   /* the track block written (line) */
    uint8_t step[16][3];            /* its steps before: on, note, velocity */
    uint8_t snd[NB];                /* the sound before (New sound) */
    uint8_t kind, voice;            /* 0 none, 1 line, 2 sound */
    uint8_t pad[3];
} undo_t;
static undo_t undo WORDS;
static uint32_t pending;                        /* an action from the page, done in the main loop: action + 1 */
static uint32_t editing, ready, active;         /* the page's voice; oh_cur filled; the page open */
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
/* the page: a Random step held (its key + 1), since when, done (undo taken); the last style */
static uint32_t act_key, act_at, act_done;

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
    return algo == POS1 ? 0 : (algo == POS3 ? 2 : 1);   /* envelope, accent, distortion */
}

static void preset_into(uint8_t *packed, int i)
{
    oh_pack(PRESETS[i & (OH_NPRESET - 1)], packed);
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
        defaults(oh_cur);
    }
}

/* ---- to the M7 ------------------------------------------------------------------------ */

static void send(int v, int flags)
{
    make_ready();
    uint8_t msg[NB + 1];
    msg[0] = (uint8_t)(v | flags);
    for (int i = 0; i < NB; i++)
        msg[1 + i] = oh_cur[v][i];
    pk_send_m7(PK_STREAM_THREE_OH, PK_MSG_THREE_OH, msg, sizeof msg);
}

static void send_all(int voices, int flags)
{
    for (int v = 0; v < 4; v++)
        if (voices >> v & 1)
            send(v, flags);
}

/* A sound changed by the page or Random: sent from the main loop (flush), never from where it
 * changed (a send from an interrupt can corrupt the stream the triggers use). */
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

/* PARAM2 of a voice running 3 Oh, by the M7's rule: its value in the sound (5 bits, 16 = 12
 * o'clock) */
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
        int q = OH_MID + (x2 * 30 + 2047) / 4095 - 15;
        q = q < 0 ? 0 : (q > 31 ? 31 : q);
        if (oh_get(oh_cur[v], OH_P2_ENV + r) != q)
            oh_put(oh_cur[v], OH_P2_ENV + r, q);
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
        int on = pk_vm_engine(v) == PK_OH_ENGINE;
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

void PK_NEXT(pk_oh_kit_load)(int kit);

void pk_oh_kit_load(int kit)
{
    PK_NEXT(pk_oh_kit_load)(kit);
    if (kit < 0 || kit >= NKITS)
        return;
    if (!(kit_ok[kit / 32] >> (kit % 32) & 1))
        defaults(kit_snd[kit]);
    for (int v = 0; v < 4; v++) {
        for (int b = 0; b < NB; b++)
            oh_cur[v][b] = kit_snd[kit][v][b];
        follow_reset(v);
    }
    undo.kind = 0;
    pending = 0;
    dirty = 0;                                  /* the page's changes: the kit's sounds instead */
    ready = 1;
    holding = 1;
    hold_from = hold_matched = (uint16_t)fw_get_tick();
    send_all(15, F_HELD);
}

uint32_t pk_oh_kit_save(int bank, int kit, uint8_t *buf, uint32_t max)
{
    (void)bank;
    poll();
    if (kit >= 0 && kit < NKITS) {
        for (int v = 0; v < 4; v++)
            for (int b = 0; b < NB; b++)
                kit_snd[kit][v][b] = oh_cur[v][b];
        kit_ok[kit / 32] |= 1u << (kit % 32);
    }
    if (max < 4 * NB)
        return 0;
    for (int v = 0; v < 4; v++)
        for (int b = 0; b < NB; b++)
            buf[v * NB + b] = oh_cur[v][b];
    return 4 * NB;
}

void pk_oh_kit_restore(int bank, int kit, const uint8_t *rec, uint32_t len)
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
        uint8_t x[OH_NFIELD];
        oh_unpack(rec + v * NB, x);
        oh_clean(x);
        oh_pack(x, kit_snd[kit][v]);
    }
}

/* ---- the main loop: PARAM2, the hold, tempo and DRIVE ---------------------------------------- */

void PK_NEXT(pk_oh_main_loop)(void);

static void do_action(int v, int a);

void pk_oh_main_loop(void)
{
    PK_NEXT(pk_oh_main_loop)();
    if (pending) {
        uint32_t p = pending - 1;
        pending = 0;
        do_action((int)(p >> 4), (int)(p & 15));
    }
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
        pk_send_m7(PK_STREAM_THREE_OH_LIVE, PK_MSG_THREE_OH_LIVE, msg, sizeof msg);
    }
}

/* ---- random numbers --------------------------------------------------------------------- */

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void seed(void)
{
    rng ^= (uint32_t)fw_get_tick() * 2654435761u;
    if (!rng)
        rng = 0x9e3779b9u;
    rnd();
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

/* ---- the line ------------------------------------------------------------------------------- */

/* the scales (as Akords', with Hungarian Minor before Chromatic) */
static const int8_t SC_MAJOR[] = {0, 2, 4, 5, 7, 9, 11};
static const int8_t SC_MINOR[] = {0, 2, 3, 5, 7, 8, 10};
static const int8_t SC_MIN_PENT[] = {0, 3, 5, 7, 10};
static const int8_t SC_HARM_MINOR[] = {0, 2, 3, 5, 7, 8, 11};
static const int8_t SC_DORIAN[] = {0, 2, 3, 5, 7, 9, 10};
static const int8_t SC_PHRYGIAN[] = {0, 1, 3, 5, 7, 8, 10};
static const int8_t SC_MAJ_PENT[] = {0, 2, 4, 7, 9};
static const int8_t SC_MIXOLYDIAN[] = {0, 2, 4, 5, 7, 9, 10};
static const int8_t SC_BLUES[] = {0, 3, 5, 6, 7, 10};
static const int8_t SC_PHRYG_DOM[] = {0, 1, 4, 5, 7, 8, 10};
static const int8_t SC_HUNGARIAN[] = {0, 2, 3, 6, 7, 8, 11};
static const int8_t SC_CHROMATIC[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const struct { const int8_t *n; uint8_t len; } SCALES[OH_NSCALE] = {
    {SC_MAJOR, 7}, {SC_MINOR, 7}, {SC_MIN_PENT, 5}, {SC_HARM_MINOR, 7}, {SC_DORIAN, 7},
    {SC_PHRYGIAN, 7}, {SC_MAJ_PENT, 5}, {SC_MIXOLYDIAN, 7}, {SC_BLUES, 6}, {SC_PHRYG_DOM, 7},
    {SC_HUNGARIAN, 7}, {SC_CHROMATIC, 12},
};

static const uint8_t NOTES[4] = {2, 3, 5, 7};          /* how many pitches */
static const uint8_t RANGE[4] = {7, 12, 19, 24};       /* their spread above the root, semitones */
static const uint8_t P_ACC[3] = {0, 18, 32};           /* % of notes, by density */
static const uint8_t P_SLIDE[3] = {0, 14, 28};
static const uint8_t P_OCT[3] = {0, 12, 25};
static const uint8_t P_ON[3][3] = {                     /* % of steps playing: on the beat, the 8th, the 16th */
    {100, 100, 100}, {95, 75, 55}, {85, 50, 25},
};
#define P_ROOT          35          /* % of notes on the root */
#define P_TIE           30          /* % of slides into the same note (a tie) */
#define BASE_C1         24          /* the line's octave 0: C1 (MIDI) */
#define VARY_STEPS      3

static uint8_t *track_block(int v)
{
    return *(uint8_t **)(vstate(v) + FW_VS_BLOCK);
}

static int vel_of(int acc, int slide)
{
    return acc ? (slide ? OH_VEL_BOTH : OH_VEL_ACCENT) : (slide ? OH_VEL_SLIDE : OH_VEL_NORMAL);
}

/* the next step after s that plays, within first-last (cyclic); its distance in steps, 0 none */
static int next_on(const uint8_t *on, int s, int first, int last)
{
    int n = last - first + 1;
    for (int d = 1; d < n; d++) {
        int t = first + (s - first + d) % n;
        if (on[t])
            return d;
    }
    return 0;
}

/* New line (A_LINE: rhythm too), Keep rhythm (A_RHYTHM: the steps that are on), Vary (A_VARY: a
 * few of the steps that play, drawn again), for voice v's track, from its line settings */
static void make_line(int v, int how)
{
    uint8_t *blk = track_block(v);
    if (!blk)
        return;
    uint8_t x[OH_NFIELD], on[16], note[16], vel[16];
    oh_unpack(oh_cur[v], x);
    int first = blk[FW_BLK_FIRST], last = blk[FW_BLK_LAST];
    if (first > 15)
        first = 0;
    if (last > 15 || last < first)
        last = 15;
    seed();
    /* undo: the steps as they are */
    undo.kind = 1;
    undo.voice = (uint8_t)v;
    undo.blk = blk;
    int any = 0;
    for (int s = 0; s < 16; s++) {
        const uint8_t *r = blk + FW_STEP_SIZE * s;
        undo.step[s][0] = r[0];
        undo.step[s][1] = r[6];
        undo.step[s][2] = r[7];
        on[s] = r[0] != 0;
        note[s] = r[6];
        vel[s] = r[7];
        if (on[s] && s >= first && s <= last)
            any = 1;
    }
    /* the rhythm: drawn by Rests (New line, or nothing on to keep) */
    if (how == A_LINE || !any) {
        const uint8_t *p = P_ON[x[OH_D_REST]];
        for (int s = 0; s < 16; s++)
            on[s] = (uint8_t)chance(p[s % 4 == 0 ? 0 : (s % 2 == 0 ? 1 : 2)]);
        on[first] = 1;
        how = A_LINE;
    }
    /* the pitches: the root and NOTES - 1 others of the scale, within RANGE above it */
    const int8_t *sc = SCALES[x[OH_SCALE]].n;
    int len = SCALES[x[OH_SCALE]].len, range = RANGE[x[OH_RANGE]], want = NOTES[x[OH_NOTES]];
    int8_t cand[32], pool[8];
    int nc = 0, np = 1;
    for (int o = 0; o <= 24 && nc < 32; o += 12)
        for (int i = 0; i < len && nc < 32; i++) {
            int semi = o + sc[i];
            if (semi > 0 && semi <= range)
                cand[nc++] = (int8_t)semi;
        }
    pool[0] = 0;
    while (np < want && nc > 0) {      /* distinct ones, at random */
        int i = rr(nc);
        pool[np++] = cand[i];
        cand[i] = cand[--nc];
    }
    int base = BASE_C1 + 12 * x[OH_OCTAVE] + x[OH_ROOT];
    /* Vary: which steps */
    uint8_t redo[16];
    for (int s = 0; s < 16; s++)
        redo[s] = how != A_VARY;
    if (how == A_VARY) {
        int playing[16], n = 0;
        for (int s = first; s <= last; s++)
            if (on[s])
                playing[n++] = s;
        for (int i = 0; i < VARY_STEPS && n > 0; i++) {
            int j = rr(n);
            redo[playing[j]] = 1;
            playing[j] = playing[--n];
        }
    }
    /* each step: its pitch, octave jump, accent, slide (a tie: the next note the same) */
    int tie = -1;
    for (int s = 0; s < 16; s++) {
        if (!on[s])
            continue;
        if (!redo[s]) {                 /* Vary leaves it: a tie can't carry past it */
            tie = -1;
            continue;
        }
        int n = tie >= 0 ? tie : base + (np > 1 && !chance(P_ROOT) ? pool[1 + rr(np - 1)] : 0);
        if (tie < 0 && chance(P_OCT[x[OH_D_OCT]]))
            n += chance(67) ? 12 : -12;
        n = n < 0 ? 0 : (n > 127 ? 127 : n);
        tie = -1;
        int acc = chance(P_ACC[x[OH_D_ACC]] + (s % 4 == 0 ? 6 : 0));
        int d = s >= first && s <= last ? next_on(on, s, first, last) : 0;
        int slide = d > 0 && d <= 2 && chance(P_SLIDE[x[OH_D_SLIDE]]);
        if (slide && chance(P_TIE) && s + d <= last)
            tie = n;
        note[s] = (uint8_t)n;
        vel[s] = (uint8_t)vel_of(acc, slide);
    }
    /* write the steps (with the sequencer held off: it reads them from its interrupt) */
    uint32_t pm = pk_irq_off();
    for (int s = 0; s < 16; s++) {
        uint8_t *r = blk + FW_STEP_SIZE * s;
        r[0] = on[s] ? (r[0] ? r[0] : 1) : 0;
        if (on[s]) {
            r[6] = note[s];
            r[7] = vel[s];
        }
    }
    pk_irq_restore(pm);
}

/* ---- New sound (on the voice's unpacked values, from its preset) --------------------------- */

static void put(uint8_t *v, int f, int x)
{
    int m = oh_max(f);
    v[f] = (uint8_t)(x < 0 ? 0 : (x > m ? m : x));
}

static void nudge(uint8_t *v, int f, int d)
{
    put(v, f, (int)v[f] + between(-d, d));
}

/* close to the preset: each amount moved a little */
static void mild(uint8_t *v)
{
    nudge(v, OH_CUTOFF, 3);
    nudge(v, OH_RES, 3);
    nudge(v, OH_ENVMOD, 3);
    nudge(v, OH_ACCENT, 3);
    if (v[OH_DIST])
        nudge(v, OH_DIST, 3);
    nudge(v, OH_TONE, 1);
    nudge(v, OH_SLIDE, 2);
}

/* further: the filter, accent and distortion drawn anew */
static void wild(uint8_t *v)
{
    put(v, OH_CUTOFF, between(2, 20));
    put(v, OH_RES, between(8, 31));
    put(v, OH_ENVMOD, between(6, 31));
    put(v, OH_ACCENT, between(6, 31));
    if (chance(70)) {
        v[OH_DTYPE] = (uint8_t)rr(OH_NDTYPE);
        put(v, OH_DIST, between(6, 31));
    } else {
        v[OH_DIST] = 0;
    }
    put(v, OH_TONE, between(2, 11));
    put(v, OH_SLIDE, between(6, 22));
}

/* a coherent acid sound of one of four kinds */
static void recipe(uint8_t *v)
{
    switch (rr(4)) {
    case 0:                             /* clean squelch: open resonance, big sweep */
        put(v, OH_CUTOFF, between(5, 12));
        put(v, OH_RES, between(22, 30));
        put(v, OH_ENVMOD, between(18, 30));
        put(v, OH_ACCENT, between(18, 30));
        v[OH_DIST] = (uint8_t)between(0, 6);
        v[OH_DTYPE] = OH_D_MUFFLER;
        v[OH_TONE] = (uint8_t)between(5, 8);
        break;
    case 1:                             /* deep roller: low and round, a little dirt */
        put(v, OH_CUTOFF, between(2, 7));
        put(v, OH_RES, between(8, 18));
        put(v, OH_ENVMOD, between(6, 14));
        put(v, OH_ACCENT, between(8, 16));
        v[OH_DIST] = (uint8_t)between(6, 16);
        v[OH_DTYPE] = chance(50) ? OH_D_MUFFLER : OH_D_FILTER;
        v[OH_TONE] = (uint8_t)between(3, 6);
        break;
    case 2:                             /* screaming lead: open, driven, bright */
        put(v, OH_CUTOFF, between(12, 20));
        put(v, OH_RES, between(18, 28));
        put(v, OH_ENVMOD, between(14, 24));
        put(v, OH_ACCENT, between(14, 26));
        v[OH_DIST] = (uint8_t)between(14, 24);
        v[OH_DTYPE] = OH_D_OVERDRIVE;
        v[OH_TONE] = (uint8_t)between(7, 10);
        break;
    default:                            /* rave: everything up, hard distortion */
        put(v, OH_CUTOFF, between(8, 14));
        put(v, OH_RES, between(24, 31));
        put(v, OH_ENVMOD, between(22, 31));
        put(v, OH_ACCENT, between(22, 31));
        v[OH_DIST] = (uint8_t)between(20, 31);
        v[OH_DTYPE] = chance(50) ? OH_D_DIST : OH_D_FILTER;
        v[OH_TONE] = (uint8_t)between(6, 10);
        break;
    }
    put(v, OH_SLIDE, between(8, 16));
}

/* the preset's sound redrawn (close, further or a recipe, at random); the line settings stay */
static void new_sound(int v)
{
    uint8_t x[OH_NFIELD], keep[OH_NFIELD];
    seed();
    undo.kind = 2;
    undo.voice = (uint8_t)v;
    for (int b = 0; b < NB; b++)
        undo.snd[b] = oh_cur[v][b];
    oh_unpack(oh_cur[v], keep);
    for (int f = 0; f < OH_NFIELD; f++)
        x[f] = PRESETS[keep[OH_PRESET]][f];
    int style = rr(3);
    if (style == 0)
        mild(x);
    else if (style == 1)
        wild(x);
    else
        recipe(x);
    for (int f = OH_PRESET; f <= OH_D_REST; f++)
        x[f] = keep[f];
    for (int f = OH_P2_ENV; f <= OH_P2_DIST; f++)
        x[f] = OH_MID;
    x[OH_RSTYLE] = 1;
    oh_clean(x);
    oh_pack(x, oh_cur[v]);
    follow_reset(v);
    mark(v, 0);
}

static void do_undo(int v)
{
    if (!undo.kind || undo.voice != v)
        return;
    if (undo.kind == 1) {
        if (undo.blk == track_block(v)) {           /* still the same pattern's track */
            uint32_t pm = pk_irq_off();
            for (int s = 0; s < 16; s++) {
                uint8_t *r = undo.blk + FW_STEP_SIZE * s;
                r[0] = undo.step[s][0];
                r[6] = undo.step[s][1];
                r[7] = undo.step[s][2];
            }
            pk_irq_restore(pm);
        }
    } else {
        for (int b = 0; b < NB; b++)
            oh_cur[v][b] = undo.snd[b];
        follow_reset(v);
        mark(v, 0);
    }
    undo.kind = 0;
}

/* a page action, from the main loop */
static void do_action(int v, int a)
{
    if (a == A_UNDO)
        do_undo(v);
    else if (a == A_SOUND)
        new_sound(v);
    else
        make_line(v, a);
}

static void ask(int v, int a)
{
    uint32_t pm = pk_irq_off();
    pending = 1u + ((uint32_t)v << 4 | (uint32_t)a);
    pk_irq_restore(pm);
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

/* a preset's sound; the line settings stay; cutoff and resonance to 12 o'clock */
static void use_preset(int v, int i)
{
    uint8_t x[OH_NFIELD], old[OH_NFIELD];
    oh_unpack(oh_cur[v], old);
    for (int f = 0; f < OH_NFIELD; f++)
        x[f] = PRESETS[i][f];
    x[OH_PRESET] = (uint8_t)i;
    for (int f = OH_SCALE; f <= OH_D_REST; f++)
        x[f] = old[f];
    oh_pack(x, oh_cur[v]);
    set_knob(v, FW_SLOT_TUNE, 2048);
    set_knob(v, FW_SLOT_PARAM1, 2048);
    follow_reset(v);
    mark(v, 0);
}

static void set_value(int f, int x)
{
    if (oh_get(oh_cur[editing], f) != x) {
        oh_put(oh_cur[editing], f, x);
        mark((int)editing, F_KEEP);
    }
}

int pk_oh_page_key(const uint8_t *evt)
{
    if (evt[1] != FW_EV_DOWN && evt[1] != FW_EV_UP)
        return 0;                       /* not a key (the panel switches): stock */
    int k = evt[0], down = evt[1] == FW_EV_DOWN;
    if (k < 0x40) {                     /* steps (their releases too: the long press's) */
        int row = k / 16, col = k % 16;
        if (row == 3 && col >= 12) {    /* an action: a press (on release); held: undo */
            if (down) {
                act_key = (uint32_t)k + 1;
                act_at = fw_get_tick();
                act_done = 0;
            } else if ((uint32_t)k + 1 == act_key) {
                if (!act_done)
                    ask((int)editing, col - 12);
                act_key = 0;
            }
            return 1;
        }
        if (!down)
            return 1;
        if (row == 0)
            use_preset((int)editing, col);
        else if (row == 1)
            set_value(col < OH_NSCALE ? OH_SCALE : OH_OCTAVE, col < OH_NSCALE ? col : col - 12);
        else if (row == 2)
            set_value(col < OH_NROOT ? OH_ROOT : OH_DTYPE, col < OH_NROOT ? col : col - 12);
        else if (col < 4)
            set_value(OH_NOTES, col);
        else if (col < 8)
            set_value(OH_RANGE, col - 4);
        else                            /* a density: off, low, high, off ... */
            set_value(OH_D_ACC + col - 8, (oh_get(oh_cur[editing], OH_D_ACC + col - 8) + 1) % 3);
        return 1;
    }
    if (k == FW_KEY_SHIFT || k == FW_KEY_KIT || k == FW_KEY_MOD) {
        if (down)
            fw_set_mode(PK_PAGE_ENGINES);
        return 1;
    }
    return 0;                           /* the trigger keys play; other page buttons leave */
}

void pk_oh_page_enter(void)
{
    make_ready();
    if (pk_vm_settings_voice < 4)
        editing = pk_vm_settings_voice;
    active = 1;
    act_key = 0;
}

void pk_oh_page_draw(void)
{
    static const uint8_t DENS[3] = {LED_DIM, LED_MID, LED_ON};
    uint8_t x[OH_NFIELD];
    if (act_key && !act_done && fw_get_tick() - act_at >= UNDO_MS) {
        act_done = 1;                   /* an action held: undo */
        ask((int)editing, A_UNDO);
    }
    oh_unpack(oh_cur[editing], x);
    for (int k = 0; k < 0x40; k++) {
        int row = k / 16, col = k % 16, lv = LED_DIM;
        if (row == 0)
            lv = col == x[OH_PRESET] ? (x[OH_RSTYLE] ? LED_MID : LED_ON) : LED_DIM;
        else if (row == 1)
            lv = (col < OH_NSCALE ? col == x[OH_SCALE] : col - 12 == x[OH_OCTAVE]) ? LED_ON : LED_DIM;
        else if (row == 2)
            lv = (col < OH_NROOT ? col == x[OH_ROOT] : col - 12 == x[OH_DTYPE]) ? LED_ON : LED_DIM;
        else if (col < 4)
            lv = col == x[OH_NOTES] ? LED_ON : LED_DIM;
        else if (col < 8)
            lv = col - 4 == x[OH_RANGE] ? LED_ON : LED_DIM;
        else if (col < 12)
            lv = DENS[x[OH_D_ACC + col - 8]];
        else
            lv = (uint32_t)k + 1 == act_key ? LED_ON : LED_DIM;
        fw_set_led(k, (uint8_t)lv);
    }
}

void pk_oh_page_leave(void)
{
    active = 0;
    int bank = FW_U8(SYM_bank_number), kit = current_kit();
    if (kit < 0 || kit >= NKITS)
        return;
    for (int v = 0; v < 4; v++)
        for (int b = 0; b < NB; b++)
            if (kit_snd[kit][v][b] != oh_cur[v][b]) {
                pk_store_save_kit(bank, kit);
                return;
            }
}
