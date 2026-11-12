/* Akords, M4 side: each kit's Akords settings, sounds and options, the settings page and
 * Random.
 *
 * Every voice has five settings: preset, chord type, scale, root and progression; options
 * (Strum up / down, Spread, Bass drop, Mutate); and a sound (ak_sound.h: its preset's own, or
 * one Random made). They're kept with the kit (the M4 host's kit sidecar, KITS/PP.KIX, record
 * AKRD: 4 x 26 bytes, below; older 4 x 9, 4 x 5 and 4 x 4 ones, the last without progressions
 * and with 14 chord types, are read too: their presets' sounds, no options); a kit without
 * one has all at 0 and the presets' sounds. Loading a kit, and every change on the page,
 * sends all four voices' settings to the M7 in order with the triggers (message "akords"),
 * and a voice's sound and options (message "akords-voice"), so the next chord plays them.
 *
 * The sound: a kit keeps the knob and switch positions, but the values the voice plays
 * aren't all on them. PARAM2 sets three values, and keeps the two ALGO isn't on; a preset
 * press gives the preset's values until a knob moves; the filter is the preset's own until
 * MODE moves. The M7 can't send anything back, so this core follows the same rules on the
 * knobs' own positions (no locks) and saves the values with the kit. Loading a kit sends
 * them (message "akords-sound"), held until the kit's positions have reached the M7: while
 * the sequencer plays, the firmware only applies them at the track's next note. A voice saved without Akords, or in a kit from before
 * they were saved, plays its knobs, with the preset's values for the rest.
 *
 * Progressions: each voice's chord moves through its progression once per cycle of its
 * track (back on the track's first step), from the first chord when the sequencer starts,
 * at a pattern switch and when stopped. Checked just before the stock trigger sending (both
 * of its tail calls), the scale-degree offset goes to the M7 when it changes (message
 * "akords-prog"), in order before that step's triggers.
 *
 * The settings page: a long press on a voice's Akords step on voice-map's engine page
 * opens it for that voice (pk_vm_settings_voice).
 *   row 1   presets (16; bright: chosen, medium after Random). A press, even on the one
 *           chosen, gives the voice all of the preset's sound again (the M7 drops the knobs'
 *           tweaks until they move)
 *   row 2   chord type (steps 1-10), progression (11 off, 12-16)
 *   row 3   scale (1-11); options Strum up, Strum down, Spread, Bass drop, Mutate (12-16, on /
 *           off; one strum at a time)
 *   row 4   root (1-12); Random Mild, Wild, Recipe, All (13-16: on the release; held 0.5 s:
 *           undo the last Random). Random changes the sound only, from the preset (a sound of
 *           its kind), and gives the knobs back to the sound's own values, as a preset press
 * The trigger keys play the voices; SHIFT, KIT or MOD goes back to the engine page; another
 * page button leaves as stock. Leaving keeps a change in the kit and writes its sidecar. */
#include <stdint.h>
#include "pk_host.h"
#include "fw_m4_seq.h"
#include "pk_vm.h"                  /* pk_vm_engine */
#include "pk_ak_engine.h"           /* PK_AK_ENGINE: Akords' engine number */
#include "ak_sound.h"                /* a voice's sound (both cores) */

#define NKITS           64
#define NSET            5           /* preset, chord type, scale, root, progression */
#define NSEND           4           /* the first four go to the M7 */
#define PROG            4           /* the progression's index in a voice's settings */
#define NCHORD          10
#define NPROG           6           /* 0 off, 1-5 */
static const uint8_t NPOS[NSET] = {16, NCHORD, 11, 12, NPROG};
_Static_assert(16 <= 16 && NCHORD <= 16 && 11 <= 16 && 12 <= 16 && NPROG <= 256,
               "each setting fits its bits in the kit table (pack)");

/* the progressions: scale degrees above TUNE's, a chord per track cycle (written for the
 * minor scales: i = 0, III = 2, iv = 3, v = 4, VI = 5, VII = 6) */
static const int8_t PROGRESSION[NPROG][4] = {
    {0, 0, 0, 0},                   /* off */
    {0, 5, 2, 6},                   /* trance: i VI III VII */
    {0, 5, 6, 4},                   /* epic: i VI VII v */
    {0, 4, 5, 3},                   /* progressive house, melodic techno: i v VI iv */
    {0, 5, 0, 5},                   /* techno: i VI */
    {0, 3, 0, 3},                   /* deep house: i iv */
};
#define LED_ON          0x80
#define LED_DIM         0x10

extern uint8_t pk_vm_settings_voice;

/* a voice's values the knobs set, as on the M7 (0-255; 128: 12 o'clock, the preset's own).
 * The cutoff isn't kept: PARAM1 has no other job, so it's on its knob or the preset's. */
enum { V_CUTOFF, V_RES, V_ENV, V_ATTACK, NVAL };
#define NKEPT           3           /* resonance, env amount, attack */
#define VAL_OWN         128
#define MOVED           82          /* knob travel (0-4095, 2%), as the M7's */
#define POS1            1           /* the switch values of panel positions 1, 3 */
#define POS3            2
/* a kit's table entry, and the record's flags byte */
#define KEPT            0x08        /* the values below are kept */
#define KEPT_MODE       0x10        /* the filter is MODE's, not the preset's own */
#define KEPT_CUT_OWN    0x20        /* the cutoff is the preset's, not PARAM1's */
#define KEPT_FLAGS      (KEPT | KEPT_MODE | KEPT_CUT_OWN)
/* the message's flags (the M7's) */
#define SOUND_APPLY     0x40
#define SOUND_HOLD      0x20
#define SOUND_OWN       0x10
#define SOUND_ARM2      0x08
#define SOUND_ARM1      0x04

typedef struct {
    uint32_t ref1 : 12;             /* the knobs when they got their jobs (0-4095) */
    uint32_t ref2 : 12;
    uint32_t role : 2;              /* what PARAM2 sets */
    uint32_t armed : 2;             /* bits: this knob follows (1 PARAM1, 2 PARAM2) */
    uint32_t own : 1;               /* the filter is the preset's own */
    uint32_t kept : 1;              /* the values come from a kit, or it's run Akords */
    uint32_t running : 1;           /* it ran Akords at the last look */
    uint8_t mode_ref;               /* MODE when the preset was chosen */
    uint8_t val[NKEPT];
} sound_t;

#define PACKED          (3 + NKEPT) /* a voice packed: 4 + 4 + 4 + 4 + 3 bits, the flags, the values */
#define RECORD          (NSET + 1 + NKEPT)      /* a voice in the 4 x 9 record */
/* a voice's options and sound, after them: the options byte (AK_O_*, and the two below),
 * then the sound (ak_sound.h) */
#define E_OPTS          PACKED
#define E_SOUND         (PACKED + 1)
#define ENTRY           (PACKED + 1 + AK_PACKED)
#define O_SOUND         0x20        /* the entry has a sound (else: its preset's) */
#define O_RANDOM        0x40        /* the sound isn't the preset's own (Random) */
#define O_KEPT          (AK_O_ALL | O_RANDOM)
#define RECORD2         (RECORD + 1 + AK_PACKED)    /* a voice in the 4 x 26 record */
#define WORDS           __attribute__((aligned(4)))
static uint8_t kit_set[NKITS][4][ENTRY] WORDS;  /* each kit's settings and sound, per voice (MAIN) */
_Static_assert(sizeof kit_set % 4 == 0, "whole words");
static sound_t snd[4] WORDS;                    /* the knobs' values playing */
/* a kit loaded: the M7 holds its sound until the positions arrive (ms, low 16 bits) */
#define HOLD_SETTLED_MS 20          /* the playing values have matched the kit's this long */
#define HOLD_MAX_MS     2000        /* or this long (locks or an LFO keep them apart) */
static uint16_t hold_from WORDS, hold_matched;
static uint32_t holding;
static uint8_t cur[4][NSET] WORDS;              /* the settings playing */
static uint32_t editing;                        /* the voice the page edits */
static uint8_t voice_snd[4][AK_PACKED] WORDS;   /* each voice's sound playing */
static uint8_t opt[4] WORDS;                    /* its options (AK_O_*, O_RANDOM) */
static uint8_t undo_snd[4][AK_PACKED] WORDS;    /* each voice's sound before its last Random */
static uint8_t undo_opt[4] WORDS;
static uint32_t undo_ok;                        /* voices (bits) with an undo */
static uint32_t ready;                          /* voice_snd filled */
static uint32_t rng;
static uint32_t key_held, key_at, key_done;     /* a Random step held (its key + 1), since (ms), undone */

/* the settings, then the voices (bits) whose preset was just pressed: they take all of
 * the preset's own settings again (the knobs' tweaks too) */
static void send_prog(int force);
static void prog_stopped(void);

static void send(uint8_t pressed)
{
    uint8_t msg[4 * NSEND + 1];
    for (int v = 0; v < 4; v++)
        for (int i = 0; i < NSEND; i++)
            msg[v * NSEND + i] = cur[v][i];
    msg[4 * NSEND] = pressed;
    pk_send_m7(PK_STREAM_AKORDS, PK_MSG_AKORDS, msg, sizeof msg);
    send_prog(0);
}

/* the voice's sound and options to the M7 (message "akords-voice") */
static void send_voice(int v)
{
    uint8_t msg[2 + AK_PACKED];
    msg[0] = (uint8_t)v;
    for (int i = 0; i < AK_PACKED; i++)
        msg[1 + i] = voice_snd[v][i];
    msg[1 + AK_PACKED] = (uint8_t)((opt[v] & AK_O_ALL) | (opt[v] & O_RANDOM ? 0 : AK_O_OWN));
    pk_send_m7(PK_STREAM_AKORDS_VOICE, PK_MSG_AKORDS_VOICE, msg, sizeof msg);
}

/* a voice's sound: preset i's own */
static void preset_sound(int v, int i)
{
    ak_pack(AK_PRESETS[i < AK_NPRESET ? i : 0], voice_snd[v]);
    opt[v] &= (uint8_t)~O_RANDOM;
}

/* before the first kit load: the presets' sounds */
static void make_ready(void)
{
    if (!ready) {
        ready = 1;
        for (int v = 0; v < 4; v++)
            preset_sound(v, cur[v][0]);
    }
}

static int current_kit(void)
{
    return *(volatile uint16_t *)(uintptr_t)SYM_current_kit;
}

/* a kit's table entry: preset | chord << 4, scale | root << 4, progression | flags, values */
static void pack_settings(uint8_t to[PACKED], const uint8_t from[NSET], uint8_t flags, const uint8_t val[NKEPT])
{
    to[0] = (uint8_t)(from[0] | from[1] << 4);
    to[1] = (uint8_t)(from[2] | from[3] << 4);
    to[2] = (uint8_t)(from[4] | flags);
    for (int i = 0; i < NKEPT; i++)
        to[3 + i] = val[i];
}

static void unpack_settings(uint8_t to[NSET], const uint8_t from[PACKED])
{
    to[0] = from[0] & 15;
    to[1] = from[0] >> 4;
    to[2] = from[1] & 15;
    to[3] = from[1] >> 4;
    to[4] = from[2] & 7;
}

/* the kit's entries hold what the page sets now: settings, options, sounds (a knob turned
 * alone doesn't write the card on leaving the page) */
static int same(const uint8_t a[4][ENTRY])
{
    for (int v = 0; v < 4; v++) {
        uint8_t s[NSET];
        unpack_settings(s, a[v]);
        for (int i = 0; i < NSET; i++)
            if (s[i] != cur[v][i])
                return 0;
        if ((a[v][E_OPTS] & (O_KEPT | O_SOUND)) != ((opt[v] & O_KEPT) | O_SOUND))
            return 0;
        for (int i = 0; i < AK_PACKED; i++)
            if (a[v][E_SOUND + i] != voice_snd[v][i])
                return 0;
    }
    return 1;
}

/* a kit entry's options and sound into the voice playing (none: the preset's) */
static void entry_sound(int v, const uint8_t *e)
{
    if (e[E_OPTS] & O_SOUND) {
        for (int i = 0; i < AK_PACKED; i++)
            voice_snd[v][i] = e[E_SOUND + i];
        opt[v] = e[E_OPTS] & O_KEPT;
    } else {
        opt[v] = 0;
        preset_sound(v, cur[v][0]);
    }
}

static void entry_put_sound(uint8_t *e, int v)
{
    e[E_OPTS] = (uint8_t)((opt[v] & O_KEPT) | O_SOUND);
    for (int i = 0; i < AK_PACKED; i++)
        e[E_SOUND + i] = voice_snd[v][i];
}

/* ---- the sound ------------------------------------------------------------------------ */

static uint32_t base12(int v, int slot)
{
    return FW_U16(fw_voice_state(v) + FW_VS_BASE + 2 * slot) >> 4;
}

/* what PARAM2 sets, for ALGO's position */
static uint8_t role_now(int v)
{
    int algo = FW_U16(fw_voice_state(v) + FW_VS_BASE + 2 * FW_SLOT_ALGO);
    return algo == POS1 ? V_RES : (algo == POS3 ? V_ATTACK : V_ENV);
}

static uint8_t mode_now(int v)
{
    return (uint8_t)FW_U16(fw_voice_state(v) + FW_VS_BASE + 2 * FW_SLOT_MODE);
}

static uint8_t to_val(uint32_t x)
{
    return (uint8_t)((x * 255u + 2047u) / 4095u);
}

/* the knobs of a voice running Akords, by the M7's rules */
static void sound_follow(int v)
{
    sound_t *s = &snd[v];
    int x1 = (int)base12(v, FW_SLOT_PARAM1), x2 = (int)base12(v, FW_SLOT_PARAM2);
    uint8_t r = role_now(v);
    if (r != s->role) {                 /* a new job: keep the value until the knob moves */
        s->role = r;
        s->armed &= 1;
        s->ref2 = (uint32_t)x2;
    }
    int d1 = x1 - (int)s->ref1, d2 = x2 - (int)s->ref2;
    if (d1 > MOVED || d1 < -MOVED)
        s->armed |= 1;
    if (d2 > MOVED || d2 < -MOVED)
        s->armed |= 2;
    if (s->armed & 2)
        s->val[s->role - V_RES] = to_val((uint32_t)x2);
    if (mode_now(v) != s->mode_ref)
        s->own = 0;
}

/* a kit's or a fresh voice's values take over, the knobs where they are now */
static void sound_set(int v, const uint8_t *e)
{
    sound_t *s = &snd[v];
    s->role = role_now(v);
    s->mode_ref = mode_now(v);
    s->ref1 = base12(v, FW_SLOT_PARAM1);
    s->ref2 = base12(v, FW_SLOT_PARAM2);
    s->kept = e && e[2] & KEPT;
    s->own = !(s->kept && e[2] & KEPT_MODE);
    for (int i = 0; i < NKEPT; i++)
        s->val[i] = s->kept ? e[3 + i] : VAL_OWN;
    if (!s->kept) {                     /* no values: the knobs are the settings */
        s->armed = 3;
        sound_follow(v);
    } else {                            /* a value on its knob just follows it */
        int d = (int)to_val(s->ref2) - (int)s->val[s->role - V_RES];
        s->armed = (e[2] & KEPT_CUT_OWN ? 0 : 1) | (d >= -1 && d <= 1 ? 2 : 0);
    }
}

/* the voices (bits) to the M7 */
static void sound_send(uint8_t voices, uint8_t hold)
{
    uint8_t msg[4 * (1 + NVAL)];
    for (int v = 0; v < 4; v++) {
        const sound_t *s = &snd[v];
        msg[v * 5] = voices >> v & 1 ? (uint8_t)(SOUND_APPLY | hold | s->role | (s->own ? SOUND_OWN : 0)
                                                 | (s->armed & 1 ? SOUND_ARM1 : 0)
                                                 | (s->armed & 2 ? SOUND_ARM2 : 0)) : 0;
        msg[v * 5 + 1] = s->armed & 1 ? to_val(base12(v, FW_SLOT_PARAM1)) : VAL_OWN;
        for (int i = 0; i < NKEPT; i++)
            msg[v * 5 + 2 + i] = s->val[i];
    }
    pk_send_m7(PK_STREAM_AKORDS_SOUND, PK_MSG_AKORDS_SOUND, msg, sizeof msg);
}

/* the voice's playing values are the kit's */
static int arrived(int v)
{
    static const uint8_t SLOTS[4] = {FW_SLOT_PARAM1, FW_SLOT_PARAM2, FW_SLOT_ALGO, FW_SLOT_MODE};
    const uint8_t *vs = fw_voice_state(v);
    for (int i = 0; i < 4; i++)
        if (FW_U16(vs + FW_VS_VALUE + 2 * SLOTS[i]) != FW_U16(vs + FW_VS_BASE + 2 * SLOTS[i]))
            return 0;
    return 1;
}

/* the M7 holding a kit's sound: let it go once the positions have arrived */
static void hold_poll(void)
{
    uint16_t now = (uint16_t)fw_get_tick();
    for (int v = 0; v < 4; v++)
        if (snd[v].running && !arrived(v))
            hold_matched = now;
    if ((uint16_t)(now - hold_matched) >= HOLD_SETTLED_MS || (uint16_t)(now - hold_from) >= HOLD_MAX_MS) {
        holding = 0;
        sound_send(15, 0);
    }
}

/* each voice running Akords follows its knobs; one that has just started running it gets
 * its values again (voice-map builds a new engine at the next note) */
static void sound_poll(void)
{
    uint8_t started = 0;
    for (int v = 0; v < 4; v++) {
        sound_t *s = &snd[v];
        int on = pk_vm_engine(v) == PK_AK_ENGINE;
        if (on && !s->running) {
            if (!s->kept) {
                sound_set(v, 0);
                s->kept = 1;
            }
            started |= (uint8_t)(1u << v);
        } else if (on) {
            sound_follow(v);
        }
        s->running = (uint8_t)on;
    }
    if (started)
        sound_send(started, 0);
    if (holding)
        hold_poll();
}

/* a preset pressed: all its own values, its own filter; each knob takes over when it moves */
static void sound_preset(int v)
{
    uint8_t e[PACKED] = {0, 0, KEPT, VAL_OWN, VAL_OWN, VAL_OWN};
    sound_set(v, e);
    snd[v].armed = 0;
}


/* kit loads (chained with the other mods' kit hooks): the kit's settings */
void PK_NEXT(pk_ak_kit_load)(int kit);

void pk_ak_kit_load(int kit)
{
    PK_NEXT(pk_ak_kit_load)(kit);
    if (kit < 0 || kit >= NKITS)
        return;
    ready = 1;
    undo_ok = 0;
    for (int v = 0; v < 4; v++) {
        unpack_settings(cur[v], kit_set[kit][v]);
        sound_set(v, kit_set[kit][v]);
        entry_sound(v, kit_set[kit][v]);
    }
    send(0);
    for (int v = 0; v < 4; v++)
        send_voice(v);
    holding = 1;
    hold_from = hold_matched = (uint16_t)fw_get_tick();
    sound_send(15, SOUND_HOLD);
}

/* the host's kit sidecar, record "AKRD": [voice][preset, chord, scale, root, progression,
 * flags, resonance, env amount, attack, options, the sound (16 bytes)]; none = all 0 and
 * the presets' sounds. A voice not running Akords keeps its settings, options and sound
 * but not the knobs' values. */
uint32_t pk_ak_kit_save(int bank, int kit, uint8_t *buf, uint32_t max)
{
    (void)bank;
    static const uint8_t none[NKEPT];
    uint32_t n = 4 * RECORD2, any = 0;
    make_ready();
    sound_poll();
    for (int v = 0; v < 4; v++) {
        const sound_t *s = &snd[v];
        uint8_t flags = s->running ? (uint8_t)(KEPT | (s->own ? 0 : KEPT_MODE)
                                               | (s->armed & 1 ? 0 : KEPT_CUT_OWN)) : 0;
        const uint8_t *val = s->running ? s->val : none;
        pack_settings(kit_set[kit][v], cur[v], flags, val);
        entry_put_sound(kit_set[kit][v], v);
        if (max >= n) {
            uint8_t *r = buf + v * RECORD2;
            for (int i = 0; i < NSET; i++)
                any |= r[i] = cur[v][i];
            any |= r[NSET] = flags;
            for (int i = 0; i < NKEPT; i++)
                r[NSET + 1 + i] = val[i];
            any |= r[RECORD] = opt[v] & O_KEPT;         /* a preset's own sound: no record needed */
            for (int i = 0; i < AK_PACKED; i++)
                r[RECORD + 1 + i] = voice_snd[v][i];
        }
    }
    return max >= n && any ? n : 0;
}

/* the 14 chord types of a 4 x 4 record: Minor 7 and Sus2 (plain, smooth) are gone; they
 * play Diatonic 7th and Diatonic */
static const uint8_t OLD_CHORD[14] = {0, 1, 2, 3, 4, 5, 2, 3, 0, 1, 6, 7, 8, 9};

void pk_ak_kit_restore(int bank, int kit, const uint8_t *rec, uint32_t len)
{
    (void)bank;
    int n = len == 4 * RECORD2 ? RECORD2 : len == 4 * RECORD ? RECORD
          : (len == 4 * NSET ? NSET : (len == 4 * 4 ? 4 : 0));
    static const uint8_t none[NKEPT];
    for (int v = 0; v < 4; v++) {
        uint8_t set[NSET];
        for (int i = 0; i < NSET; i++) {
            uint8_t x = i < n ? rec[v * n + i] : 0;
            if (n == 4 && i == 1)
                x = x < 14 ? OLD_CHORD[x] : 0;
            set[i] = x < NPOS[i] ? x : 0;
        }
        const uint8_t *r = rec + v * n;
        uint8_t flags = n >= RECORD && r[NSET] & KEPT ? r[NSET] & KEPT_FLAGS : 0;
        uint8_t *e = kit_set[kit][v];
        pack_settings(e, set, flags, flags ? r + NSET + 1 : none);
        e[E_OPTS] = 0;                  /* older records: the preset's sound */
        if (n == RECORD2) {
            uint8_t f[AK_NFIELD];
            ak_unpack(r + RECORD + 1, f);   /* held within each value's range */
            ak_pack(f, e + E_SOUND);
            e[E_OPTS] = (uint8_t)((r[RECORD] & O_KEPT) | O_SOUND);
        }
    }
}

/* ---- DRIVE ---------------------------------------------------------------------------- */

/* Each voice's DRIVE (the value playing: knob, kit, locks) goes to the M7 when it changes
 * (message "akords-drive", 4 x 0-255, ordered: the stream the settings use): the engine's
 * level follows it. Checked from the main loop (every DRIVE_EVERY_MS), never from an
 * interrupt; sent again every DRIVE_RESEND_MS unchanged too, so a lost one heals. */
#define DRIVE_EVERY_MS  5
#define DRIVE_RESEND_MS 250
static uint8_t drive_sent[4] WORDS;     /* 0 at boot, as the M7's */
static uint16_t drive_checked WORDS, drive_at;      /* ms, low 16 bits: the last check, the last send */

void PK_NEXT(pk_ak_main_loop)(void);

void pk_ak_main_loop(void)
{
    PK_NEXT(pk_ak_main_loop)();
    prog_stopped();
    uint16_t now_ms = (uint16_t)fw_get_tick();
    if ((uint16_t)(now_ms - drive_checked) < DRIVE_EVERY_MS)
        return;
    drive_checked = now_ms;
    sound_poll();
    uint8_t now[4];
    int changed = 0;
    for (int v = 0; v < 4; v++) {
        now[v] = (uint8_t)(FW_U16(fw_voice_state(v) + FW_VS_VALUE + 2 * FW_SLOT_DRIVE) >> 8);
        changed |= now[v] != drive_sent[v];
    }
    if (changed || (uint16_t)(now_ms - drive_at) >= DRIVE_RESEND_MS) {
        drive_at = now_ms;
        for (int v = 0; v < 4; v++)
            drive_sent[v] = now[v];
        pk_send_m7(PK_STREAM_AKORDS_DRIVE, PK_MSG_AKORDS_DRIVE, now, sizeof now);
    }
}

/* ---- progressions ------------------------------------------------------------------------ */

static uint16_t prog_step[4] WORDS;     /* each track's step when last seen */
static uint8_t prog_cycle[4] WORDS;     /* its cycles since the start (the chord: & 3) */
static int8_t prog_sent[4] WORDS;       /* the offsets the M7 has (0 at boot, as its own) */
static uint32_t prog_playing;
static const void *prog_pattern;        /* the pattern playing */

static uint8_t *seq(void)
{
    return (uint8_t *)(uintptr_t)FW_U32(SYM_seq_ptr);
}

/* each voice's offset now to the M7, if it changed (or `force`) */
static void send_prog(int force)
{
    int8_t off[4];
    int changed = force;
    for (int v = 0; v < 4; v++) {
        off[v] = PROGRESSION[cur[v][PROG]][prog_cycle[v] & 3];
        changed |= off[v] != prog_sent[v];
    }
    if (!changed)
        return;
    for (int v = 0; v < 4; v++)
        prog_sent[v] = off[v];
    pk_send_m7(PK_STREAM_AKORDS_PROG, PK_MSG_AKORDS_PROG, off, sizeof off);
}

static void prog_restart(void)
{
    for (int v = 0; v < 4; v++) {
        prog_cycle[v] = 0;
        prog_step[v] = FW_U16(fw_voice_state(v) + FW_VS_STEP);
    }
}

/* the stock trigger sending (tail-called by the sequencer's ticks): each track's cycle
 * first, so the chord's offset reaches the M7 before the step's triggers */
int PK_NEXT(pk_ak_send_triggers)(void);

int pk_ak_send_triggers(void)
{
    const uint8_t *s = seq();
    if (s[FW_SEQ_PLAY] == 1) {
        const void *pat = (const void *)(uintptr_t)FW_U32(s + FW_SEQ_CURRENT);
        if (!prog_playing || pat != prog_pattern) {     /* started, or a new pattern */
            prog_playing = 1;
            prog_pattern = pat;
            prog_restart();
        }
        for (int v = 0; v < 4; v++) {
            const uint8_t *vs = fw_voice_state(v);
            uint16_t step = FW_U16(vs + FW_VS_STEP);
            if (step != prog_step[v]) {
                const uint8_t *blk = (const uint8_t *)(uintptr_t)FW_U32(vs + FW_VS_BLOCK);
                if (step == blk[FW_BLK_FIRST])          /* back on its first step: a cycle */
                    prog_cycle[v]++;
                prog_step[v] = step;
            }
        }
        send_prog(0);
    }
    return PK_NEXT(pk_ak_send_triggers)();
}

/* stopped: the first chord again (from the main loop) */
static void prog_stopped(void)
{
    if (seq()[FW_SEQ_PLAY] == 1 || !prog_playing)
        return;
    prog_playing = 0;
    for (int v = 0; v < 4; v++)
        prog_cycle[v] = 0;
    send_prog(0);
}

/* ---- Random (on the voice's unpacked values) ---------------------------------------------- */

/* a preset's kind: Random keeps a sound of it */
enum { K_BASS, K_PAD, K_LEAD, K_KEYS, K_PLUCK, K_AIR };
static const uint8_t KIND[AK_NPRESET] = {
    K_BASS, K_PAD, K_LEAD, K_KEYS, K_PLUCK, K_PLUCK, K_PAD,     /* Bass .. Hollow */
    K_BASS, K_KEYS, K_KEYS, K_AIR, K_PAD, K_AIR, K_AIR,         /* Reese .. Talk */
    K_PAD, K_AIR,                                               /* Chant, Wah */
};
enum { R_ALL, R_MILD, R_WILD, R_RECIPE };

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

static void put(uint8_t *x, int f, int v)
{
    int lo = f < (int)sizeof AK_MIN ? AK_MIN[f] : 0;
    x[f] = (uint8_t)(v < lo ? lo : (v > AK_MAX[f] ? AK_MAX[f] : v));
}

static void nudge(uint8_t *x, int f, int d)
{
    put(x, f, (int)x[f] + between(-d, d));
}

static void osc(uint8_t *x, int k, int wave, int oct, int det, int lev, int pw)
{
    uint8_t *o = x + AK_OSC(k);
    o[AK_WAVE] = (uint8_t)wave;
    o[AK_OCT] = (uint8_t)(oct + 2);
    o[AK_DET] = (uint8_t)(AK_MID_DET + det);
    o[AK_LEV] = (uint8_t)lev;
    o[AK_PW] = (uint8_t)pw;
}

static void env(uint8_t *x, int at, int att, int dec, int sus, int rel)
{
    x[at + AK_E_ATT] = (uint8_t)att;
    x[at + AK_E_DEC] = (uint8_t)dec;
    x[at + AK_E_SUS] = (uint8_t)sus;
    x[at + AK_E_REL] = (uint8_t)rel;
}

static void filter(uint8_t *x, int mode, int cut, int res, int env_amt, int key)
{
    x[AK_FMODE] = (uint8_t)mode;
    x[AK_CUT] = (uint8_t)cut;
    x[AK_RES] = (uint8_t)res;
    x[AK_ENVAMT] = (uint8_t)(AK_MID_ENV + env_amt);
    x[AK_KEYTRK] = (uint8_t)key;
}

/* Mild: the preset, every value moved a little */
static void mild(uint8_t *x)
{
    for (int k = 0; k < 2; k++) {
        nudge(x, AK_OSC(k) + AK_DET, 3);
        if (x[AK_OSC(k) + AK_WAVE] == AK_W_PULSE)
            nudge(x, AK_OSC(k) + AK_PW, 2);
    }
    nudge(x, AK_OSC(1) + AK_LEV, 3);
    nudge(x, AK_CUT, 10);
    nudge(x, AK_RES, 6);
    nudge(x, AK_ENVAMT, 3);
    nudge(x, AK_KEYTRK, 2);
    for (int e = AK_AENV; e <= AK_FENV; e += 4) {
        nudge(x, e + AK_E_ATT, x[e + AK_E_ATT] >= 30 ? 3 : 1);
        nudge(x, e + AK_E_DEC, 3);
        nudge(x, e + AK_E_SUS, 2);
        nudge(x, e + AK_E_REL, 3);
    }
    if (x[AK_GLIDE])
        nudge(x, AK_GLIDE, 2);
}

/* the cutoffs a kind sounds right in (low-pass) */
static const uint8_t CUT_LO[6] = {70, 110, 140, 130, 110, 100}, CUT_HI[6] = {150, 200, 215, 220, 190, 210};

/* Wild: the preset taken further, and some of its structure drawn anew */
static void wild(uint8_t *x, int kind)
{
    mild(x);
    for (int k = 0; k < 2; k++) {
        uint8_t *o = x + AK_OSC(k);
        if (chance(35))
            o[AK_WAVE] = (uint8_t)rr(AK_NWAVE);
        if (o[AK_WAVE] == AK_W_PULSE && chance(40))
            o[AK_PW] = (uint8_t)between(4, 20);
    }
    if (chance(30))                     /* oscillator 2 an octave elsewhere */
        put(x, AK_OSC(1) + AK_OCT, x[AK_OSC(1) + AK_OCT] + (chance(50) ? 1 : -1));
    if (chance(30)) {                   /* detuned apart */
        int d = between(4, 24);
        x[AK_OSC(0) + AK_DET] = (uint8_t)(AK_MID_DET - d / 2);
        x[AK_OSC(1) + AK_DET] = (uint8_t)(AK_MID_DET + d - d / 2);
    }
    if (chance(30))
        x[AK_OSC(1) + AK_LEV] = (uint8_t)between(10, 28);
    if (kind != K_BASS && kind != K_PLUCK && chance(15)) {
        x[AK_FMODE] = (uint8_t)rr(AK_NFILT);
        if (x[AK_FMODE] == AK_F_HP12)
            x[AK_CUT] = (uint8_t)between(60, 130);
        else if (x[AK_FMODE] == AK_F_FORMANT)
            x[AK_CUT] = (uint8_t)between(40, 220);
    }
    if (x[AK_FMODE] == AK_F_LP24 && chance(40))
        x[AK_CUT] = (uint8_t)between(CUT_LO[kind], CUT_HI[kind]);
    if (chance(30))
        x[AK_RES] = (uint8_t)between(0, 60);
    nudge(x, AK_ENVAMT, 6);
    for (int e = AK_AENV; e <= AK_FENV; e += 4) {
        nudge(x, e + AK_E_DEC, 5);
        nudge(x, e + AK_E_SUS, 4);
        nudge(x, e + AK_E_REL, 5);
    }
    if (chance(15))
        x[AK_GLIDE] = (uint8_t)(chance(50) ? 0 : between(2, 8));
}

/* Recipe: a fresh sound of the preset's kind, from one of three templates */
static void recipe(uint8_t *x, int kind)
{
    int r = rr(3), d = between(5, 10);
    x[AK_GLIDE] = 0;
    switch (kind) {
    case K_BASS:
        if (r == 0) {                   /* a sub stack: saw and a narrow pulse */
            osc(x, 0, AK_W_SAW, -1, 0, 28, 20);
            osc(x, 1, AK_W_PULSE, -1, between(3, 6), between(16, 22), between(8, 16));
            filter(x, AK_F_LP24, between(90, 130), between(20, 40), between(6, 10), between(4, 8));
            env(x, AK_AENV, between(5, 10), between(36, 42), between(8, 14), between(30, 36));
            env(x, AK_FENV, between(5, 10), between(34, 40), between(0, 4), between(30, 36));
            x[AK_TRIM] = 22;
        } else if (r == 1) {            /* a Reese: two saws far apart */
            int w = between(12, 22);
            osc(x, 0, AK_W_SAW, -1, -w, 28, 20);
            osc(x, 1, AK_W_SAW, -1, w, 28, 20);
            filter(x, AK_F_LP24, between(90, 120), between(10, 30), between(4, 7), between(4, 8));
            env(x, AK_AENV, between(10, 14), between(40, 45), between(16, 19), between(34, 40));
            env(x, AK_FENV, between(12, 18), between(42, 46), between(10, 14), between(36, 40));
            x[AK_GLIDE] = (uint8_t)between(0, 5);
            x[AK_TRIM] = 8;
        } else {                        /* acid: one oscillator, a resonant sweep, glide */
            osc(x, 0, chance(50) ? AK_W_SAW : AK_W_PULSE, -1, 0, 28, 20);
            osc(x, 1, AK_W_SAW, -1, 0, 0, 20);
            filter(x, AK_F_LP24, between(70, 110), between(50, 75), between(10, 14), between(4, 8));
            env(x, AK_AENV, between(5, 8), between(38, 42), between(10, 16), between(30, 34));
            env(x, AK_FENV, between(3, 6), between(34, 40), 0, between(30, 34));
            x[AK_GLIDE] = (uint8_t)between(4, 8);
            x[AK_TRIM] = 24;
        }
        break;
    case K_PAD:
        if (r == 0) {                   /* warm: two detuned saws, slow */
            osc(x, 0, AK_W_SAW, 0, -d, 22, 20);
            osc(x, 1, AK_W_SAW, 0, d, 22, 20);
            filter(x, AK_F_LP24, between(140, 165), between(10, 25), between(6, 8), between(8, 12));
            env(x, AK_AENV, between(44, 50), between(46, 50), between(16, 18), between(49, 53));
            env(x, AK_FENV, between(46, 50), between(46, 50), between(10, 14), between(49, 53));
            x[AK_TRIM] = 28;
        } else if (r == 1) {            /* hollow: two pulses */
            osc(x, 0, AK_W_PULSE, 0, -d, 25, between(12, 20));
            osc(x, 1, AK_W_PULSE, chance(30), d, 22, between(6, 14));
            filter(x, AK_F_LP24, between(150, 175), between(15, 30), between(6, 9), between(8, 12));
            env(x, AK_AENV, between(30, 44), between(43, 47), between(15, 17), between(43, 49));
            env(x, AK_FENV, between(30, 44), between(44, 47), between(10, 12), between(43, 49));
            x[AK_TRIM] = 8;
        } else {                        /* a choir: detuned saws on a vowel */
            osc(x, 0, AK_W_SAW, 0, -d, 25, 20);
            osc(x, 1, AK_W_SAW, 0, d, 25, 20);
            filter(x, AK_F_FORMANT, between(64, 192), 50, between(2, 4), 0);
            env(x, AK_AENV, between(42, 46), between(44, 48), between(17, 19), between(49, 52));
            env(x, AK_FENV, between(44, 48), between(48, 51), between(12, 15), between(49, 52));
            x[AK_TRIM] = 31;
        }
        break;
    case K_LEAD:
        if (r == 0) {                   /* saw and pulse, glide */
            osc(x, 0, AK_W_SAW, 0, 0, 26, 20);
            osc(x, 1, AK_W_PULSE, 0, between(2, 6), between(16, 22), between(10, 16));
            filter(x, AK_F_LP24, between(170, 200), between(20, 40), between(7, 10), between(12, 16));
            x[AK_GLIDE] = (uint8_t)between(4, 10);
            x[AK_TRIM] = 20;
        } else if (r == 1) {            /* saw and an octave up */
            osc(x, 0, AK_W_SAW, 0, 0, 26, 20);
            osc(x, 1, chance(50) ? AK_W_SAW : AK_W_TRI, 1, between(0, 4), between(14, 20), 20);
            filter(x, AK_F_LP24, between(165, 195), between(15, 35), between(6, 10), between(12, 16));
            x[AK_TRIM] = 20;
        } else {                        /* two squares, resonant */
            osc(x, 0, AK_W_PULSE, 0, 0, 24, 20);
            osc(x, 1, AK_W_PULSE, 0, between(3, 8), between(16, 22), between(8, 14));
            filter(x, AK_F_LP24, between(160, 190), between(30, 50), between(6, 10), between(10, 14));
            x[AK_GLIDE] = (uint8_t)between(6, 12);
            x[AK_TRIM] = 12;
        }
        env(x, AK_AENV, between(10, 16), between(38, 43), between(14, 17), between(37, 42));
        env(x, AK_FENV, between(10, 16), between(40, 44), between(8, 12), between(38, 42));
        break;
    case K_KEYS:
        if (r == 0) {                   /* electric piano: saw and a triangle an octave up */
            osc(x, 0, AK_W_SAW, 0, 0, 25, 20);
            osc(x, 1, AK_W_TRI, 1, 0, between(14, 18), 20);
            filter(x, AK_F_LP24, between(160, 185), between(10, 20), between(8, 11), between(10, 14));
            env(x, AK_AENV, between(8, 12), between(43, 47), between(5, 9), between(40, 44));
            env(x, AK_FENV, between(8, 12), between(42, 46), between(3, 6), between(40, 44));
            x[AK_TRIM] = 23;
        } else if (r == 1) {            /* organ: triangle and a sine an octave or two up */
            osc(x, 0, AK_W_TRI, 0, 0, 25, 20);
            osc(x, 1, AK_W_SINE, between(1, 2), 0, between(18, 24), 20);
            filter(x, AK_F_LP24, between(210, 230), 0, 0, 6);
            env(x, AK_AENV, between(10, 14), between(36, 40), between(18, 20), between(28, 32));
            env(x, AK_FENV, 12, 38, 20, 30);
            x[AK_TRIM] = 31;
        } else {                        /* a bell: sine and a triangle above, a long decay */
            osc(x, 0, AK_W_SINE, 1, 0, 26, 20);
            osc(x, 1, chance(50) ? AK_W_TRI : AK_W_SINE, between(2, 3), between(0, 6), between(12, 18), 20);
            filter(x, AK_F_LP24, between(190, 215), 0, 0, 12);
            env(x, AK_AENV, between(3, 6), between(46, 51), 0, between(43, 47));
            env(x, AK_FENV, 5, 46, 0, 45);
            x[AK_TRIM] = 29;
        }
        break;
    case K_PLUCK:
        if (r == 0) {                   /* a pluck: two saws, a fast resonant sweep */
            osc(x, 0, AK_W_SAW, 0, 0, 26, 20);
            osc(x, 1, AK_W_SAW, 0, d, 19, 20);
            filter(x, AK_F_LP24, between(140, 170), between(30, 50), between(11, 14), between(8, 12));
            x[AK_TRIM] = 10;
        } else if (r == 1) {            /* a stab: detuned saws, low and resonant */
            osc(x, 0, AK_W_SAW, 0, -d, 25, 20);
            osc(x, 1, AK_W_SAW, 0, d, 25, 20);
            filter(x, AK_F_LP24, between(120, 145), between(38, 48), between(10, 13), between(6, 10));
            x[AK_TRIM] = 4;
        } else {                        /* a pulse pluck */
            osc(x, 0, AK_W_PULSE, 0, 0, 26, between(8, 14));
            osc(x, 1, AK_W_SAW, 0, d, 18, 20);
            filter(x, AK_F_LP24, between(130, 160), between(25, 40), between(10, 13), between(8, 12));
            x[AK_TRIM] = 10;
        }
        env(x, AK_AENV, between(3, 7), between(36, 42), 0, between(35, 39));
        env(x, AK_FENV, between(3, 7), between(35, 39), 0, between(35, 39));
        break;
    default:                            /* K_AIR */
        if (r == 0) {                   /* air: high-pass, thin and bright */
            osc(x, 0, AK_W_SAW, 0, -d, 23, 20);
            osc(x, 1, AK_W_PULSE, 1, d, 16, between(12, 18));
            filter(x, AK_F_HP12, between(80, 110), between(20, 40), -between(4, 8), 16);
            env(x, AK_AENV, between(15, 20), between(44, 47), between(12, 16), between(44, 47));
            env(x, AK_FENV, between(10, 14), between(42, 44), between(4, 8), between(42, 44));
            x[AK_TRIM] = 19;
        } else if (r == 1) {            /* vox: the vowel opens on each chord */
            osc(x, 0, AK_W_PULSE, 0, 0, 25, between(10, 14));
            osc(x, 1, AK_W_SAW, 0, between(3, 8), 19, 20);
            filter(x, AK_F_FORMANT, between(50, 80), 50, between(10, 14), 0);
            env(x, AK_AENV, between(15, 20), between(40, 42), between(15, 17), between(40, 42));
            env(x, AK_FENV, between(20, 24), between(40, 43), between(4, 6), between(40, 42));
            x[AK_GLIDE] = (uint8_t)between(0, 3);
            x[AK_TRIM] = 31;
        } else {                        /* talk: the vowel dips and comes back */
            osc(x, 0, AK_W_SAW, 0, 0, 26, 20);
            osc(x, 1, AK_W_SAW, 1, between(2, 6), 16, 20);
            filter(x, AK_F_FORMANT, between(180, 200), 50, -between(12, 16), 0);
            env(x, AK_AENV, between(10, 14), between(41, 45), between(10, 14), between(38, 42));
            env(x, AK_FENV, between(10, 14), between(38, 42), 0, between(36, 40));
            x[AK_TRIM] = 31;
        }
        break;
    }
}

/* about how loud the oscillators are (RMS of each waveform, by its level) */
static int loudness(const uint8_t *x)
{
    static const uint8_t RMS[AK_NWAVE] = {71, 58, 58, 100};     /* sine, triangle, saw, square (%) */
    int l = 0;
    for (int k = 0; k < 2; k++)
        l += RMS[x[AK_OSC(k) + AK_WAVE]] * x[AK_OSC(k) + AK_LEV];
    return l > 0 ? l : 1;
}

/* the level trim for a changed sound (Wild): up when its waveforms are quieter than the
 * preset's, never down (the waveforms' RMS overstates a louder one, which the filter and
 * resonance shape; the output limiter holds the peaks); a new filter type starts from that
 * type's own level */
static void trim_like(uint8_t *x, const uint8_t *was)
{
    static const uint8_t OWN[AK_NFILT] = {20, 19, 31};
    int t = x[AK_FMODE] == was[AK_FMODE] ? was[AK_TRIM] : OWN[x[AK_FMODE]];
    int lw = loudness(was), lx = loudness(x);
    int g = (31 + 2 * t) * (lw > lx ? lw : lx) / lx;   /* the gain in 62nds: 31 + 2 x trim */
    put(x, AK_TRIM, (g - 31) / 2);
}

/* the rules that keep a sound playable */
static void tidy(uint8_t *x, int kind)
{
    if (x[AK_OSC(0) + AK_LEV] < 12)
        x[AK_OSC(0) + AK_LEV] = 12;
    for (int k = 0; k < 2; k++)
        if (x[AK_OSC(k) + AK_OCT] > 4)          /* at most 2 octaves up */
            x[AK_OSC(k) + AK_OCT] = 4;
    if (kind == K_BASS && x[AK_OSC(0) + AK_OCT] > 2)
        x[AK_OSC(0) + AK_OCT] = 2;
    if (kind != K_PAD && x[AK_AENV + AK_E_ATT] > 25)    /* no slow attack but on pads */
        x[AK_AENV + AK_E_ATT] = 25;
    if ((kind == K_PAD || kind == K_LEAD) && x[AK_AENV + AK_E_SUS] < 10)
        x[AK_AENV + AK_E_SUS] = 10;
    if (x[AK_FMODE] == AK_F_LP24 && x[AK_CUT] < 60)     /* not closed to a murmur */
        x[AK_CUT] = 60;
    if (x[AK_FMODE] == AK_F_HP12 && x[AK_CUT] > 150)    /* nor high-passed to a hiss */
        x[AK_CUT] = 150;
    if (x[AK_FMODE] == AK_F_FORMANT)    /* a vowel needs harmonics: saw or pulse */
        for (int k = 0; k < 2; k++)
            if (x[AK_OSC(k) + AK_WAVE] < AK_W_SAW)
                x[AK_OSC(k) + AK_WAVE] = (uint8_t)(k ? AK_W_PULSE : AK_W_SAW);
}

static void save_undo(int v)
{
    for (int i = 0; i < AK_PACKED; i++)
        undo_snd[v][i] = voice_snd[v][i];
    undo_opt[v] = opt[v];
    undo_ok |= 1u << v;
}

/* the voice's sound changed by Random or undo: its knobs back to the sound's own values (as
 * a preset press), to the M7 */
static void sound_changed(int v)
{
    sound_preset(v);
    send((uint8_t)(1u << v));
    send_voice(v);
}

/* a press of Random (R_MILD, R_WILD, R_RECIPE, R_ALL) for the voice */
static void do_random(int v, int style)
{
    rng ^= (uint32_t)fw_get_tick() * 2654435761u;
    if (!rng)
        rng = 0x9e3779b9u;
    rnd();
    save_undo(v);
    int preset = cur[v][0] < AK_NPRESET ? cur[v][0] : 0, kind = KIND[preset];
    uint8_t x[AK_NFIELD];
    for (int f = 0; f < AK_NFIELD; f++)
        x[f] = AK_PRESETS[preset][f];
    if (style == R_ALL)
        style = R_MILD + rr(3);
    if (style == R_MILD) {
        mild(x);
    } else if (style == R_WILD) {
        wild(x, kind);
        trim_like(x, AK_PRESETS[preset]);
    } else {
        recipe(x, kind);
    }
    tidy(x, kind);
    ak_pack(x, voice_snd[v]);
    opt[v] |= O_RANDOM;
    sound_changed(v);
}

static void do_undo(int v)
{
    if (!(undo_ok >> v & 1))
        return;
    undo_ok &= ~(1u << v);
    for (int i = 0; i < AK_PACKED; i++)
        voice_snd[v][i] = undo_snd[v][i];
    opt[v] = undo_opt[v];
    sound_changed(v);
}

/* ---- the settings page ------------------------------------------------------------------ */

/* the page's steps beyond the settings: row 3 options (steps 12-16), row 4 Random (13-16) */
#define OPT_COL         11
#define RANDOM_COL      12
#define UNDO_MS         500         /* a Random step held this long: undo */
#define LED_MID         0x38
static const uint8_t OPT_BIT[5] = {AK_O_STRUM_UP, AK_O_STRUM_DOWN, AK_O_SPREAD, AK_O_DROP, AK_O_MUTATE};

int pk_ak_page_key(const uint8_t *evt)
{
    if (evt[1] != FW_EV_DOWN && evt[1] != FW_EV_UP)
        return 0;                       /* not a key (the panel switches): stock */
    int k = evt[0], down = evt[1] == FW_EV_DOWN;
    make_ready();
    if (k < 0x40) {                     /* steps (their releases too: the long press's) */
        int row = k / 16, col = k % 16, v = (int)editing;
        if (row == 3 && col >= RANDOM_COL) {            /* Random: on release; held, undo */
            if (down) {
                key_held = (uint32_t)k + 1;
                key_at = fw_get_tick();
                key_done = 0;
            } else if ((uint32_t)k + 1 == key_held) {
                if (!key_done)
                    do_random(v, col == 15 ? R_ALL : R_MILD + (col - RANDOM_COL));
                key_held = 0;
            }
            return 1;
        }
        if (!down)
            return 1;
        if (row == 2 && col >= OPT_COL) {               /* an option on / off */
            uint8_t b = OPT_BIT[col - OPT_COL];
            opt[v] ^= b;
            if (b == AK_O_STRUM_UP)                     /* one strum at a time */
                opt[v] &= (uint8_t)~AK_O_STRUM_DOWN;
            else if (b == AK_O_STRUM_DOWN)
                opt[v] &= (uint8_t)~AK_O_STRUM_UP;
            send_voice(v);
            return 1;
        }
        if (col >= (row == 1 ? NCHORD + NPROG : NPOS[row]))
            return 1;
        if (row == 0) {                 /* a preset, even the one chosen: all its sound */
            cur[v][0] = (uint8_t)col;
            preset_sound(v, col);
            sound_preset(v);
            send((uint8_t)(1u << v));
            send_voice(v);
        } else if (row == 1 && col >= NCHORD) {         /* off, or a progression */
            if (cur[v][PROG] != col - NCHORD) {
                cur[v][PROG] = (uint8_t)(col - NCHORD);
                send(0);
            }
        } else if (cur[v][row] != col) {
            cur[v][row] = (uint8_t)col;
            send(0);
        }
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
void pk_ak_page_enter(void)
{
    if (pk_vm_settings_voice < 4)
        editing = pk_vm_settings_voice;
    key_held = 0;
}

void pk_ak_page_draw(void)
{
    make_ready();
    int v = (int)editing;
    if (key_held && !key_done && fw_get_tick() - key_at >= UNDO_MS) {
        key_done = 1;                   /* a Random step held: undo */
        do_undo(v);
    }
    for (int k = 0; k < 0x40; k++) {
        int row = k / 16, col = k % 16, lvl = 0;
        if (row == 0 && col == cur[v][0])               /* bright; medium after Random */
            lvl = opt[v] & O_RANDOM ? LED_MID : LED_ON;
        else if (row == 1 && col >= NCHORD && col < NCHORD + NPROG)
            lvl = cur[v][PROG] == col - NCHORD ? LED_ON : LED_DIM;
        else if (row == 2 && col >= OPT_COL)
            lvl = opt[v] & OPT_BIT[col - OPT_COL] ? LED_ON : LED_DIM;
        else if (row == 3 && col >= RANDOM_COL)
            lvl = (uint32_t)k + 1 == key_held ? LED_ON : LED_DIM;
        else if (col < NPOS[row])
            lvl = cur[v][row] == col ? LED_ON : LED_DIM;
        fw_set_led(k, lvl);
    }
}

/* leaving the page: changed settings, options and sounds go into the kit and its sidecar */
void pk_ak_page_leave(void)
{
    int bank = FW_U8(SYM_bank_number), kit = current_kit();
    key_held = 0;
    if (kit >= 0 && kit < NKITS && !same(kit_set[kit]))
        pk_store_save_kit(bank, kit);
}
