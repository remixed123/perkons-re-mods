"""BREAK MAKER on the M4: the break from the card (format, bank folder first, cue points, slices on
the hits, what each slice is), the steps to the M7, kits (record BMKR), the configuration page
(break, pads, step types, slices, character, pitch mode), the generator and its undo."""
import shutil
import struct
import tempfile
import unittest

from fixtures import build_patched, require_image
from perkons import m7host
from perkons.emu import Symbols
from perkons.fwimage import Image
from perkons.rig import M4Rig
from bm_rig import (BM_BASE, GHOST, HAT, KICK, MAGIC, SNARE, VEL, drum_break, make_break, p2_of_slice, p2_zone,
                    wav)

MAIN, KIT_MODE = 0, 5
SHIFT, KIT, MOD, SHUFFLE = 0x47, 0x41, 0x43, 0x48
TUNE, PARAM1, PARAM2, ALGO = 0, 1, 5, 8
VS_VALUE, VS_BASE, VS_STEP = 0x50, 0x6a, 0x1c
BLK_LOCKS, LOCK_SIZE = 0x8c, 0x1a
RUN, CHOP = 1, 2
VOICE = 1                                   # the voice running BREAK MAKER
_image = {}


def image():
    if not _image:
        img, p = build_patched(Image.load(require_image('v1.3.0')), ['voice_map', 'voice_break_maker:5'],
                               tempfile.mkdtemp())
        names = sorted(['break-maker', 'break-maker-step', 'voice-map'])
        msg = {n: m7host.FIRST_MESSAGE + i for i, n in enumerate(names)}
        _image['x'] = (img, p.cave_symbols['m4host'], msg)
        _image['m7'] = p.cave_symbols['m7host']
    return _image['x']


def ctrl(r):
    """The control block: (magic, ready, gen, frames, rate, pcm, slices[17], types[16], number)."""
    b = bytes(r.e.uc.mem_read(BM_BASE, 0x80))
    head = struct.unpack_from('<6I', b)
    sl = list(struct.unpack_from('<17I', b, 24))
    return head, sl, list(b[92:108]), b[108]


def bars(r):
    return r.e.r8(BM_BASE + 109)


def cut(r):
    """(how many slices, the step each starts on)."""
    n = r.e.r8(BM_BASE + 110)
    return n, list(bytes(r.e.uc.mem_read(BM_BASE + 112, n)))


def set_tempo(r, bpm):
    r.e.uc.mem_write(r.sym.g['clock_state'] + 0x14, struct.pack('<f', bpm))


class TestBreakMakerM4(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('arm-none-eabi-gcc'):
            raise unittest.SkipTest('arm-none-eabi-gcc not installed')
        cls.drums, cls.kinds, cls.hits = drum_break(offset=200)
        cls.tones, cls.starts = make_break(bpm=175.0)

    def rig(self, sd=None, engine=True):
        img, cave, self.msg = image()
        if sd is None:
            sd = {'BREAKS/1.wav': wav(self.drums)}
        sd.setdefault('BANKS/00/KITS/00.KIT', b'')
        r = M4Rig(img, cave, sd=sd)
        first = r.sym.const['page_count']
        self.pages = {n: first + i for i, n in enumerate(sorted(['break_maker', 'engines']))}
        r.ms = 1000
        r.e.stub(r.sym.fn['hal_get_tick'], lambda e, *a: r.ms)
        r.e.stub(r.sym.fn['knob_handler'], lambda *a: 0)
        r.e.sent(2)
        if engine:
            self.use_engine(r)
        return r

    def use_engine(self, r, voice=VOICE, on=True):
        m = r.r16(r.cave['cur'])
        nib = 4 if on else voice
        r.w16(r.cave['cur'], (m & ~(0xf << 4 * voice)) | nib << 4 * voice)

    def loop(self, r, ms=5, n=1):
        for _ in range(n):
            r.ms += ms
            r.e.call(r.cave['pk_bm_main_loop'], max_insns=200_000_000)

    def load(self, r, passes=20):
        self.loop(r, n=passes)
        return ctrl(r)

    def sent(self, r, kind):
        return [p for t, p in r.e.sent(2) if t == self.msg[kind]]

    # -- the break from the card ----------------------------------------------------------------
    def test_loads_the_break_with_slices_on_the_hits(self):
        r = self.rig()
        (magic, ready, gen, frames, rate, pcm), sl, types, number = self.load(r)
        self.assertEqual((magic, ready, frames, rate, number), (MAGIC, 1, len(self.drums), 44100, 0))
        got = struct.unpack(f'<{frames}h', bytes(r.e.uc.mem_read(pcm, 2 * frames)))
        self.assertEqual(list(got[:2000]), self.drums[:2000])
        self.assertEqual(list(got[-500:]), self.drums[-500:])
        self.assertEqual(sl[0], 0)
        self.assertEqual(sl[16], frames)
        for i in range(1, 16):              # each cut moved onto its hit (which is 200 frames late)
            self.assertLess(abs(sl[i] - self.hits[i]), 48, i)

    def test_sorts_the_slices(self):
        r = self.rig()
        _, _, types, _ = self.load(r)
        self.assertEqual(types, self.kinds)

    def cycle_cut(self, r, times=1):
        self.open_page(r)
        for _ in range(times):
            self.press(r, 48 + 10)
        self.loop(r)

    def test_the_cut_cycles_and_recuts_live(self):
        r = self.rig()
        (_, _, gen, _, _, _), _, _, _ = self.load(r)
        self.assertEqual(cut(r), (16, list(range(16))))
        self.cycle_cut(r)
        n, steps = cut(r)
        _, sl, types, _ = ctrl(r)
        self.assertEqual((n, steps), (8, list(range(0, 16, 2))))
        for i in range(1, 8):                        # each cut on its hit
            self.assertLess(abs(sl[i] - self.hits[2 * i]), 48, i)
        self.assertEqual(types[:8], self.kinds[::2])
        self.assertEqual(sl[8], len(self.drums))
        leds = self.ui(r)
        self.assertEqual(leds[3][10], 0x38)          # 8: medium
        self.assertEqual(leds[2][7:9], [0x38 if self.kinds[14] in (KICK, SNARE) else 0x10, 0])
        self.cycle_cut(r)
        self.assertEqual(cut(r), (4, [0, 4, 8, 12]))
        self.assertEqual(ctrl(r)[0][2], gen)         # not loaded again
        self.cycle_cut(r)                            # Mix: 16 slices
        self.assertEqual(cut(r), (16, list(range(16))))
        self.assertEqual(r.e.r8(BM_BASE + 111), 3)
        self.cycle_cut(r)                            # 16 again
        self.assertEqual((cut(r)[0], r.e.r8(BM_BASE + 111)), (16, 0))

    def test_the_cut_is_kept_with_the_kit(self):
        r = self.rig()
        self.load(r)
        self.cycle_cut(r, 2)                         # 4
        buf = 0x2407e000
        n = r.e.call(r.cave['pk_bm_kit_save'], 0, 5, buf, 64)
        self.assertEqual(bytes(r.e.uc.mem_read(buf, n))[0], 0 | 2 << 4)

    def test_generator_with_eight_slices(self):
        r = self.rig()
        self.load(r)
        self.cycle_cut(r)
        for algo in (RUN, CHOP):
            bar = self.generate(r, 4, algo=algo)     # Two-step
            k = self.kinds[::2]                      # the 8 slices' kinds
            self.assertEqual(k[bar[0][0]], KICK, algo)
            self.assertEqual(k[bar[4][0]], SNARE, algo)
            self.assertEqual(k[bar[12][0]], SNARE, algo)
            self.assertEqual(k[bar[10][0]], KICK, algo)

    def on_steps(self, r):
        blk = r.block(VOICE)
        return [s for s in range(16) if r.e.r8(blk + 8 * s) == 1]

    def test_cut_in_8_characters_stay_on_the_eighths(self):
        r = self.rig()
        self.load(r)
        self.cycle_cut(r)
        for chr_ in range(8):
            for algo in (RUN, CHOP):
                for _ in range(3):
                    self.generate(r, chr_, algo=algo)
                    self.assertEqual([s for s in self.on_steps(r) if s % 2], [], (chr_, algo))

    def test_cut_in_4_plays_whole_beats(self):
        r = self.rig()
        self.load(r)
        self.cycle_cut(r, 2)
        beat_kind = [self.kinds[t] for t in (0, 4, 8, 12)]         # kick, snare, hat, snare
        for chr_ in range(8):
            for algo in (RUN, CHOP):
                self.generate(r, chr_, algo=algo)
                self.assertTrue(set(self.on_steps(r)) <= {0, 4, 8, 12}, (chr_, algo))
        bar = self.generate(r, 1, algo=CHOP)                         # Sparse: the bare backbeat
        self.assertEqual([beat_kind[bar[t][0]] for t in (0, 4, 8, 12)], [KICK, SNARE, KICK, SNARE])
        for _ in range(4):
            bar = self.generate(r, 3, algo=CHOP)                     # Halftime: the snare on 3 only
            self.assertEqual([beat_kind[bar[t][0]] == SNARE for t in (0, 4, 8, 12)],
                             [False, False, True, False])

    def pieces(self, bar):
        """The bar as runs of slices in order: [(step, first slice, steps)]."""
        out = []
        for s, x in enumerate(bar):
            if out and x is not None and bar[s - 1] is not None and x[0] == (bar[s - 1][0] + 1) % 16:
                out[-1][2] += 1
            elif x is not None:
                out.append([s, x[0], 1])
        return out

    def test_mix_plays_pieces_of_1_2_and_4_steps(self):
        r = self.rig()
        self.load(r)
        self.cycle_cut(r, 3)
        self.assertEqual(cut(r), (16, list(range(16))))
        self.assertEqual(r.e.r8(BM_BASE + 111), 3)
        lens = {0: set(), 5: set(), 7: set()}
        for _ in range(10):
            for chr_ in lens:
                bar = self.generate(r, chr_, algo=CHOP)
                r.ms += 37
                for s, q, n in self.pieces(bar):
                    lens[chr_].add(min(n, 4))
                    if s % 2:                                # off the eighth: only 1-step pieces
                        self.assertTrue(n == 1 or bar[s + 1][0] == (q + 1) % 16, (chr_, bar))
        self.assertIn(4, lens[0])                                    # Straight: beats
        self.assertTrue({1, 2} <= lens[5], lens[5])                  # Jungle: short pieces
        self.assertIn(1, lens[7])                                    # Breakcore: single steps

    def test_cue_points_when_there_are_16(self):
        cues = [0] + [i * 4000 + 37 for i in range(1, 16)]
        r = self.rig({'BREAKS/1.wav': wav(self.drums, cues=cues)})
        _, sl, _, _ = self.load(r)
        self.assertEqual(sl[:16], cues)

    def test_bank_folder_first_and_only_when_a_voice_runs_it(self):
        short = self.drums[:40000]
        r = self.rig({'BREAKS/1.wav': wav(self.drums), 'BANKS/00/BREAKS/1.wav': wav(short)}, engine=False)
        self.assertEqual(self.load(r)[0][1], 0)         # no voice runs it: nothing loaded
        self.use_engine(r)
        (_, ready, _, frames, _, _), _, _, _ = self.load(r)
        self.assertEqual((ready, frames), (1, 40000))

    def test_rejects_other_formats(self):
        for name, data in (('stereo', wav(self.drums, channels=2)), ('24-bit', wav(self.drums, bits=24)),
                           ('not a wav', b'hello' * 100)):
            r = self.rig({'BREAKS/1.wav': data})
            self.assertEqual(self.load(r)[0][1], 0, name)

    def test_a_break_too_long_is_cut_to_the_room(self):
        long_, _, _ = drum_break(bpm=50.0)          # one bar at 50 BPM: 4.8 s at 44.1 kHz
        r = self.rig({'BREAKS/1.wav': wav(long_)})
        set_tempo(r, 50.0)
        (_, ready, _, frames, _, _), _, _, _ = self.load(r, passes=40)
        self.assertEqual((ready, bars(r)), (1, 1))
        self.assertEqual(frames, (0x24080000 - 0x400 - (BM_BASE + 0x100) - 4) // 2)

    def test_a_longer_loop_plays_its_first_bar(self):
        for n, bpm in ((2, 120.0), (4, 100.0)):
            one, _, hits = drum_break(bpm=bpm)
            r = self.rig({'BREAKS/1.wav': wav(one * n)})
            set_tempo(r, bpm)
            (_, ready, _, frames, _, pcm), sl, types, _ = self.load(r, passes=40)
            self.assertEqual((ready, bars(r), frames), (1, n, len(one)), n)
            for i in range(1, 16):
                self.assertLess(abs(sl[i] - hits[i]), 48, (n, i))
            self.assertEqual(types, self.kinds, n)

    def test_the_tempo_decides_a_loop_that_could_be_either(self):
        """2 bars at 175 BPM is also 1 bar at 87.5: the Pērkons's tempo when it loads decides."""
        one, _, _ = drum_break(bpm=175.0)
        for tempo, n in ((90.0, 1), (120.0, 1), (170.0, 2)):
            r = self.rig({'BREAKS/1.wav': wav(one * 2)})
            set_tempo(r, tempo)
            self.load(r, passes=40)
            self.assertEqual(bars(r), n, tempo)

    def test_the_page_chooses_the_break(self):
        r = self.rig({'BREAKS/1.wav': wav(self.drums), 'BREAKS/2.wav': wav(self.tones, cues=self.starts)})
        self.assertEqual(self.load(r)[3], 0)
        self.open_page(r)
        self.press(r, 1)                # row 1, 2nd
        _, sl, _, number = self.load(r)
        self.assertEqual(number, 1)
        self.assertEqual(sl[:16], self.starts)
        leds = self.ui(r)
        self.assertEqual(leds[0][1], 0x80)
        self.assertEqual(leds[0][0], 0x10)          # on the card, not chosen
        self.assertEqual(leds[0][2], 0)             # not on the card

    # -- the steps to the M7 ----------------------------------------------------------------------
    def test_sends_each_step_before_its_triggers(self):
        r = self.rig()
        self.load(r)
        r.set_step(VOICE, 2, velocity=127)
        lk = r.block(VOICE) + BLK_LOCKS + LOCK_SIZE * 2              # step 3: slice 10
        r.w16(lk + 2 * PARAM2, p2_of_slice(9) << 4)
        r.w16(lk + 2 * 11, 1 << PARAM2)
        r.e.sent(2)
        r.play()
        got = []
        for _ in range(4):
            r.step()
            for t, p in r.e.sent(2):
                if t == self.msg['break-maker-step'] and p[4] & 4:
                    got.append(('hit', p[0], p[1], p[7] | p[8] << 8))
                elif t == self.msg['break-maker-step']:
                    got.append(('step', p[0], p[1], p[4]))
                elif t == 0 and p[4 * VOICE]:
                    got.append(('trig', VOICE))
        steps = [g for g in got if g[0] == 'step']
        self.assertEqual([g[2] for g in steps], [0, 1, 2, 3])
        self.assertTrue(all(g[1] == VOICE and g[3] & 1 for g in steps))
        self.assertEqual([g[3] & 2 for g in steps], [0, 0, 2, 0])      # step 3 is on
        hit = got.index(('hit', VOICE, 2, p2_of_slice(9)))
        self.assertLess(got.index(('step', VOICE, 2, 3)), hit)
        self.assertLess(hit, got.index(('trig', VOICE)))
        self.assertEqual(len([g for g in got if g[0] == 'hit']), 1)
        r.e.w8(r.seq, 0)                                             # stopped
        self.loop(r)
        stop = self.sent(r, 'break-maker-step')
        self.assertEqual([(p[0], p[4]) for p in stop], [(VOICE, 0)])

    def test_no_steps_for_other_engines(self):
        r = self.rig(engine=False)
        r.play()
        for _ in range(3):
            r.step()
        self.assertEqual(self.sent(r, 'break-maker-step'), [])

    # -- the page ---------------------------------------------------------------------------------
    def key(self, r, k, down):
        r.event(k, down)

    def press(self, r, k):
        self.key(r, k, True)
        self.key(r, k, False)

    def apply(self, r):
        r.e.call(r.sym.fn['apply_mode'], max_insns=5_000_000)

    def ui(self, r):
        r.leds.clear()
        r.e.call(r.sym.fn['ui_update'], max_insns=20_000_000)
        self.apply(r)
        return [[r.leds.get(16 * v + e, 0) for e in range(16)] for v in range(4)]

    def open_page(self, r, steps=()):
        """Steps (of VOICE's row) held + SHUFFLE on the main page; none: the engine page's long press."""
        r.set_mode(MAIN)
        self.apply(r)
        if steps:
            for s in steps:
                r.held.add(16 * VOICE + s)
            self.key(r, SHUFFLE, True)
            self.apply(r)
            self.key(r, SHUFFLE, False)
            for s in steps:
                r.held.discard(16 * VOICE + s)
        else:
            r.set_mode(KIT_MODE)
            self.key(r, KIT, True)
            self.apply(r)
            self.press(r, MOD)
            self.key(r, KIT, False)
            self.apply(r)
            self.key(r, 16 * VOICE + 4, True)
            self.ui(r)
            r.ms += 800
            self.ui(r)
            self.key(r, 16 * VOICE + 4, False)
        self.apply(r)
        self.assertEqual(r.mode(), self.pages['break_maker'])

    def lock(self, r, step):
        blk = r.block(VOICE)
        lk = blk + BLK_LOCKS + LOCK_SIZE * step
        mask = r.r16(lk + 2 * 11)
        return r.r16(lk + 2 * PARAM2) >> 4 if mask >> PARAM2 & 1 else None

    def vel(self, r, step):
        return r.e.r8(r.block(VOICE) + 8 * step + 7)

    def test_steps_and_shuffle_open_the_page_for_those_steps(self):
        r = self.rig()
        self.load(r)
        for s in (3, 6):
            r.set_step(VOICE, s, velocity=127)
        self.open_page(r, steps=(3, 6))
        self.press(r, 32 + 9)               # row 3: slice 10
        self.assertEqual([self.lock(r, 3), self.lock(r, 6)], [p2_of_slice(9)] * 2)
        self.assertIsNone(self.lock(r, 4))
        self.press(r, 16 + 13)              # row 2, 14: roll x2
        self.assertEqual(self.vel(r, 3), VEL['roll2'])
        leds = self.ui(r)
        self.assertEqual(leds[1][13], 0x80)
        self.press(r, 16 + 12)              # reverse with roll x2: both
        self.assertEqual(self.vel(r, 6), VEL['revroll'])
        self.press(r, 16 + 11)              # ghost
        self.assertEqual(self.vel(r, 3), VEL['ghost'])
        self.press(r, 16 + 11)              # ghost again: plain
        self.assertEqual(self.vel(r, 3), VEL['plain'])
        self.press(r, SHUFFLE)
        self.apply(r)
        self.assertEqual(r.mode(), MAIN)

    def test_engine_page_long_press_opens_it_and_shift_goes_back(self):
        r = self.rig()
        self.open_page(r)
        self.press(r, SHIFT)
        self.apply(r)
        self.assertEqual(r.mode(), self.pages['engines'])

    def test_pads_while_held(self):
        r = self.rig()
        self.open_page(r)
        self.key(r, 16 + 1, True)           # roll 1/16
        self.loop(r)
        p = self.sent(r, 'break-maker')[-1]
        self.assertEqual((p[0], p[2] | p[3] << 8), (VOICE, 2))
        self.key(r, 16 + 1, False)
        self.loop(r)
        self.assertEqual(self.sent(r, 'break-maker')[-1][2], 0)

    def test_audition_and_character_and_pitch_mode(self):
        r = self.rig()
        self.open_page(r)
        self.press(r, 32 + 6)               # row 3 without held steps: hear slice 7
        self.loop(r)
        a = self.sent(r, 'break-maker')[-1][5]
        self.press(r, 32 + 6)
        self.loop(r)
        b = self.sent(r, 'break-maker')[-1][5]
        self.assertEqual((a & 31, b & 31), (7, 7))
        self.assertNotEqual(a, b)
        self.press(r, 48 + 5)               # Jungle
        self.press(r, 48 + 9)               # keep length
        self.loop(r)
        p = self.sent(r, 'break-maker')[-1]
        self.assertEqual((p[1], p[4]), (1, 5))

    # -- kits -------------------------------------------------------------------------------------
    def test_kit_record(self):
        r = self.rig()
        self.open_page(r)
        self.press(r, 3)                    # break 4
        self.press(r, 48 + 2)               # Boom bap
        self.press(r, 48 + 9)               # keep length
        buf = 0x2407e000
        n = r.e.call(r.cave['pk_bm_kit_save'], 0, 5, buf, 64)
        rec = bytes(r.e.uc.mem_read(buf, n))
        self.assertEqual(rec, bytes([3, 0, 2 | 8, 0, 0]))
        r2 = self.rig()
        r2.e.uc.mem_write(buf, bytes([7, 1, 0, 6 | 8, 0]))
        r2.e.call(r2.cave['pk_bm_kit_restore'], 0, 9, buf, 5)
        r2.e.sent(2)
        r2.e.call(r2.cave['pk_bm_kit_load'], 9, max_insns=50_000_000)
        self.loop(r2)
        got = {p[0]: (p[1], p[4]) for p in self.sent(r2, 'break-maker')}
        self.assertEqual(got, {0: (0, 1), 1: (0, 0), 2: (1, 6), 3: (0, 0)})
        self.assertEqual(r2.e.r8(r2.cave['bm_cur']), 7)

    # -- the generator ----------------------------------------------------------------------------
    def played(self, r, run):
        """The bar the engine plays from VOICE's track (as break_maker.c): (slice, velocity) per step,
        None silent (CHOP)."""
        n, steps = cut(r)
        cover = lambda t: max(k for k in range(n) if steps[k] <= t)     # noqa: E731
        blk, out, offset, playing = r.block(VOICE), [], 0, None
        for s in range(16):
            on = r.e.r8(blk + 8 * s) == 1
            if s == 0:
                offset = 0
            k0 = steps.index(s) if s in steps else None
            sl = (k0 + offset) % n if k0 is not None else (cover(s) if playing is None else playing)
            if on:
                lk = self.lock(r, s)
                z = p2_zone(lk, n) if lk is not None else 0
                sl = z - 1 if z else cover(s)
                if run:
                    offset = (sl - cover(s)) % n
            playing = sl
            out.append((sl, self.vel(r, s)) if on or run else None)
        return out

    def generate(self, r, chr_, algo=RUN, action=12):
        r.w16(r.voice_state(VOICE) + VS_BASE + 2 * ALGO, algo)
        self.open_page(r)
        self.press(r, 48 + chr_)
        self.press(r, 48 + action)
        self.loop(r)
        return self.played(r, algo == RUN)

    def test_two_step_puts_kicks_and_snares_in_place(self):
        r = self.rig()
        self.load(r)
        kinds = self.kinds
        for algo in (RUN, CHOP):
            bar = self.generate(r, 4, algo=algo)
            self.assertEqual(kinds[bar[0][0]], KICK, algo)
            self.assertEqual(kinds[bar[10][0]], KICK, algo)
            self.assertEqual(kinds[bar[4][0]], SNARE, algo)
            self.assertEqual(kinds[bar[12][0]], SNARE, algo)

    def test_run_writes_triggers_only_where_needed(self):
        r = self.rig()
        self.load(r)
        self.generate(r, 0, algo=RUN)       # Straight: a few swaps
        blk = r.block(VOICE)
        on = [r.e.r8(blk + 8 * s) for s in range(16)]
        self.assertLess(sum(on), 12)
        self.assertGreater(sum(on), 0)

    def test_jungle_and_breakcore_use_velocity_kinds(self):
        r = self.rig()
        self.load(r)
        seen = set()
        for _ in range(6):
            for chr_ in (5, 7):
                bar = self.generate(r, chr_, algo=CHOP)
                seen |= {v for x in bar if x for v in [x[1]]}
                r.ms += 37
        self.assertTrue({VEL['ghost'], VEL['roll2'], VEL['reverse']} <= seen, seen)

    def test_undo_and_clear(self):
        r = self.rig()
        self.load(r)
        before = self.played(r, True)
        self.generate(r, 7, algo=RUN)
        self.assertNotEqual(self.played(r, True), before)
        self.key(r, 48 + 12, True)          # held 0.5 s: undo
        r.ms += 600
        self.ui(r)
        self.key(r, 48 + 12, False)
        self.loop(r)
        self.assertEqual(self.played(r, True), before)
        self.generate(r, 7, algo=RUN)
        self.press(r, 48 + 15)              # Clear
        self.loop(r)
        self.assertEqual([x[0] for x in self.played(r, True)], list(range(16)))


if __name__ == '__main__':
    unittest.main()
