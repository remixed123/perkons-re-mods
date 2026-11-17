"""3 Oh on the M7, through main()'s message loop and the real audio callback, as a voice-map
engine: the wiring, the presets, pitch and scale, the filter, accent, slide, the gate, the
distortions, CPU. Needs arm-none-eabi-gcc."""
import shutil
import tempfile
import unittest

from fixtures import build_patched
from perkons import verify
from perkons.patch import PatchError
from oh_rig import (ACC, ACCENT, CUTOFF, DIST, DISTAMT, DRONE, DTYPE, ENV, ENVMOD, MID, PRESETS, PULSE, RESO, SAW,
                    SQUARE, VEL_ACCENT, VEL_BOTH, VEL_SLIDE, WIDTH, ThreeOh, bright, cents, db, pack, peak, preset,
                    rms, unpack)

C2 = 65.406
SR_C2 = 48000 / C2      # C2's period, samples


def period(x, lo=40, hi=1600):
    """The period (samples) by autocorrelation: the first lag within 5% of the best."""
    n = len(x) - hi
    c = [sum(x[i] * x[i + lag] for i in range(0, n, 2)) for lag in range(lo, hi)]
    best = max(c)
    i = next(i for i, v in enumerate(c) if v > 0.9 * best)
    while i + 1 < len(c) and c[i + 1] >= c[i]:     # up to the peak's top
        i += 1
    if 0 < i < len(c) - 1:                          # parabolic interpolation
        a, b, d = c[i - 1], c[i], c[i + 1]
        return lo + i + 0.5 * (a - d) / (a - 2 * b + d)
    return lo + i


def pcents(x, ref_hz):
    return cents(48000 / period(x), ref_hz)


def plain(i=0):
    return preset(i)


class TestThreeOh(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('arm-none-eabi-gcc'):
            raise unittest.SkipTest('arm-none-eabi-gcc not installed')
        cls.tmp = tempfile.mkdtemp(prefix='pk-oh-')
        cls.oh = ThreeOh(cls.tmp + '/a')
        cls.stock, cls.sym, cls.img = cls.oh.stock, cls.oh.sym, cls.oh.img

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def machine(self, outs=(0,)):
        return self.oh.machine(outs)

    def steady(self, v, cut=MID, note=36, mode=SAW, algo=ENV, res=MID, p2=MID, dec=DRONE, n=9600, vel=127):
        e = self.machine()
        self.oh.send_sound(e, v)
        self.oh.play(e, [cut, dec, res, MID], mode=mode, algo=algo, n=64, trig=False)  # the job first
        return self.oh.play(e, [cut, dec, res, p2], note=note, mode=mode, algo=algo, n=n, vel=vel)

    # -- the sound format ---------------------------------------------------------------------
    def test_sound_is_83_bits_in_11_bytes(self):
        self.assertEqual(sum(WIDTH), 83)
        v = [(i * 7 + 3) & ((1 << w) - 1) for i, w in enumerate(WIDTH)]
        self.assertEqual(unpack(pack(v)), v)

    # -- wiring ---------------------------------------------------------------------------
    def test_remap_to_three_oh_and_back(self):
        e = self.machine((2,))
        self.assertEqual(e.class_of(e.slot(2)), 'cThreeOh')
        self.assertGreater(peak(self.oh.play(e, [MID, DRONE, MID, MID], out=2)), 1000)
        self.oh.remap(e, [0, 1, 2, 3])
        self.assertEqual(e.class_of(e.slot(2)), 'cThirdSlot')

    def test_needs_voice_map(self):
        with self.assertRaises(PatchError):
            build_patched(self.stock, ['voice_3_oh'], self.tmp + '/b')

    def test_stock_outputs_unchanged(self):
        bad = [r for r in verify.compare_to_stock(self.stock, self.img, self.sym) if not r[4]]
        self.assertEqual(bad, [])

    def test_outputs_start_on_presets_1_2_10_14(self):
        e = self.machine((0, 1, 2, 3))
        self.assertEqual([self.oh.sound(e, o)[9] for o in range(4)], [0, 1, 9, 13])

    # -- presets --------------------------------------------------------------------------------
    def test_every_preset_sounds_and_stays_in_range(self):
        levels = {}
        for i, name in enumerate(PRESETS):
            with self.subTest(preset=name):
                v = preset(i)
                for mode in (SAW, SQUARE):
                    x = self.steady(v, note=43, mode=mode, n=19200)
                    p = peak(x)
                    self.assertGreater(p, 1000, 'audible')
                    self.assertLessEqual(p, 10000, 'held by the limiter and the soft clip')
                    levels[(name, mode)] = (p, rms(x))
        print('\npresets (peak, rms):', {f'{k[0]}/{k[1]}': (round(a), round(b)) for k, (a, b) in levels.items()})

    # -- pitch: the step's note ----------------------------------------------------------------
    def test_the_note_is_the_steps(self):
        v = plain()
        v[CUTOFF], v[RESO], v[ENVMOD] = 31, 0, 0
        self.assertLess(abs(pcents(self.steady(v, note=36)[2400:], C2)), 15)
        self.assertLess(abs(pcents(self.steady(v, note=43)[2400:], C2) - 700), 15)
        self.assertLess(abs(pcents(self.steady(v, note=48)[2400:], C2) - 1200), 15)

    def test_tune_is_the_cutoff_not_the_pitch(self):
        v = plain()
        v[RESO], v[ENVMOD] = 8, 0
        dark = self.steady(v, cut=600)[4800:]
        open_ = self.steady(v, cut=3500)[4800:]
        self.assertGreater(bright(open_), 2 * bright(dark))
        self.assertAlmostEqual(period(dark), period(open_), delta=10)       # the same note

    def test_mode_chooses_the_wave(self):
        v = plain()
        v[CUTOFF], v[RESO], v[ENVMOD] = 31, 0, 0
        saw, sq, pulse = (self.steady(v, mode=m)[2400:] for m in (SAW, SQUARE, PULSE))
        for x in (sq, pulse):
            self.assertLess(abs(pcents(x, C2)), 15)
        self.assertNotEqual(saw[:960], sq[:960])
        self.assertNotEqual(sq[:960], pulse[:960])

    # -- the filter -----------------------------------------------------------------------------
    def test_param1_is_the_resonance(self):
        """Full resonance: the sound rings near the cutoff (more energy in its upper harmonics),
        and it's still the note."""
        v = plain()
        v[ENVMOD] = 0
        lo = self.steady(v, res=0)[4800:]
        hi = self.steady(v, res=4095)[4800:]
        self.assertGreater(bright(hi), 1.3 * bright(lo))
        self.assertLess(abs(pcents(hi, C2)), 30)

    def test_high_cutoff_and_full_resonance_scream(self):
        """Cutoff and resonance both high: most of the sound's energy is in the resonant peak, more
        than in the fundamental (the classic scream)."""
        import math
        v = plain()
        v[ENVMOD], v[DISTAMT] = 0, 0
        x = self.steady(v, cut=3300, res=4095, note=36, n=19200)[9600:]
        f0 = 48000 / period(x)
        per = round(48000 / f0)
        y = x[:per * (len(x) // per)]

        def h(n):
            w = 2 * math.pi * f0 * n / 48000
            return math.hypot(sum(v * math.cos(w * i) for i, v in enumerate(y)),
                              sum(v * math.sin(w * i) for i, v in enumerate(y))) * 2 / len(y)
        H = [h(n) for n in range(1, 120)]
        top = 3 + max(range(len(H) - 3), key=lambda i: H[3 + i])          # the peak, above the low harmonics
        band = sum(a * a for a in H[max(0, top - 4):top + 5])
        share = band / sum(a * a for a in H)
        print(f'\nscream: the peak at harmonic {top + 1} ({(top + 1) * f0:.0f} Hz), {100 * share:.0f}% of the energy around it, '
              f'{20 * math.log10(H[top] / H[0]):+.1f} dB on the fundamental')
        self.assertGreater(share, 0.2)
        self.assertGreater(20 * math.log10(H[top] / H[0]), -10.0)

    def harmonics(self, x, n=120):
        import math
        f0 = 48000 / period(x)
        per = round(48000 / f0)
        y = x[:per * (len(x) // per)]

        def h(m):
            w = 2 * math.pi * f0 * m / 48000
            return math.hypot(sum(v * math.cos(w * i) for i, v in enumerate(y)),
                              sum(v * math.sin(w * i) for i, v in enumerate(y))) * 2 / len(y)
        return [h(m) for m in range(1, n)]

    def test_a_low_cutoff_with_full_resonance_goes_dark_not_screaming(self):
        v = plain()
        v[ENVMOD], v[DISTAMT] = 0, 0
        H = self.harmonics(self.steady(v, cut=300, res=4095, note=36, n=19200)[9600:], 40)
        top = max(range(len(H)), key=lambda i: H[i])
        self.assertLess(top, 6, f'the loudest harmonic: {top + 1}')

    def test_full_resonance_keeps_some_body(self):
        import math
        v = plain()
        v[ENVMOD], v[DISTAMT] = 0, 0
        lo = self.harmonics(self.steady(v, cut=3300, res=0, note=36, n=19200)[9600:], 4)
        hi = self.harmonics(self.steady(v, cut=3300, res=4095, note=36, n=19200)[9600:], 4)
        loss = 20 * math.log10(hi[0] / lo[0])
        print(f'\nfull resonance: the fundamental {loss:+.1f} dB')
        self.assertGreater(loss, -11.0)

    def test_decay_is_heard_within_a_note(self):
        """A plain note (58% of a sixteenth): DECAY fully left, the sweep falls back within it;
        fully right (below the drone), it stays bright."""
        v = plain()
        v[ENVMOD], v[CUTOFF], v[RESO] = 24, 6, 16

        def tail(dec):
            e = self.machine()
            self.oh.send_live(e, bpm=120)
            self.oh.send_sound(e, v)
            return bright(self.oh.play(e, [MID, dec, MID, MID], n=4800)[1800:3300])
        short, long_ = tail(0), tail(3800)
        print(f'\ndecay: brightness late in the note {short:.4f} (left) vs {long_:.4f} (right)')
        self.assertGreater(long_, 1.5 * short)

    def test_envelope_sweeps_down(self):
        v = plain()
        v[ENVMOD], v[CUTOFF] = 28, 6
        x = self.steady(v, dec=1000, n=24000)
        self.assertGreater(bright(x[480:2400]), 1.5 * bright(x[-4800:]))

    # -- velocity: accent, slide; not the level ----------------------------------------------------
    def test_velocity_doesnt_set_the_level(self):
        v = plain()
        a = self.steady(v, vel=127)[2400:]
        b = self.steady(v, vel=113)[2400:]
        self.assertLess(abs(db(rms(a), rms(b))), 0.2)

    def test_an_accented_step_is_louder_and_brighter(self):
        """Louder and brighter at its start (its sweep is the fastest, as the bass line's)."""
        v = plain()
        v[ACCENT] = 31
        plain_ = self.steady(v, dec=1000, n=4800)[:2400]
        for vel in (VEL_ACCENT, VEL_BOTH):
            with self.subTest(vel=vel):
                acc = self.steady(v, dec=1000, n=4800, vel=vel)[:2400]
                self.assertGreater(peak(acc), 1.3 * peak(plain_))
                self.assertGreater(bright(acc[100:800]), bright(plain_[100:800]))

    def test_param2_on_algo_2_sets_the_accent(self):
        v = plain()
        v[ACCENT] = 16
        lo = self.steady(v, algo=ACC, p2=0, dec=1000, n=4800, vel=VEL_ACCENT)[:2400]
        hi = self.steady(v, algo=ACC, p2=4095, dec=1000, n=4800, vel=VEL_ACCENT)[:2400]
        self.assertGreater(peak(hi), 1.2 * peak(lo))

    def test_a_slide_holds_the_gate_and_glides(self):
        v = plain()
        v[CUTOFF], v[RESO], v[ENVMOD] = 31, 0, 0
        e = self.machine()
        self.oh.send_live(e, bpm=120)
        self.oh.send_sound(e, v)
        first = self.oh.play(e, [MID, 0, MID, MID], note=36, vel=VEL_SLIDE, n=6000)  # a sixteenth
        self.assertGreater(peak(first[4800:6000]), 0.5 * peak(first[:2400]), 'the gate held past 58%')
        nxt = self.oh.play(e, [MID, DRONE, MID, MID], note=48, n=19200)            # held, to hear it arrive
        early = 48000 / period(nxt[:1500], lo=200, hi=740)
        late = 48000 / period(nxt[14400:], lo=200, hi=740)                          # 5 time constants on
        self.assertLess(abs(cents(late, 2 * C2)), 20, 'arrives at C3')
        self.assertTrue(C2 * 1.05 < early < 2 * C2 * 0.97, f'gliding: {early:.1f} Hz')
        plain_ = self.oh.play(e, [MID, 0, MID, MID], note=36, n=6000)                 # a plain step
        self.assertLess(peak(plain_[4800:6000]), 0.1 * peak(plain_[:2400]), 'released at 58%')

    def test_a_slide_into_the_same_note_is_a_tie(self):
        """The envelopes carry on: after a slid step, the same note doesn't open the filter again
        (a new note does)."""
        v = plain()
        v[CUTOFF], v[RESO], v[ENVMOD] = 2, 0, 31

        def second(vel):
            e = self.machine()
            self.oh.send_live(e, bpm=120)
            self.oh.send_sound(e, v)
            before = self.oh.play(e, [MID, 600, MID, MID], note=36, vel=vel, n=6000)
            after = self.oh.play(e, [MID, 600, MID, MID], note=36, n=2400)
            return bright(before[4500:6000]), bright(after[150:1000])
        b, a = second(VEL_SLIDE)
        b2, a2 = second(127)
        print(f'\ntie: before {b:.4f} after {a:.4f}; new note: after {a2:.4f}')
        self.assertLess(a, 1.15 * b, 'tied: the sweep carries on')
        self.assertGreater(a2, 1.3 * a, 'a new note: the sweep starts again')

    # -- the gate ------------------------------------------------------------------------------
    def test_gate_is_58_percent_of_a_sixteenth_and_drone_holds(self):
        v = plain()
        e = self.machine()
        self.oh.send_live(e, bpm=120)
        self.oh.send_sound(e, v)
        x = self.oh.play(e, [MID, 0, MID, MID], n=9600)        # 58% of a sixteenth at 120: 72.5 ms
        self.assertGreater(peak(x[:2400]), 1000)
        # then the release: only the DC blocker's slow (subsonic) offset is left, fading out; the
        # audible part (the sample-to-sample change) is gone
        d = lambda y: [b - a for a, b in zip(y, y[1:])]                      # noqa: E731
        self.assertLess(peak(d(x[4320:4800])), 0.02 * peak(d(x[:2400])))
        x = self.steady(v, note=43, n=19200)
        self.assertGreater(rms(x[-4800:]), 0.3 * rms(x[2400:7200]))

    # -- distortion ------------------------------------------------------------------------------
    def test_every_distortion_changes_the_sound_at_about_its_level(self):
        v = plain()
        v[DISTAMT] = 0
        clean = self.steady(v, note=43)[4800:]
        for t in range(4):
            with self.subTest(dtype=t):
                v[DTYPE] = t
                x = self.steady(v, note=43, algo=DIST, p2=3600)[4800:]
                self.assertNotEqual(x[:960], clean[:960])
                self.assertLess(abs(db(rms(x), rms(clean))), 6.0)
                self.assertAlmostEqual(period(x), period(clean), delta=15)      # the same note

    # -- CPU ----------------------------------------------------------------------------------
    def insns(self, e):
        e.count_insns(True)
        e.insn_count = 0
        e.audio(960)
        e.count_insns(False)
        return e.insn_count / 960

    def cpu(self, outs, v):
        params = [3000, DRONE, MID, MID]
        runs = []
        for e, trig in ((self.machine(outs), True), (verify.boot(self.img, self.sym), True),
                        (self.machine(outs), False)):
            for o in outs:
                if trig and e.class_of(e.slot(o)) == 'cThreeOh':
                    self.oh.send_sound(e, v, out=o)
                if trig:
                    self.oh.play(e, params, out=o, n=64)
            e.audio(960)
            runs.append(self.insns(e))
        return runs[0] - runs[1], runs[0] - runs[2]

    def test_cpu(self):
        res = {}
        for name, i in (('clean', 0), ('filter drive', 9), ('overdrive', 11), ('distortion', 15)):
            v = preset(i)
            (one, own1), (four, own4) = self.cpu((0,), v), self.cpu((0, 1, 2, 3), v)
            res[name] = (one, own1, four)
        print('\n3 Oh CPU (over stock one voice, own one voice, over stock four):',
              {k: tuple(round(x) for x in t) for k, t in res.items()})
        self.assertLess(max(r[0] for r in res.values()), 400)


if __name__ == '__main__':
    unittest.main()
