"""Kick Lab tests: the shared rig (a booted M7 running Kick Lab on voice-map slots) and the
settings message. Needs arm-none-eabi-gcc."""
import math
import os
import sys

from fixtures import SYMBOLS_V130, build_patched, require_image
from perkons import m7host, verify
from perkons.emu import Symbols
from perkons.fwimage import Image

from perkons.voicemap import remap  # noqa: E402

SR = 48000
POS1, POS2, POS3 = 1, 0, 2          # switch values for panel positions 1, 2, 3
ATTACK, DELAY_MIX, DRIVE_MIX = POS1, POS2, POS3     # ALGO: PARAM2's job
CLEAN, WHITE, PINK = POS1, POS2, POS3               # MODE
DRONE = 4095
DRONE_AT = round(0.95 * 4095)
MID_DECAY = 2048                    # ~0.36 s
PRESETS = ['Basic', 'Tight Techno', '9x Punch', '8x Punch', 'Warm House', 'Boom Bap', 'Acoustic',
           'Hardstyle Rave', 'Drop', '8x Melodic Sub', '8x Sub Boom', 'Sustained', 'Hammer', 'Lo-Fi Crush',
           'Dub Echo', 'Spring Thud']
SOFT, HARD, CRUSH, FOLD = range(4)
TIME_MS = [15, 25, 40, 60, 80, 100, 125, 150, 190, 250, 375, 500, 750, 1000]
DIV_BEATS = [1 / 16, 1 / 8, 1 / 6, 1 / 4, 3 / 8, 1 / 3, 1 / 2, 3 / 4, 2 / 3, 1, 3 / 2, 4 / 3, 2, 3]


def tune_for(hz):
    return round(math.log10(hz / 20) * 4095)


def decay_for(s):
    return round(0.95 * math.log2(s / 0.03) / 7.0588937 * 4095)


def settings(preset=0, drv_type=SOFT, drv_amt=2, drv_tone=1, sync=0, time=8, fb=0, dl_tone=5,
             attack=0, delay_mix=0, drive_mix=0):
    """A voice's 6 bytes in the "kick-lab" message (mixes and attack 0-255)."""
    return bytes([preset | drv_type << 4 | drv_tone << 6, drv_amt | time << 4, fb | dl_tone << 4 | sync << 7,
                  attack, delay_mix, drive_mix])


def rms(x):
    return (sum(v * v for v in x) / len(x)) ** 0.5 if x else 0.0


def env(x, w=480):
    return [max(abs(v) for v in x[i:i + w]) for i in range(0, len(x) - w + 1, w)]


class KickLab:
    """The image, built once per test class (setUpClass: KickLab.build(cls, tmp))."""
    def __init__(self, tmp, extra=(), messages=()):
        """extra: more patches; messages: the M7 messages they add."""
        self.stock = Image.load(require_image('v1.3.0'))
        self.sym = Symbols(SYMBOLS_V130)
        self.img, self.p = build_patched(self.stock, ['voice_map', 'voice_kick_lab', *extra], tmp)
        self.cave = self.p.cave_symbols['m7host']
        # the M7 host's message types, in name order (a cached build keeps no table)
        names = sorted(['kick-lab', 'kick-lab-tempo', 'voice-map'] + list(messages))
        self.msg = {n: m7host.FIRST_MESSAGE + i for i, n in enumerate(names)}

    def machine(self, outs=(0,)):
        """Booted M7 with Kick Lab on `outs` (engine 5: map value 4)."""
        e = verify.boot(self.img, self.sym)
        self.remap(e, [4 if o in outs else o for o in range(4)])
        e.audio(960)            # let the mute-fade gain settle (it starts at 0 after boot)
        return e

    def remap(self, e, m):
        remap(e, m, self.cave, self.msg['voice-map'])

    def send_settings(self, e, voices, apply=0xf, held=0):
        """voices: {output: settings(...)}; the others' bytes are zero."""
        buf = b''.join(voices.get(o, bytes(6)) for o in range(4)) + bytes([apply, held])
        e.send(2, self.msg['kick-lab'], buf)
        e.pump()
        e.audio(64)             # taken at the next block, with the knobs as they are

    def send_tempo(self, e, bpm):
        t = round(bpm * 10)
        e.send(2, self.msg['kick-lab-tempo'], bytes([t & 255, t >> 8]))
        e.pump()

    def play(self, e, params, algo=ATTACK, mode=CLEAN, note=36, vel=127, n=4800, out=0, trig=True):
        voices = [{'params': [2048] * 4} for _ in range(4)]
        voices[out] = {'params': list(params), 'algo': algo, 'mode': mode}
        e.send_voice_state(voices)
        e.pump()
        if trig:
            am = [(0, 0)] * 4
            am[out] = (algo, mode)
            e.send_triggers([o == out for o in range(4)], am, vel, note)
        return e.audio(n)[out]
