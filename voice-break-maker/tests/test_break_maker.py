"""BREAK MAKER on the M7, through main()'s message loop and the real audio callback, as a voice-map
engine: the running break, tempo, jumps, CHOP, the gate, velocity kinds, PARAM1's jobs, the pads,
the live generator, pitch modes, CPU. Needs arm-none-eabi-gcc."""
import shutil
import tempfile
import unittest

from fixtures import build_patched
from perkons import verify
from perkons.patch import PatchError
from bm_rig import (CHOP, GATE_OFF, GEN, MANGLE, MID, ROLL, RUN, STUTTER, VEL, BreakMaker, db, drum_break, freq,
                    make_break, p2_of_slice, peak, rms, slice_of)

SR = 48000


def step_len(bpm):
    return SR * 15.0 / bpm


class TestBreakMaker(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('arm-none-eabi-gcc'):
            raise unittest.SkipTest('arm-none-eabi-gcc not installed')
        cls.tmp = tempfile.mkdtemp(prefix='pk-bm-')
        cls.bm = BreakMaker(cls.tmp + '/a')
        cls.pcm, cls.starts = make_break()          # 140 BPM, 44.1 kHz: slice s a tone at 200 + 50 s Hz
        cls.drums, cls.kinds, cls.hits = drum_break()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def machine(self, drums=False, outs=(0,)):
        if drums:
            return self.bm.machine(outs, pcm=self.drums)
        return self.bm.machine(outs, pcm=self.pcm, starts=self.starts)

    def slices(self, out, at, speed=1.0, a=300, b=1100):
        """The slice each step plays (its tone), measured just after the step begins."""
        return [slice_of(out[t + a:t + b], speed=speed) for t in at]

    def bar(self, e, params, algo=RUN, mode=ROLL, bpm=140.0, **kw):
        self.bm.knobs(e, params, algo=algo, mode=mode)
        return self.bm.run_bar(e, bpm=bpm, algo=algo, mode=mode, **kw)

    # -- wiring ---------------------------------------------------------------------------------
    def test_remap_to_break_maker_and_back(self):
        e = self.machine(outs=(2,))
        self.assertEqual(e.class_of(e.slot(2)), 'cBreakMaker')
        self.bm.remap(e, [0, 1, 2, 3])
        self.assertEqual(e.class_of(e.slot(2)), 'cThirdSlot')

    def test_needs_voice_map(self):
        with self.assertRaises(PatchError):
            build_patched(self.bm.stock, ['voice_break_maker'], self.tmp + '/b')

    def test_not_with_step_sampler(self):
        with self.assertRaises(PatchError):
            build_patched(self.bm.stock, ['voice_map', 'voice_step_sampler', 'voice_break_maker'], self.tmp + '/c')

    def test_silent_without_a_break(self):
        e = self.bm.machine()
        self.bm.knobs(e, [MID, GATE_OFF, 0, 0])
        out, _ = self.bm.run_bar(e, steps=range(4))
        self.assertEqual(peak(out), 0)

    # -- the running break ----------------------------------------------------------------------
    def test_run_plays_the_break_in_order(self):
        e = self.machine()
        out, at = self.bar(e, [MID, GATE_OFF, 0, 0])
        self.assertEqual(self.slices(out, at), list(range(16)))
        self.assertGreater(peak(out), 4000)

    def test_tempo_sets_the_speed(self):
        """At 175 BPM the 140 BPM break keeps its slices on the steps, a third faster (repitch)."""
        e = self.machine()
        out, at = self.bar(e, [MID, GATE_OFF, 0, 0], bpm=175.0)
        got = self.slices(out[:], at[4:], speed=175 / 140.0)
        self.assertEqual(got, list(range(4, 16)))

    def test_a_trigger_jumps_and_the_break_carries_on(self):
        e = self.machine()
        out, at = self.bar(e, [MID, GATE_OFF, 0, p2_of_slice(9)], on={4}, trig_at={4: VEL['plain']})
        want = [0, 1, 2, 3] + [(9 + i) % 16 for i in range(12)]
        self.assertEqual(self.slices(out, at), want)
        out, at = self.bm.run_bar(e, steps=range(3))            # the next bar: in place again
        self.assertEqual(self.slices(out, at), [0, 1, 2])

    def test_a_trigger_on_follow_restarts_its_own_slice(self):
        e = self.machine()
        self.bm.knobs(e, [MID, GATE_OFF, 0, 0])
        self.bm.run_bar(e, steps=range(4))
        n0 = self.bm.status(e)[1]
        out, at = self.bm.run_bar(e, steps=[4, 5], on={4}, trig_at={4: VEL['plain']})
        self.assertEqual(self.slices(out, at), [4, 5])
        self.assertEqual(self.bm.status(e)[1] - n0, 2)

    def test_a_step_that_is_on_waits_for_its_trigger(self):
        """A step whose trigger doesn't come (probability): the slice before runs on."""
        e = self.machine()
        self.bm.knobs(e, [MID, GATE_OFF, 0, 0])
        self.bm.run_bar(e, steps=range(4))
        n0 = self.bm.status(e)[1]
        self.bm.run_bar(e, steps=[4, 5], on={4})
        self.assertEqual(self.bm.status(e)[1] - n0, 1)         # only step 5 started one

    def test_the_loops_first_step_resets_the_jump(self):
        e = self.machine()
        out, at = self.bar(e, [MID, GATE_OFF, 0, p2_of_slice(2)], steps=range(4, 9), first=4, last=8,
                           on={5}, trig_at={5: VEL['plain']})
        self.assertEqual(self.slices(out, at), [4, 2, 3, 4, 5])
        out, at = self.bm.run_bar(e, steps=[4, 5, 6], first=4, last=8)
        self.assertEqual(self.slices(out, at), [4, 5, 6])

    def test_stopping_fades_out(self):
        e = self.machine()
        self.bm.knobs(e, [MID, GATE_OFF, 0, 0])
        self.bm.run_bar(e, steps=range(3))
        self.bm.step(e, 0, playing=False)
        out = e.audio(1920)[0]
        self.assertLess(peak(out[960:]), 20)
        self.assertEqual(self.bm.status(e)[2], -1)

    # -- CHOP -----------------------------------------------------------------------------------
    def test_chop_plays_only_triggers(self):
        e = self.machine()
        out, at = self.bar(e, [MID, 1600, 0, p2_of_slice(7)], algo=CHOP, steps=range(6), on={2},
                           trig_at={2: VEL['plain']})
        L = int(step_len(140))
        self.assertLess(peak(out[:at[2]]), 20)
        self.assertEqual(slice_of(out[at[2] + 300:at[2] + 1100]), 7)
        self.assertLess(peak(out[at[4]:]), 20)                 # the gate closed

    def test_chop_trigger_key_while_stopped(self):
        e = self.machine()
        self.bm.knobs(e, [MID, GATE_OFF, 0, p2_of_slice(11)], algo=CHOP)
        self.bm.trig(e, algo=CHOP)
        out = e.audio(1280)[0]
        self.assertEqual(slice_of(out[300:1100]), 11)

    # -- the gate -------------------------------------------------------------------------------
    def test_gate_short_leaves_most_of_each_step_silent(self):
        e = self.machine()
        out, at = self.bar(e, [MID, 0, 0, 0], steps=range(8))
        L = int(step_len(140))
        for t in at[1:7]:
            self.assertGreater(rms(out[t:t + 40]), 500)
            self.assertLess(peak(out[t + L // 3:t + L - 100]), 30)

    # -- velocity: how a step plays -------------------------------------------------------------
    def level_at(self, vel, p1=0, mode=ROLL, drums=False, slice_=5):
        e = self.machine(drums)
        self.bm.knobs(e, [MID, GATE_OFF, p1, p2_of_slice(slice_)], mode=mode)
        self.bm.run_bar(e, steps=range(4))
        n0 = self.bm.status(e)[1]
        out, at = self.bm.run_bar(e, steps=[4], on={4}, trig_at={4: vel}, mode=mode)
        return out, self.bm.status(e)[1] - n0

    def test_ghost_is_12_db_down(self):
        plain, _ = self.level_at(VEL['plain'])
        ghost, _ = self.level_at(VEL['ghost'])
        self.assertAlmostEqual(db(rms(ghost[200:]), rms(plain[200:])), -12, delta=1.0)

    def test_reverse_plays_the_slice_backwards(self):
        fwd, _ = self.level_at(VEL['plain'], drums=True, slice_=0)
        rev, _ = self.level_at(VEL['reverse'], drums=True, slice_=0)
        q = len(fwd) // 4
        self.assertGreater(rms(fwd[:q]), 1.5 * rms(fwd[-q:]))     # the kick decays ...
        self.assertGreater(rms(rev[-q:]), 1.5 * rms(rev[:q]))     # ... backwards it swells

    def test_velocity_rolls(self):
        for kind, n in (('plain', 1), ('roll2', 2), ('roll3', 3), ('roll4', 4), ('revroll', 2)):
            _, starts = self.level_at(VEL[kind])
            self.assertEqual(starts, n, kind)

    def test_param1_roll(self):
        for p1, n in ((0, 1), (800, 2), (1500, 3), (2200, 4), (2900, 6), (4095, 8)):
            _, starts = self.level_at(VEL['plain'], p1=p1)
            self.assertEqual(starts, n, p1)

    def test_stutter_holds_the_attack(self):
        plain, _ = self.level_at(VEL['plain'], drums=True, slice_=0)
        stut, _ = self.level_at(VEL['plain'], p1=4095, mode=STUTTER, drums=True, slice_=0)
        q = len(plain) // 4
        self.assertGreater(db(rms(plain[:q]), rms(plain[-q:])), 4)
        self.assertLess(abs(db(rms(stut[:q]), rms(stut[-q:]))), 2)

    # -- pitch ----------------------------------------------------------------------------------
    def test_tune_repitch_and_keep_length(self):
        up = round(4095 * (0.5 + 12 / 24))
        L = int(step_len(140))
        for keep, end_slice in ((False, 4), (True, 3)):
            e = self.machine()
            self.bm.settings(e, keep=keep)
            out, at = self.bar(e, [up, GATE_OFF, 0, 0], steps=range(4))
            t = at[3]
            self.assertAlmostEqual(freq(out[t + 200:t + 700]) / 2, 350, delta=15)
            m = int(0.6 * L)
            self.assertEqual(slice_of(out[t + m:t + m + 600], speed=2.0), end_slice, keep)

    # -- pads -----------------------------------------------------------------------------------
    def test_pad_half_speed(self):
        e = self.machine()
        self.bm.settings(e, pads=('half',))
        out, at = self.bar(e, [MID, GATE_OFF, 0, 0], steps=range(4))
        self.assertAlmostEqual(freq(out[at[2] + 300:at[2] + 1100]), (200 + 50 * 2) / 2, delta=8)

    def test_pad_roll_repeats_the_slice(self):
        e = self.machine()
        self.bm.knobs(e, [MID, GATE_OFF, 0, 0])
        self.bm.run_bar(e, steps=range(6))
        self.bm.settings(e, pads=('roll16',))
        out, at = self.bm.run_bar(e, steps=range(6, 10))
        self.assertEqual(set(self.slices(out, at)), {5})
        self.bm.settings(e)
        out, at = self.bm.run_bar(e, steps=[10, 11])
        self.assertEqual(self.slices(out, at), [10, 11])

    def test_pad_tape_stop(self):
        e = self.machine()
        self.bm.knobs(e, [MID, GATE_OFF, 0, 0])
        self.bm.run_bar(e, steps=range(2))
        self.bm.settings(e, pads=('tapestop',))
        out, at = self.bm.run_bar(e, steps=range(2, 8))
        self.assertLess(rms(out[at[5]:]), rms(out[:at[1]]) / 4)

    def test_pad_filter_throw(self):
        e = self.machine()
        self.bm.knobs(e, [MID, GATE_OFF, 0, p2_of_slice(14)])
        self.bm.settings(e, pads=('filter',))
        out, at = self.bm.run_bar(e, steps=range(6), on=set(range(6)), trig_at={s: 127 for s in range(6)})
        e2 = self.machine()
        self.bm.knobs(e2, [MID, GATE_OFF, 0, p2_of_slice(14)])
        dry, _ = self.bm.run_bar(e2, steps=range(6), on=set(range(6)), trig_at={s: 127 for s in range(6)})
        self.assertLess(db(rms(out[at[5]:]), rms(dry[at[5]:])), -12)

    def test_pad_random_and_reverse(self):
        e = self.machine()
        self.bm.settings(e, pads=('random',))
        out, at = self.bar(e, [MID, GATE_OFF, 0, 0])
        self.assertNotEqual(self.slices(out, at), list(range(16)))
        e = self.machine(drums=True)
        self.bm.settings(e, pads=('reverse',))
        out, at = self.bar(e, [MID, GATE_OFF, 0, 0], steps=[0])
        q = len(out) // 4
        self.assertGreater(rms(out[-q:]), 1.5 * rms(out[:q]))

    # -- the live generator ---------------------------------------------------------------------
    def test_generate_changes_steps_and_keeps_most_beats(self):
        e = self.machine()
        got = []
        for _ in range(3):
            out, at = self.bar(e, [MID, GATE_OFF, 4095, 0], algo=GEN, mode=MANGLE)
            got.append(self.slices(out, at))
        off = sum(g[s] != s for g in got for s in range(16) if s % 4)
        beats = sum(g[s] == s for g in got for s in range(0, 16, 4))
        self.assertGreater(off, 20)                             # of 36
        self.assertGreater(beats, 6)                            # of 12 (anchors at 70%)

    def test_run_without_mangle_is_straight(self):
        e = self.machine()
        out, at = self.bar(e, [MID, GATE_OFF, 0, 0], mode=MANGLE)
        self.assertEqual(self.slices(out, at), list(range(16)))

    # -- the page plays a slice -----------------------------------------------------------------
    def test_audition(self):
        e = self.machine()
        self.bm.knobs(e, [MID, GATE_OFF, 0, 0])
        self.bm.settings(e, aud=(5 + 1) | 0x20)
        out = e.audio(1280)[0]
        self.assertEqual(self.bm.status(e)[0], 5)
        self.assertEqual(slice_of(out[300:1100]), 5)

    # -- CPU ------------------------------------------------------------------------------------
    def insns(self, e):
        e.count_insns(True)
        e.insn_count = 0
        e.audio(960)
        e.count_insns(False)
        return e.insn_count / 960

    def cpu(self, outs, params=(MID, GATE_OFF, 0, 0), keep=False, pads=()):
        runs = []
        for e in (self.bm.machine(outs, pcm=self.pcm, starts=self.starts), verify.boot(self.bm.img, self.bm.sym)):
            if e.class_of(e.slot(outs[0])) == 'cBreakMaker':
                for o in outs:
                    self.bm.settings(e, out=o, keep=keep, pads=pads)
                    self.bm.knobs(e, list(params), out=o)
                    self.bm.step(e, 3, out=o)
            e.audio(960)
            runs.append(self.insns(e))
        return runs[0] - runs[1]

    def test_cpu(self):
        up = round(4095 * (0.5 + 7 / 24))
        res = {'run': self.cpu((0,)), 'keep length': self.cpu((0,), params=(up, GATE_OFF, 0, 0), keep=True),
               'filter throw': self.cpu((0,), pads=('filter',)), 'four': self.cpu((0, 1, 2, 3))}
        print('\nBREAK MAKER CPU (over stock):', {k: round(v) for k, v in res.items()})
        self.assertLess(max(res['run'], res['keep length'], res['filter throw']), 300)


if __name__ == '__main__':
    unittest.main()
