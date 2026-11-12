"""Synth Lab on the M4: the configuration page (a long press on its step on voice-map's engine page),
the cells edited with the LFO knobs, Random, Tweak and its undo, each kit's sounds (kit
sidecar record SLAB), PARAM2's three values, tempo and DRIVE, and the messages to the M7."""
import shutil
import struct
import tempfile
import unittest

from fixtures import build_patched, require_image
from perkons.fwimage import Image
from perkons.rig import M4Rig
from sy_rig import (CLFO, CUTOFF, FILT_ON, FILT_TYPE, FOLD_AMT, LEVEL, MUTATE, OSC_N, P2_RES, P2_SYNC, PLFO, PRESET, PW,
                    ROOT, RSTYLE, SCALE, WAVE, FLFO, FOLD_ON, OSWEEP, pack, unpack)

SHIFT, KIT, MOD, PATTERN, SYNC, TRIG1 = 0x47, 0x41, 0x43, 0x40, 0x42, 0x4b
RATE, WAVE_POT, LEVEL_POT = 0x21, 0x22, 0x23
KIT_MODE = 5
ENGINES_PAGE, SYNTH_LAB_PAGE = 17, 18         # the mods' pages after the stock 0-16, in name order
MSG_SOUND, MSG_LIVE, MSG_VOICE_MAP = 4, 5, 6  # the M7 host's message types, in name order
COL = 4                                       # Synth Lab's column (engine 5)
TUNE, PARAM1, DECAY, PARAM2, DRIVE, ALGO, MODE = 0, 1, 4, 5, 6, 8, 9
VS_VALUE, VS_BASE, VS_CATCH = 0x50, 0x6a, 0xf8
POS1, POS2, POS3 = 1, 0, 2
HELD, KEEP = 0x10, 0x20
_image = {}


def image():
    if not _image:
        img, p = build_patched(Image.load(require_image('v1.3.0')), ['voice_map', 'voice_synth_lab:5'],
                               tempfile.mkdtemp())
        _image['x'] = (img, p.cave_symbols['m4host'])
    return _image['x']


def preset(i):
    import os
    import re
    src = open(os.path.join(os.path.dirname(__file__), '..', 'src', 'common', 'sl_presets.h')).read()
    rows = re.findall(r'\{([^{}]*)\}', src[src.index('= {') + 3:])
    return [int(x) for x in rows[i].replace('\n', ' ').split(',') if x.strip()]


DEFAULTS = [preset(i) for i in (0, 4, 11, 13)]


def starter(j):
    """the starters follow the presets in sl_presets.h"""
    return preset(16 + j)


def record(voices):
    return b'PKX1SLAB' + struct.pack('<I', 160) + b''.join(pack(v) for v in voices)


class TestSynthLabPage(unittest.TestCase):
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
        self.assertEqual(r.mode(), SYNTH_LAB_PAGE)

    def sent(self, r, flush=True):
        """The sound messages since last asked: [(voice, flags, 88 values)]; first the main
        loop's pass, which sends the page's changes."""
        if flush:
            r.e.call(r.cave['pk_sy_main_loop'], max_insns=5_000_000)
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
        r.e.call(r.cave['pk_sy_main_loop'], max_insns=5_000_000)

    def poll_knobs(self, r):
        r.e.call(r.sym.fn['lfo_knob_poll'], max_insns=5_000_000)

    def sound(self, r, v):
        return unpack(bytes(r.e.uc.mem_read(r.cave['sy_cur'] + 40 * v, 40)))

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
        self.assertEqual(r.mode(), SYNTH_LAB_PAGE)

    # -- presets, scale, root, Mutate -------------------------------------------------------
    def test_a_preset_press_gives_its_sound_keeping_scale_root_mutate(self):
        r = self.rig()
        self.open_page(r, voice=1)
        self.press(r, 16 * 2 + 4)                               # scale 5 (Dorian)
        self.press(r, 16 * 3 + 7)                               # root G
        self.press(r, 16 * 2 + 15)                              # Mutate on
        r.e.sent(2)
        self.press(r, 16 * 0 + 9)                               # Classic Analog Pad
        v, flags, x = self.sent(r)[-1]
        want = preset(9)
        want[SCALE], want[ROOT], want[MUTATE] = 4, 7, 1
        self.assertEqual((v, flags, x), (1, 0, want))
        self.assertEqual(self.slot(r, 1, VS_BASE, PARAM1), 2048 << 4, 'PARAM1 to 12 o\'clock')

    def test_a_held_preset_step_gives_its_groups_starter(self):
        for step, j in ((0, 0), (3, 0), (4, 1), (8, 2), (10, 2), (11, 3), (12, 3), (13, 4), (15, 4)):
            with self.subTest(step=step):
                r = self.rig()
                self.open_page(r, voice=2)
                self.press(r, 16 * 2 + 3)                       # scale 4
                r.ms += 13
                self.press(r, 16 * 0 + 6)                       # a preset first, to undo to
                before = self.sound(r, 2)
                self.sent(r)                                    # the preset's message out
                self.key(r, step, True)
                self.ui(r)
                self.assertEqual(self.sent(r), [], 'nothing until it is released or held')
                r.ms += 600
                rows = self.ui(r)                               # held 0.5 s: the starter
                self.assertEqual(rows[0][step], 0x80)
                self.key(r, step, False)
                v, flags, x = self.sent(r)[-1]
                want = starter(j)
                want[PRESET], want[SCALE] = step, 3
                self.assertEqual((v, flags, x), (2, 0, want))
                self.assertEqual(self.sound(r, 2), want, 'released: no preset on top')
                self.key(r, 16 * 2 + 12, True)                  # hold Tweak: undo
                r.ms += 600
                self.ui(r)
                self.key(r, 16 * 2 + 12, False)
                self.assertEqual(self.sound(r, 2), before)

    def test_starters_have_no_modulation_on(self):
        for j in range(5):
            x = starter(j)
            with self.subTest(starter=j):
                for at in (8, 12, 24, 28, 40, 44, CLFO, FLFO):
                    self.assertEqual(x[at + 2], 0, 'LFO depth')
                    self.assertGreater(x[at], 0, 'a speed to start from')
                self.assertEqual((x[FOLD_ON], x[OSWEEP], x[16 + OSWEEP], x[32 + OSWEEP], x[MUTATE]), (0, 0, 0, 0, 0))
                self.assertGreater(x[LEVEL], 0)

    def test_rows_3_and_4_set_scale_root_mutate(self):
        r = self.rig()
        self.open_page(r, voice=0)
        self.sent(r)
        s = []
        for k in (16 * 2 + 9, 16 * 3 + 11, 16 * 2 + 15):
            self.press(r, k)
            s += self.sent(r)
        self.assertEqual([f for _, f, _ in s], [KEEP] * 3)
        self.assertEqual([s[-1][2][f] for f in (SCALE, ROOT, MUTATE)], [9, 11, 1])
        rows = self.ui(r)
        self.assertEqual(rows[2][9], 0x80)
        self.assertEqual(rows[3][11], 0x80)
        self.assertEqual(rows[2][15], 0x80)
        self.assertEqual(rows[0][0], 0x80, 'preset 1')
        self.press(r, 16 * 2 + 9)                               # already chosen: nothing sent
        self.assertEqual(self.sent(r), [])

    # -- cells --------------------------------------------------------------------------------
    def test_a_held_cell_takes_the_lfo_knobs(self):
        r = self.rig()
        self.open_page(r, voice=1)
        r.knob(WAVE_POT, 0.5)
        self.poll_knobs(r)
        r.e.sent(2)
        self.key(r, 16 * 1 + 4, True)                           # osc 2's Wave cell
        r.turn(WAVE_POT, 0.0, lambda: self.poll_knobs(r))
        self.assertEqual(self.sent(r, flush=False), [], 'nothing sent from the knob poll (an interrupt)')
        s = self.sent(r)
        self.assertTrue(s)
        self.assertEqual(s[-1][2][OSC_N + PW], 0, 'WAVE: the pulse width')
        self.assertEqual(s[-1][1], KEEP)
        r.turn(RATE, 0.75, lambda: self.poll_knobs(r))
        self.assertEqual(self.sent(r)[-1][2][OSC_N + WAVE], 4, 'RATE: the waveform (pulse)')
        lfo = r.sym.g['mod_lfo']
        self.assertEqual(r.e.r8(lfo + 0x2c) & 3, 3, 'the stock LFO keeps its rate and wave')
        before = self.sound(r, 1)[OSC_N + 3]
        self.press(r, SYNC)                                     # the cell's switch: osc 2 sync
        self.assertEqual(self.sound(r, 1)[OSC_N + 3], 1 - before)
        self.assertEqual(self.ui(r)[1][4], 0x80, 'held: bright')
        self.key(r, 16 * 1 + 4, False)
        self.sent(r)
        r.turn(LEVEL_POT, 0.1, lambda: self.poll_knobs(r))
        self.assertEqual(self.sent(r), [], 'released: the knobs are the stock LFO\'s again')

    def test_row_2_cells_map_to_their_fields(self):
        r = self.rig()
        self.open_page(r, voice=0)
        for col, field in ((2, PLFO + 2), (13, 52 + 2), (15, 60 + 2)):
            with self.subTest(col=col):
                self.key(r, 16 + col, True)
                r.knob(LEVEL_POT, 0.5)
                self.poll_knobs(r)
                r.turn(LEVEL_POT, 1.0, lambda: self.poll_knobs(r))
                self.key(r, 16 + col, False)
                self.assertEqual(self.sound(r, 0)[field], 31)

    def test_row_3_filter_and_fold_cells(self):
        r = self.rig()
        self.open_page(r, voice=0)
        self.key(r, 16 * 2 + 10, True)                          # Filter: RATE = the type
        r.knob(RATE, 0.5)
        self.poll_knobs(r)
        r.turn(RATE, 0.99, lambda: self.poll_knobs(r))
        self.press(r, SYNC)                                     # filter off
        self.key(r, 16 * 2 + 10, False)
        x = self.sound(r, 0)
        self.assertEqual((x[FILT_TYPE], x[FILT_ON]), (6, 0))
        self.key(r, 16 * 2 + 11, True)                          # Fold: RATE = the amount
        r.knob(RATE, 0.5)
        self.poll_knobs(r)
        r.turn(RATE, 0.0, lambda: self.poll_knobs(r))
        self.key(r, 16 * 2 + 11, False)
        self.assertEqual(self.sound(r, 0)[FOLD_AMT], 0)

    # -- Random, Tweak, undo --------------------------------------------------------------------
    def test_random_styles(self):
        r = self.rig()
        self.open_page(r, voice=0)
        self.press(r, 16 * 2 + 3)                               # scale 4
        for col, style in ((12, 1), (13, 2), (14, 3), (15, None)):
            with self.subTest(col=col):
                r.ms += 37
                r.e.sent(2)
                self.press(r, 16 * 3 + col)
                v, flags, x = self.sent(r)[-1]
                self.assertEqual(flags, 0)
                self.assertEqual((x[PRESET], x[SCALE]), (0, 3))
                self.assertIn(x[RSTYLE], (style,) if style else (1, 2, 3))
                self.assertEqual(x[P2_RES:P2_SYNC + 1], [16] * 3)
                self.assertGreaterEqual(x[LEVEL], 8)
                rows = self.ui(r)
                self.assertEqual(rows[3][col], 0x80)

    def test_tweak_then_hold_to_undo(self):
        r = self.rig()
        self.open_page(r, voice=0)
        before = self.sound(r, 0)
        for amount in (1, 2, 3):
            r.ms += 13
            self.press(r, 16 * 2 + 11 + amount)
        tweaked = self.sound(r, 0)
        self.assertNotEqual(tweaked, before)
        self.assertEqual(tweaked[PRESET:], before[PRESET:], 'the page values stay')
        self.key(r, 16 * 2 + 12, True)
        r.ms += 600
        self.ui(r)                                              # held 0.5 s: undo (the last one)
        self.key(r, 16 * 2 + 12, False)
        undone = self.sound(r, 0)
        self.assertNotEqual(undone, tweaked)
        self.ui(r)
        self.assertEqual(self.sound(r, 0), undone, 'released: no Tweak')

    def test_random_hold_to_undo(self):
        r = self.rig()
        self.open_page(r, voice=0)
        before = self.sound(r, 0)
        r.ms += 13
        self.press(r, 16 * 3 + 13)                              # Random Wild
        changed = self.sound(r, 0)
        self.assertNotEqual(changed, before)
        self.key(r, 16 * 3 + 12, True)
        r.ms += 600
        self.ui(r)                                              # held 0.5 s: undo
        self.key(r, 16 * 3 + 12, False)
        self.assertEqual(self.sound(r, 0), before, 'back to the sound before the Random')
        self.ui(r)
        self.assertEqual(self.sound(r, 0), before, 'released: no Random')

    # -- PARAM2 ---------------------------------------------------------------------------
    def test_param2_sets_algos_value_after_it_moves(self):
        r = self.rig()
        self.open_page(r, voice=1)
        self.press(r, 16 * 0 + 0)
        self.set_slot(r, 1, ALGO, POS1)                         # resonance
        self.loop(r)
        self.set_slot(r, 1, PARAM2, 4095 << 4)
        self.loop(r)
        self.set_slot(r, 1, ALGO, POS3)                         # sync: keeps its value
        self.loop(r)
        x = self.sound(r, 1)
        self.assertEqual(x[P2_RES:P2_SYNC + 1], [31, 16, 16])
        self.set_slot(r, 1, PARAM2, 0)
        self.loop(r)
        self.assertEqual(self.sound(r, 1)[P2_RES:P2_SYNC + 1], [31, 16, 1])

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
        self.press(r, 16 * 0 + 14)                              # Voltage Drift
        self.press(r, SHIFT)
        self.apply(r)
        kix = r.e.sd_written['banks/00/kits/00.kix']
        i = kix.index(b'SLAB')
        self.assertEqual(struct.unpack_from('<I', kix, i + 4)[0], 160)
        self.assertEqual(unpack(kix[i + 8 + 40:i + 8 + 80])[:PRESET], preset(14)[:PRESET], 'voice 2')
        self.assertEqual(unpack(kix[i + 8:i + 8 + 40]), DEFAULTS[0], 'voice 1')

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
        r.e.call(r.cave['pk_sy_kit_load'], 0, max_insns=50_000_000)
        s = self.sent(r)
        self.assertEqual([(v, f) for v, f, _ in s], [(v, HELD) for v in range(4)], '... dropped: the kit\'s, held')
        self.assertEqual(s[1][2][:PRESET], preset(14)[:PRESET])
        self.loop(r, 10)
        self.assertEqual(self.sent(r), [], 'not yet')
        self.loop(r, 15)
        self.assertEqual([(v, f) for v, f, _ in self.sent(r)], [(v, 0) for v in range(4)], 'then free')
        r.e.call(r.cave['pk_sy_kit_load'], 1, max_insns=50_000_000)
        self.assertEqual([x for _, _, x in self.sent(r)], DEFAULTS, 'a kit without a record: the defaults')

    def test_bank_load_reads_the_kits(self):
        odd = preset(3)
        odd[WAVE] = 7                                           # out of range
        rec = record([preset(9), DEFAULTS[1], odd, preset(15)])
        r = self.rig(sd={'BANKS/02/KITS/05.KIX': rec, 'BANKS/02/00.PAT': b''})
        r.e.call(r.cave['pk4_store_bank_load'], 2, max_insns=900_000_000)
        r.e.sent(2)
        r.e.call(r.cave['pk_sy_kit_load'], 5, max_insns=50_000_000)
        s = [x for _, _, x in self.sent(r)]
        self.assertEqual(s[0], preset(9))
        self.assertEqual(s[2][WAVE], 5, 'out of range: the largest')
        self.assertEqual(s[3], preset(15))
        buf = r.sym.g['json_buf']
        self.assertEqual(r.e.call(r.cave['pk_sy_kit_save'], 2, 5, buf, 512, max_insns=5_000_000), 160)
        self.assertEqual(unpack(bytes(r.e.uc.mem_read(buf, 40))), preset(9))


if __name__ == '__main__':
    unittest.main()
