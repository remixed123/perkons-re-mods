"""Kick Lab on the M7, through main()'s message loop and the real audio callback, as a
voice-map engine: the panel, the envelopes, the noise, the level. Needs arm-none-eabi-gcc."""
import math
import shutil
import tempfile
import unittest

from fixtures import build_patched
from perkons import verify
from perkons.patch import PatchError
from kl_rig import (ATTACK, CLEAN, DELAY_MIX, DRIVE_MIX, DRONE, DRONE_AT, MID_DECAY, PINK, SR, WHITE,
                    KickLab, env, rms, tune_for)


def f0(x):
    """Frequency from interpolated upward zero crossings."""
    t = [i + -a / (b - a) for i, (a, b) in enumerate(zip(x, x[1:])) if a < 0 <= b]
    return SR * (len(t) - 1) / (t[-1] - t[0]) if len(t) > 2 else float('nan')


def cents(f, ref):
    return 1200 * math.log2(f / ref)


def hf(x):
    """First-difference RMS: high-frequency energy."""
    return rms([b - a for a, b in zip(x, x[1:])])


@unittest.skipUnless(shutil.which('arm-none-eabi-gcc'), 'arm-none-eabi-gcc not installed')
class TestKickLab(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix='pk-kl-')
        cls.kl = KickLab(cls.tmp + '/a')
        cls.stock, cls.sym, cls.img = cls.kl.stock, cls.kl.sym, cls.kl.img

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    # -- helpers -------------------------------------------------------------------------
    def machine(self, outs=(0,)):
        return self.kl.machine(outs)

    def play(self, e, params, **kw):
        return self.kl.play(e, params, **kw)

    # -- wiring ---------------------------------------------------------------------------
    def test_remap_to_kick_lab_and_back(self):
        e = self.machine((2,))
        self.assertEqual(e.class_of(e.slot(2)), 'cKickLab')
        self.assertGreater(max(map(abs, self.play(e, [tune_for(50), MID_DECAY, 2048, 0], out=2))), 1000)
        self.kl.remap(e, [0, 1, 2, 3])
        self.assertEqual(e.class_of(e.slot(2)), 'cThirdSlot')

    def test_needs_voice_map(self):
        with self.assertRaises(PatchError):
            build_patched(self.stock, ['voice_kick_lab'], self.tmp + '/b')

    def test_stock_outputs_unchanged(self):
        bad = [r for r in verify.compare_to_stock(self.stock, self.img, self.sym) if not r[4]]
        self.assertEqual(bad, [])

    # -- pitch ----------------------------------------------------------------------------
    def steady_pitch(self, tune, note=36):
        """The pitch just after the hit, no sweep, in drone (the drop then takes 4 s)."""
        x = self.play(self.machine(), [tune, DRONE, 0, 0], note=note, n=9600)
        return f0(x[2400:9600])

    def test_tune_range_is_20_to_200_hz(self):
        for hz in (20, 50, 63.2, 120, 200):
            with self.subTest(hz=hz):
                self.assertLess(abs(cents(self.steady_pitch(tune_for(hz)), hz)), 15)

    def test_midi_note_transposes(self):
        ref = self.steady_pitch(tune_for(50))
        for note, st in ((24, -12), (48, 12), (43, 7)):
            with self.subTest(note=note):
                self.assertLess(abs(cents(self.steady_pitch(tune_for(50), note), ref) - 100 * st), 10)

    def test_sweep_raises_the_start(self):
        def cycles(p1):                 # in the first 30 ms (the preset's sweep lasts 40 ms)
            x = self.play(self.machine(), [tune_for(50), DRONE, p1, 0], n=1440)
            return sum(1 for a, b in zip(x, x[1:]) if a < 0 <= b)
        self.assertLessEqual(cycles(0), 2)                  # 50 Hz: 1.5 cycles
        self.assertGreaterEqual(cycles(2048), cycles(0) + 2)
        self.assertGreater(cycles(4095), cycles(2048))

    def test_drop_lands_below_the_pitch_in_drone(self):
        """The preset's drop (-2.16 semitones) runs over 4 s in drone, then stays."""
        e = self.machine()
        x = self.play(e, [tune_for(80), DRONE, 0, 0], n=4800)
        for _ in range(9):
            x = self.play(e, [tune_for(80), DRONE, 0, 0], trig=False, n=24000)
        self.assertLess(abs(cents(f0(x[4800:]), 80) - (-216)), 15)

    # -- envelopes -----------------------------------------------------------------------
    def length_s(self, decay):
        """How long the hit stays above -40 dB of its peak: half the decay time (-80 dB)."""
        x = self.play(self.machine(), [tune_for(50), decay, 0, 0], n=4 * SR)
        e = env(x)
        pk = max(e)
        return max(i for i, v in enumerate(e) if v > pk * 0.01) * 0.01

    def test_decay_range(self):
        short, mid, long_ = self.length_s(0), self.length_s(MID_DECAY), self.length_s(DRONE_AT - 60)
        self.assertLess(short, 0.03)
        self.assertTrue(0.1 < mid < 0.3, mid)
        self.assertGreater(long_, 1.6)

    def test_drone_from_95_percent(self):
        for d in (DRONE_AT + 5, DRONE):
            with self.subTest(decay=d):
                e = self.machine()
                x = self.play(e, [tune_for(50), d, 0, 0], n=SR)
                x = self.play(e, [tune_for(50), d, 0, 0], trig=False, n=SR)
                e_ = env(x)
                self.assertGreater(min(e_), 0.9 * max(e_), 'holds a second later')

    def test_turning_decay_down_releases_a_drone(self):
        e = self.machine()
        self.play(e, [tune_for(50), DRONE, 0, 0], n=SR)
        x = self.play(e, [tune_for(50), 0, 0, 0], trig=False, n=SR)
        self.assertEqual(max(map(abs, x[-4800:])), 0)

    def test_silent_after_a_short_decay(self):
        x = self.play(self.machine(), [tune_for(50), 0, 0, 0], n=SR)
        self.assertEqual(max(map(abs, x[-4800:])), 0)

    def peak_ms(self, p2, algo):
        """ALGO set first, then PARAM2 (a new ALGO keeps its job's value until the knob moves)."""
        e = self.machine()
        self.play(e, [tune_for(50), DRONE, 0, 4095 - p2], algo=algo, n=64, trig=False)
        x = self.play(e, [tune_for(50), DRONE, 0, p2], algo=algo, vel=48, n=SR // 2)   # (below the limiter)
        e = env(x, 48)
        return next(i for i, v in enumerate(e) if v >= 0.95 * max(e))

    def test_attack_on_algo_1(self):
        self.assertLess(self.peak_ms(0, ATTACK), 25)
        self.assertGreater(self.peak_ms(4095, ATTACK), 150)

    def test_param2_is_not_attack_on_algo_2_and_3(self):
        for algo, p2 in ((DELAY_MIX, 1000), (DRIVE_MIX, 4095)):     # (a full delay mix has no dry hit)
            with self.subTest(algo=algo):
                self.assertLess(self.peak_ms(p2, algo), 25)

    # -- noise ------------------------------------------------------------------------------
    def test_mode_noise(self):
        """MODE 1 clean, 2 white, 3 pink (darker than white)."""
        tail = {}
        for m in (CLEAN, WHITE, PINK):
            x = self.play(self.machine(), [tune_for(50), MID_DECAY, 0, 0], mode=m, n=9600)
            tail[m] = x[960:4800]
        self.assertGreater(hf(tail[WHITE]), 5 * hf(tail[CLEAN]))
        self.assertGreater(hf(tail[PINK]), 2 * hf(tail[CLEAN]))
        self.assertGreater(hf(tail[WHITE]), 2 * hf(tail[PINK]))

    def test_noise_decays_with_decay_and_holds_in_drone(self):
        short = self.play(self.machine(), [tune_for(50), 0, 0, 0], mode=WHITE, n=SR)
        self.assertEqual(max(map(abs, short[-4800:])), 0)
        e = self.machine()
        self.play(e, [tune_for(50), DRONE, 0, 0], mode=WHITE, n=SR)
        x = self.play(e, [tune_for(50), DRONE, 0, 0], mode=WHITE, trig=False, n=9600)
        self.assertGreater(hf(x), 200)

    # -- level --------------------------------------------------------------------------------
    def test_velocity_is_linear(self):
        def peak(vel):
            return max(map(abs, self.play(self.machine(), [tune_for(50), MID_DECAY, 0, 0], vel=vel, n=4800)))
        self.assertAlmostEqual(peak(32) / peak(64), 32 / 64, delta=0.03)       # (below the limiter)

    def test_peaks_stay_under_the_ceiling(self):
        """Peaks near the stock kick's (~15000-16250): the limiter holds 16000, the soft clip never passes 16800."""
        for params, mode in (([4095, DRONE, 4095, 0], WHITE), ([0, DRONE, 4095, 0], PINK),
                             ([tune_for(50), MID_DECAY, 2048, 0], CLEAN)):
            with self.subTest(params=params, mode=mode):
                x = self.play(self.machine(), params, mode=mode, n=9600)
                self.assertLessEqual(max(map(abs, x)), 16800)       # (the soft clip's ceiling)
                self.assertGreater(max(map(abs, x)), 10000)

    def test_mute_fades_like_a_stock_slot(self):
        e = self.machine()
        self.play(e, [tune_for(80), DRONE, 0, 0], n=960)
        voices = [{'params': [2048] * 4} for _ in range(4)]
        voices[0] = {'params': [tune_for(80), DRONE, 0, 0], 'algo': ATTACK, 'mode': CLEAN, 'mute': 1}
        e.send_voice_state(voices)
        e.pump()
        x = e.audio(1440)[0]
        self.assertGreater(max(map(abs, x[:96])), 500, 'fades, not cuts')
        self.assertEqual(max(map(abs, x[720:])), 0, 'silent ~10 ms after mute')

    def test_retrigger_restarts_the_hit(self):
        e = self.machine()
        a = self.play(e, [tune_for(50), MID_DECAY, 2048, 0], n=9600)
        b = self.play(e, [tune_for(50), MID_DECAY, 2048, 0], n=9600)
        self.assertGreater(max(map(abs, b[:960])), 0.9 * max(map(abs, a[:960])))

    # -- CPU ----------------------------------------------------------------------------------
    def insns(self, e):
        e.count_insns(True)
        e.insn_count = 0
        e.audio(960)
        e.count_insns(False)
        return e.insn_count / 960

    def cpu(self, outs):
        """(over stock, the engine's own): instructions per sample, the heaviest settings
        (sweep and saturation drive up, drone, pink noise) on `outs`."""
        params = [tune_for(50), DRONE, 4095, 0]
        runs = []
        for e, trig in ((self.machine(outs), True), (verify.boot(self.img, self.sym), True),
                        (self.machine(outs), False)):
            for o in outs:
                if trig:
                    self.play(e, params, mode=PINK, out=o, n=64)
            e.audio(960)
            runs.append(self.insns(e))
        return runs[0] - runs[1], runs[0] - runs[2]

    def test_cpu(self):
        (one, own1), (four, own4) = self.cpu((0,)), self.cpu((0, 1, 2, 3))
        print(f'\nKick Lab: one voice ~{own1:.0f} instructions per sample of its own, ~{one:.0f} over stock; '
              f'four ~{own4:.0f} of their own, ~{four:.0f} over stock')
        self.assertLess(one, 800)
