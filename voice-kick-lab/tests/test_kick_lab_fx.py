"""Kick Lab's presets, body peak, drive, delay and rumble, set through the "kick-lab" and
"kick-lab-tempo" messages. Needs arm-none-eabi-gcc."""
import math
import shutil
import tempfile
import unittest

from perkons import verify
from kl_rig import (ATTACK, CRUSH, DELAY_MIX, DRIVE_MIX, DRONE, FOLD, HARD, MID_DECAY, PINK, PRESETS, SOFT, SR,
                    KickLab, decay_for, env, rms, settings, tune_for)


def third(x, hz=50.0):
    """The 3rd harmonic against the fundamental, 50-150 ms (5 cycles of 50 Hz)."""
    seg = x[2400:7200]

    def mag(f):
        w = 2 * math.pi * f / SR
        re = sum(v * math.cos(w * n) for n, v in enumerate(seg))
        im = sum(v * math.sin(w * n) for n, v in enumerate(seg))
        return math.hypot(re, im)
    return mag(3 * hz) / (mag(hz) or 1)


def onset_ms(x, start, level):
    """The first sample after `start` (s) above `level`, in ms."""
    i = next(i for i in range(int(start * SR), len(x)) if abs(x[i]) > level)
    return i * 1000 / SR


@unittest.skipUnless(shutil.which('arm-none-eabi-gcc'), 'arm-none-eabi-gcc not installed')
class TestKickLabFx(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix='pk-klfx-')
        cls.kl = KickLab(cls.tmp + '/a')

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def hit(self, s, params=None, n=SR, algo=ATTACK, mode=1, e=None, out=0):
        """A machine with these settings on `out`, one hit."""
        e = e or self.kl.machine((out,))
        self.kl.send_settings(e, {out: s})
        return self.kl.play(e, params or [tune_for(50), decay_for(0.1), 0, 0], algo=algo, mode=mode, n=n, out=out)

    # -- presets --------------------------------------------------------------------------
    def test_every_preset_plays_near_the_stock_kick(self):
        for i, name in enumerate(PRESETS):
            with self.subTest(preset=name):
                x = self.hit(settings(i), [tune_for(50), MID_DECAY, 2048, 0], n=SR // 2)
                self.assertLessEqual(max(map(abs, x)), 16800)       # (the soft clip's ceiling)
                self.assertGreater(max(map(abs, x)), 8000)

    def test_settings_survive_a_voice_map_rebuild(self):
        e = self.kl.machine()
        s = settings(0, sync=0, time=9, fb=0, delay_mix=128)
        a = self.hit(s, e=e)
        self.kl.remap(e, [0, 1, 2, 3])
        self.kl.remap(e, [4, 1, 2, 3])
        e.audio(960)
        b = self.kl.play(e, [tune_for(50), decay_for(0.1), 0, 0], n=SR)
        self.assertAlmostEqual(onset_ms(b, 0.2, 1000), onset_ms(a, 0.2, 1000), delta=2)

    # -- delay ----------------------------------------------------------------------------
    def test_free_times(self):
        for t in (3, 9, 12, 13):        # 60, 250, 750, 1000 ms (rates 16, 16, 8, 4 kHz)
            with self.subTest(ms=[60, 250, 750, 1000][t // 4 if t < 12 else t - 10]):
                ms = [15, 25, 40, 60, 80, 100, 125, 150, 190, 250, 375, 500, 750, 1000][t]
                x = self.hit(settings(0, time=t, delay_mix=128), n=int(SR * (ms / 1000 + 0.15)) // 64 * 64)
                self.assertAlmostEqual(onset_ms(x, ms / 1000 - 0.008, 800), ms, delta=3 + ms * 0.01)

    def test_sync_follows_the_tempo(self):
        for bpm, div, want in ((120, 9, 500), (100, 9, 600), (120, 7, 375), (90, 6, 333.3)):
            with self.subTest(bpm=bpm, div=div):
                e = self.kl.machine()
                self.kl.send_tempo(e, bpm)
                x = self.hit(settings(0, sync=1, time=div, delay_mix=128), e=e,
                             n=int(SR * (want / 1000 + 0.15)) // 64 * 64)
                self.assertAlmostEqual(onset_ms(x, want / 1000 - 0.01, 800), want, delta=4 + want * 0.01)

    def test_the_longest_division_fits_at_60_bpm(self):
        """1/2D at 60 BPM: 3 s, on the 2 kHz line."""
        e = self.kl.machine()
        self.kl.send_tempo(e, 60)
        x = self.hit(settings(0, sync=1, time=13, delay_mix=128), e=e, n=int(SR * 3.15) // 64 * 64)
        self.assertAlmostEqual(onset_ms(x, 2.95, 500), 3000, delta=40)

    def test_delay_rings_out_after_the_hit(self):
        x = self.hit(settings(0, time=9, fb=6, delay_mix=128), [tune_for(50), decay_for(0.05), 0, 0], n=2 * SR)
        e_ = env(x, 4800)                   # 100 ms windows
        self.assertGreater(max(e_[10:14]), 500, 'echoes a second after a 50 ms hit')
        self.assertGreater(max(e_[2:4]), max(e_[10:14]), 'and they fall')

    def test_delay_mix_0_is_dry(self):
        a = self.hit(settings(0, time=9, fb=6, delay_mix=0))
        b = self.hit(settings(0, time=9, fb=6, delay_mix=0, drive_mix=0))
        self.assertEqual(a, b)
        self.assertEqual(max(map(abs, a[SR // 2:])), 0)

    def test_param2_on_algo_2_sets_the_delay_mix(self):
        e = self.kl.machine()
        self.kl.send_settings(e, {0: settings(0, time=9, delay_mix=0)})
        x = self.kl.play(e, [tune_for(50), decay_for(0.1), 0, 4095], algo=DELAY_MIX, n=SR // 2)
        self.assertGreater(max(map(abs, x[int(0.24 * SR):int(0.3 * SR)])), 1000, 'the knob moved: an echo')

    # -- PARAM2's values ------------------------------------------------------------------
    def test_a_new_algo_keeps_the_value_until_the_knob_moves(self):
        e = self.kl.machine()
        self.kl.send_settings(e, {0: settings(0, time=9, delay_mix=0, drive_mix=0)})
        p = [tune_for(50), decay_for(0.1), 0, 4095]
        self.kl.play(e, p, algo=ATTACK, n=64, trig=False)
        x = self.kl.play(e, p, algo=DELAY_MIX, n=SR // 2)              # ALGO moved, the knob didn't
        self.assertEqual(max(map(abs, x[int(0.24 * SR):])), 0, 'no echo yet')
        p[3] = 4095 - 400
        x = self.kl.play(e, p, algo=DELAY_MIX, n=SR // 2)              # now it moves
        self.assertGreater(max(map(abs, x[int(0.24 * SR):int(0.3 * SR)])), 1000)

    # -- drive ----------------------------------------------------------------------------
    def test_drive_types_differ_and_mix_0_is_dry(self):
        held = [tune_for(50), DRONE, 0, 0]          # (drone: the level holds while the shape is read)
        out = {}
        for t in (SOFT, HARD, CRUSH, FOLD):
            out[t] = self.hit(settings(0, drv_type=t, drv_amt=8, drive_mix=255), held, n=SR // 4)
        for a in out:
            for b in out:
                if a < b:
                    self.assertNotEqual(out[a], out[b], (a, b))
        dry = self.hit(settings(0), held, n=SR // 4)
        off = self.hit(settings(0, drv_type=FOLD, drv_amt=8, drive_mix=0), held, n=SR // 4)
        self.assertEqual(dry, off)
        self.assertLess(third(dry), 0.05)
        self.assertGreater(third(out[HARD]), 0.2, 'hard drive: odd harmonics')

    def test_param2_on_algo_3_sets_the_drive_mix(self):
        held = [tune_for(50), DRONE, 0, 0]
        dry = self.hit(settings(0, drv_type=HARD, drv_amt=8), held, n=SR // 4)
        e = self.kl.machine()
        self.kl.send_settings(e, {0: settings(0, drv_type=HARD, drv_amt=8)})
        self.kl.play(e, held, algo=DRIVE_MIX, n=64, trig=False)                    # ALGO first
        x = self.kl.play(e, held[:3] + [4095], algo=DRIVE_MIX, n=SR // 4)          # then the knob
        self.assertLess(third(dry), 0.05)
        self.assertGreater(third(x), 0.2)

    # -- rumble ---------------------------------------------------------------------------
    def test_sustained_rumbles_after_the_hit(self):
        """Sustained (preset 12) has rumble: sound after the amp has gone."""
        x = self.hit(settings(11, attack=0), [tune_for(50), decay_for(0.1), 0, 0], n=SR)
        self.assertGreater(max(map(abs, x[int(0.3 * SR):int(0.5 * SR)])), 20)
        y = self.hit(settings(0), [tune_for(50), decay_for(0.1), 0, 0], n=SR)
        self.assertEqual(max(map(abs, y[int(0.3 * SR):int(0.5 * SR)])), 0)

    # -- CPU --------------------------------------------------------------------------------
    def heaviest(self):
        """8x Punch (body, tick, rumble) with crush drive, delay on, pink noise, sweep up, drone."""
        return settings(3, drv_type=CRUSH, drv_amt=8, time=9, fb=6, dl_tone=2, delay_mix=128, drive_mix=255)

    def insns(self, e):
        e.count_insns(True)
        e.insn_count = 0
        e.audio(960)
        e.count_insns(False)
        return e.insn_count / 960

    def cpu(self, outs):
        params = [tune_for(50), DRONE, 4095, 0]
        runs = []
        for e, trig in ((self.kl.machine(outs), True), (verify.boot(self.kl.img, self.kl.sym), True),
                        (self.kl.machine(outs), False)):
            if e.class_of(e.slot(0)) == 'cKickLab':
                self.kl.send_settings(e, {o: self.heaviest() for o in outs})
            e.send_voice_state([{'params': params, 'algo': ATTACK, 'mode': PINK} if o in outs
                                else {'params': [2048] * 4} for o in range(4)])
            e.pump()
            if trig:
                e.send_triggers([o in outs for o in range(4)], [(ATTACK, PINK)] * 4, 127, 36)
            e.audio(SR // 2)            # into the echoes and the rumble
            runs.append(self.insns(e))
        return runs[0] - runs[1], runs[0] - runs[2]

    def test_cpu(self):
        (one, own1), (four, own4) = self.cpu((0,)), self.cpu((0, 1, 2, 3))
        print(f'\nKick Lab, everything on: one voice ~{own1:.0f} instructions per sample of its own, '
              f'~{one:.0f} over stock; four ~{own4:.0f} of their own, ~{four:.0f} over stock')
        self.assertLess(one, 800)
