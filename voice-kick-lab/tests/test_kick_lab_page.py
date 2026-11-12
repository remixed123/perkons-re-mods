"""Kick Lab on the M4: the settings page (a long press on its step on voice-map's engine page),
each kit's settings (kit sidecar record KLAB), PARAM2's three values, the tempo, and the
messages to the M7."""
import shutil
import struct
import tempfile
import unittest

from fixtures import build_patched, require_image
from perkons.fwimage import Image
from perkons.rig import M4Rig

SHIFT, KIT, MOD, PATTERN, TRIG1 = 0x47, 0x41, 0x43, 0x40, 0x4b
KIT_MODE = 5
ENGINES_PAGE, KICK_LAB_PAGE = 17, 18          # the mods' pages after the stock 0-16, in name order
MSG_KICK_LAB, MSG_TEMPO, MSG_VOICE_MAP = 4, 5, 6        # the M7 host's message types, in name order
COL = 4                                       # Kick Lab's column (engine 5)
TUNE, PARAM1, DECAY, PARAM2, ALGO, MODE = 0, 1, 4, 5, 8, 9
VS_VALUE, VS_BASE, VS_CATCH = 0x50, 0x6a, 0xf8
POS1, POS2, POS3 = 1, 0, 2
BASIC = [0x40, 0x82, 0x23, 0, 0, 0]
DUB_ECHO = [0x4e, 0x71, 0xa6, 25, 89, 128]
_image = {}


def image():
    if not _image:
        img, p = build_patched(Image.load(require_image('v1.3.0')), ['voice_map', 'voice_kick_lab:5'],
                               tempfile.mkdtemp())
        _image['x'] = (img, p.cave_symbols['m4host'])
    return _image['x']


def record(voices):
    return b'PKX1KLAB' + struct.pack('<I', 24) + bytes(b for v in voices for b in v)


@unittest.skipUnless(shutil.which('arm-none-eabi-gcc'), 'arm-none-eabi-gcc not installed')
class TestKickLabPage(unittest.TestCase):
    def rig(self, sd=None):
        img, cave = image()
        r = M4Rig(img, cave, sd=sd if sd is not None else {'BANKS/00/KITS/00.KIT': b''})
        seg = img.segment('m4')
        site = r.sym.site('keys.handler_lit')['addr']
        r.handler = struct.unpack_from('<I', seg.data, site - seg.addr)[0] & ~1
        r.ms = 1000                                             # the ms clock (hal_get_tick)
        r.e.stub(r.sym.fn['hal_get_tick'], lambda e, *a: r.ms)
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
        self.assertEqual(r.mode(), KICK_LAB_PAGE)

    def sent(self, r):
        """The settings messages since last asked: [(4 x [6 bytes], apply, held)]."""
        return [([list(p[6 * v:6 * v + 6]) for v in range(4)], p[24], p[25])
                for t, p in r.e.sent(2) if t == MSG_KICK_LAB]

    def slot(self, r, v, base, slot):
        return struct.unpack('<H', bytes(r.e.uc.mem_read(r.voice_state(v) + base + 2 * slot, 2)))[0]

    def set_slot(self, r, v, slot, x, value=True):
        vs = r.voice_state(v)
        r.e.uc.mem_write(vs + VS_BASE + 2 * slot, struct.pack('<H', x))
        if value:
            r.e.uc.mem_write(vs + VS_VALUE + 2 * slot, struct.pack('<H', x))

    def loop(self, r, ms=10):
        r.ms += ms
        r.e.call(r.cave['pk_kl_main_loop'], max_insns=5_000_000)

    # -- opening ------------------------------------------------------------------------
    def test_a_long_press_opens_it(self):
        r = self.rig()
        self.open_page(r, voice=2)                              # chooses Kick Lab, then opens
        self.assertEqual([list(p) for t, p in r.e.sent(2) if t == MSG_VOICE_MAP], [[0, 1, 4, 3]])
        self.press(r, 16 * 3 + 13)                              # row 4 step 14: hard drive
        self.assertEqual(self.sent(r)[-1][1], 1 << 2, 'editing voice 3')

    def test_the_panel_switches_work_on_the_page(self):
        from perkons.rig import EVT
        r = self.rig()
        self.open_page(r, voice=1)
        self.press(r, 16 * 0 + 3)                               # a preset
        for which, pos in ((2, 2), (1, 1)):                     # voice 2's ALGO, MODE: switch events 5, 4
            r.e.uc.mem_write(EVT, bytes([3 + which, 0x15]) + struct.pack('<H', pos) + bytes(4))
            r.e.call(r.key_handler, EVT, max_insns=50_000_000)
        self.assertEqual([self.slot(r, 1, VS_VALUE, s) for s in (ALGO, MODE)], [2, 1])
        self.assertEqual(r.mode(), KICK_LAB_PAGE)

    # -- the rows -------------------------------------------------------------------------
    def test_the_rows_set_the_voices_settings(self):
        r = self.rig()
        self.open_page(r, voice=1)
        r.e.sent(2)
        for k in (16 * 1 + 9, 16 * 1 + 15, 16 * 2 + 6, 16 * 2 + 13, 16 * 3 + 7, 16 * 3 + 11, 16 * 3 + 15):
            self.press(r, k)
        s = self.sent(r)
        self.assertEqual(len(s), 7)
        v = s[-1][0][1]
        self.assertEqual(v[0] & 15, 0, 'preset 1')
        self.assertEqual([v[1] >> 4, v[2] >> 7], [9, 1], 'row 2: step 10 (1/4), sync on')
        self.assertEqual([v[2] & 15, v[2] >> 4 & 7], [6, 3], 'row 3: feedback 7th, tone 4th')
        self.assertEqual([v[1] & 15, v[0] >> 6, v[0] >> 4 & 3], [7, 2, 3], 'row 4: amount 8th, tone 3rd, fold')
        self.assertEqual([x[1:] for x in s], [(1 << 1, 0)] * 7)
        rows = self.ui(r)
        on = [[c for c in range(16) if rows[i][c] == 0x80] for i in range(4)]
        self.assertEqual(on, [[0], [9, 15], [6, 13], [7, 11, 15]])
        self.assertTrue(all(x == 0x10 for i in range(4) for c, x in enumerate(rows[i]) if c not in on[i]))
        self.press(r, 16 * 2 + 6)                               # already chosen: nothing sent
        self.assertEqual(self.sent(r), [])

    def test_a_preset_press_gives_its_whole_sound(self):
        """The preset's settings and PARAM2 values, and TUNE, DECAY, PARAM1 as a kit load
        sets them (base and value, the catch armed as configured)."""
        r = self.rig()
        catch = r.e.r8(r.sym.g['device_config'] + 4)
        self.open_page(r, voice=1)
        r.e.sent(2)
        self.press(r, 16 * 0 + 14)                              # Dub Echo
        self.press(r, 16 * 0 + 14)                              # even the chosen one
        s = self.sent(r)
        self.assertEqual([x[0][1] for x in s], [DUB_ECHO] * 2)
        self.assertEqual([x[1:] for x in s], [(1 << 1, 0)] * 2)
        for slot, x in ((TUNE, 1630), (DECAY, 2153), (PARAM1, 1654)):
            self.assertEqual(self.slot(r, 1, VS_BASE, slot), x << 4)
            self.assertEqual(self.slot(r, 1, VS_VALUE, slot), x << 4)
            self.assertEqual(r.e.r8(r.voice_state(1) + VS_CATCH + slot), catch)

    def test_the_page_edits_the_voice_it_was_opened_for(self):
        r = self.rig()
        self.open_engines(r)
        self.press(r, 16 * 3 + COL)                             # voice 4 on Kick Lab too
        self.long_press(r, 3)
        r.e.sent(2)
        self.press(r, 16 * 0 + 7)
        s = self.sent(r)[-1]
        self.assertEqual(s[0][3][0] & 15, 7)
        self.assertEqual(s[0][1], BASIC, 'voice 2 untouched')
        self.assertEqual(s[1], 1 << 3)

    def test_the_trigger_keys_play(self):
        r = self.rig()
        self.open_page(r)
        played = []
        r.e.stub(r.sym.fn['voice_play'], lambda e, voice, on, *a: played.append((voice, on)))
        self.press(r, TRIG1 + 1)
        self.assertEqual(played, [(1, 1), (1, 0)])
        self.assertEqual(r.mode(), KICK_LAB_PAGE)

    def test_shift_kit_mod_go_back_to_the_engine_page(self):
        for k in (SHIFT, KIT, MOD):
            with self.subTest(key=hex(k)):
                r = self.rig()
                self.open_page(r)
                self.press(r, k)
                self.apply(r)
                self.assertEqual(r.mode(), ENGINES_PAGE)

    def test_another_page_button_leaves(self):
        r = self.rig()
        self.open_page(r)
        self.press(r, PATTERN)
        self.apply(r)
        self.assertNotIn(r.mode(), (KICK_LAB_PAGE, ENGINES_PAGE))

    # -- PARAM2 ---------------------------------------------------------------------------
    def test_param2_sets_algos_value_after_it_moves(self):
        r = self.rig()
        self.open_page(r, voice=1)
        self.press(r, 16 * 0 + 0)                               # Basic: all three at 0
        self.set_slot(r, 1, ALGO, POS2)                         # delay mix
        self.loop(r)
        self.set_slot(r, 1, PARAM2, 4095 << 4)
        self.loop(r)
        self.set_slot(r, 1, ALGO, POS3)                         # drive mix: keeps its value
        self.loop(r)
        r.e.sent(2)
        self.press(r, 16 * 2 + 5)                               # a page change sends the values
        self.assertEqual(self.sent(r)[-1][0][1][3:], [0, 255, 0])
        self.set_slot(r, 1, PARAM2, 2048 << 4)                  # now it moves
        self.loop(r)
        self.press(r, 16 * 2 + 7)
        self.assertEqual(self.sent(r)[-1][0][1][3:], [0, 255, 127])

    # -- the tempo --------------------------------------------------------------------------
    def test_the_tempo_goes_to_the_m7_when_it_changes(self):
        r = self.rig()
        bpm = r.sym.g['clock_state'] + 0x14

        def tempo(ms):
            self.loop(r, ms)
            return [struct.unpack('<H', p)[0] for t, p in r.e.sent(2) if t == MSG_TEMPO]
        r.e.uc.mem_write(bpm, struct.pack('<f', 124.5))
        r.e.sent(2)
        self.assertEqual(tempo(10), [1245])
        self.assertEqual(tempo(10) + tempo(10), [], 'unchanged: nothing')
        self.assertEqual(tempo(240), [1245], 'again after 250 ms')
        r.e.uc.mem_write(bpm, struct.pack('<f', 90.0))
        self.assertEqual(tempo(2), [], 'not within 5 ms of the last check')
        self.assertEqual(tempo(5), [900])

    # -- kits ---------------------------------------------------------------------------
    def test_leaving_keeps_the_settings_in_the_kit(self):
        r = self.rig()
        self.open_page(r, voice=1)
        self.press(r, 16 * 0 + 14)                              # Dub Echo
        self.press(r, SHIFT)
        self.apply(r)
        kix = r.e.sd_written['banks/00/kits/00.kix']
        i = kix.index(b'KLAB')
        self.assertEqual(struct.unpack_from('<I', kix, i + 4)[0], 24)
        self.assertEqual(list(kix[i + 8 + 6:i + 8 + 12]), DUB_ECHO, 'voice 2')
        self.assertEqual(list(kix[i + 8:i + 8 + 6]), BASIC, 'voice 1')

    def test_kit_load_sends_its_settings_held_then_free(self):
        r = self.rig()
        self.open_page(r, voice=1)
        self.press(r, 16 * 0 + 14)
        self.press(r, SHIFT)
        self.apply(r)
        r.e.sent(2)
        r.e.call(r.cave['pk_kl_kit_load'], 0, max_insns=50_000_000)
        s = self.sent(r)
        self.assertEqual(s, [([BASIC, DUB_ECHO, BASIC, BASIC], 15, 15)], 'held')
        self.loop(r, 10)
        self.assertEqual(self.sent(r), [], 'not yet: 20 ms of the positions matching')
        self.loop(r, 15)
        self.assertEqual(self.sent(r), [([BASIC, DUB_ECHO, BASIC, BASIC], 15, 0)], 'then free')
        r.e.call(r.cave['pk_kl_kit_load'], 1, max_insns=50_000_000)
        self.assertEqual(self.sent(r)[0][0], [BASIC] * 4, 'a kit without a record: Basic')

    def test_the_hold_waits_for_the_positions(self):
        """While a running voice's playing PARAM2, ALGO or MODE isn't the kit's (stock applies
        them at the track's next note): held, for 2 s at most (a lock or an LFO)."""
        r = self.rig()
        self.open_engines(r)
        self.press(r, 16 * 1 + COL)                             # voice 2 runs Kick Lab
        self.loop(r)
        r.e.call(r.cave['pk4_store_kit_save'], 0, 0, max_insns=90_000_000)      # keeps it on the kit
        r.e.call(r.cave['pk_kl_kit_load'], 0, max_insns=50_000_000)
        self.loop(r, 30)                                        # (positions matched: let go)
        r.e.sent(2)
        r.e.call(r.cave['pk_kl_kit_load'], 0, max_insns=50_000_000)
        self.assertEqual([x[2] for x in self.sent(r)], [15], 'held')
        vs = r.voice_state(1)
        for ms in (10, 500, 1000, 400):                         # the playing value kept off the kit's
            r.w16(vs + VS_VALUE + 2 * PARAM2, r.r16(vs + VS_BASE + 2 * PARAM2) ^ 0x4000)
            self.loop(r, ms)
            self.assertEqual(self.sent(r), [], f'after {ms}')
        self.loop(r, 200)
        self.assertEqual([x[2] for x in self.sent(r)], [0], 'let go after 2 s')

    def test_bank_load_reads_the_kits(self):
        odd = [0x4f, 0xfd, 0x7f, 1, 2, 3]                       # amount 13, time 15, tone 7: out of range
        rec = record([DUB_ECHO, BASIC, odd, [0x6d, 0x53, 0x33, 38, 64, 153]])
        r = self.rig(sd={'BANKS/02/KITS/05.KIX': rec, 'BANKS/02/00.PAT': b''})
        r.e.call(r.cave['pk4_store_bank_load'], 2, max_insns=900_000_000)
        r.e.sent(2)
        r.e.call(r.cave['pk_kl_kit_load'], 5, max_insns=50_000_000)
        v = self.sent(r)[0][0]
        self.assertEqual(v[0], DUB_ECHO)
        self.assertEqual(v[2], [0x4f, 0x72, 0x14, 1, 2, 3], 'out of range: the preset\'s page, the values kept')
        self.assertEqual(v[3], [0x6d, 0x53, 0x33, 38, 64, 153])
        buf = r.sym.g['json_buf']                               # the host's record buffer
        self.assertEqual(r.e.call(r.cave['pk_kl_kit_save'], 2, 5, buf, 64, max_insns=5_000_000), 24)
        self.assertEqual(list(r.e.uc.mem_read(buf, 6)), DUB_ECHO)
