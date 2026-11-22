/* BREAK MAKER, M4 side: the break from the card, each step of its tracks to the M7, each kit's
 * settings, the configuration page and the generator.
 *
 * The break: BANKS/NN/BREAKS/n.wav if the bank has it, else BREAKS/n.wav (n = 1-16): mono, 16-bit,
 * any rate (44.1 kHz expected), one bar. Loaded when a voice runs BREAK MAKER and the kit's break
 * isn't the one in RAM (not at bank load: start-up stays quick), a chunk per main loop pass, into
 * the stock custom samples' RAM (bm_shared.h). Its 16 slices: the WAV's cue points if it has 16,
 * else the bar cut in 16 with each cut moved onto the nearest hit. Each slice is then sorted
 * (kick, snare, hat, ghost) by its first 25 ms: its level, how much of it is low, how bright.
 *
 * The steps: each tick of the sequencer (the tail of send_triggers), each track running BREAK
 * MAKER whose step changed sends it to the M7 ("break-maker-step"), before the step's triggers;
 * and once when the sequencer stops.
 *
 * A kit keeps (record BMKR, 5 bytes): the break, and per voice the generator's character and the
 * pitch mode. A kit without one: break 1, Straight, repitch.
 *
 * The configuration page: a long press on a voice's BREAK MAKER step on the engine page, or steps
 * of a BREAK MAKER track held + SHUFFLE on the main page (the page then works on those steps).
 *   row 1   the break (lit: on the card; bright: the kit's)
 *   row 2   pads 1-11, while held: roll 1/8, 1/16, 1/32, reverse, freeze, half, tape stop, gate,
 *           random, filter throw, stutter; 12-16: step types for the held steps: ghost, reverse,
 *           roll x2, roll x3, roll x4 (the lit one again: plain; reverse with roll x2: both)
 *   row 3   slices 1-16: with held steps, their slice (a PARAM2 lock); else heard
 *   row 4   the generator's character (1-8), pitch repitch / keep length (9-10), New pattern, Keep
 *           rhythm, Vary, Clear (13-16: on release; held 0.5 s: undo the last of them)
 * The trigger keys play; SHIFT, KIT or MOD goes back to the engine page; SHUFFLE to the main page. */
#include <stdint.h>
#include "pk_host.h"
#include "fw_m4_seq.h"
#include "pk_vm.h"                  /* pk_vm_engine */
#include "pk_bm_engine.h"           /* PK_BM_ENGINE: this engine's number */
#include "bm_shared.h"

#define NKITS           64
#define NREC            5           /* the kit record: break, then per voice character | keep << 3 */
#define LED_ON          0x80
#define LED_MID         0x38
#define LED_DIM         0x10
#define UNDO_MS         500
#define CHUNK           32768u      /* bytes read per main loop pass */
#define SETTLE_MS       3           /* the M7 goes quiet before the break is rewritten */
#define WORDS __attribute__((aligned(4)))
#define POS1            1
#define POS3            2
enum { A_NEW, A_RHYTHM, A_VARY, A_CLEAR, A_UNDO };      /* row 4, 13-16; held: undo */
enum { L_IDLE, L_SETTLE, L_DATA };

extern uint8_t pk_vm_settings_voice;

/* (every variable whole words, word-aligned: the M4 host clears its RAM a word at a time) */
static uint8_t bm_kit_rec[NKITS][8] WORDS;         /* each kit's record (NREC used) */
static uint32_t kit_ok[NKITS / 32];
static uint8_t bm_cur[8] WORDS;                    /* the kit playing: break, per voice settings */
static uint16_t bm_pads[4] WORDS;
static uint8_t bm_aud[4] WORDS;                    /* each voice's audition byte */
static uint32_t dirty;                          /* voices (bits) whose settings go to the M7 */
static uint32_t bank_now, avail, avail_bank;    /* breaks on the card (bits), for which bank (+ 1) */
/* the loader */
static uint32_t lstate, l_number, l_at, l_data, l_frames, l_done, l_rate, l_ncue, l_bars;
static int32_t loaded, failed;                  /* the break in RAM / the one that failed: number + 256 x bank + 1, 0 none */
static uint32_t cue[16];
/* steps */
static int8_t last_step[4] WORDS;
static uint8_t sent_play[4] WORDS;
/* the page */
static uint32_t editing, active, bm_held[2];
static uint32_t act_key, act_at, act_done, pending;
static uint32_t rng;
typedef struct {
    uint8_t *blk;
    uint16_t p2[16];                /* PARAM2 lock values */
    uint8_t on[16], vel[16], note[16];
    uint16_t mask;                  /* steps whose PARAM2 was locked */
    uint8_t voice, kind;            /* kind 1: the track's steps */
    uint8_t pad[2];
} undo_t;
static undo_t undo WORDS;

static inline void dmb(void)
{
    __asm volatile("dmb" ::: "memory");
}

static int current_kit(void)
{
    return *(volatile uint16_t *)(uintptr_t)SYM_current_kit;
}

static int ours(int v)
{
    return pk_vm_engine(v) == PK_BM_ENGINE;
}

void pk_bm_init(void)
{
    BM->ready = 0;
    BM->magic = BM_MAGIC;
    for (int v = 0; v < 4; v++)
        last_step[v] = -1;
}

/* ---- to the M7 ----------------------------------------------------------------------------- */

static void send_settings(int v)
{
    uint8_t s = bm_cur[1 + v];
    uint8_t msg[BM_SET_LEN] = {(uint8_t)v, (uint8_t)(s >> 3 & 1 ? BM_SET_KEEP : 0), (uint8_t)bm_pads[v],
                               (uint8_t)(bm_pads[v] >> 8), (uint8_t)(s & 7), bm_aud[v]};
    pk_send_m7(PK_STREAM_BREAK_MAKER, PK_MSG_BREAK_MAKER, msg, sizeof msg);
}

static void mark(int v)
{
    uint32_t pm = pk_irq_off();
    dirty |= 1u << v;
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
            send_settings(v);
}

static const uint8_t *seq(void)
{
    return (const uint8_t *)(uintptr_t)FW_U32(SYM_seq_ptr);
}

static uint16_t bpm10(void)
{
    float b = FW_CLOCK_BPM;
    return b > 0.0f && b < 999.0f ? (uint16_t)(b * 10.0f + 0.5f) : 1200;
}

/* PARAM2 for step s's trigger: its lock, else the value playing (0-4095) */
static uint32_t step_p2(const uint8_t *vs, const uint8_t *blk, int s)
{
    if (blk) {
        const uint8_t *lk = blk + FW_BLK_LOCKS + FW_LOCK_SIZE * (s & 15);
        if (FW_U16((uintptr_t)lk + 2 * FW_NSLOTS) >> FW_SLOT_PARAM2 & 1)
            return FW_U16((uintptr_t)lk + 2 * FW_SLOT_PARAM2) >> 4;
    }
    return FW_U16((uintptr_t)vs + FW_VS_VALUE + 2 * FW_SLOT_PARAM2) >> 4;
}

static void send_step(int v, int step, int flags)
{
    const uint8_t *vs = fw_voice_state(v);
    const uint8_t *blk = (const uint8_t *)(uintptr_t)FW_U32((uintptr_t)vs + FW_VS_BLOCK);
    uint16_t t = bpm10();
    uint32_t p2 = step_p2(vs, blk, step);
    uint8_t msg[BM_STEP_LEN] = {(uint8_t)v, (uint8_t)(step & 15), blk ? blk[FW_BLK_FIRST] : 0,
                                blk ? blk[FW_BLK_LAST] : 15, (uint8_t)flags, (uint8_t)t, (uint8_t)(t >> 8),
                                (uint8_t)p2, (uint8_t)(p2 >> 8)};
    if (blk && (flags & BM_STEP_PLAYING) && blk[FW_STEP_SIZE * (step & 15)] == 1)
        msg[4] |= BM_STEP_ON;
    pk_send_m7(PK_STREAM_BREAK_MAKER_STEP, PK_MSG_BREAK_MAKER_STEP, msg, sizeof msg);
}

/* the stock trigger sending (tail-called by the sequencer's ticks): each BREAK MAKER track's new
 * step first, so it reaches the M7 before the step's triggers */
int PK_NEXT(pk_bm_send_triggers)(void);

int pk_bm_send_triggers(void)
{
    int playing = seq()[FW_SEQ_PLAY] == 1;
    for (int v = 0; v < 4; v++) {
        if (!ours(v))
            continue;
        if (playing) {
            int step = (int16_t)FW_U16(fw_voice_state(v) + FW_VS_STEP);
            if (step < 0 || step > 15)
                continue;
            if (step != last_step[v] || !sent_play[v]) {
                last_step[v] = (int8_t)step;
                sent_play[v] = 1;
                send_step(v, step, BM_STEP_PLAYING);
            }
            if (FW_U8(fw_voice_state(v) + FW_VS_FIRED))     /* its trigger goes out now: its slice first */
                send_step(v, step, BM_STEP_PLAYING | BM_STEP_HIT);
        } else if (sent_play[v]) {
            sent_play[v] = 0;
            last_step[v] = -1;
            send_step(v, 0, 0);
        }
    }
    return PK_NEXT(pk_bm_send_triggers)();
}

/* ---- the card -------------------------------------------------------------------------------- */

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static int small_read(uint32_t ofs, uint8_t *buf, unsigned n)
{
    unsigned got;
    void *fil = (void *)(uintptr_t)BM_FIL;
    return fw_f_lseek(fil, ofs) == 0 && fw_f_read(fil, buf, n, &got) == 0 && got == n ? 0 : -1;
}

/* which of a folder's files are "<n>.wav", n 1-16: bit n - 1 */
static uint32_t listing(const char *dir)
{
    uint8_t *d = (uint8_t *)(uintptr_t)BM_DIR, *fno = (uint8_t *)(uintptr_t)BM_FNO;
    uint32_t mask = 0;
    if (fw_f_opendir(d, dir) != 0)
        return 0;
    while (fw_f_readdir(d, fno) == 0 && fno[FW_FNO_NAME]) {
        const char *s = (const char *)fno + FW_FNO_NAME;
        int n = 0, i = 0;
        while (i < 2 && s[i] >= '0' && s[i] <= '9')
            n = n * 10 + (s[i++] - '0');
        if (!i || s[0] == '0' || s[i] != '.' || (s[i + 1] & ~0x20) != 'W' || (s[i + 2] & ~0x20) != 'A' ||
            (s[i + 3] & ~0x20) != 'V' || s[i + 4] || n < 1 || n > 16)
            continue;
        mask |= 1u << (n - 1);
    }
    fw_f_closedir(d);
    return mask;
}

static void refresh_avail(void)
{
    char dir[24];
    fw_snprintf(dir, sizeof dir, "BANKS/%02d/BREAKS", (int)bank_now);
    avail = listing(dir) | listing("BREAKS");
    avail_bank = bank_now + 1;
}

/* the break's file opened in BM_FIL and its header read: 0 on success */
static int open_break(int n)
{
    void *fil = (void *)(uintptr_t)BM_FIL;
    char p[32];
    fw_snprintf(p, sizeof p, "BANKS/%02d/BREAKS/%d.wav", (int)bank_now, n + 1);
    if (fw_f_open(fil, p, FW_FA_READ) != 0) {
        fw_snprintf(p, sizeof p, "BREAKS/%d.wav", n + 1);
        if (fw_f_open(fil, p, FW_FA_READ) != 0)
            return -1;
    }
    uint32_t size = FW_U32((uintptr_t)fil + FW_FIL_SIZE);
    uint8_t h[16];
    if (size < 12 || small_read(0, h, 12) != 0 || rd32(h) != 0x46464952u || rd32(h + 8) != 0x45564157u)
        return -1;
    uint32_t pos = 12, data = 0, frames = 0, rate = 0;
    int pcm = 0;
    l_ncue = 0;
    while (pos + 8 <= size && small_read(pos, h, 8) == 0) {
        uint32_t id = rd32(h), len = rd32(h + 4), body = pos + 8;
        if (id == 0x20746d66u && len >= 16 && small_read(body, h, 16) == 0) {           /* "fmt " */
            uint16_t tag = rd16(h), ch = rd16(h + 2), bits = rd16(h + 14);
            rate = rd32(h + 4);
            if (tag == 0xfffeu && len >= 26 && small_read(body + 24, h, 2) == 0)
                tag = rd16(h);
            pcm = tag == 1 && ch == 1 && bits == 16 && rate >= 8000 && rate <= 96000;
        } else if (id == 0x61746164u) {                                                 /* "data" */
            data = body;
            frames = (len < size - body ? len : size - body) / 2u;
        } else if (id == 0x20657563u && len >= 4 && small_read(body, h, 4) == 0) {      /* "cue " */
            uint32_t c = rd32(h);
            for (uint32_t i = 0; i < c && l_ncue < 16 && 4 + 24 * (i + 1) <= len; i++) {
                if (small_read(body + 4 + 24 * i + 20, h, 4) != 0)
                    break;
                cue[l_ncue++] = rd32(h);
            }
        }
        pos = body + len + (len & 1u);
    }
    uint32_t room = (BM_PCM_BYTES - 4u) / 2u;
    if (!pcm || !data || frames < 1024)
        return -1;
    /* how many bars: 1, 2 or 4, whichever puts the break's own tempo nearest the Pērkons's now
     * (a 4 s loop at 120 BPM is 2 bars at 120, not 1 at 60); the first bar is played */
    float tempo = FW_CLOCK_BPM, secs = (float)frames / (float)rate, best = 1e9f;
    if (!(tempo >= 30.0f && tempo <= 300.0f))
        tempo = 120.0f;
    l_bars = 1;
    for (uint32_t n = 1; n <= 4; n <<= 1) {
        float r = 240.0f * (float)n / secs / tempo;     /* its tempo as n bars, over the Pērkons's */
        float d = r >= 1.0f ? r : 1.0f / r;
        if (d < best) {
            best = d;
            l_bars = n;
        }
    }
    frames /= l_bars;
    l_frames = frames < room ? frames : room;
    l_data = data;
    l_rate = rate;
    return 0;
}

static uint32_t pcm_base(void)          /* frame 0's address: the file's data word-aligned with it */
{
    return BM_PCM + (l_data & 3u);
}

/* ---- slices ----------------------------------------------------------------------------------- */

static int32_t absi(int32_t x)
{
    return x < 0 ? -x : x;
}

/* a hop's attack energy: its sample-to-sample differences (a low tone's slow swell within its own
 * cycle doesn't count; hits do) */
static int32_t hop_e(const int16_t *x, int32_t h)
{
    int32_t e = 0;
    for (int i = 0; i < 32; i++)
        e += absi(x[h + i] - x[h + i - 1]);
    return e;
}

#define JUMP_W          512         /* the level jump's windows (frames: ~12 ms at 44.1 kHz) */

/* the mean level over [a, b), scaled to a hop of 32 */
static int32_t level(const int16_t *x, int32_t n, int32_t a, int32_t b)
{
    a = a < 0 ? 0 : a;
    b = b > n ? n : b;
    if (b <= a)
        return 0;
    int32_t s = 0;
    for (int32_t i = a; i < b; i++)
        s += absi(x[i]);
    return s * 32 / (b - a);            /* (within 32 bits: s <= 512 x 32767) */
}

/* a hit's strength at hop h: the rise in attack energy since the hop before (hits with an edge),
 * plus how much louder the next ~12 ms are than the last (a soft start, as a deep kick's) */
static int32_t strength(const int16_t *x, int32_t n, int32_t h, int32_t prev_e)
{
    int32_t jump = level(x, n, h, h + JUMP_W) - level(x, n, h - JUMP_W, h);
    return hop_e(x, h) - prev_e + (jump > 0 ? jump : 0);
}

/* the hop of the strongest hit within [a, b) (hops of 32 frames), its strength, or -1 */
static int32_t onset(const int16_t *x, int32_t n, int32_t a, int32_t b, int32_t *rise)
{
    int32_t best = -1, br = 0, prev = hop_e(x, a);
    for (int32_t h = a + 32; h + 32 <= b; h += 32) {
        int32_t r = strength(x, n, h, prev);
        if (r > br) {
            br = r;
            best = h;
        }
        prev = hop_e(x, h);
    }
    *rise = br;
    return best;
}

static void find_slices(void)
{
    const int16_t *x = (const int16_t *)(uintptr_t)pcm_base();
    int32_t n = (int32_t)l_frames, g = n / 16;
    uint32_t *sl = BM->slice;
    if (l_ncue == 16) {                 /* its own cue points (ascending, inside) */
        for (int i = 1; i < 16; i++)
            for (int j = i; j > 0 && cue[j - 1] > cue[j]; j--) {
                uint32_t t = cue[j];
                cue[j] = cue[j - 1];
                cue[j - 1] = t;
            }
        int ok = cue[15] < (uint32_t)n;
        for (int i = 1; i < 16; i++)
            ok &= cue[i] > cue[i - 1];
        if (ok) {
            for (int i = 0; i < 16; i++)
                sl[i] = i ? cue[i] : 0;
            sl[16] = (uint32_t)n;
            return;
        }
    }
    /* the loudest hop's level: how big a rise counts as a hit */
    int32_t emax = 1;
    for (int32_t h = 64; h + JUMP_W <= n; h += 32) {
        int32_t e = strength(x, n, h, hop_e(x, h - 32));
        emax = e > emax ? e : emax;
    }
    sl[0] = 0;
    for (int i = 1; i < 16; i++) {
        int32_t c = i * g, w = g * 3 / 10, r;
        int32_t a = c - w, lo = (int32_t)sl[i - 1] + g / 4;
        a = a < lo ? lo : a;
        a = a < 32 ? 32 : a;
        int32_t h = onset(x, n, a, c + w, &r);
        int32_t at = h >= 0 && r * 40 > emax ? h + 16 : c;  /* the hit (its hop's middle), else the grid */
        at -= 24;                                           /* a little before: the attack's start */
        sl[i] = (uint32_t)(at < lo ? lo : at);
    }
    sl[16] = (uint32_t)n;
}

/* each slice's kind, from its first 25 ms: level E, low part L (one pole at ~150 Hz), brightness D
 * (sample to sample differences) */
static void sort_slices(void)
{
    const int16_t *x = (const int16_t *)(uintptr_t)pcm_base();
    uint32_t e[16], lo[16], d[16], emax = 1;
    int32_t a = (int32_t)(65536.0f * 6.2831853f * 150.0f / (float)l_rate);
    for (int s = 0; s < 16; s++) {
        uint32_t b = BM->slice[s], m = BM->slice[s + 1] - b, w = l_rate / 40u;
        m = m < w ? m : w;
        uint32_t se = 0, sl = 0, sd = 0;
        int32_t lp = 0, prev = x[b];
        for (uint32_t i = 0; i < m; i++) {
            int32_t v = x[b + i];
            lp += (int32_t)(((int64_t)(v - lp) * a) >> 16);
            se += (uint32_t)absi(v);
            sl += (uint32_t)absi(lp);
            sd += (uint32_t)absi(v - prev);
            prev = v;
        }
        m = m ? m : 1;
        e[s] = se / m;
        lo[s] = sl / m;
        d[s] = sd / m;
        emax = e[s] > emax ? e[s] : emax;
    }
    for (int s = 0; s < 16; s++) {
        uint8_t t;
        if (d[s] * 100 > e[s] * 140)           /* bright noise: hat (whatever its level) */
            t = BM_T_HAT;
        else if (lo[s] * 100 > e[s] * 60)       /* mostly low: kick */
            t = BM_T_KICK;
        else if (e[s] * 4 < emax)               /* quiet and not bright: a ghost note */
            t = BM_T_GHOST;
        else
            t = BM_T_SNARE;
        BM->type[s] = t;
    }
}

/* ---- the loader (main loop) -------------------------------------------------------------------- */

static void load_step(void)
{
    void *fil = (void *)(uintptr_t)BM_FIL;
    int want = bm_cur[0] & 15, any = 0;
    for (int v = 0; v < 4; v++)
        any |= ours(v);
    int32_t id = want + 256 * (int32_t)bank_now + 1;
    if (lstate == L_IDLE) {
        if (!any || id == loaded || id == failed)
            return;
        BM->ready = 0;                  /* the M7 goes quiet */
        dmb();
        loaded = 0;
        l_number = (uint32_t)want;
        l_at = fw_get_tick();
        lstate = L_SETTLE;
        return;
    }
    if (lstate == L_SETTLE) {
        if (fw_get_tick() - l_at < SETTLE_MS)
            return;
        if ((int)l_number != want) {    /* changed meanwhile: start again */
            lstate = L_IDLE;
            return;
        }
        if (open_break(want) != 0) {
            fw_f_close(fil);
            failed = id;
            lstate = L_IDLE;
            return;
        }
        l_done = 0;
        lstate = L_DATA;
        return;
    }
    /* L_DATA: a chunk (the file's data and the RAM word-aligned alike: the card's DMA) */
    uint32_t bytes = 2u * l_frames, n = bytes - l_done;
    n = n < CHUNK ? n : CHUNK;
    unsigned got = 0;
    if (fw_f_lseek(fil, l_data + l_done) != 0 ||
        fw_f_read(fil, (uint8_t *)(uintptr_t)pcm_base() + l_done, n, &got) != 0 || got != n) {
        fw_f_close(fil);
        failed = id;
        lstate = L_IDLE;
        return;
    }
    l_done += n;
    if (l_done < bytes)
        return;
    fw_f_close(fil);
    BM->frames = l_frames;
    BM->rate = l_rate;
    BM->pcm = pcm_base();
    find_slices();
    sort_slices();
    BM->number = (uint8_t)l_number;
    BM->bars = (uint8_t)l_bars;
    BM->gen++;
    dmb();
    BM->ready = 1;
    loaded = id;
    failed = 0;
    lstate = L_IDLE;
}

/* bank load (in place of the stock sample load): the stock custom samples left out (the stock
 * sampler plays its factory ones); the break loads later, when a voice wants it */
void pk_bm_load_samples(int bank)
{
    uint8_t m = 0;
    pk_send_m7(3, 3, &m, 1);            /* as stock: samples changing */
    for (uint32_t a = SYM_sample_table; a < SYM_sample_data; a += 4)
        FW_U32(a) = 0;
    bank_now = (uint32_t)bank;
    avail_bank = 0;
    failed = 0;
    m = 1;
    pk_send_m7(3, 3, &m, 1);            /* as stock: samples ready */
}

/* ---- kits --------------------------------------------------------------------------------------- */

static void defaults(uint8_t *r)
{
    for (int i = 0; i < NREC; i++)
        r[i] = 0;
}

void PK_NEXT(pk_bm_kit_load)(int kit);

void pk_bm_kit_load(int kit)
{
    PK_NEXT(pk_bm_kit_load)(kit);
    if (kit < 0 || kit >= NKITS)
        return;
    if (!(kit_ok[kit / 32] >> (kit % 32) & 1))
        defaults(bm_kit_rec[kit]);
    for (int i = 0; i < NREC; i++)
        bm_cur[i] = bm_kit_rec[kit][i];
    undo.kind = 0;
    pending = 0;
    for (int v = 0; v < 4; v++)
        mark(v);
}

uint32_t pk_bm_kit_save(int bank, int kit, uint8_t *buf, uint32_t max)
{
    (void)bank;
    if (kit >= 0 && kit < NKITS) {
        for (int i = 0; i < NREC; i++)
            bm_kit_rec[kit][i] = bm_cur[i];
        kit_ok[kit / 32] |= 1u << (kit % 32);
    }
    if (max < NREC)
        return 0;
    for (int i = 0; i < NREC; i++)
        buf[i] = bm_cur[i];
    return NREC;
}

void pk_bm_kit_restore(int bank, int kit, const uint8_t *rec, uint32_t len)
{
    (void)bank;
    if (kit < 0 || kit >= NKITS)
        return;
    kit_ok[kit / 32] |= 1u << (kit % 32);
    defaults(bm_kit_rec[kit]);
    if (len != NREC)
        return;
    bm_kit_rec[kit][0] = rec[0] & 15;
    for (int v = 0; v < 4; v++)
        bm_kit_rec[kit][1 + v] = (uint8_t)((rec[1 + v] & 7) | (rec[1 + v] & 8));
}

/* ---- random numbers -------------------------------------------------------------------------- */

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

static int rr(int n)
{
    return n > 1 ? (int)(rnd() % (uint32_t)n) : 0;
}

static int chance(int pct)
{
    return rr(100) < pct;
}

/* ---- the generator ------------------------------------------------------------------------------- */

/* a slice of kind t (at random), else `fallback` */
static int pick(int t, int fallback)
{
    int c[16], n = 0;
    if (BM->ready)
        for (int s = 0; s < 16; s++)
            if (BM->type[s] == t)
                c[n++] = s;
    return n ? c[rr(n)] : fallback;
}

/* the break's own kinds if it has a kick and a snare, else by position (kick 1, snares 5 and 13) */
static int typed(void)
{
    int k = 0, s = 0;
    if (BM->ready)
        for (int i = 0; i < 16; i++) {
            k |= BM->type[i] == BM_T_KICK;
            s |= BM->type[i] == BM_T_SNARE;
        }
    return k && s;
}

static int kick(void)
{
    return typed() ? pick(BM_T_KICK, 0) : (chance(50) ? 0 : 8);
}

static int snare(void)
{
    return typed() ? pick(BM_T_SNARE, 4) : (chance(50) ? 4 : 12);
}

static int hat(int s)
{
    return typed() ? pick(BM_T_HAT, pick(BM_T_GHOST, s)) : s;
}

static int ghost(int s)
{
    return typed() ? pick(BM_T_GHOST, pick(BM_T_SNARE, s)) : s;
}

/* a character's step: on / off, slice, kind (bm_vel_kind) */
typedef struct {
    uint8_t on[16], slice[16], kind[16];
} line_t;

static void hit(line_t *l, int s, int slice, int kind)
{
    l->on[s] = 1;
    l->slice[s] = (uint8_t)slice;
    l->kind[s] = (uint8_t)kind;
}

/* the character's whole bar */
static void make(line_t *l, int chr)
{
    for (int s = 0; s < 16; s++) {
        l->on[s] = 0;
        l->slice[s] = (uint8_t)s;
        l->kind[s] = BM_V_PLAIN;
    }
    switch (chr) {
    case BM_C_STRAIGHT:                 /* the break, a few swaps */
        for (int s = 0; s < 16; s++)
            hit(l, s, s, BM_V_PLAIN);
        for (int i = 0, n = 2 + rr(2); i < n; i++) {
            int s = 1 + 2 * rr(8);             /* an off-beat 16th */
            int t = typed() ? BM->type[s & 15] : BM_T_HAT;
            hit(l, s & 15, pick(t, rr(16)), BM_V_PLAIN);
        }
        break;
    case BM_C_SPARSE:                   /* kick and snare, room between */
        hit(l, 0, kick(), BM_V_PLAIN);
        hit(l, 4, snare(), BM_V_PLAIN);
        hit(l, chance(50) ? 10 : 8, kick(), BM_V_PLAIN);
        hit(l, 12, snare(), BM_V_PLAIN);
        if (chance(50))
            hit(l, 2 * rr(8) + 1, hat(7), BM_V_GHOST);
        break;
    case BM_C_BOOMBAP:                  /* kick doubles, heavy snare on 2 and 4, 8th hats */
        for (int s = 2; s < 16; s += 4)
            hit(l, s, hat(s), BM_V_PLAIN);
        hit(l, 0, kick(), BM_V_PLAIN);
        hit(l, chance(50) ? 7 : 3, kick(), BM_V_PLAIN);
        hit(l, 8, kick(), BM_V_PLAIN);
        hit(l, chance(60) ? 10 : 11, kick(), BM_V_PLAIN);
        hit(l, 4, snare(), BM_V_PLAIN);
        hit(l, 12, snare(), BM_V_PLAIN);
        break;
    case BM_C_HALFTIME:                 /* snare on 3 */
        for (int s = 2; s < 16; s += 2)
            hit(l, s, hat(s), BM_V_PLAIN);
        hit(l, 0, kick(), BM_V_PLAIN);
        if (chance(60))
            hit(l, 6, kick(), BM_V_PLAIN);
        hit(l, 8, snare(), BM_V_PLAIN);
        if (chance(50))
            hit(l, 14, ghost(14), BM_V_GHOST);
        break;
    case BM_C_TWOSTEP:                  /* kick on 1 and the "and" of 3, snares on 2 and 4 */
        for (int s = 2; s < 16; s += 2)
            hit(l, s, hat(s), BM_V_PLAIN);
        hit(l, 0, kick(), BM_V_PLAIN);
        hit(l, 10, kick(), BM_V_PLAIN);
        hit(l, 4, snare(), BM_V_PLAIN);
        hit(l, 12, snare(), BM_V_PLAIN);
        if (chance(60))
            hit(l, chance(50) ? 7 : 15, ghost(7), BM_V_GHOST);
        break;
    case BM_C_JUNGLE: {                 /* shuffled kicks, ghosts, a snare roll before the bar's end */
        static const uint8_t KS[5] = {2, 3, 10, 11, 13};
        for (int s = 1; s < 16; s += 2)
            hit(l, s, hat(s), BM_V_PLAIN);
        hit(l, 0, kick(), BM_V_PLAIN);
        hit(l, KS[rr(5)], kick(), BM_V_PLAIN);
        hit(l, KS[rr(5)], kick(), BM_V_PLAIN);
        hit(l, 4, snare(), BM_V_PLAIN);
        hit(l, 12, snare(), BM_V_PLAIN);
        static const uint8_t GS[5] = {6, 7, 9, 14, 15};
        for (int i = 0, n = 2 + rr(2); i < n; i++) {
            int s = GS[rr(5)];
            hit(l, s, ghost(s), BM_V_GHOST);
        }
        if (chance(60))
            hit(l, 14 + rr(2), snare(), chance(50) ? BM_V_ROLL3 : BM_V_ROLL4);
        break;
    }
    case BM_C_FILLS:                    /* the break, then a busy last quarter */
        for (int s = 0; s < 12; s++)
            hit(l, s, s, BM_V_PLAIN);
        for (int s = 12; s < 16; s++) {
            static const uint8_t K[4] = {BM_V_PLAIN, BM_V_ROLL2, BM_V_ROLL3, BM_V_REVERSE};
            hit(l, s, chance(70) ? snare() : kick(), K[rr(4)]);
        }
        break;
    default: {                          /* BREAKCORE: anything, dense, rolls and reverses */
        static const uint8_t K[8] = {BM_V_PLAIN, BM_V_PLAIN, BM_V_PLAIN, BM_V_ROLL2, BM_V_ROLL3, BM_V_ROLL4,
                                     BM_V_REVERSE, BM_V_REVROLL};
        for (int s = 0; s < 16; s++)
            if (s == 0 || chance(85))
                hit(l, s, s == 0 ? kick() : rr(16), s == 0 ? BM_V_PLAIN : K[rr(8)]);
        break;
    }
    }
}

static uint8_t *track_block(int v)
{
    return *(uint8_t **)(fw_voice_state(v) + FW_VS_BLOCK);
}

static int algo_run(int v)              /* RUN or GENERATE (not CHOP), by the knob's own position */
{
    return FW_U16(fw_voice_state(v) + FW_VS_BASE + 2 * FW_SLOT_ALGO) != POS3;
}

#define LOCK(blk, s)    ((blk) + FW_BLK_LOCKS + FW_LOCK_SIZE * (s))
#define LMASK(lk)       FW_U16((uintptr_t)(lk) + 2 * FW_NSLOTS)
#define LP2(lk)         FW_U16((uintptr_t)(lk) + 2 * FW_SLOT_PARAM2)

static void save_undo(int v, uint8_t *blk)
{
    undo.kind = 1;
    undo.voice = (uint8_t)v;
    undo.blk = blk;
    undo.mask = 0;
    for (int s = 0; s < 16; s++) {
        const uint8_t *r = blk + FW_STEP_SIZE * s, *lk = LOCK(blk, s);
        undo.on[s] = r[0];
        undo.note[s] = r[6];
        undo.vel[s] = r[7];
        undo.p2[s] = LP2(lk);
        if (LMASK(lk) >> FW_SLOT_PARAM2 & 1)
            undo.mask |= (uint16_t)(1u << s);
    }
}

/* a step written: on (velocity, PARAM2 lock to `slice`, or no lock: -1) or off */
static void put_step(uint8_t *blk, int s, int on, int slice, int vel)
{
    uint8_t *r = blk + FW_STEP_SIZE * s, *lk = LOCK(blk, s);
    if (!on) {
        r[0] = 0;
        LMASK(lk) &= (uint16_t)~(1u << FW_SLOT_PARAM2);
        return;
    }
    r[0] = 1;
    if (!r[6])
        r[6] = 36;
    r[7] = (uint8_t)vel;
    if (slice >= 0) {
        LP2(lk) = (uint16_t)(bm_p2_of_slice(slice) << 4);
        LMASK(lk) |= (uint16_t)(1u << FW_SLOT_PARAM2);
    } else {
        LMASK(lk) &= (uint16_t)~(1u << FW_SLOT_PARAM2);
    }
}

/* the bar into voice v's track. RUN / GENERATE: a trigger only where the break wouldn't play that
 * slice anyway (it carries on from each jump; from the loop's first step in place) or where the
 * step plays other than plain; CHOP: each hit */
static void write_line(int v, const line_t *l)
{
    uint8_t *blk = track_block(v);
    if (!blk)
        return;
    int first = blk[FW_BLK_FIRST] & 15, last = blk[FW_BLK_LAST] & 15;
    int run = algo_run(v), offset = 0;
    uint32_t pm = pk_irq_off();
    for (int s = 0; s < 16; s++) {
        if (s == first)
            offset = 0;
        int on = l->on[s];
        if (run && on && s >= first && s <= last) {
            int auto_s = (s + offset) & 15;
            on = l->slice[s] != auto_s || l->kind[s] != BM_V_PLAIN;
            if (on)
                offset = (l->slice[s] - s) & 15;
        } else if (run) {
            on = 0;
        }
        put_step(blk, s, on, l->slice[s], BM_VEL_OF[l->kind[s]]);
    }
    pk_irq_restore(pm);
}

/* the steps as they are: on (in CHOP; in RUN the bar as the break plays it), slice, kind */
static void read_line(int v, const uint8_t *blk, line_t *l)
{
    int first = blk[FW_BLK_FIRST] & 15, run = algo_run(v), offset = 0;
    for (int s = 0; s < 16; s++) {
        if (s == first)
            offset = 0;
        const uint8_t *r = blk + FW_STEP_SIZE * s, *lk = LOCK(blk, s);
        int on = r[0] == 1, sl = (s + offset) & 15;
        if (on && LMASK(lk) >> FW_SLOT_PARAM2 & 1) {
            int z = bm_p2_zone(LP2(lk) >> 4);
            sl = z ? z - 1 : s;
        }
        if (on && run)
            offset = (sl - s) & 15;
        l->on[s] = (uint8_t)(run ? 1 : on);
        l->slice[s] = (uint8_t)sl;
        l->kind[s] = (uint8_t)(on ? bm_vel_kind(r[7]) : BM_V_PLAIN);
    }
}

static void action(int v, int a)
{
    if (a == A_UNDO) {
        if (undo.kind == 1 && undo.voice == v && undo.blk == track_block(v)) {
            uint32_t pm = pk_irq_off();
            for (int s = 0; s < 16; s++) {
                uint8_t *r = undo.blk + FW_STEP_SIZE * s, *lk = LOCK(undo.blk, s);
                r[0] = undo.on[s];
                r[6] = undo.note[s];
                r[7] = undo.vel[s];
                LP2(lk) = undo.p2[s];
                if (undo.mask >> s & 1)
                    LMASK(lk) |= (uint16_t)(1u << FW_SLOT_PARAM2);
                else
                    LMASK(lk) &= (uint16_t)~(1u << FW_SLOT_PARAM2);
            }
            pk_irq_restore(pm);
        }
        undo.kind = 0;
        return;
    }
    uint8_t *blk = track_block(v);
    if (!blk)
        return;
    seed();
    save_undo(v, blk);
    int chr = bm_cur[1 + v] & 7;
    line_t l, now;
    if (a == A_CLEAR) {                 /* the break as it is: no steps of ours */
        for (int s = 0; s < 16; s++) {
            l.on[s] = 0;
            l.slice[s] = (uint8_t)s;
            l.kind[s] = BM_V_PLAIN;
        }
        if (!algo_run(v))
            for (int s = 0; s < 16; s++)
                l.on[s] = 1;
        write_line(v, &l);
        return;
    }
    make(&l, chr);
    if (a == A_RHYTHM || a == A_VARY) {
        read_line(v, blk, &now);
        int any = 0;
        for (int s = 0; s < 16; s++)
            any |= now.on[s];
        if (any) {
            if (a == A_RHYTHM) {        /* the steps that are on, drawn again (the character's slice
                                         * where it has one, else one of its kind) */
                for (int s = 0; s < 16; s++) {
                    if (!now.on[s])
                        continue;
                    if (!l.on[s]) {
                        int t = typed() ? BM->type[now.slice[s]] : BM_T_HAT;
                        l.slice[s] = (uint8_t)pick(t, now.slice[s]);
                        l.kind[s] = BM_V_PLAIN;
                    }
                    l.on[s] = 1;
                }
                for (int s = 0; s < 16; s++)
                    if (!now.on[s])
                        l.on[s] = 0;
            } else {                    /* a few of the steps that play, drawn again */
                int idx[16], n = 0;
                for (int s = 0; s < 16; s++)
                    if (now.on[s])
                        idx[n++] = s;
                for (int i = 0; i < 3 && n > 0; i++) {
                    int j = rr(n), s = idx[j];
                    idx[j] = idx[--n];
                    int t = typed() ? BM->type[now.slice[s]] : BM_T_HAT;
                    now.slice[s] = (uint8_t)pick(t, rr(16));
                    now.kind[s] = l.kind[s];
                }
                for (int s = 0; s < 16; s++) {
                    l.on[s] = now.on[s];
                    l.slice[s] = now.slice[s];
                    l.kind[s] = now.kind[s];
                }
            }
        }
    }
    write_line(v, &l);
}

/* ---- the main loop --------------------------------------------------------------------------- */

void PK_NEXT(pk_bm_main_loop)(void);

void pk_bm_main_loop(void)
{
    PK_NEXT(pk_bm_main_loop)();
    if (pending) {
        uint32_t p = pending - 1;
        pending = 0;
        action((int)(p >> 4), (int)(p & 15));
    }
    if (dirty)
        flush();
    if (avail_bank != bank_now + 1 && lstate == L_IDLE)
        refresh_avail();
    load_step();
    if (seq()[FW_SEQ_PLAY] != 1)        /* stopped (the ticks may not run): tell the M7 */
        for (int v = 0; v < 4; v++)
            if (sent_play[v]) {
                sent_play[v] = 0;
                last_step[v] = -1;
                send_step(v, 0, 0);
            }
}

/* ---- held steps (main page) ------------------------------------------------------------------- */

static int held_ours(uint32_t m[2])
{
    m[0] = m[1] = 0;
    for (int k = 0; k < 64; k++)
        if (fw_key_held(k) && ours(k / 16))
            m[k >> 5] |= 1u << (k & 31);
    return (m[0] | m[1]) != 0;
}

int PK_NEXT(pk_bm_key_event)(const uint8_t *evt);

static uint32_t swallow_shuffle;        /* our SHUFFLE press opened the page: its release is ours too */

int pk_bm_key_event(const uint8_t *evt)
{
    if (evt[1] == FW_EV_UP && evt[0] == FW_KEY_SHUFFLE && swallow_shuffle) {
        swallow_shuffle = 0;            /* (stock: SHUFFLE's release goes to the main page) */
        return 1;
    }
    if (evt[1] == FW_EV_DOWN && evt[0] == FW_KEY_SHUFFLE && fw_get_mode() == FW_MODE_MAIN) {
        uint32_t m[2];
        if (held_ours(m)) {
            bm_held[0] = m[0];
            bm_held[1] = m[1];
            for (int k = 0; k < 64; k++)
                if (m[k >> 5] >> (k & 31) & 1) {
                    editing = (uint32_t)(k / 16);
                    break;
                }
            fw_set_mode(PK_PAGE_BREAK_MAKER);
            swallow_shuffle = 1;
            return 1;
        }
    }
    return PK_NEXT(pk_bm_key_event)(evt);
}

/* ---- the configuration page ------------------------------------------------------------------- */

static int any_held(void)
{
    return (bm_held[0] | bm_held[1]) != 0;
}

static int is_held(int k)
{
    return bm_held[k >> 5] >> (k & 31) & 1;
}

/* the first held step's kind (bm_vel_kind), or -1 */
static int held_kind(void)
{
    for (int k = 0; k < 64; k++)
        if (is_held(k)) {
            uint8_t *blk = track_block(k / 16);
            return blk ? bm_vel_kind(blk[FW_STEP_SIZE * (k % 16) + 7]) : -1;
        }
    return -1;
}

static void set_held(int kind, int slice)
{
    uint32_t pm = pk_irq_off();
    for (int k = 0; k < 64; k++) {
        if (!is_held(k))
            continue;
        uint8_t *blk = track_block(k / 16);
        if (!blk)
            continue;
        uint8_t *r = blk + FW_STEP_SIZE * (k % 16);
        if (kind >= 0) {
            r[7] = BM_VEL_OF[kind];
        }
        if (slice >= 0) {
            uint8_t *lk = LOCK(blk, k % 16);
            LP2(lk) = (uint16_t)(bm_p2_of_slice(slice) << 4);
            LMASK(lk) |= (uint16_t)(1u << FW_SLOT_PARAM2);
        }
    }
    pk_irq_restore(pm);
}

static void ask(int v, int a)
{
    uint32_t pm = pk_irq_off();
    pending = 1u + ((uint32_t)v << 4 | (uint32_t)a);
    pk_irq_restore(pm);
}

static const uint8_t TYPE_BTN[5] = {BM_V_GHOST, BM_V_REVERSE, BM_V_ROLL2, BM_V_ROLL3, BM_V_ROLL4};

int pk_bm_page_key(const uint8_t *evt)
{
    if (evt[1] != FW_EV_DOWN && evt[1] != FW_EV_UP)
        return 0;
    int k = evt[0], down = evt[1] == FW_EV_DOWN, v = (int)editing;
    if (k < 0x40) {
        int row = k / 16, col = k % 16;
        if (row == 1 && col < BM_NPAD) {        /* a pad, while held */
            uint16_t b = (uint16_t)(1u << col);
            bm_pads[v] = down ? (uint16_t)(bm_pads[v] | b) : (uint16_t)(bm_pads[v] & ~b);
            mark(v);
            return 1;
        }
        if (row == 3 && col >= 12) {            /* an action: on release; held: undo */
            if (down) {
                act_key = (uint32_t)k + 1;
                act_at = fw_get_tick();
                act_done = 0;
            } else if ((uint32_t)k + 1 == act_key) {
                if (!act_done)
                    ask(v, col - 12);
                act_key = 0;
            }
            return 1;
        }
        if (!down)
            return 1;
        if (row == 0) {                         /* the break */
            bm_cur[0] = (uint8_t)col;
        } else if (row == 1) {                  /* a step type for the held steps */
            int want = TYPE_BTN[col - BM_NPAD], now = held_kind();
            if (now == want)
                want = BM_V_PLAIN;
            else if ((now == BM_V_REVERSE && want == BM_V_ROLL2) || (now == BM_V_ROLL2 && want == BM_V_REVERSE))
                want = BM_V_REVROLL;
            if (any_held())
                set_held(want, -1);
        } else if (row == 2) {                  /* a slice: the held steps', or heard */
            if (any_held()) {
                set_held(-1, col);
            } else {
                bm_aud[v] = (uint8_t)((col + 1) | ((bm_aud[v] + 0x20) & 0xe0));
                mark(v);
            }
        } else if (col < 8) {                   /* the character */
            bm_cur[1 + v] = (uint8_t)((bm_cur[1 + v] & ~7) | col);
            mark(v);
        } else if (col < 10) {                  /* pitch: repitch / keep length */
            bm_cur[1 + v] = (uint8_t)((bm_cur[1 + v] & 7) | (col == 9 ? 8 : 0));
            mark(v);
        }
        return 1;
    }
    if (k == FW_KEY_SHUFFLE) {
        if (down)
            fw_set_mode(FW_MODE_MAIN);
        return 1;
    }
    if (k == FW_KEY_SHIFT || k == FW_KEY_KIT || k == FW_KEY_MOD) {
        if (down)
            fw_set_mode(PK_PAGE_ENGINES);
        return 1;
    }
    return 0;                                   /* the trigger keys play; other page buttons leave */
}

void pk_bm_page_enter(void)
{
    if (fw_get_mode() == PK_PAGE_BREAK_MAKER && pk_vm_settings_voice < 4 && !any_held())
        editing = pk_vm_settings_voice;
    if (editing > 3)
        editing = 0;
    active = 1;
    act_key = 0;
}

void pk_bm_page_draw(void)
{
    if (act_key && !act_done && fw_get_tick() - act_at >= UNDO_MS) {
        act_done = 1;
        ask((int)editing, A_UNDO);
    }
    int v = (int)editing, hk = any_held() ? held_kind() : -1;
    for (int k = 0; k < 0x40; k++) {
        int row = k / 16, col = k % 16, lv = 0;
        if (row == 0)
            lv = col == (bm_cur[0] & 15) ? LED_ON : (avail >> col & 1 ? LED_DIM : 0);
        else if (row == 1 && col < BM_NPAD)
            lv = bm_pads[v] >> col & 1 ? LED_ON : LED_DIM;
        else if (row == 1) {
            int t = TYPE_BTN[col - BM_NPAD];
            int lit = hk == t || (hk == BM_V_REVROLL && (t == BM_V_REVERSE || t == BM_V_ROLL2));
            lv = hk < 0 ? 0 : (lit ? LED_ON : LED_DIM);
        } else if (row == 2)
            lv = BM->ready ? (BM->type[col] == BM_T_KICK || BM->type[col] == BM_T_SNARE ? LED_MID : LED_DIM) : 0;
        else if (col < 8)
            lv = col == (bm_cur[1 + v] & 7) ? LED_ON : LED_DIM;
        else if (col < 10)
            lv = (col == 9) == ((bm_cur[1 + v] >> 3) & 1) ? LED_ON : LED_DIM;
        else if (col >= 12)
            lv = (uint32_t)k + 1 == act_key ? LED_ON : LED_DIM;
        fw_set_led(k, (uint8_t)lv);
    }
}

void pk_bm_page_leave(void)
{
    active = 0;
    bm_held[0] = bm_held[1] = 0;
    int v = (int)editing;
    if (bm_pads[v]) {                      /* a pad still held: let go */
        bm_pads[v] = 0;
        mark(v);
    }
    int bank = FW_U8(SYM_bank_number), kit = current_kit();
    if (kit < 0 || kit >= NKITS)
        return;
    for (int i = 0; i < NREC; i++)
        if (bm_kit_rec[kit][i] != bm_cur[i] || !(kit_ok[kit / 32] >> (kit % 32) & 1)) {
            pk_store_save_kit(bank, kit);
            return;
        }
}
