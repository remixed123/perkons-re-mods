"""Akords: Random, the options (Strum, Spread, Bass drop, Mutate) and each voice's own sound:
the page on the M4 (row 3 steps 12-16, row 4 steps 13-16), the kit's record, and what the M7
plays from the "akords-voice" message."""
import os
import shutil
import struct
import sys
import tempfile
import unittest

from fixtures import build_patched, require_image
from perkons.fwimage import Image
from perkons.rig import M4Rig

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'lib'))
from ak_rig import (AkordsCase, DRONE, HAVE_GCC, MAJOR, MSG_AKORDS, MSG_AKORDS_VOICE,  # noqa: E402
                    goertzel, model_chord, note_hz, rms, tune_for)
import ak_sound as S  # noqa: E402

SHIFT, KIT, MOD = 0x47, 0x41, 0x43
KIT_MODE, AKORDS_PAGE, ENGINES_PAGE = 5, 17, 18
COL = 4                                     # Akords' column (engine 5)
ROW3, ROW4 = 32, 48                         # the page's rows 3 and 4 (key of step 1)
STRUM_UP, STRUM_DOWN, SPREAD, DROP, MUTATE = (ROW3 + c for c in range(11, 16))
MILD, WILD, RECIPE, ALL = (ROW4 + c for c in range(12, 16))
ON, MID, DIM = 0x80, 0x38, 0x10
_image = {}


def image():
    if not _image:
        img, p = build_patched(Image.load(require_image('v1.3.0')), ['voice_map', 'voice_akords:5'],
                               tempfile.mkdtemp())
        _image['x'] = (img, p.cave_symbols['m4host'])
    return _image['x']


@unittest.skipUnless(HAVE_GCC, 'arm-none-eabi-gcc not installed')
class TestRandomPage(unittest.TestCase):
    def rig(self, sd=None):
        img, cave = image()
        r = M4Rig(img, cave, sd=sd if sd is not None else {'BANKS/00/KITS/00.KIT': b''})
        seg = img.segment('m4')
        site = r.sym.site('keys.handler_lit')['addr']
        r.handler = struct.unpack_from('<I', seg.data, site - seg.addr)[0] & ~1
        r.ms = 1000
        r.e.stub(r.sym.fn['hal_get_tick'], lambda e, *a: r.ms)
        r.e.sent(2)
        return r

    def key(self, r, k, down):
        r.event(k, down, handler=r.handler)

    def press(self, r, k):
        self.key(r, k, True)
        r.ms += 50
        self.key(r, k, False)

    def apply(self, r):
        r.e.call(r.sym.fn['apply_mode'], max_insns=5_000_000)

    def ui(self, r):
        r.leds.clear()
        r.e.call(r.sym.fn['ui_update'], max_insns=20_000_000)
        self.apply(r)
        return [[r.leds.get(16 * v + e, 0) for e in range(16)] for v in range(4)]

    def open_page(self, r, voice=1):
        r.set_mode(KIT_MODE)
        self.key(r, KIT, True)
        self.apply(r)
        self.key(r, MOD, True)
        self.key(r, MOD, False)
        self.key(r, KIT, False)
        self.apply(r)
        self.key(r, 16 * voice + COL, True)
        self.ui(r)
        r.ms += 800
        self.ui(r)
        self.key(r, 16 * voice + COL, False)
        self.apply(r)
        self.assertEqual(r.mode(), AKORDS_PAGE)
        r.e.sent(2)

    def voices(self, r, own=False):
        """The "akords-voice" messages since last asked: (voice, fields, options); `own`: with
        the flag for a preset's own sound."""
        return [(p[0], S.ak_unpack(p[1:17]), p[17] if own else p[17] & ~S.O_OWN)
                for t, p in r.e.sent(2) if t == MSG_AKORDS_VOICE]

    def sent(self, r):
        return [(t, bytes(p)) for t, p in r.e.sent(2)]

    def random(self, r, k):
        r.ms += 97                              # a new seed each press (the clock seeds it)
        r.e.sent(2)
        self.press(r, k)
        v = [x for x in self.voices(r)]
        self.assertEqual(len(v), 1)
        return v[0]

    # -- the options ------------------------------------------------------------------------
    def test_the_options_turn_on_and_off(self):
        r = self.rig()
        self.open_page(r, voice=1)
        self.press(r, STRUM_UP)
        self.assertEqual([(v, o) for v, _, o in self.voices(r)], [(1, S.O_STRUM_UP)])
        self.press(r, STRUM_DOWN)
        self.assertEqual(self.voices(r)[0][2], S.O_STRUM_DOWN, 'one strum at a time')
        for k in (SPREAD, DROP, MUTATE):
            self.press(r, k)
        self.assertEqual(self.voices(r)[-1][2], S.O_STRUM_DOWN | S.O_SPREAD | S.O_DROP | S.O_MUTATE)
        self.assertEqual(self.ui(r)[2][11:], [DIM, ON, ON, ON, ON])
        self.press(r, STRUM_DOWN)
        self.press(r, SPREAD)
        self.assertEqual(self.voices(r)[-1][2], S.O_DROP | S.O_MUTATE)
        self.assertEqual(self.ui(r)[2][11:], [DIM, DIM, DIM, ON, ON])

    def test_the_options_and_sound_are_kept_with_the_kit(self):
        r = self.rig()
        self.open_page(r, voice=1)
        self.press(r, 16 * 0 + 3)                               # Keys
        self.press(r, STRUM_UP)
        self.press(r, MUTATE)
        _, fields, _ = self.random(r, RECIPE)
        self.press(r, SHIFT)
        self.apply(r)
        kix = r.e.sd_written['banks/00/kits/00.kix']
        self.assertEqual(kix[:12], b'PKX1AKRD' + struct.pack('<I', 104))
        rec = kix[12 + 26:12 + 52]
        self.assertEqual(rec[9], S.O_STRUM_UP | S.O_MUTATE | 0x40, 'options, Random')
        self.assertEqual(S.ak_unpack(rec[10:26]), fields)
        # load another kit, then this one: the sound and options come back
        r.e.call(r.cave['pk_ak_kit_load'], 1, max_insns=50_000_000)
        self.assertEqual(self.voices(r)[1], (1, S.PRESET_FIELDS[0], 0), 'a kit without a record: presets')
        r.e.call(r.cave['pk_ak_kit_load'], 0, max_insns=50_000_000)
        self.assertEqual(self.voices(r)[1], (1, fields, S.O_STRUM_UP | S.O_MUTATE))

    def test_a_record_reads_back_as_it_was_saved(self):
        r = self.rig()
        self.open_page(r, voice=2)
        self.press(r, DROP)
        _, fields, _ = self.random(r, WILD)
        buf = r.sym.g['json_buf']
        n = r.e.call(r.cave['pk_ak_kit_save'], 0, 3, buf, 128, max_insns=5_000_000)
        rec = bytes(r.e.uc.mem_read(buf, n))
        r2 = self.rig(sd={'BANKS/00/KITS/03.KIX': b'PKX1AKRD' + struct.pack('<I', n) + rec,
                          'BANKS/00/00.PAT': b''})
        r2.e.call(r2.cave['pk4_store_bank_load'], 0, max_insns=900_000_000)
        r2.e.sent(2)
        r2.e.call(r2.cave['pk_ak_kit_load'], 3, max_insns=50_000_000)
        self.assertEqual(self.voices(r2)[2], (2, fields, S.O_DROP))

    def test_an_older_record_plays_the_presets(self):
        rec = b'PKX1AKRD' + struct.pack('<I', 36) + bytes([3, 0, 0, 0, 0, 0, 128, 128, 128] * 4)
        r = self.rig(sd={'BANKS/00/KITS/02.KIX': rec, 'BANKS/00/00.PAT': b''})
        r.e.call(r.cave['pk4_store_bank_load'], 0, max_insns=900_000_000)
        r.e.sent(2)
        r.e.call(r.cave['pk_ak_kit_load'], 2, max_insns=50_000_000)
        self.assertEqual(self.voices(r), [(v, S.PRESET_FIELDS[3], 0) for v in range(4)])

    # -- Random -------------------------------------------------------------------------------
    def test_random_changes_only_the_sound(self):
        r = self.rig()
        self.open_page(r, voice=1)
        for k in (16 * 0 + 1, 16 * 1 + 3, 16 * 1 + 13, ROW3 + 5, ROW4 + 7, SPREAD):   # Pad, Dia 7th smooth, ...
            self.press(r, k)
        self.sent(r)
        self.key(r, MILD, True)
        self.assertEqual(self.sent(r), [], 'not on the press: on the release')
        r.ms += 100
        self.key(r, MILD, False)
        msgs = self.sent(r)
        self.assertEqual([t for t, _ in msgs], [MSG_AKORDS, MSG_AKORDS_VOICE])
        settings = msgs[0][1]
        self.assertEqual(list(settings[4:8]), [1, 3, 5, 7], 'preset, chord, scale, root as they were')
        self.assertEqual(settings[16], 1 << 1, 'the knobs back to the sound\'s own, as a preset press')
        v, fields, o = msgs[1][1][0], S.ak_unpack(msgs[1][1][1:17]), msgs[1][1][17]
        self.assertEqual((v, o), (1, S.O_SPREAD), 'the options as they were')
        self.assertNotEqual(fields, S.PRESET_FIELDS[1])
        self.assertEqual(self.ui(r)[0][1], MID, 'the preset: medium after Random')
        self.press(r, 16 * 0 + 1)
        self.assertEqual(self.voices(r, own=True)[0][1:], (S.PRESET_FIELDS[1], S.O_SPREAD | S.O_OWN),
                         'the preset again: its own sound')
        self.assertEqual(self.ui(r)[0][1], ON)

    def test_mild_stays_close_to_the_preset(self):
        r = self.rig()
        self.open_page(r, voice=0)
        for p in range(len(S.PRESET_FIELDS)):
            self.press(r, 16 * 0 + p)
            for _ in range(4):
                _, f, _ = self.random(r, MILD)
                base = S.PRESET_FIELDS[p]
                for i, (a, b) in enumerate(zip(f, base)):
                    if i in (S.WAVE, S.OSC_N + S.WAVE, S.FMODE, S.TRIM, S.OCT, S.OSC_N + S.OCT):
                        self.assertEqual(a, b, (p, i))
                    else:
                        self.assertLessEqual(abs(a - b), 15, (p, i))     # tidy may move a value further

    def test_wild_and_recipe_make_new_sounds_of_the_kind(self):
        r = self.rig()
        self.open_page(r, voice=0)
        for p in range(len(S.PRESET_FIELDS)):
            self.press(r, 16 * 0 + p)
            seen = set()
            for k in (WILD, RECIPE, RECIPE, RECIPE, ALL):
                _, f, _ = self.random(r, k)
                seen.add(tuple(f))
                self.assertGreaterEqual(f[S.LEV], 12, 'oscillator 1 heard')
                self.assertLessEqual(f[S.OCT], 4)
                if p in (0, 7):                                 # Bass, Reese: low-pass, low
                    self.assertEqual(f[S.FMODE], S.F_LP24)
                    self.assertLessEqual(f[S.OCT], 2)
                if p not in (1, 6, 11, 14):                     # only pads attack slowly
                    self.assertLessEqual(f[S.AENV + S.E_ATT], 25)
            self.assertGreaterEqual(len(seen), 3, 'Random gives different sounds')

    def test_holding_a_random_step_undoes(self):
        r = self.rig()
        self.open_page(r, voice=3)
        self.press(r, 16 * 0 + 4)                               # Pluck
        _, first, _ = self.random(r, WILD)
        _, second, _ = self.random(r, RECIPE)
        self.key(r, ALL, True)
        self.assertEqual(self.ui(r)[3][15], ON, 'held: bright')
        r.ms += 520
        self.ui(r)
        self.assertEqual(self.voices(r)[0][1], first, 'held: the sound before the last Random')
        self.key(r, ALL, False)
        self.assertEqual(self.voices(r), [], 'and no Random on its release')
        self.key(r, ALL, True)
        r.ms += 520
        self.ui(r)
        self.key(r, ALL, False)
        self.assertEqual(self.voices(r), [], 'one undo only')
        self.assertEqual(self.ui(r)[3][12:], [DIM] * 4)


class TestVoiceSound(AkordsCase):
    """The M7 playing a voice's own sound and options."""

    def voice(self, e, fields, opts=0):
        e.send(2, MSG_AKORDS_VOICE, bytes([e.out]) + S.ak_pack(fields) + bytes([opts]))
        e.audio(32)

    def test_a_presets_own_sound_is_the_preset_exactly(self):
        for p in (1, 4, 11):                                    # Pad, Pluck, Choir
            with self.subTest(preset=p):
                e = self.machine(preset=p)
                a = self.play(e, tune_for(7, MAJOR), n=9600)
                e = self.machine(preset=p)
                self.voice(e, S.PRESET_FIELDS[p], S.O_OWN)
                b = self.play(e, tune_for(7, MAJOR), n=9600)
                self.assertEqual(a, b)

    def test_the_presets_values_sound_like_the_preset(self):
        """What Random starts from: each preset as values, within about a dB of the preset."""
        import math
        for p in range(len(S.PRESET_FIELDS)):
            with self.subTest(preset=p):
                e = self.machine(preset=p)
                a = sum(rms(self.play(e, tune_for(7, MAJOR), decay=DRONE, n=48000)))
                e = self.machine(preset=p)
                self.voice(e, S.PRESET_FIELDS[p])
                b = sum(rms(self.play(e, tune_for(7, MAJOR), decay=DRONE, n=48000)))
                self.assertLess(abs(20 * math.log10(b / a)), 1.2)

    def test_its_own_sound_plays(self):
        e = self.machine(preset=8)                              # Organ: triangle, sine
        f = list(S.PRESET_FIELDS[8])
        f[S.WAVE] = f[S.OSC_N + S.WAVE] = S.W_SAW
        x_organ = self.play(e, tune_for(0, MAJOR), n=9600)
        self.voice(e, f)
        x_saw = self.play(e, tune_for(0, MAJOR), n=9600)
        hz = note_hz(model_chord(0)[0]) * 6                     # the root's 6th harmonic: a saw's
        self.assertGreater(goertzel(x_saw[4800:], hz), 20 * goertzel(x_organ[4800:], hz))

    def test_spread_and_bass_drop_move_notes_down(self):
        base = model_chord(3, chord='dia7')
        for opts, want in ((S.O_SPREAD, base[:2] + [base[2] - 12] + base[3:]),
                           (S.O_DROP, [base[0] - 12] + base[1:]),
                           (S.O_SPREAD | S.O_DROP, [base[0] - 12, base[1], base[2] - 12, base[3]])):
            with self.subTest(opts=opts):
                e = self.machine(chord='dia7')
                self.voice(e, S.PRESET_FIELDS[8], opts)
                self.play(e, tune_for(3, MAJOR), n=64)
                self.assertEqual(self.info(e)['last'], sorted(want))

    def test_smooth_chords_lead_from_the_chord_before_spread(self):
        e = self.machine(chord='dia_s')
        self.play(e, tune_for(0, MAJOR), n=64)
        self.play(e, tune_for(3, MAJOR), n=64)                  # C, then F: C F A
        plain = self.info(e)['last']
        e = self.machine(chord='dia_s')
        self.voice(e, S.PRESET_FIELDS[8], S.O_SPREAD)
        self.play(e, tune_for(0, MAJOR), n=64)
        self.play(e, tune_for(3, MAJOR), n=64)
        self.assertEqual(self.info(e)['last'], sorted([plain[0], plain[1] - 12, plain[2]]))

    def strum_onsets(self, opts):
        """When the chord's lowest and highest notes start (ms): a plain sine through an open
        filter, a Power chord high up (C5 G5 C6: far enough apart to tell in 10 ms)."""
        e = self.machine(preset=8, chord='power')
        f = list(S.PRESET_FIELDS[8])
        f[S.WAVE], f[S.OSC_N + S.LEV] = S.W_SINE, 0
        f[S.CUT], f[S.ENVAMT] = 255, 20
        self.voice(e, f, opts)
        x = self.play(e, tune_for(21, MAJOR), n=4800)
        notes = model_chord(21, chord='power')
        w = 480                                                 # 10 ms windows, every 5 ms

        def onset(n):
            lv = [goertzel(x[i:i + w], note_hz(n)) ** .5 for i in range(0, len(x) - w, w // 2)]
            return next(i for i, v in enumerate(lv) if v > 0.3 * max(lv)) * 5
        return onset(notes[0]), onset(notes[-1])

    def test_strum_starts_the_notes_one_by_one(self):
        lo, hi = self.strum_onsets(0)
        up_lo, up_hi = self.strum_onsets(S.O_STRUM_UP)
        self.assertEqual(up_lo, lo, 'up: the lowest note first')
        self.assertGreaterEqual(up_hi - hi, 25, 'up: the top note ~30 ms later')
        down_lo, down_hi = self.strum_onsets(S.O_STRUM_DOWN)
        self.assertEqual(down_hi, hi, 'down: the top note first')
        self.assertGreaterEqual(down_lo - lo, 25, 'down: the lowest ~30 ms later')

    def test_strum_doesnt_click(self):
        e = self.machine(preset=4)                              # Pluck: saws
        self.voice(e, S.PRESET_FIELDS[4], S.O_STRUM_UP)
        x = self.play(e, tune_for(7, MAJOR), n=4800)
        steps = max(abs(x[i + 1] - x[i]) for i in range(len(x) - 1))
        e = self.machine(preset=4)
        y = self.play(e, tune_for(7, MAJOR), n=4800)
        self.assertLess(steps, 1.5 * max(abs(y[i + 1] - y[i]) for i in range(len(y) - 1)))

    def slot_floats(self, e):
        b = e.slot(e.out) + 0xd0
        n = e.r32(e.slot(e.out) + 0xcc)                         # sizeof(slot_t)
        return struct.unpack('<%df' % (n // 4), bytes(e.uc.mem_read(b, n - n % 4)))

    def cutoffs(self, opts, chords=8):
        """The cutoff (Hz) of each chord, read from the chord slot (Pad: 2200 Hz)."""
        e = self.machine(preset=1)
        self.play(e, tune_for(0, MAJOR), n=64)
        at = [i for i, x in enumerate(self.slot_floats(e)) if x == 2200.0]
        self.assertEqual(len(at), 1, 'the cutoff in the slot')
        self.voice(e, S.PRESET_FIELDS[1], opts | S.O_OWN)
        got = []
        for _ in range(chords):
            self.play(e, tune_for(0, MAJOR), n=64)
            got.append(self.slot_floats(e)[at[0]])
        return got

    def test_mutate_varies_each_chord(self):
        import math
        self.assertEqual(set(self.cutoffs(0)), {2200.0}, 'no Mutate: the sound as saved')
        got = self.cutoffs(S.O_MUTATE)
        self.assertGreaterEqual(len(set(got)), 4, got)
        steps = [math.log2(x / 100) / 7.4919 * 256 - 152 for x in got]
        self.assertTrue(all(abs(d) <= 8.01 for d in steps), ('small, from the saved sound each time', steps))

    def test_strum_stays_within_the_cpu_budget(self):
        """While the notes start (a 5-note chord: ~60 ms), against a stock voice."""
        from perkons import verify
        from ak_rig import POS1, POS2, PLAY
        worst = {}
        for opts in (0, S.O_STRUM_UP, S.O_STRUM_UP | S.O_MUTATE | S.O_SPREAD | S.O_DROP):
            e = self.machine(preset=4, chord='dia9')            # Pluck: saws, 5 notes
            self.voice(e, S.PRESET_FIELDS[4], opts)
            self.play(e, tune_for(0, MAJOR), n=960)
            self.play(e, tune_for(3, MAJOR), n=0)
            base = verify.boot(self.img, self.sym)
            v = [{'params': [2048] * 4} for _ in range(4)]
            v[0] = {'params': [2048, DRONE, 0, 4095], 'algo': POS1, 'mode': POS1}
            base.send_voice_state(v)
            base.pump()
            base.send_triggers([1, 0, 0, 0], [(POS1, POS1)] + [(0, 0)] * 3)
            base.audio(960)
            counts = []
            for m in (e, base):
                m.count_insns(True)
                m.insn_count = 0
                m.audio(2880)
                counts.append(m.insn_count / 2880)
                m.count_insns(False)
            worst[opts] = counts[0] - counts[1]
        print('\nAkords strum: ~%d instructions per sample over stock (plain ~%d)'
              % (max(worst.values()), worst[0]))
        self.assertLess(max(worst.values()), 800)

    def test_the_sound_survives_voice_map_rebuilding_it(self):
        from ak_rig import MSG_VOICE_MAP, remap
        e = self.machine(preset=8)
        f = list(S.PRESET_FIELDS[8])
        f[S.WAVE] = f[S.OSC_N + S.WAVE] = S.W_SAW
        self.voice(e, f, S.O_DROP)
        m = [0, 1, 2, 3]
        remap(e, m, self.cave, MSG_VOICE_MAP)
        m[e.out] = 4
        remap(e, m, self.cave, MSG_VOICE_MAP)
        e.audio(960)
        x = self.play(e, tune_for(0, MAJOR), n=9600)
        self.assertEqual(self.info(e)['last'][0], model_chord(0)[0] - 12, 'its options')
        hz = note_hz(model_chord(0)[1]) * 6
        e2 = self.machine(preset=8)
        y = self.play(e2, tune_for(0, MAJOR), n=9600)
        self.assertGreater(goertzel(x[4800:], hz), 20 * goertzel(y[4800:], hz), 'its sound')


@unittest.skipUnless(HAVE_GCC, 'arm-none-eabi-gcc not installed')
class TestRandomLoudness(AkordsCase):
    """Random's sounds, from the M4's own code, played on the M7: about as loud as the presets."""

    def test_random_sounds_sit_with_the_presets(self):
        page = TestRandomPage()
        r = page.rig()
        page.open_page(r, voice=0)
        sounds = []
        for p in range(len(S.PRESET_FIELDS)):
            page.press(r, 16 * 0 + p)
            r.e.sent(2)
            for k in (WILD, RECIPE, RECIPE, RECIPE, WILD, RECIPE):
                sounds.append((p, k, page.random(r, k)[1]))
        levels = {}
        for p in range(len(S.PRESET_FIELDS)):
            e = self.machine(preset=p)
            levels[p] = max(rms(self.play(e, tune_for(7, MAJOR), decay=DRONE, n=48000)))
        lo, hi = min(levels.values()), max(levels.values())
        quiet, loud = [], []
        for p, k, f in sounds:
            e = self.machine(preset=p)
            self.voice(e, f)
            x = max(rms(self.play(e, tune_for(7, MAJOR), decay=DRONE, n=48000)))
            tag = (p, 'mild wild recipe all'.split()[k - MILD], round(x), f[S.FMODE], f[S.CUT], f[S.TRIM])
            if x < 0.5 * lo:
                quiet.append(tag)
            if x > 1.6 * hi:
                loud.append(tag)
        self.assertEqual((quiet, loud), ([], []), 'presets %d-%d' % (lo, hi))

    voice = TestVoiceSound.voice


if __name__ == '__main__':
    unittest.main()
