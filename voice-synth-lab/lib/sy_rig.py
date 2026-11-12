"""Synth Lab tests: the shared rig (a booted M7 running Synth Lab on voice-map slots), the
sound format (sl_sound.h) and its messages. Needs arm-none-eabi-gcc."""
import math
import os
import re
import sys

from fixtures import SYMBOLS_V130, build_patched, require_image
from perkons import m7host, verify
from perkons.emu import Symbols
from perkons.fwimage import Image

HERE = os.path.dirname(os.path.abspath(__file__))
from perkons.voicemap import remap  # noqa: E402

SR = 48000
POS1, POS2, POS3 = 1, 0, 2          # switch values for panel positions 1, 2, 3
RES, ENV, SYNC = POS1, POS2, POS3   # ALGO: PARAM2's job
FREE, SYNCED, SWEEP = POS1, POS2, POS3          # MODE
DRONE = 4095
PRESETS = ['Fat Analog Bass', 'Acid Square', 'Dark Bite Bass', 'West Coast Bongo', 'Aggressive Sync',
           'Hoover', 'Synced Vibrato Lead', 'Tearing Lead', 'Chorus Pad', 'Classic Analog Pad',
           'Ethereal Shimmer', 'Sync Pluck', 'Sync Siren', 'Polyrhythm', 'Voltage Drift', 'Crosscurrent']
SOUND_OFF = 0xc0                    # the object's sound (sy_t.v)


def _fields():
    """(names, widths) from sl_sound.h: the C header is the one definition."""
    src = open(os.path.join(HERE, '..', 'src', 'common', 'sl_sound.h')).read()
    body = src[src.index('SL_WIDTH[SL_NFIELD] = {'):]
    body = body[:body.index('};')]
    osc = [int(x) for x in re.search(r'#define SL_W_OSC ([\d, ]+)', body).group(1).split(',')]
    rest = re.sub(r'/\*.*?\*/', '', body.split('#undef SL_W_OSC')[1])
    return osc * 3 + [int(x) for x in re.findall(r'\d+', rest)]


WIDTH = _fields()
NFIELD, PACKED = len(WIDTH), 40
# indices (as sl_sound.h)
OSC_N = 16
WAVE, PW, LEVEL, OSYNC, OCT, FINE, OSWEEP, RATIO, PLFO, VLFO = 0, 1, 2, 3, 4, 5, 6, 7, 8, 12
FENV, AENV, CLFO, FLFO = 48, 52, 56, 60
FOLD_AMT, FOLD_SYM, FOLD_PUSH, FOLD_ON = 64, 65, 66, 67
FILT_TYPE, KEYTRACK, FDECAY, FILT_ON = 68, 69, 70, 71
CUTOFF, RESO, ENVAMT, ADECAY, GLIDE, LEGATO = 72, 73, 74, 75, 76, 77
PRESET, SCALE, ROOT, P2_RES, P2_ENV, P2_SYNC, MUTATE, RSTYLE, LFO_FREE, TRIM = range(78, 88)
SAW, TRI, PULSE = 2, 1, 4
LP24, LP12 = 3, 1


def pack(v):
    out, bit = bytearray(PACKED), 0
    for f, w in enumerate(WIDTH):
        x = v[f] & ((1 << w) - 1)
        for b in range(w):
            if x >> b & 1:
                out[bit >> 3] |= 1 << (bit & 7)
            bit += 1
    return bytes(out)


def unpack(buf):
    v, bit = [], 0
    for w in WIDTH:
        x = 0
        for b in range(w):
            x |= (buf[bit >> 3] >> (bit & 7) & 1) << b
            bit += 1
        v.append(x)
    return v


def rms(x):
    return (sum(v * v for v in x) / len(x)) ** 0.5 if x else 0.0


def peak(x):
    return max(map(abs, x)) if x else 0


def f0(x):
    """Frequency from interpolated upward zero crossings."""
    t = [i + -a / (b - a) for i, (a, b) in enumerate(zip(x, x[1:])) if a < 0 <= b]
    return SR * (len(t) - 1) / (t[-1] - t[0]) if len(t) > 2 else float('nan')


def cents(f, ref):
    return 1200 * math.log2(f / ref)


class SynthLab:
    """The image, built once per test class."""
    def __init__(self, tmp, extra=(), messages=()):
        self.stock = Image.load(require_image('v1.3.0'))
        self.sym = Symbols(SYMBOLS_V130)
        self.img, self.p = build_patched(self.stock, ['voice_map', 'voice_synth_lab', *extra], tmp)
        self.cave = self.p.cave_symbols['m7host']
        names = sorted(['synth-lab', 'synth-lab-live', 'voice-map'] + list(messages))
        self.msg = {n: m7host.FIRST_MESSAGE + i for i, n in enumerate(names)}

    def machine(self, outs=(0,)):
        """Booted M7 with Synth Lab on `outs` (engine 5: map value 4)."""
        e = verify.boot(self.img, self.sym)
        self.remap(e, [4 if o in outs else o for o in range(4)])
        e.audio(960)            # let the mute-fade gain settle (it starts at 0 after boot)
        return e

    def remap(self, e, m):
        remap(e, m, self.cave, self.msg['voice-map'])

    def sound(self, e, out=0):
        """The sound output `out` plays (its 88 values)."""
        a = e.slot(out) + SOUND_OFF
        return [e.r8(a + i) for i in range(NFIELD)]

    def send_sound(self, e, v, out=0, held=0):
        e.send(2, self.msg['synth-lab'], bytes([out | held << 4]) + pack(v))
        e.pump()

    def send_live(self, e, bpm=120, drive=(128, 128, 128, 128)):
        t = round(bpm * 10)
        e.send(2, self.msg['synth-lab-live'], bytes([t & 255, t >> 8, *drive]))
        e.pump()

    def play(self, e, params, algo=RES, mode=SYNCED, note=36, vel=127, n=4800, out=0, trig=True):
        voices = [{'params': [2048] * 4} for _ in range(4)]
        voices[out] = {'params': list(params), 'algo': algo, 'mode': mode}
        e.send_voice_state(voices)
        e.pump()
        if trig:
            am = [(0, 0)] * 4
            am[out] = (algo, mode)
            e.send_triggers([o == out for o in range(4)], am, vel, note)
        return e.audio(n)[out]
