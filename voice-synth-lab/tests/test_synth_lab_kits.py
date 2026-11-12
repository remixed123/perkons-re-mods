"""Synth Lab with the M4 and the M7 running together: what the M4 sends, the M7 plays. A preset
pressed on the page, a sound sent before the output runs Synth Lab, kits changed while the
sequencer plays, and Mutate's nudges."""
import shutil
import struct
import unittest

from fixtures import SYMBOLS_V130
from perkons import verify
from perkons.emu import Symbols
from sy_rig import MUTATE, NFIELD, PRESET, ROOT, SCALE, pack
import test_synth_lab_page as page                        # (not its class: it'd run here too)

SOUND = 0xc0                                              # sy_t.v; the saved sound follows it


class TestSynthLabKits(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('arm-none-eabi-gcc'):
            raise unittest.SkipTest('arm-none-eabi-gcc not installed')

    def setUp(self):
        self.c = page.TestSynthLabPage()
        self.r = self.c.rig()
        self.m = verify.boot(page.image()[0], Symbols(SYMBOLS_V130))
        self.t = 0
        self.m.audio(960)

    def fwd(self):
        for st in (1, 2, 3):
            for t, p in self.r.e.sent(st):
                self.m.send(st, t, p)
        self.m.pump()

    def tick(self, n=1, playing=True):
        r = self.r
        for _ in range(n):
            r.e.call(r.sym.fn['lfo_knob_poll'], max_insns=5_000_000)
            r.ms += 5
            r.e.call(r.cave['pk_sy_main_loop'], max_insns=5_000_000)
            self.fwd()
            self.m.audio(256)
            self.t += 1
            if playing and self.t % 24 == 0:
                r.step()
                self.fwd()
                self.m.audio(64)

    def m7(self, out, saved=True):
        a = self.m.slot(out) + SOUND + (NFIELD if saved else 0)
        return [self.m.r8(a + i) for i in range(NFIELD)]

    def engine_on(self, voice):
        self.c.open_engines(self.r)
        self.c.press(self.r, 16 * voice + page.COL)
        self.tick(2, playing=False)

    def note(self, out=0):
        trig = [o == out for o in range(4)]
        self.m.send_triggers(trig, [(0, 0)] * 4, 127, 36)
        self.m.audio(64)

    def test_a_preset_press_reaches_the_m7(self):
        c, r = self.c, self.r
        self.engine_on(0)
        self.note()
        c.long_press(r, 0)
        self.assertEqual(r.mode(), page.SYNTH_LAB_PAGE)
        c.press(r, 16 * 2 + 6)                          # scale 7
        c.press(r, 16 * 0 + 12)                         # Sync Siren
        self.tick(2, playing=False)
        want = page.preset(12)
        want[SCALE] = 6
        self.assertEqual(self.m7(0), want)
        self.assertEqual(self.m7(0, saved=False), want, 'playing as saved (no Mutate)')

    def test_a_sound_sent_before_the_engine_runs_is_kept(self):
        """Voice 2 still runs its stock engine when its sound arrives (a kit load sends the
        engines and the sounds together; voice-map switches at the voice's next note)."""
        x = page.preset(9)
        self.engine_on(1)
        self.assertEqual(self.m.class_of(self.m.slot(1)), 'cSecondSlot', 'not yet')
        self.m.send(2, page.MSG_SOUND, bytes([1]) + pack(x))
        self.m.pump()
        self.note(1)
        self.assertEqual(self.m.class_of(self.m.slot(1)), 'cSynthLab')
        self.assertEqual(self.m7(1), x)

    def test_kits_changed_while_playing_play_their_sounds(self):
        c, r = self.c, self.r
        self.engine_on(0)
        for s in range(16):
            r.set_step(0, s, on=True)
        r.play()
        self.tick(30)
        for kit, p in ((0, 3), (1, 7)):
            r.e.uc.mem_write(r.sym.g['current_kit'], struct.pack('<H', kit))    # editing that kit
            c.long_press(r, 0)
            c.press(r, 16 * 0 + p)
            c.press(r, page.SHIFT)
            c.apply(r)
            r.e.call(r.cave['pk4_store_kit_save'], 0, kit, max_insns=90_000_000)
            self.tick(4)
        for kit, p in ((0, 3), (1, 7), (0, 3), (1, 7)):
            r.e.call(r.cave['pk_sy_kit_load'], kit, max_insns=90_000_000)
            self.fwd()
            self.tick(40)
            self.assertEqual(self.m.class_of(self.m.slot(0)), 'cSynthLab', f'kit {kit}')
            self.assertEqual(self.m7(0)[:PRESET + 1], page.preset(p)[:PRESET + 1], f'kit {kit}')

    def test_mutate_nudges_each_note_and_keeps_the_saved_sound(self):
        x = page.preset(4)
        x[MUTATE] = 1
        self.engine_on(0)
        self.note()
        self.m.send(2, page.MSG_SOUND, bytes([0]) + pack(x))
        self.m.pump()
        heard = []
        for _ in range(8):
            self.note()
            self.m.audio(960)
            heard.append(self.m7(0, saved=False))
            self.assertEqual(self.m7(0), x, 'the saved sound')
        self.assertGreater(len({tuple(h) for h in heard}), 4, 'a new nudge each note')
        for h in heard:
            d = [abs(a - b) for a, b in zip(h, x)]
            self.assertLessEqual(max(d), 3, 'small nudges, from the saved sound each time')
            self.assertEqual(h[SCALE:ROOT + 1], x[SCALE:ROOT + 1])


if __name__ == '__main__':
    unittest.main()
