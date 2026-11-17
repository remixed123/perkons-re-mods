/* seq-unquantized: the trigger keys and MIDI notes record where they were played, not on
 * the nearest step.
 *
 * On (config row 3 step 13), while playing with REC on: a trigger key or a MIDI note writes
 * the step playing now and its delay (the % of a step the step plays late, the value
 * shuffle sets) from how far into the step it was played. The delay is in the track's own
 * clock ticks (24 per step at every rate), so the timing holds at other tempos and rates.
 * The hit has already sounded and its tick has passed, so the step doesn't play again this
 * time round. Off: stock (the nearest step, its delay left as it was). */
#include <stdint.h>
#include "pk_host.h"
#include "fw_m4_seq.h"

#define seq_play_state      FW_CALL(SYM_seq_play_state, int (*)(void))     /* 1: playing */

PK_AXI uint8_t pk_cfg_unquantized;          /* system/mods.json "unquantized" */
PK_AXI static uint8_t played;               /* the trigger key being handled: row + 1, 0 none */

static int recording(void)
{
    return pk_cfg_unquantized && FW_U8(SYM_rec_flags + FW_REC_ON);
}

static uint32_t step_ticks(uint8_t *vs)
{
    return FW_U16(FW_U32(vs + FW_VS_CLOCK) + FW_CLK_STEP_TICKS);
}

/* the step track `vs` is in, and the delay that plays it at the tick it's at now; -1 none */
static int delay_now(uint8_t *vs, int *step)
{
    uint32_t per = step_ticks(vs);
    uint32_t tick = FW_U16(vs + FW_VS_TICK);
    *step = (int16_t)FW_U16(vs + FW_VS_STEP);
    if (per == 0 || *step < 0 || *step >= FW_STEPS)
        return -1;
    if (tick >= per)
        tick = per - 1;
    uint32_t delay = (100 * tick + per - 1) / per;       /* the player: delay * per / 100 = tick */
    return delay > 99 ? 99 : (int)delay;
}

static uint8_t *step_rec(uint8_t *vs, int step)
{
    return (uint8_t *)(uintptr_t)FW_U32(vs + FW_VS_BLOCK) + step * FW_STEP_SIZE;
}

/* the delay before `on`: a clock tick in between finds the step's time already passed */
static void set_delay(uint8_t *rec, int delay)
{
    *(volatile uint16_t *)(rec + FW_STEP_DELAY) = (uint16_t)delay;
}

/* the global key handler (chained): which trigger key key_event is handling */
int PK_NEXT(pk_uq_key_event)(const uint8_t *evt);
int pk_uq_key_event(const uint8_t *evt)
{
    int trig = evt[0] - FW_KEY_TRIG(0);
    played = evt[1] == FW_EV_DOWN && trig >= 0 && trig < 4 ? (uint8_t)(trig + 1) : 0;
    int r = PK_NEXT(pk_uq_key_event)(evt);
    played = 0;
    return r;
}

/* key_event, right after: stock writes the nearest step when this returns 1 (playing) and
 * REC is on. Recording unquantized, write the step here and return 0: stock then stops. */
int PK_NEXT(pk_uq_playing)(void);
int pk_uq_playing(void)
{
    int state = PK_NEXT(pk_uq_playing)();
    if (state != 1 || !recording() || !played)
        return state;
    int step;
    uint8_t *vs = fw_voice_state(played - 1);
    int delay = delay_now(vs, &step);
    if (delay < 0)
        return state;
    uint8_t *rec = step_rec(vs, step);
    set_delay(rec, delay);
    rec[7] = 0x7f;
    rec[0] = 1;
    return 0;
}

/* MIDI note-on (chained; ch: the voice in multi channel mode, 4 and up in single channel
 * mode, where the note picks the voice). Stock records the nearest step, then plays the
 * voice. When that's this step, its delay is set first; when it's the next step, that one
 * is put back as it was afterwards and the note (with the velocity stock gave it) goes in
 * this one. */
void PK_NEXT(pk_uq_midi_note)(uint32_t ch, uint32_t note, uint32_t vel);
void pk_uq_midi_note(uint32_t ch, uint32_t note, uint32_t vel)
{
    int track = ch < 4 ? (int)ch : -1;
    for (int t = 0; track < 0 && t < 4; t++)
        if (FW_U8(SYM_device_config + FW_DC_TRIG_NOTE + t) == note)
            track = t;
    int step, delay = -1;
    uint8_t *vs = 0;
    if (track >= 0 && recording() && seq_play_state() == 1) {
        vs = fw_voice_state(track);
        delay = delay_now(vs, &step);
    }
    if (delay < 0) {
        PK_NEXT(pk_uq_midi_note)(ch, note, vel);
        return;
    }
    uint8_t *rec = step_rec(vs, step);
    int next = (int16_t)FW_U16(vs + FW_VS_NEXT);
    if (FW_U16(vs + FW_VS_SUBTICK) < step_ticks(vs) / 2 || next < 0 || next == step || next >= FW_STEPS) {
        set_delay(rec, delay);              /* stock writes this step */
        PK_NEXT(pk_uq_midi_note)(ch, note, vel);
        return;
    }
    uint8_t *later = step_rec(vs, next), was[FW_STEP_SIZE];
    for (int i = 0; i < FW_STEP_SIZE; i++)
        was[i] = later[i];
    uint8_t play_next = FW_U8(vs + FW_VS_PLAY_NEXT);
    PK_NEXT(pk_uq_midi_note)(ch, note, vel);
    if ((int16_t)FW_U16(vs + FW_VS_STEP) != step)
        return;                             /* the clock moved on meanwhile: as stock wrote it */
    uint8_t v = later[7], n = later[6];
    for (int i = 0; i < FW_STEP_SIZE; i++)
        later[i] = was[i];
    FW_U8(vs + FW_VS_PLAY_NEXT) = play_next;
    set_delay(rec, delay);
    if (ch < 4)
        rec[6] = n;
    rec[7] = v;
    rec[0] = 1;
}
