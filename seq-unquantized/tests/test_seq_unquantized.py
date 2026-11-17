"""seq-unquantized: the trigger keys record the step playing and its delay, as played."""
import os
import sys
import tempfile
import unittest

PERKONS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'perkons')
sys.path[:0] = [os.path.join(PERKONS, 'tools'), os.path.join(PERKONS, 'tests')]
os.environ.setdefault('PERKONS_MODS', os.path.join(PERKONS, '..', 'perkons-re-mods'))

from fixtures import build_patched, require_image  # noqa: E402
from perkons.emu import Symbols  # noqa: E402
from perkons.fwimage import Image  # noqa: E402
from perkons.rig import M4Rig  # noqa: E402

SYM = Symbols(os.path.join(PERKONS, 'symbols', 'v1.3.0-m4.json'))
REC = SYM.g['rec_flags'] + 1
TRIG = 0x4b                   # trigger key 1
DC = SYM.g['device_config']
MIDI_MSG = 0x2407fe00         # scratch for a MIDI message
STEP_TICKS = 24
_image = []


def image():
    if not _image:
        img, p = build_patched(Image.load(require_image('v1.3.0')), ['seq_unquantized'], tempfile.mkdtemp())
        _image.append((img, p.cave_symbols['m4host']))
    return _image[0]


class Rig(M4Rig):
    """Clocked one sub-tick at a time, so a key can land anywhere in a step."""

    def __init__(self, on=True):
        super().__init__(*image())
        self.e.w8(self.cave['pk_cfg_unquantized'], 1 if on else 0)
        self.sub = 0
        self.fired = []           # (step, tick) of each trigger the sequencer sent for track 1
        # the MIDI input as the firmware sets it up at init: channels and callbacks
        self.e.call(self.sym.fn['midi_in_init'], self.sym.g['midi_channels'], 4, self.sym.g['midi_callbacks'],
                    max_insns=5_000_000)

    def tick(self):
        c = self.clock
        n = self.r16(c + 0x1c) * self.r16(c + 0x1e)
        if self.sub % n == 0:
            self.w16(c + 0x22, 0)
            self.w16(c + 0x20, self.r16(c + 0x20) + 1)
            self.e.call(self.e.r32(c + 0x3c) & ~1, 1, max_insns=5_000_000)
        else:
            self.subtick()
        self.sub += 1
        vs = self.voice_state(0)
        if any(p[0] for typ, p in self.e.sent(2) if typ == 0):
            self.fired.append((self.r16(vs + 0x1c), self.r16(vs + 0x24)))

    def to(self, step, tick):
        """Clock until track 1 reaches `tick` ticks into `step` (the next time it gets there)."""
        vs = self.voice_state(0)
        for _ in range(288 * 40):
            was = (self.r16(vs + 0x1c), self.r16(vs + 0x24))
            self.tick()
            if was != (step, tick) == (self.r16(vs + 0x1c), self.r16(vs + 0x24)):
                return
        raise AssertionError(f'never reached step {step} tick {tick}')

    def hit(self, track=0):
        """A trigger key tap. The voice plays at once (sent on the next sub-tick): that
        one isn't counted in `fired`."""
        self.event(TRIG + track, True)
        self.event(TRIG + track, False)
        self.e.sent(2)
        n = len(self.fired)
        self.tick()
        del self.fired[n:]

    def note(self, note, vel=100, ch=0):
        """A MIDI note-on through the MIDI input (the voice plays at once, not counted in `fired`)."""
        self.e.uc.mem_write(MIDI_MSG, bytes([0x90 | ch, note, vel, 0]))
        self.e.call(self.sym.fn['midi_dispatch'], MIDI_MSG, max_insns=5_000_000)
        self.e.sent(2)
        n = len(self.fired)
        self.tick()
        del self.fired[n:]

    def rec(self, on=True):
        self.e.w8(REC, 2 if on else 0)

    def step_rec(self, step, track=0):
        a = self.block(track) + 8 * step
        return {'on': self.e.r8(a), 'delay': self.r16(a + 2), 'vel': self.e.r8(a + 7)}

    def step_note(self, step, track=0):
        return self.e.r8(self.block(track) + 8 * step + 6)

    def start(self):
        self.play()
        self.tick()


class TestUnquantized(unittest.TestCase):
    def test_config_key_led_off_quantized_on_unquantized(self):
        """Row 3 step 13 on the config page: off at power-on (quantized, as stock), a press
        turns it on (unquantized) with its LED lit, another press off again."""
        r = M4Rig(*image())
        key = 0x20 + 12
        evt = 0x2407ff40

        def press():
            r.e.uc.mem_write(evt, bytes([key, 0x11, 0, 0, 0, 0, 0, 0]))
            r.e.call(r.cave['pk4_cfg_event'], evt, max_insns=5_000_000)

        def led():
            r.e.call(r.cave['pk4_cfg_render'], 0, 0, 0, 0, max_insns=5_000_000)
            return r.leds.get(key)

        self.assertEqual((r.e.r8(r.cave['pk_cfg_unquantized']), led()), (0, 0))
        press()
        self.assertEqual((r.e.r8(r.cave['pk_cfg_unquantized']), led()), (1, 0x80))
        press()
        self.assertEqual((r.e.r8(r.cave['pk_cfg_unquantized']), led()), (0, 0))

    def test_a_hit_records_the_step_playing_and_how_late_it_was(self):
        r = Rig()
        r.start()
        r.rec()
        r.to(5, 7)
        r.hit()
        self.assertEqual(r.step_rec(5), {'on': 1, 'delay': 30, 'vel': 0x7f})   # 30 % of 24 ticks: tick 7
        r.fired.clear()
        r.to(5, 23)                       # the rest of this pass: it doesn't play again
        self.assertEqual(r.fired, [])
        r.to(6, 0)
        r.to(5, 23)                       # next time round: at the tick it was played
        self.assertEqual(r.fired, [(5, 7)])

    def test_every_tick_plays_back_where_it_was_played(self):
        for first in (0, 1):            # even ticks, then odd: all 24
            with self.subTest(first=first):
                r = Rig()
                r.start()
                r.rec()
                want = list(enumerate(range(first, 24, 2)))
                for step, tick in want:
                    r.to(step, tick)
                    r.hit()
                r.fired.clear()
                r.to(15, 23)
                r.to(15, 23)            # a whole pass
                self.assertEqual(r.fired, want)

    def test_backwards(self):
        r = Rig()
        r.e.w8(r.block(0) + 0x82, 1)    # play mode BWD
        r.start()
        r.rec()
        r.to(9, 17)
        r.hit()
        r.fired.clear()
        r.to(9, 0)
        r.to(9, 0)
        self.assertEqual(r.fired, [(9, 17)])

    def test_late_in_a_step_stays_in_that_step(self):
        """Stock moves a hit in a step's second half to the next step, on the grid."""
        r = Rig()
        r.start()
        r.rec()
        r.to(3, 20)
        r.hit()
        self.assertEqual(r.step_rec(3)['on'], 1)
        self.assertEqual(r.step_rec(4)['on'], 0)
        r.fired.clear()
        r.to(3, 19)
        r.to(4, 0)
        self.assertEqual(r.fired, [(3, 20)])

    def test_other_track_rates(self):
        for rate in (2, 5, 7):          # /2, x2, x4
            with self.subTest(rate=rate):
                r = Rig()
                r.e.w8(r.block(0) + 0x86, rate)
                r.start()
                r.rec()
                r.to(2, 13)
                r.hit()
                self.assertEqual(r.step_rec(2)['delay'], 55)
                r.fired.clear()
                r.to(3, 0)
                self.assertEqual(r.fired, [])
                r.to(3, 0)
                self.assertEqual(r.fired, [(2, 13)])

    def test_off_is_stock(self):
        r = Rig(on=False)
        r.start()
        r.rec()
        r.to(3, 20)
        r.hit()
        self.assertEqual((r.step_rec(3)['on'], r.step_rec(4)['on']), (0, 1), 'the nearest step')
        self.assertEqual(r.step_rec(4)['delay'], 0)
        r.to(6, 3)
        r.hit()
        self.assertEqual(r.step_rec(6), {'on': 1, 'delay': 0, 'vel': 0x7f})

    def test_without_rec_nothing_is_written(self):
        r = Rig()
        r.start()
        r.to(5, 7)
        r.hit()
        self.assertEqual(r.step_rec(5)['on'], 0)

    def test_stopped_plays_the_voice_and_writes_nothing(self):
        r = Rig()
        r.rec()
        r.hit()
        self.assertEqual([r.step_rec(s)['on'] for s in range(16)], [0] * 16)

    def test_other_tracks(self):
        r = Rig()
        r.start()
        r.rec()
        r.to(9, 11)
        r.hit(track=3)
        self.assertEqual(r.step_rec(9, track=3), {'on': 1, 'delay': 46, 'vel': 0x7f})
        self.assertEqual(r.step_rec(9)['on'], 0)


class TestMidi(unittest.TestCase):
    """MIDI notes record through the MIDI input's own path (single channel by default:
    notes 36-39 play voices 1-4)."""

    def test_a_note_records_the_step_playing_and_how_late_it_was(self):
        for tick, delay in ((7, 30), (20, 84)):         # stock: this step / the next
            with self.subTest(tick=tick):
                r = Rig()
                r.start()
                r.rec()
                r.to(5, tick)
                r.note(36)
                self.assertEqual(r.step_rec(5), {'on': 1, 'delay': delay, 'vel': 0x7f})
                self.assertEqual(r.step_rec(6)['on'], 0)
                r.fired.clear()
                r.to(5, 23)
                self.assertEqual(r.fired, [])
                r.to(6, 0)
                r.to(5, 23)
                self.assertEqual(r.fired, [(5, tick)])

    def test_late_note_leaves_the_next_step_as_it_was(self):
        r = Rig()
        a = r.block(0) + 8 * 6
        r.e.uc.mem_write(a, bytes([1, 10, 50, 0, 0, 0, 40, 60]))     # on, (+1 as a new step), delay 50 %, note 40, velocity 60
        r.start()
        r.rec()
        r.to(5, 20)
        r.note(36)
        self.assertEqual(bytes(r.e.uc.mem_read(a, 8)), bytes([1, 10, 50, 0, 0, 0, 40, 60]))
        self.assertEqual(r.step_rec(5), {'on': 1, 'delay': 84, 'vel': 0x7f})
        r.fired.clear()
        r.to(6, 23)
        self.assertEqual(r.fired, [(6, 12)], 'the next step still plays this pass')

    def test_multi_channel_note_and_velocity(self):
        r = Rig()
        r.e.w8(DC + 0x1d, 1)                            # multi channel: voice n on channel n
        r.e.w8(DC + 0x1b, 1)                            # midi-velocity
        for v in range(4):
            r.e.call(r.sym.fn['midi_in_channel'], v, v, max_insns=100_000)
        r.start()
        r.rec()
        for step, tick in ((3, 5), (8, 18)):
            r.to(step, tick)
            r.note(48 + step, vel=90, ch=1)
        for step, delay in ((3, 21), (8, 75)):
            self.assertEqual(r.step_rec(step, track=1), {'on': 1, 'delay': delay, 'vel': 90})
            self.assertEqual(r.step_note(step, track=1), 48 + step)
        self.assertEqual([r.step_rec(s, track=1)['on'] for s in (4, 9)], [0, 0])

    def test_the_note_picks_the_voice(self):
        r = Rig()
        r.start()
        r.rec()
        r.to(9, 22)
        r.note(38)
        self.assertEqual(r.step_rec(9, track=2)['delay'], 92)
        self.assertEqual([r.step_rec(s, track=t)['on'] for t in (0, 2) for s in (9, 10)], [0, 0, 1, 0])

    def test_other_notes_record_nothing(self):
        r = Rig()
        r.start()
        r.rec()
        r.to(5, 20)
        r.note(60)
        self.assertEqual([r.step_rec(s, track=t)['on'] for t in range(4) for s in range(16)], [0] * 64)

    def test_off_is_stock(self):
        r = Rig(on=False)
        r.start()
        r.rec()
        r.to(5, 20)
        r.note(36)
        self.assertEqual((r.step_rec(5)['on'], r.step_rec(6)), (0, {'on': 1, 'delay': 0, 'vel': 0x7f}))

    def test_without_rec_nothing_is_written(self):
        r = Rig()
        r.start()
        r.to(5, 7)
        r.note(36)
        self.assertEqual([r.step_rec(s)['on'] for s in range(16)], [0] * 16)


if __name__ == '__main__':
    unittest.main()
