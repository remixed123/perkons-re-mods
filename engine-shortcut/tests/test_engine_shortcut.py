"""engine-shortcut: SHUFFLE + a trigger key opens the voice's engine settings page, and the
voice doesn't play."""
import shutil
import tempfile
import unittest

from fixtures import build_patched, require_image
from perkons.fwimage import Image
from perkons.rig import M4Rig

SHIFT, SHUFFLE, KIT, MOD = 0x47, 0x48, 0x41, 0x43
TRIG = [0x4b + v for v in range(4)]
MAIN, SHUFFLE_PAGE, FILL = 0, 8, 9
AKORDS_PAGE = 17                            # the mods' pages after the stock 0-16, in name order
AKORDS, ZIBENS = 4, 5                       # engine page columns (slots 5 and 6)
_image = {}


def image():
    if not _image:
        img, p = build_patched(Image.load(require_image('v1.3.0')),
                               ['voice_map', 'voice_akords:5', 'voice_zibens:6', 'engine_shortcut'],
                               tempfile.mkdtemp())
        _image['x'] = (img, p.cave_symbols['m4host'])
    return _image['x']


@unittest.skipUnless(shutil.which('arm-none-eabi-gcc'), 'arm-none-eabi-gcc not installed')
class TestEngineShortcut(unittest.TestCase):
    def setUp(self):
        img, cave = image()
        r = self.r = M4Rig(img, cave, sd={'BANKS/00/KITS/00.KIT': b''})
        self.played = []
        r.e.stub(r.sym.fn['voice_play'], lambda emu, v, on, *_: self.played.append((v, on)))
        # voice 1 Akords, voice 2 Zibens, voices 3, 4 stock; then back to MAIN
        r.event(KIT, True)
        r.event(MOD, True)
        self.assertEqual(self.apply(), 18, 'the engine page')
        r.event(MOD, False)
        r.event(KIT, False)
        r.press(0 * 16 + AKORDS)
        r.press(1 * 16 + ZIBENS)
        r.set_mode(MAIN)
        self.voice(0xff)

    def apply(self):
        self.r.e.call(self.r.sym.fn['apply_mode'], max_insns=5_000_000)
        return self.r.mode()

    def voice(self, v=None):
        a = self.r.cave['pk_vm_settings_voice']
        if v is not None:
            self.r.e.uc.mem_write(a, bytes([v]))
        return self.r.e.uc.mem_read(a, 1)[0]

    def shuffle_trigger(self, v):
        self.r.event(SHUFFLE, True)
        self.assertEqual(self.apply(), SHUFFLE_PAGE)
        self.r.event(TRIG[v], True)
        mode = self.apply()
        self.r.event(TRIG[v], False)
        self.r.event(SHUFFLE, False)
        return mode, self.apply()

    def test_opens_the_engine_page_without_playing(self):
        self.assertEqual(self.shuffle_trigger(0), (AKORDS_PAGE, AKORDS_PAGE), 'stays open after the releases')
        self.assertEqual(self.voice(), 0)
        self.assertEqual(self.played, [])

    def test_the_page_is_for_that_voice(self):
        self.r.set_mode(MAIN)
        self.r.event(KIT, True)
        self.r.event(MOD, True)
        self.apply()
        self.r.event(MOD, False)
        self.r.event(KIT, False)
        self.r.press(3 * 16 + AKORDS)
        self.r.set_mode(MAIN)
        self.assertEqual(self.shuffle_trigger(3)[1], AKORDS_PAGE)
        self.assertEqual(self.voice(), 3)

    def test_no_page_no_sound_no_change(self):
        for v in (1, 2):                    # Zibens (no settings page), a stock engine
            with self.subTest(voice=v):
                self.assertEqual(self.shuffle_trigger(v), (SHUFFLE_PAGE, MAIN), 'as stock: SHUFFLE\'s page, then MAIN')
                self.assertEqual(self.voice(), 0xff)
                self.assertEqual(self.played, [])

    def test_triggers_play_on_the_page_and_without_shuffle(self):
        self.r.press(TRIG[1])
        self.assertEqual(self.played, [(1, 1), (1, 0)])
        self.shuffle_trigger(0)
        self.played.clear()
        self.r.press(TRIG[0])
        self.assertEqual(self.played, [(0, 1), (0, 0)], 'on the settings page the trigger keys play')

    def test_shift_shuffle_trigger_is_still_a_fill(self):
        self.r.event(SHIFT, True)
        self.r.event(SHUFFLE, True)
        self.apply()
        self.r.event(TRIG[0], True)
        self.assertEqual(self.apply(), FILL)
        self.assertEqual(self.voice(), 0xff)
