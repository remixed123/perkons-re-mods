/* Kick Lab, M4 side: each kit's Kick Lab settings, the settings page, the tempo.
 *
 * A voice's settings are 6 bytes, the same as in the "kick-lab" message to the M7:
 *   [0] preset 0-15 | drive type << 4 | drive tone << 6
 *   [1] drive amount 0-8 | delay time or division 0-13 << 4
 *   [2] feedback 0-9 | delay tone 0-5 << 4 | sync << 7
 *   [3] [4] [5] attack, delay mix, drive mix (0-255): PARAM2's three values
 * They're kept with the kit (the M4 host's kit sidecar, KITS/PP.KIX, record KLAB: 4 x 6 bytes);
 * a kit without one has preset 1 (Basic) on every voice.
 *
 * PARAM2: ALGO chooses its job, and it sets three values. The kit keeps only the knob's
 * position, so this core follows the M7's rule on the knob's own position (no locks) and
 * keeps the values: a new job, or a value loaded, holds until the knob moves 2% of its travel;
 * then the knob sets it. Loading a kit sends the settings held (the M7 doesn't follow the
 * knob) until the kit's positions have reached the M7 (stock applies them at the track's next
 * note while the sequencer plays), then sends them again, free.
 *
 * The settings page: a long press on a voice's Kick Lab step on voice-map's engine page opens
 * it for that voice (pk_vm_settings_voice).
 *   row 1   presets 1-16 (a press, even on the one chosen, gives the preset's whole sound:
 *           TUNE, DECAY and PARAM1 are set as a kit load sets them, the knobs catch them)
 *   row 2   delay time or division (steps 1-14), sync off (15), on (16)
 *   row 3   delay feedback (1-10), delay tone (11-16)
 *   row 4   drive amount (1-9), drive tone (10-12), drive type soft, hard, crush, fold (13-16)
 * Bright: chosen. The trigger keys play the voices; SHIFT, KIT or MOD goes back to the engine
 * page; another page button leaves as stock. Leaving keeps a change in the kit and writes the
 * kit's sidecar.
 *
 * The tempo (for the delay's sync) goes to the M7 from the main loop when it changes (message
 * "kick-lab-tempo"), and again every 250 ms. */
#include <stdint.h>
#include "pk_host.h"
#include "fw_m4_seq.h"
#include "pk_vm.h"                  /* pk_vm_engine */
#include "pk_kl_engine.h"           /* PK_KL_ENGINE: Kick Lab's engine number */

#define NKITS           64
#define NB              6           /* a voice's settings, in bytes */
#define NPRESET         16
#define NTIME           14
#define NFB             10
#define NDTONE          6
#define NDAMT           9
#define LED_ON          0x80
#define LED_DIM         0x10
#define MOVED           82          /* knob travel (0-4095, 2%), as the M7's */
#define END_ZONE_LO     82          /* PARAM2's ends (2%), as the M7's */
#define POS1            1           /* the switch values of panel positions 1, 3 */
#define POS3            2
enum { ROLE_ATTACK, ROLE_DELAY_MIX, ROLE_DRIVE_MIX };
#define VAL             3           /* the values' first byte */

extern uint8_t pk_vm_settings_voice;

/* the presets: TUNE, DECAY, PARAM1 (0-4095), then their settings (generated with the M7's
 * preset table, from the same values) */
typedef struct {
    uint16_t tune, decay, p1;
    uint8_t set[NB];
} preset_t;

static const preset_t PRESETS[NPRESET] = {
    {1630, 2059, 2048, {0x40, 0x82, 0x23,   0,   0,   0}},   /*  1 Basic */
    {1799, 1686, 2244, {0x41, 0x81, 0x23,  38,   0, 128}},   /*  2 Tight Techno */
    {1954, 1953, 2244, {0x42, 0x82, 0x23,   0,   0, 204}},   /*  3 9x Punch */
    {1630, 2504, 1851, {0x43, 0x81, 0x23, 128,   0, 153}},   /*  4 8x Punch */
    {1630, 2153, 1654, {0x44, 0x81, 0x23,  25,   0, 128}},   /*  5 Warm House */
    {1954, 1831, 2048, {0x45, 0x82, 0x23,   0,   0, 153}},   /*  6 Boom Bap */
    {1799, 1882, 1654, {0x46, 0x82, 0x23,   0,   0,   0}},   /*  7 Acoustic */
    {1799, 2382, 2441, {0x57, 0x85, 0x23,   0,   0, 230}},   /*  8 Hardstyle Rave */
    {2236, 2614, 2525, {0x48, 0x82, 0x23,   0,   0, 143}},   /*  9 Drop */
    {1442, 3255,  672, {0x49, 0x81, 0x23,   0,   0, 128}},   /* 10 8x Melodic Sub */
    {1828, 3304,  470, {0x4a, 0x82, 0x23,  37,   0,   0}},   /* 11 8x Sub Boom */
    {1699, 3162, 1654, {0x4b, 0x82, 0x23,   0,   0,   0}},   /* 12 Sustained */
    {1315, 3339, 2334, {0xbc, 0x83, 0x23,   0,   0,  89}},   /* 13 Hammer */
    {1799, 1686, 2244, {0x6d, 0x53, 0x33,  38,  42, 153}},   /* 14 Lo-Fi Crush */
    {1630, 2153, 1654, {0x4e, 0x71, 0xa6,  25,  69, 128}},   /* 15 Dub Echo */
    {1799, 1882, 1654, {0x4f, 0x72, 0x14,   0,  55,   0}},   /* 16 Spring Thud */
};

/* PARAM2's state per voice, by the M7's rule */
typedef struct {
    uint16_t ref2;                  /* the knob when the job or the value was set (0-4095) */
    uint8_t role, armed, running;
} follow_t;

/* (every variable whole words, word-aligned: the M4 host records each by its size, and clears
 * a word at a time) */
#define WORDS __attribute__((aligned(4)))
static uint8_t kit_set[NKITS][4][NB] WORDS;     /* each kit's settings (MAIN) */
static uint8_t cur[4][NB] WORDS;                /* the settings playing */
static follow_t fol[4] WORDS;
static uint32_t editing;                        /* the voice the page edits */
static uint32_t ready;                          /* cur holds settings (a kit's, or Basic) */
static uint32_t kit_ok[NKITS / 32];             /* kits (bits) whose table entry has been filled */
/* a kit loaded: the M7 holds its values until the positions arrive (ms, low 16 bits) */
#define HOLD_SETTLED_MS 20          /* the playing values have matched the kit's this long */
#define HOLD_MAX_MS     2000        /* or this long (locks or an LFO keep them apart) */
static uint32_t hold_from, hold_matched;       /* (low 16 bits) */
static uint32_t holding;
/* the tempo */
#define TEMPO_EVERY_MS  5
#define TEMPO_RESEND_MS 250
static uint32_t tempo_sent, tempo_checked, tempo_at;     /* (low 16 bits) */

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
    return algo == POS1 ? ROLE_ATTACK : (algo == POS3 ? ROLE_DRIVE_MIX : ROLE_DELAY_MIX);
}

/* PARAM2's position as the value it sets (0-255), the ends of the travel as the ends */
static uint8_t to_val(uint32_t x)
{
    int32_t v = ((int32_t)x - END_ZONE_LO) * 255 / (4095 - 2 * END_ZONE_LO);
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void defaults(uint8_t to[4][NB]);

/* before any kit has loaded: Basic on every voice */
static void make_ready(void)
{
    if (!ready) {
        ready = 1;
        defaults(cur);
    }
}

/* ---- to the M7 ------------------------------------------------------------------------ */

/* the settings, applied to `voices` (bits), held (the M7 doesn't follow PARAM2) for `held` */
static void send(uint8_t voices, uint8_t held)
{
    make_ready();
    uint8_t msg[4 * NB + 2];
    for (int v = 0; v < 4; v++)
        for (int i = 0; i < NB; i++)
            msg[v * NB + i] = cur[v][i];
    msg[4 * NB] = voices;
    msg[4 * NB + 1] = held;
    pk_send_m7(PK_STREAM_KICK_LAB, PK_MSG_KICK_LAB, msg, sizeof msg);
}

/* a voice's PARAM2 holds its values until the knob moves (as the M7 does on a message) */
static void follow_reset(int v)
{
    fol[v].role = role_now(v);
    fol[v].ref2 = (uint16_t)base12(v, FW_SLOT_PARAM2);
    fol[v].armed = 0;
}

/* PARAM2 of a voice running Kick Lab, by the M7's rule */
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
    if (f->armed)
        cur[v][VAL + f->role] = to_val((uint32_t)x2);
}

/* the voice's playing values are the kit's */
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
    uint8_t started = 0;
    for (int v = 0; v < 4; v++) {
        int on = pk_vm_engine(v) == PK_KL_ENGINE;
        if (on && !fol[v].running) {
            follow_reset(v);                    /* (the engine is built at the next note) */
            started |= (uint8_t)(1u << v);
        } else if (on && !holding) {
            follow(v);
        }
        fol[v].running = (uint8_t)on;
    }
    if (started)
        send(started, holding ? started : 0);
    if (holding) {
        uint16_t now = (uint16_t)fw_get_tick();
        for (int v = 0; v < 4; v++)
            if (fol[v].running && !arrived(v))
                hold_matched = now;
        if ((uint16_t)(now - hold_matched) >= HOLD_SETTLED_MS || (uint16_t)(now - hold_from) >= HOLD_MAX_MS) {
            holding = 0;
            for (int v = 0; v < 4; v++)
                follow_reset(v);
            send(15, 0);
        }
    }
}

/* ---- presets and kits ----------------------------------------------------------------- */

/* a knob's position, as a kit load sets it: its value, and its catch armed as configured */
static void set_knob(int v, int slot, uint32_t x)
{
    uint8_t *vs = vstate(v);
    FW_U16(vs + FW_VS_BASE + 2 * slot) = (uint16_t)(x << 4);
    FW_U16(vs + FW_VS_VALUE + 2 * slot) = (uint16_t)(x << 4);
    FW_U8(vs + FW_VS_CATCH + slot) = FW_U8(SYM_device_config + FW_DC_POT_CATCH);
}

/* a preset pressed: its whole sound */
static void use_preset(int v, int i)
{
    const preset_t *p = &PRESETS[i];
    for (int b = 0; b < NB; b++)
        cur[v][b] = p->set[b];
    set_knob(v, FW_SLOT_TUNE, p->tune);
    set_knob(v, FW_SLOT_DECAY, p->decay);
    set_knob(v, FW_SLOT_PARAM1, p->p1);
    follow_reset(v);
    send((uint8_t)(1u << v), 0);
}

static void defaults(uint8_t to[4][NB])
{
    for (int v = 0; v < 4; v++)
        for (int b = 0; b < NB; b++)
            to[v][b] = PRESETS[0].set[b];
}

/* kit loads (chained with the other mods' kit hooks): the kit's settings, held at first */
void PK_NEXT(pk_kl_kit_load)(int kit);

void pk_kl_kit_load(int kit)
{
    PK_NEXT(pk_kl_kit_load)(kit);
    if (kit < 0 || kit >= NKITS)
        return;
    if (!(kit_ok[kit / 32] >> (kit % 32) & 1))
        defaults(kit_set[kit]);                 /* (a bank load fills every kit; before one: Basic) */
    for (int v = 0; v < 4; v++) {
        for (int b = 0; b < NB; b++)
            cur[v][b] = kit_set[kit][v][b];
        follow_reset(v);
    }
    ready = 1;
    holding = 1;
    hold_from = hold_matched = (uint16_t)fw_get_tick();
    send(15, 15);
}

/* the host's kit sidecar, record "KLAB": 4 x the 6 bytes */
uint32_t pk_kl_kit_save(int bank, int kit, uint8_t *buf, uint32_t max)
{
    (void)bank;
    poll();
    if (kit >= 0 && kit < NKITS) {
        for (int v = 0; v < 4; v++)
            for (int b = 0; b < NB; b++)
                kit_set[kit][v][b] = cur[v][b];
        kit_ok[kit / 32] |= 1u << (kit % 32);
    }
    if (max < 4 * NB)
        return 0;
    for (int v = 0; v < 4; v++)
        for (int b = 0; b < NB; b++)
            buf[v * NB + b] = cur[v][b];
    return 4 * NB;
}

/* a setting's value checked (a record from a later version can't send nonsense) */
static void clean(uint8_t s[NB])
{
    if ((s[1] & 15) >= NDAMT || s[1] >> 4 >= NTIME || (s[2] & 15) >= NFB || (s[2] >> 4 & 7) >= NDTONE
        || s[0] >> 6 >= 3) {
        for (int b = 0; b < VAL; b++)
            s[b] = PRESETS[s[0] & 15].set[b];
    }
}

void pk_kl_kit_restore(int bank, int kit, const uint8_t *rec, uint32_t len)
{
    (void)bank;
    if (kit < 0 || kit >= NKITS)
        return;
    kit_ok[kit / 32] |= 1u << (kit % 32);
    if (len != 4 * NB) {                        /* none (or not ours): Basic */
        defaults(kit_set[kit]);
        return;
    }
    for (int v = 0; v < 4; v++) {
        for (int b = 0; b < NB; b++)
            kit_set[kit][v][b] = rec[v * NB + b];
        clean(kit_set[kit][v]);
    }
}

/* ---- the main loop: PARAM2, the hold, the tempo -------------------------------------------- */

void PK_NEXT(pk_kl_main_loop)(void);

void pk_kl_main_loop(void)
{
    PK_NEXT(pk_kl_main_loop)();
    uint16_t now = (uint16_t)fw_get_tick();
    if ((uint16_t)(now - tempo_checked) < TEMPO_EVERY_MS)
        return;
    tempo_checked = now;
    poll();
    float bpm = FW_CLOCK_BPM;
    uint16_t t = bpm > 0.0f && bpm < 999.0f ? (uint16_t)(bpm * 10.0f + 0.5f) : 1200;
    if (t != tempo_sent || (uint16_t)(now - tempo_at) >= TEMPO_RESEND_MS) {
        tempo_sent = t;
        tempo_at = now;
        uint8_t msg[2] = {(uint8_t)t, (uint8_t)(t >> 8)};
        pk_send_m7(PK_STREAM_KICK_LAB_TEMPO, PK_MSG_KICK_LAB_TEMPO, msg, sizeof msg);
    }
}

/* ---- the settings page ------------------------------------------------------------------ */

/* a setting changed on the page: to the M7 if it's new */
static void set_field(int b, int shift, int mask, int x)
{
    uint8_t *s = &cur[editing][b];
    uint8_t n = (uint8_t)((*s & ~(mask << shift)) | (x & mask) << shift);
    if (n != *s) {
        *s = n;
        follow_reset(editing);
        send((uint8_t)(1u << editing), 0);
    }
}

int pk_kl_page_key(const uint8_t *evt)
{
    if (evt[1] != FW_EV_DOWN && evt[1] != FW_EV_UP)
        return 0;                       /* not a key (the panel switches): stock */
    int k = evt[0], down = evt[1] == FW_EV_DOWN;
    if (k < 0x40) {                     /* steps (their releases too: the long press's) */
        int row = k / 16, col = k % 16;
        if (!down)
            return 1;
        if (row == 0)                   /* a preset, even the one chosen: all its sound */
            use_preset(editing, col);
        else if (row == 1)
            col < NTIME ? set_field(1, 4, 15, col) : set_field(2, 7, 1, col - NTIME);
        else if (row == 2)
            col < NFB ? set_field(2, 0, 15, col) : set_field(2, 4, 7, col - NFB);
        else
            col < NDAMT ? set_field(1, 0, 15, col)
                        : (col < NDAMT + 3 ? set_field(0, 6, 3, col - NDAMT) : set_field(0, 4, 3, col - NDAMT - 3));
        return 1;
    }
    if (k == FW_KEY_SHIFT || k == FW_KEY_KIT || k == FW_KEY_MOD) {
        if (down)
            fw_set_mode(PK_PAGE_ENGINES);
        return 1;                       /* (a release here would go to MAIN) */
    }
    return 0;                           /* the trigger keys play; other page buttons leave */
}

/* entering the page (from the engine page's long press): that voice */
void pk_kl_page_enter(void)
{
    make_ready();
    if (pk_vm_settings_voice < 4)
        editing = pk_vm_settings_voice;
}

void pk_kl_page_draw(void)
{
    const uint8_t *s = cur[editing];
    int on[4][2] = {
        {s[0] & 15, -1},
        {s[1] >> 4, NTIME + (s[2] >> 7)},
        {s[2] & 15, NFB + (s[2] >> 4 & 7)},
        {s[1] & 15, -1},
    };
    for (int k = 0; k < 0x40; k++) {
        int row = k / 16, col = k % 16;
        int lit = col == on[row][0] || col == on[row][1];
        if (row == 3 && col >= NDAMT)
            lit = col < NDAMT + 3 ? col - NDAMT == s[0] >> 6 : col - NDAMT - 3 == (s[0] >> 4 & 3);
        fw_set_led(k, lit ? LED_ON : LED_DIM);
    }
}

/* leaving the page: a change goes into the kit and its sidecar */
void pk_kl_page_leave(void)
{
    int bank = FW_U8(SYM_bank_number), kit = current_kit();
    if (kit < 0 || kit >= NKITS)
        return;
    for (int v = 0; v < 4; v++)
        for (int b = 0; b < VAL; b++)
            if (kit_set[kit][v][b] != cur[v][b]) {
                pk_store_save_kit(bank, kit);
                return;
            }
}
