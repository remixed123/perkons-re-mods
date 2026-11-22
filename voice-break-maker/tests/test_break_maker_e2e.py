"""BREAK MAKER with the M4 and the M7 running together, on the firmware's own sequencer: the break
loaded from the card on the M4 is the one the M7 plays; a pattern the generator writes plays as
the track says (jumps, the break carrying on, the loop's first step in place); a step's PARAM2
lock reaches the M7 with its trigger."""
import shutil
import struct
import unittest

from fixtures import SYMBOLS_V130
from perkons import verify
from perkons.emu import Symbols
from perkons.voicemap import remap
import test_break_maker_m4 as m4                        # (not its class: it'd run here too)
from bm_rig import BM_BASE, VEL, make_break, p2_of_slice, slice_of, wav

VOICE = m4.VOICE
SR = 48000
BPM = 120.0                                             # the rig's sequencer


@unittest.skipUnless(shutil.which('arm-none-eabi-gcc'), 'arm-none-eabi-gcc not installed')
class TestBreakMakerE2E(unittest.TestCase):
    def setUp(self):
        m4.TestBreakMakerM4.setUpClass()
        self.c = m4.TestBreakMakerM4()
        self.tones, self.starts = make_break(bpm=120.0)          # slice s: a tone at 200 + 50 s Hz
        self.r = self.c.rig({'BREAKS/1.wav': wav(self.tones, cues=self.starts)})
        self.c.load(self.r)
        img = m4.image()[0]
        self.m = verify.boot(img, Symbols(SYMBOLS_V130))
        n = struct.unpack('<I', bytes(self.r.e.uc.mem_read(BM_BASE + 12, 4)))[0]
        self.m.uc.mem_write(BM_BASE, bytes(self.r.e.uc.mem_read(BM_BASE, 0x100 + 2 * n + 8)))
        remap(self.m, [0, 4, 2, 3], m4._image['m7'], self.c.msg['voice-map'])
        self.m.audio(960)
        # the voice's knobs as the M7 has them: TUNE 12 o'clock, no gate, PARAM1 off, PARAM2 follows
        voices = [{'params': [2048] * 4} for _ in range(4)]
        voices[VOICE] = {'params': [2048, 4095, 0, 0], 'algo': 1, 'mode': 1}
        self.m.send_voice_state(voices)
        self.m.pump()

    def fwd(self):
        for t, p in self.r.e.sent(2):
            self.m.send(2, t, p)
        self.m.pump()

    def play(self, steps=16):
        """The sequencer runs `steps` steps; the slice the M7 plays on each."""
        r, got = self.r, []
        r.play()
        L = SR * 15.0 / BPM
        for i in range(steps):
            r.step()
            self.fwd()
            out = self.m.audio(int(L) // 64 * 64)[VOICE]
            got.append(slice_of(out[300:1500], speed=1.0))
        return got

    def test_the_break_plays_in_order(self):
        self.assertEqual(self.play(16), list(range(16)))

    def test_a_locked_step_jumps_and_the_break_carries_on(self):
        r = self.r
        r.set_step(VOICE, 5, velocity=127)
        lk = r.block(VOICE) + m4.BLK_LOCKS + m4.LOCK_SIZE * 5
        r.w16(lk + 2 * m4.PARAM2, p2_of_slice(12) << 4)
        r.w16(lk + 2 * 11, 1 << m4.PARAM2)
        want = [0, 1, 2, 3, 4] + [(12 + i) % 16 for i in range(11)] + [0, 1]
        self.assertEqual(self.play(18), want)

    def test_a_generated_pattern_plays_as_the_track_says(self):
        r = self.r
        for chr_ in (4, 7):                             # Two-step, Breakcore
            bar = self.c.generate(r, chr_, algo=m4.RUN)
            r.e.sent(2)
            r.set_mode(m4.MAIN)
            r.e.w8(r.seq, 0)
            self.c.loop(r)
            self.fwd()
            got = self.play(16)
            want = [x[0] for x in bar]
            # rolls and reverses change what a step's start sounds like: compare the plain steps
            plain = [s for s in range(16) if bar[s][1] == VEL['plain'] or r.e.r8(r.block(VOICE) + 8 * s) == 0]
            self.assertEqual([got[s] for s in plain], [want[s] for s in plain], chr_)
            r.e.w8(r.seq, 0)
            self.c.loop(r)
            self.fwd()
            self.m.audio(1920)


if __name__ == '__main__':
    unittest.main()
