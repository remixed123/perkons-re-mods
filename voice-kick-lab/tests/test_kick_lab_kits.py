"""Kick Lab with the M4 and the M7 running together: what the M4 sends, the M7 gets. A preset
pressed on the page, PARAM2's three values turned on the panel, saved with the kit, and the
kits changed while the sequencer plays (a kit's positions only reach the M7 with the track's
next note)."""
import shutil
import struct
import unittest

from fixtures import SYMBOLS_V130
from perkons import verify
from perkons.emu import Symbols
from perkons.rig import EVT
import test_kick_lab_page as page                        # (not its class: it'd run here too)

PARAM2_POT, ALGO_SW = 5, 2
POS1, POS2, POS3 = 1, 0, 2
ATTACK, DELAY_MIX, DRIVE_MIX = POS1, POS2, POS3


def ends(x):
    """A knob position (0-1) as the value it sets: the ends of the travel are the ends."""
    return min(1.0, max(0.0, (x - 0.02) / 0.96))


@unittest.skipUnless(shutil.which('arm-none-eabi-gcc'), 'arm-none-eabi-gcc not installed')
class TestKickLabKits(unittest.TestCase):
    def setUp(self):
        self.c = page.TestKickLabPage()
        self.r = self.c.rig()
        self.m = verify.boot(page.image()[0], Symbols(SYMBOLS_V130))
        self.t = 0
        self.m.audio(960)
        self.c.open_engines(self.r)
        self.c.press(self.r, 16 * 0 + page.COL)          # voice 1 runs Kick Lab
        self.c.loop(self.r)
        self.fwd()

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
            r.e.call(r.cave['pk_kl_main_loop'], max_insns=5_000_000)
            self.fwd()
            self.m.audio(256)
            self.t += 1
            if playing and self.t % 24 == 0:
                r.step()
                self.fwd()
                self.m.audio(64)

    def knob(self, pot, x):
        r = self.r
        poll = r.sym.fn['lfo_knob_poll']
        r.turn(pot, x, lambda: r.e.call(poll, max_insns=5_000_000), steps=6)
        for _ in range(4):
            r.e.call(poll, max_insns=5_000_000)
        self.tick(2)

    def switch(self, which, pos):
        r = self.r
        r.e.uc.mem_write(EVT, bytes([which, 0x15]) + struct.pack('<H', pos) + bytes(4))
        r.e.call(r.key_handler, EVT, max_insns=50_000_000)
        self.tick(2)

    def values(self):
        """The M7's attack, delay mix, drive mix on output 1."""
        a = self.m.slot(0) + 0xcc
        return [struct.unpack('<f', bytes(self.m.uc.mem_read(a + 4 * i, 4)))[0] for i in range(3)]

    def m7_preset(self):
        """The M7's preset (its index from the pointer) and page bytes on output 1."""
        o = self.m.slot(0)
        return self.m.r32(o + 0xc0), list(self.m.uc.mem_read(o + 0xc4, 7))

    def test_a_preset_press_reaches_the_m7(self):
        c, r = self.c, self.r
        c.long_press(r, 0)
        self.assertEqual(r.mode(), page.KICK_LAB_PAGE)
        self.tick(2)
        basic, _ = self.m7_preset()
        c.press(r, 16 * 0 + 14)                     # Dub Echo
        self.tick(2)
        self.m.send_triggers([1, 0, 0, 0], [(ATTACK, POS1), (0, 0), (0, 0), (0, 0)], 127, 36)
        self.m.audio(64)
        pre, pg = self.m7_preset()
        self.assertNotEqual(pre, basic)
        self.assertEqual(pg, [0, 1, 1, 1, 7, 6, 2], 'soft, amount 2, tone flat, sync, 1/8D, fb 0.6, tone 3')
        for got, want in zip(self.values(), (25, 89, 128)):
            self.assertAlmostEqual(got, want / 255, delta=0.003)

    def set_up_kits(self):
        r = self.r
        for s in range(16):
            r.set_step(0, s, on=s % 4 == 0)
        r.play()
        self.tick(60)
        kits = {}
        for kit, algo, drive, delay in ((0, DRIVE_MIX, 0.9, 0.4), (1, ATTACK, 0.1, 0.8)):
            self.switch(ALGO_SW, DRIVE_MIX)
            self.knob(PARAM2_POT, drive)
            self.switch(ALGO_SW, DELAY_MIX)
            self.knob(PARAM2_POT, delay)
            self.switch(ALGO_SW, algo)              # ALGO elsewhere: the knob's off its job
            self.fwd()
            self.tick(30)
            r.e.call(r.cave['pk4_store_kit_save'], 0, kit, max_insns=90_000_000)
            self.fwd()
            kits[kit] = (ends(delay), ends(drive))
        return kits

    def check(self, kits, kit, playing):
        self.r.e.call(self.r.cave['pk_kl_kit_load'], kit, max_insns=90_000_000)
        self.fwd()
        for _ in range(10):
            self.tick(12, playing)
            v = self.values()
            self.assertAlmostEqual(v[1], kits[kit][0], delta=0.01, msg=f'kit {kit} delay mix')
            self.assertAlmostEqual(v[2], kits[kit][1], delta=0.01, msg=f'kit {kit} drive mix')

    def test_changing_kits_while_playing_keeps_each_kits_values(self):
        kits = self.set_up_kits()
        for kit in (0, 1, 0, 1, 0, 1):
            self.check(kits, kit, True)

    def test_changing_kits_while_stopped_keeps_each_kits_values(self):
        kits = self.set_up_kits()
        self.r.e.w8(self.r.seq, 0)
        self.tick(4, playing=False)
        for kit in (0, 1, 0, 1):
            self.check(kits, kit, False)
