"""3 Oh on the M4: the configuration page (a long press on its step on voice-map's engine page),
Random and its undo, each kit's sounds (kit sidecar record 3OHS), PARAM2's three values, tempo and
DRIVE, and the messages to the M7."""
import shutil
import struct
import tempfile
import unittest

from fixtures import build_patched, require_image
from perkons.fwimage import Image
from perkons.rig import M4Rig
from oh_rig import (D_ACC, D_OCT, D_REST, D_SLIDE, DTYPE, NOTES, OCTAVE, P2_DIST, P2_ENV, PRESET, RANGE, ROOT, RSTYLE,
                    SCALE, VEL_ACCENT, VEL_BOTH, VEL_NORMAL, VEL_SLIDE, pack, preset, unpack)

SHIFT, KIT, MOD, TRIG1 = 0x47, 0x41, 0x43, 0x4b
KIT_MODE = 5
ENGINES_PAGE, THREE_OH_PAGE = 17, 18          # the mods' pages after the stock 0-16, in name order
MSG_SOUND, MSG_LIVE, MSG_VOICE_MAP = 4, 5, 6  # the M7 host's message types, in name order
COL = 4                                       # 3 Oh's column (engine 5)
TUNE, PARAM1, DRIVE, PARAM2, ALGO = 0, 1, 6, 5, 8
VS_VALUE, VS_BASE = 0x50, 0x6a
POS1, POS2, POS3 = 1, 0, 2
HELD, KEEP = 0x10, 0x20
NB = 11
_image = {}


def image():
    if not _image:
        img, p = build_patched(Image.load(require_image('v1.3.0')), ['voice_map', 'voice_3_oh:5'],
                               tempfile.mkdtemp())
        _image['x'] = (img, p.cave_symbols['m4host'])
    return _image['x']


DEFAULTS = [preset(i) for i in (0, 1, 9, 13)]


def record(voices):
    return b'PKX13OHS' + struct.pack('<I', 4 * NB) + b''.join(pack(v) for v in voices)


class TestThreeOhPage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('arm-none-eabi-gcc'):
            raise unittest.SkipTest('arm-none-eabi-gcc not installed')

    def rig(self, sd=None):
        img, cave = image()
        r = M4Rig(img, cave, sd=sd if sd is not None else {'BANKS/00/KITS/00.KIT': b''})
        seg = img.segment('m4')
        site = r.sym.site('keys.handler_lit')['addr']
        r.handler = struct.unpack_from('<I', seg.data, site - seg.addr)[0] & ~1
        r.ms = 1000
        r.e.stub(r.sym.fn['hal_get_tick'], lambda e, *a: r.ms)
        r.e.stub(r.sym.fn['knob_handler'], lambda *a: 0)
        r.e.sent(2)
        return r

    def key(self, r, k, down):
        r.event(k, down, handler=r.handler)

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

    def open_engines(self, r):
        r.set_mode(KIT_MODE)
        self.key(r, KIT, True)
        self.apply(r)
        self.press(r, MOD)
        self.key(r, KIT, False)
        self.apply(r)
        self.assertEqual(r.mode(), ENGINES_PAGE)

    def long_press(self, r, voice, ms=800):
        self.key(r, 16 * voice + COL, True)
        self.ui(r)
        r.ms += ms
        self.ui(r)
        self.key(r, 16 * voice + COL, False)
        self.apply(r)

    def open_page(self, r, voice=1):
        self.open_engines(r)
        self.long_press(r, voice)
        self.assertEqual(r.mode(), THREE_OH_PAGE)

    def sent(self, r, flush=True):
        """The sound messages since last asked: [(voice, flags, 23 values)]; first the main loop's
        pass, which does the page's actions and sends its changes."""
        if flush:
            r.e.call(r.cave['pk_oh_main_loop'], max_insns=5_000_000)
        return [(p[0] & 3, p[0] & 0xf0, unpack(p[1:])) for t, p in r.e.sent(2) if t == MSG_SOUND]

    def slot(self, r, v, base, slot):
        return struct.unpack('<H', bytes(r.e.uc.mem_read(r.voice_state(v) + base + 2 * slot, 2)))[0]

    def set_slot(self, r, v, slot, x, value=True):
        vs = r.voice_state(v)
        r.e.uc.mem_write(vs + VS_BASE + 2 * slot, struct.pack('<H', x))
        if value:
            r.e.uc.mem_write(vs + VS_VALUE + 2 * slot, struct.pack('<H', x))

    def loop(self, r, ms=10):
        r.ms += ms
        r.e.call(r.cave['pk_oh_main_loop'], max_insns=5_000_000)

    def sound(self, r, v):
        return unpack(bytes(r.e.uc.mem_read(r.cave['oh_cur'] + NB * v, NB)))

    # -- opening ------------------------------------------------------------------------
    def test_a_long_press_opens_it(self):
        r = self.rig()
        self.open_page(r, voice=2)
        self.assertEqual([list(p) for t, p in r.e.sent(2) if t == MSG_VOICE_MAP], [[0, 1, 4, 3]])
        self.press(r, 16 * 0 + 3)
        s = self.sent(r)
        self.assertEqual(s[-1][0], 2, 'editing voice 3')

    def test_shift_kit_mod_go_back_to_the_engine_page(self):
        for k in (SHIFT, KIT, MOD):
            with self.subTest(key=hex(k)):
                r = self.rig()
                self.open_page(r)
                self.press(r, k)
                self.apply(r)
                self.assertEqual(r.mode(), ENGINES_PAGE)

    def test_the_trigger_keys_play(self):
        r = self.rig()
        self.open_page(r)
        played = []
        r.e.stub(r.sym.fn['voice_play'], lambda e, voice, on, *a: played.append((voice, on)))
        self.press(r, TRIG1 + 1)
        self.assertEqual(played, [(1, 1), (1, 0)])
        self.assertEqual(r.mode(), THREE_OH_PAGE)

    def steps(self, r, v):
        """Track v's steps: [(on, note, velocity)]."""
        blk = r.block(v)
        return [tuple(bytes(r.e.uc.mem_read(blk + 8 * s, 8))[i] for i in (0, 6, 7)) for s in range(16)]

    def act(self, r, col, hold=False):
        """Row 4's action col (12 New line, 13 Keep rhythm, 14 Vary, 15 New sound); held: undo."""
        r.ms += 17
        self.key(r, 16 * 3 + col, True)
        if hold:
            r.ms += 600
            self.ui(r)
        self.key(r, 16 * 3 + col, False)
        self.sent(r)                                            # the main loop does it

    # -- the rows -------------------------------------------------------------------------------
    def test_a_preset_press_gives_its_sound_keeping_the_line_settings(self):
        r = self.rig()
        self.open_page(r, voice=1)
        self.press(r, 16 * 1 + 4)                               # scale 5 (Dorian)
        self.press(r, 16 * 1 + 14)                              # octave C3
        self.press(r, 16 * 2 + 7)                               # root G
        self.press(r, 16 * 3 + 2)                               # 5 notes
        self.press(r, 16 * 3 + 10)                              # Octave jumps: high
        r.e.sent(2)
        self.press(r, 16 * 0 + 9)                               # Hot Filter
        v, flags, x = self.sent(r)[-1]
        want = preset(9)
        want[SCALE], want[OCTAVE], want[ROOT], want[NOTES], want[D_OCT] = 4, 2, 7, 2, 2
        self.assertEqual((v, flags, x), (1, 0, want))
        self.assertEqual(self.slot(r, 1, VS_BASE, TUNE), 2048 << 4, 'cutoff to 12 o\'clock')
        self.assertEqual(self.slot(r, 1, VS_BASE, PARAM1), 2048 << 4, 'resonance to 12 o\'clock')

    def test_rows_2_and_3_set_scale_octave_root_type(self):
        r = self.rig()
        self.open_page(r, voice=0)
        self.sent(r)
        for k in (16 * 1 + 11, 16 * 1 + 12, 16 * 2 + 11, 16 * 2 + 15):
            self.press(r, k)
        s = self.sent(r)
        self.assertEqual({f for _, f, _ in s}, {KEEP})
        x = s[-1][2]
        self.assertEqual([x[f] for f in (SCALE, OCTAVE, ROOT, DTYPE)], [11, 0, 11, 3])
        rows = self.ui(r)
        self.assertEqual([i for i, v in enumerate(rows[1]) if v == 0x80], [11, 12])
        self.assertEqual([i for i, v in enumerate(rows[2]) if v == 0x80], [11, 15])
        self.assertEqual(rows[0][0], 0x80, 'preset 1')
        self.press(r, 16 * 1 + 11)                              # already chosen: nothing sent
        self.assertEqual(self.sent(r), [])

    def test_row_4_notes_range_densities(self):
        r = self.rig()
        self.open_page(r, voice=0)
        self.press(r, 16 * 3 + 3)                               # 7 notes
        self.press(r, 16 * 3 + 7)                               # two octaves
        self.press(r, 16 * 3 + 8)                               # Accent: low -> high
        self.press(r, 16 * 3 + 9)
        self.press(r, 16 * 3 + 9)                               # Slide: low -> high -> off
        x = self.sound(r, 0)
        self.assertEqual([x[f] for f in (NOTES, RANGE, D_ACC, D_SLIDE, D_OCT, D_REST)], [3, 3, 2, 0, 1, 1])
        rows = self.ui(r)
        self.assertEqual(rows[3][:12], [0x10] * 3 + [0x80] + [0x10] * 3 + [0x80] + [0x80, 0x10, 0x38, 0x38])

    # -- the line -------------------------------------------------------------------------------
    def test_new_line_writes_the_track(self):
        r = self.rig()
        self.open_page(r, voice=1)
        self.press(r, 16 * 3 + 11)                              # Rests: low -> high
        self.act(r, 12)
        st = self.steps(r, 1)
        on = [s for s in st if s[0]]
        self.assertTrue(st[0][0], 'the first step plays')
        self.assertTrue(3 <= len(on) <= 14, st)
        x = self.sound(r, 1)                                    # Natural Minor on C, octave C2, an octave
        base = 24 + 12 * x[OCTAVE]
        minor = {0, 2, 3, 5, 7, 8, 10}
        for _, n, vel in on:
            self.assertIn((n - base) % 12, minor)
            self.assertTrue(base - 12 <= n <= base + 12 + 12, n)
            self.assertIn(vel, (VEL_NORMAL, VEL_ACCENT, VEL_SLIDE, VEL_BOTH))
        self.assertEqual([s for s in self.steps(r, 0) if s[0]], [], 'other tracks untouched')

    def test_rests_off_fills_every_step_and_notes_limits_the_pitches(self):
        r = self.rig()
        self.open_page(r, voice=0)
        self.press(r, 16 * 3 + 11)
        self.press(r, 16 * 3 + 11)                              # Rests: low -> high -> off
        self.press(r, 16 * 3 + 10)
        self.press(r, 16 * 3 + 10)                              # Octave jumps: off
        self.press(r, 16 * 3 + 0)                               # 2 notes
        for i in range(4):
            self.act(r, 12)
            st = self.steps(r, 0)
            self.assertTrue(all(s[0] for s in st))
            self.assertLessEqual(len({s[1] for s in st}), 2)

    def test_slides_only_where_the_next_step_is_close(self):
        r = self.rig()
        self.open_page(r, voice=0)
        self.press(r, 16 * 3 + 9)                               # Slide: high
        self.press(r, 16 * 3 + 11)                              # Rests: high
        for i in range(6):
            self.act(r, 12)
            st = self.steps(r, 0)
            for s, (on, _, vel) in enumerate(st):
                if on and vel in (VEL_SLIDE, VEL_BOTH):
                    nxt = next(d for d in range(1, 17) if st[(s + d) % 16][0])
                    self.assertLessEqual(nxt, 2, (s, st))

    def test_keep_rhythm_rewrites_only_the_steps_that_are_on(self):
        r = self.rig()
        self.open_page(r, voice=2)
        mine = {0, 3, 6, 8, 11, 14}
        for s in range(16):
            r.set_step(2, s, note=36, velocity=127, on=s in mine)
        self.act(r, 13)
        st = self.steps(r, 2)
        self.assertEqual({s for s in range(16) if st[s][0]}, mine)
        self.assertNotEqual([st[s][1:] for s in sorted(mine)], [(36, 127)] * len(mine))

    def test_keep_rhythm_on_an_empty_track_makes_a_new_line(self):
        r = self.rig()
        self.open_page(r, voice=0)
        self.act(r, 13)
        self.assertTrue(any(s[0] for s in self.steps(r, 0)))

    def test_vary_changes_a_few_steps(self):
        r = self.rig()
        self.open_page(r, voice=0)
        self.press(r, 16 * 3 + 11)
        self.press(r, 16 * 3 + 11)                              # Rests off: 16 steps
        self.press(r, 16 * 3 + 3)                               # 7 notes
        self.act(r, 12)
        before = self.steps(r, 0)
        self.act(r, 14)
        after = self.steps(r, 0)
        self.assertEqual([s[0] for s in after], [s[0] for s in before], 'the rhythm stays')
        self.assertLessEqual(sum(a != b for a, b in zip(after, before)), 3)

    def test_hold_undoes_the_last_line(self):
        r = self.rig()
        self.open_page(r, voice=0)
        self.act(r, 12)
        first = self.steps(r, 0)
        self.act(r, 12)
        self.assertNotEqual(self.steps(r, 0), first)
        self.act(r, 14, hold=True)                              # held: undo (no Vary)
        self.assertEqual(self.steps(r, 0), first)

    def test_new_sound_then_hold_to_undo(self):
        r = self.rig()
        self.open_page(r, voice=0)
        self.press(r, 16 * 1 + 3)                               # scale 4
        before = self.sound(r, 0)
        self.act(r, 15)
        x = self.sound(r, 0)
        self.assertNotEqual(x[:PRESET], before[:PRESET])
        self.assertEqual(x[PRESET:P2_ENV], before[PRESET:P2_ENV], 'the preset and line settings stay')
        self.assertEqual((x[RSTYLE], x[P2_ENV:P2_DIST + 1]), (1, [16] * 3))
        self.assertEqual(self.ui(r)[0][0], 0x38, 'the preset changed by New sound: medium')
        self.act(r, 15, hold=True)
        self.assertEqual(self.sound(r, 0), before)

    # -- PARAM2 ---------------------------------------------------------------------------
    def test_param2_sets_algos_value_after_it_moves(self):
        r = self.rig()
        self.open_page(r, voice=1)
        self.press(r, 16 * 0 + 0)
        self.set_slot(r, 1, ALGO, POS1)                         # envelope amount
        self.loop(r)
        self.set_slot(r, 1, PARAM2, 4095 << 4)
        self.loop(r)
        self.set_slot(r, 1, ALGO, POS3)                         # distortion: keeps its value
        self.loop(r)
        x = self.sound(r, 1)
        self.assertEqual(x[P2_ENV:P2_DIST + 1], [31, 16, 16])
        self.set_slot(r, 1, PARAM2, 0)
        self.loop(r)
        self.assertEqual(self.sound(r, 1)[P2_ENV:P2_DIST + 1], [31, 16, 1])

    # -- tempo, DRIVE -------------------------------------------------------------------------
    def test_tempo_and_drive_go_to_the_m7(self):
        r = self.rig()
        bpm = r.sym.g['clock_state'] + 0x14

        def live(ms):
            self.loop(r, ms)
            return [list(p) for t, p in r.e.sent(2) if t == MSG_LIVE]
        r.e.uc.mem_write(bpm, struct.pack('<f', 124.5))
        self.set_slot(r, 2, DRIVE, 0xff00)
        r.e.sent(2)
        m = live(10)
        self.assertEqual(len(m), 1)
        self.assertEqual(m[0][:2], [1245 & 255, 1245 >> 8])
        self.assertEqual(m[0][4], 0xff)
        self.assertEqual(live(10) + live(10), [], 'unchanged: nothing')
        self.assertEqual(len(live(240)), 1, 'again after 250 ms')

    # -- kits ---------------------------------------------------------------------------
    def test_leaving_keeps_the_sounds_in_the_kit(self):
        r = self.rig()
        self.open_page(r, voice=1)
        self.press(r, 16 * 0 + 14)                              # Fuzz Line
        self.press(r, SHIFT)
        self.apply(r)
        kix = r.e.sd_written['banks/00/kits/00.kix']
        i = kix.index(b'3OHS')
        self.assertEqual(struct.unpack_from('<I', kix, i + 4)[0], 4 * NB)
        self.assertEqual(unpack(kix[i + 8 + NB:i + 8 + 2 * NB])[:PRESET + 1], preset(14)[:PRESET + 1], 'voice 2')
        self.assertEqual(unpack(kix[i + 8:i + 8 + NB]), DEFAULTS[0], 'voice 1')

    def test_kit_load_sends_its_sounds_held_then_free(self):
        r = self.rig()
        self.open_page(r, voice=1)
        self.press(r, 16 * 0 + 14)
        self.press(r, SHIFT)
        self.apply(r)
        self.sent(r)
        self.long_press(r, 1)
        self.press(r, 16 * 2 + 3)                               # a change not yet sent ...
        r.e.sent(2)
        r.e.call(r.cave['pk_oh_kit_load'], 0, max_insns=50_000_000)
        s = self.sent(r)
        self.assertEqual([(v, f) for v, f, _ in s], [(v, HELD) for v in range(4)], '... dropped: the kit\'s, held')
        self.assertEqual(s[1][2][:PRESET + 1], preset(14)[:PRESET + 1])
        self.loop(r, 10)
        self.assertEqual(self.sent(r), [], 'not yet')
        self.loop(r, 15)
        self.assertEqual([(v, f) for v, f, _ in self.sent(r)], [(v, 0) for v in range(4)], 'then free')
        r.e.call(r.cave['pk_oh_kit_load'], 1, max_insns=50_000_000)
        self.assertEqual([x for _, _, x in self.sent(r)], DEFAULTS, 'a kit without a record: the defaults')

    def test_bank_load_reads_the_kits(self):
        odd = preset(3)
        odd[D_REST] = 3                                         # out of range
        rec = record([preset(9), DEFAULTS[1], odd, preset(15)])
        r = self.rig(sd={'BANKS/02/KITS/05.KIX': rec, 'BANKS/02/00.PAT': b''})
        r.e.call(r.cave['pk4_store_bank_load'], 2, max_insns=900_000_000)
        r.e.sent(2)
        r.e.call(r.cave['pk_oh_kit_load'], 5, max_insns=50_000_000)
        s = [x for _, _, x in self.sent(r)]
        self.assertEqual(s[0], preset(9))
        self.assertEqual(s[2][D_REST], 2, 'out of range: the largest')
        self.assertEqual(s[3], preset(15))
        buf = r.sym.g['json_buf']
        self.assertEqual(r.e.call(r.cave['pk_oh_kit_save'], 2, 5, buf, 512, max_insns=5_000_000), 4 * NB)
        self.assertEqual(unpack(bytes(r.e.uc.mem_read(buf, NB))), preset(9))


if __name__ == '__main__':
    unittest.main()
