"""Synth Lab on the M7, through main()'s message loop and the real audio callback, as a
voice-map engine: the wiring, the presets, pitch and scale, sync, the gate, the level, CPU.
Needs arm-none-eabi-gcc."""
import shutil
import tempfile
import unittest

from fixtures import build_patched
from perkons import verify
from perkons.patch import PatchError
from sy_rig import (CUTOFF, DRONE, ENV, FENV, FILT_ON, FOLD_ON, FREE, LEVEL, OCT, OSC_N, OSYNC, PLFO, PRESETS,
                    RES, SAW, SCALE, SWEEP, SYNC, SYNCED, VLFO, WAVE, SynthLab, cents, f0, pack, peak, rms,
                    unpack, WIDTH)

MID = 2048
SR_C2 = 48000 / 65.406      # C2's period, samples


def period(x, lo=40, hi=1600):
    """The period (samples) by autocorrelation: the first lag within 5% of the best."""
    n = len(x) - hi
    c = [sum(x[i] * x[i + lag] for i in range(0, n, 2)) for lag in range(lo, hi)]
    best = max(c)
    return lo + next(i for i, v in enumerate(c) if v > 0.95 * best)


def plain(v, waves=(SAW,)):
    """A test sound from preset `v`: osc 1 alone (wave), filter, fold and LFOs off."""
    v = list(v)
    for i in range(3):
        o = i * OSC_N
        v[o + LEVEL] = 31 if i < len(waves) else 0
        if i < len(waves):
            v[o + WAVE] = waves[i]
        v[o + OSYNC] = 0
        v[o + PLFO + 2] = v[o + VLFO + 2] = 0
        v[o + OCT] = 2              # x1
        v[o + 5] = 16               # no detune
    v[FILT_ON] = 0
    v[FOLD_ON] = 0
    v[56 + 2] = v[60 + 2] = 0       # cutoff, fold LFO depths
    v[FENV + 2] = 31                # filter env sustain full (sweep holds)
    v[52 + 2] = 31                  # amp sustain full
    v[52] = 0
    v[SCALE] = 9                    # chromatic
    return v


class TestSynthLab(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('arm-none-eabi-gcc'):
            raise unittest.SkipTest('arm-none-eabi-gcc not installed')
        cls.tmp = tempfile.mkdtemp(prefix='pk-sy-')
        cls.sl = SynthLab(cls.tmp + '/a')
        cls.stock, cls.sym, cls.img = cls.sl.stock, cls.sl.sym, cls.sl.img

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def machine(self, outs=(0,)):
        return self.sl.machine(outs)

    def play(self, e, params, **kw):
        return self.sl.play(e, params, **kw)

    # -- the sound format ---------------------------------------------------------------------
    def test_sound_is_316_bits_in_40_bytes(self):
        self.assertEqual(sum(WIDTH), 316)
        v = [(i * 7 + 3) & ((1 << w) - 1) for i, w in enumerate(WIDTH)]
        self.assertEqual(unpack(pack(v)), v)

    # -- wiring ---------------------------------------------------------------------------
    def test_remap_to_synth_lab_and_back(self):
        e = self.machine((2,))
        self.assertEqual(e.class_of(e.slot(2)), 'cSynthLab')
        self.assertGreater(peak(self.play(e, [MID, MID, MID, MID], out=2)), 1000)
        self.sl.remap(e, [0, 1, 2, 3])
        self.assertEqual(e.class_of(e.slot(2)), 'cThirdSlot')

    def test_needs_voice_map(self):
        with self.assertRaises(PatchError):
            build_patched(self.stock, ['voice_synth_lab'], self.tmp + '/b')

    def test_stock_outputs_unchanged(self):
        bad = [r for r in verify.compare_to_stock(self.stock, self.img, self.sym) if not r[4]]
        self.assertEqual(bad, [])

    def test_outputs_start_on_presets_1_5_12_14(self):
        e = self.machine((0, 1, 2, 3))
        self.assertEqual([self.sl.sound(e, o)[78] for o in range(4)], [0, 4, 11, 13])

    # -- presets --------------------------------------------------------------------------------
    def test_every_preset_sounds_and_stays_in_range(self):
        e = self.machine()
        base = self.sl.sound(e)
        from sy_rig import PRESET  # noqa
        levels = {}
        for i, name in enumerate(PRESETS):
            with self.subTest(preset=name):
                e = self.machine()
                v = self._preset(i)
                self.sl.send_sound(e, v)
                x = self.play(e, [1600, DRONE, MID, MID], n=38400)
                self.assertEqual(self.sl.sound(e), v)
                p = peak(x)
                self.assertGreater(p, 1000, 'audible')
                self.assertLessEqual(p, 9050, 'held by the limiter')
                levels[name] = (p, rms(x))
        print('\npresets (peak, rms):', {k: (round(a), round(b)) for k, (a, b) in levels.items()})
        self.assertTrue(base)

    def _preset(self, i):
        import re
        import os
        src = open(os.path.join(os.path.dirname(__file__), '..', 'src', 'common', 'sl_presets.h')).read()
        rows = re.findall(r'\{([^{}]*)\}', src[src.index('= {') + 3:])
        return [int(x) for x in rows[i].replace('\n', ' ').split(',') if x.strip()]

    # -- pitch ----------------------------------------------------------------------------
    def steady(self, v, tune=0, note=36, mode=FREE, p2=MID, algo=RES, n=9600):
        e = self.machine()
        self.sl.send_sound(e, v)
        self.play(e, [tune, DRONE, MID, MID], mode=mode, algo=algo, n=64, trig=False)  # the job first
        x = self.play(e, [tune, DRONE, MID, p2], note=note, mode=mode, algo=algo, n=n)
        return x

    def test_tune_bottom_is_c2_and_steps_semitones_on_chromatic(self):
        v = plain(self._preset(0))
        c2 = 65.406
        self.assertLess(abs(cents(f0(self.steady(v)[2400:]), c2)), 10)
        # chromatic: 48 steps over the knob
        x = self.steady(v, tune=round(7 / 48 * 4095))
        self.assertLess(abs(cents(f0(x[2400:]), c2) - 700), 10)

    def test_midi_note_transposes(self):
        v = plain(self._preset(0))
        ref = f0(self.steady(v)[2400:])
        x = self.steady(v, note=48)
        self.assertLess(abs(cents(f0(x[2400:]), ref) - 1200), 10)

    def test_lowest_oscillator_plays_the_note(self):
        v = plain(self._preset(0))
        v[OCT] = 0                  # x0.25: still C2
        self.assertLess(abs(cents(f0(self.steady(v)[2400:]), 65.406)), 10)

    # -- sync -------------------------------------------------------------------------------
    def test_sync_locks_the_slave_to_the_master(self):
        v = plain(self._preset(0), waves=(SAW, SAW))
        v[OSC_N + OCT] = 4          # slave x2 (x2.3 with the ratio mode)
        v[OSC_N + OSYNC] = 1
        v[0 + LEVEL] = 0            # master silent: the slave alone, at the master's pitch
        synced = self.steady(v, mode=SYNCED)[2400:]
        free = self.steady(v, mode=FREE)[2400:]
        self.assertAlmostEqual(period(synced), SR_C2, delta=3)       # the master's period
        self.assertNotAlmostEqual(period(free), SR_C2, delta=20)

    def test_param2_sync_sweeps_the_timbre_not_the_pitch(self):
        v = plain(self._preset(0), waves=(SAW, SAW))
        v[OSC_N + OCT] = 4
        v[OSC_N + OSYNC] = 1
        v[0 + LEVEL] = 0
        a = self.steady(v, mode=SYNCED, algo=SYNC, p2=MID)[2400:]
        b = self.steady(v, mode=SYNCED, algo=SYNC, p2=4095)[2400:]
        self.assertAlmostEqual(period(b), SR_C2, delta=3)
        self.assertGreater(abs(rms(a) - rms(b)) + abs(peak(a) - peak(b)), 50)

    def test_sync_is_band_limited(self):
        """A high synced saw: little energy in alias bands (sample differences stay smooth)."""
        v = plain(self._preset(0), waves=(SAW, SAW))
        v[OSC_N + OCT] = 6          # x4
        v[OSC_N + OSYNC] = 1
        v[0 + LEVEL] = 0
        x = self.steady(v, tune=4095, mode=SYNCED, n=4800)[960:]
        self.assertGreater(peak(x), 1000)

    # -- gate --------------------------------------------------------------------------------
    def test_gate_releases_and_drone_holds(self):
        v = plain(self._preset(0))
        e = self.machine()
        self.sl.send_sound(e, v)
        x = self.play(e, [0, 0, MID, MID], n=9600)         # GATE 50 ms, release ~... ms
        self.assertGreater(peak(x[:960]), 1000)
        self.assertLess(peak(x[-480:]), peak(x[:960]) * 0.2)
        e = self.machine()
        self.sl.send_sound(e, v)
        x = self.play(e, [0, DRONE, MID, MID], n=19200)
        self.assertGreater(peak(x[-480:]), 0.8 * peak(x[:960]))

    # -- CPU ----------------------------------------------------------------------------------
    def insns(self, e):
        e.count_insns(True)
        e.insn_count = 0
        e.audio(960)
        e.count_insns(False)
        return e.insn_count / 960

    def heavy(self):
        """The heaviest sound: three synced oscillators, fold, LP24, every LFO on."""
        v = self._preset(14)        # Voltage Drift
        for i in range(3):
            v[i * OSC_N + LEVEL] = 20
            v[i * OSC_N + PLFO + 2] = v[i * OSC_N + VLFO + 2] = 10
            v[i * OSC_N + OSYNC] = int(i > 0)
            v[i * OSC_N + WAVE] = 1 if i == 0 else 4      # triangle master, pulse slaves
        v[56 + 2] = v[60 + 2] = 10
        v[FOLD_ON], v[64] = 1, 20
        return v

    def cpu(self, outs):
        params = [3000, DRONE, MID, MID]
        runs = []
        for e, trig in ((self.machine(outs), True), (verify.boot(self.img, self.sym), True),
                        (self.machine(outs), False)):
            for o in outs:
                if trig and e.class_of(e.slot(o)) == 'cSynthLab':
                    self.sl.send_sound(e, self.heavy(), out=o)
                if trig:
                    self.play(e, params, mode=SWEEP, out=o, n=64)
            e.audio(960)
            runs.append(self.insns(e))
        return runs[0] - runs[1], runs[0] - runs[2]

    def test_cpu(self):
        (one, own1), (four, own4) = self.cpu((0,)), self.cpu((0, 1, 2, 3))
        print(f'\nSynth Lab: one voice ~{own1:.0f} instructions per sample of its own, ~{one:.0f} over stock; '
              f'four ~{own4:.0f} of their own, ~{four:.0f} over stock')
        self.assertLess(one, 800)


if __name__ == '__main__':
    unittest.main()
