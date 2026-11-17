"""3 Oh tests: the shared rig (a booted M7 running 3 Oh on voice-map slots), the sound format
(oh_sound.h) and its messages. Needs arm-none-eabi-gcc."""
import math
import os
import re

from fixtures import SYMBOLS_V130, build_patched, require_image
from perkons import m7host, verify
from perkons.emu import Symbols
from perkons.fwimage import Image

HERE = os.path.dirname(os.path.abspath(__file__))
from perkons.voicemap import remap  # noqa: E402

SR = 48000
POS1, POS2, POS3 = 1, 0, 2          # switch values for panel positions 1, 2, 3
ENV, ACC, DIST = POS1, POS2, POS3   # ALGO: PARAM2's job
SAW, SQUARE, PULSE = POS1, POS2, POS3           # MODE: the wave
DRONE = 4095
MID = 2048
PRESETS = ['Classic Acid', 'Squelch', 'Rubber Bass', 'Deep Roller', 'Chirp', 'Wow', 'Liquid', 'Pluck',
           'Muffled', 'Hot Filter', 'Gritty Sub', 'Screamer', 'Acid Lead', 'Rave', 'Fuzz Line', 'Industrial']
SOUND_OFF = 0xc0                    # the object's sound (oh_t.v)


def _widths():
    """oh_sound.h's OH_WIDTH: the C header is the one definition."""
    src = open(os.path.join(HERE, '..', 'src', 'common', 'oh_sound.h')).read()
    body = src[src.index('OH_WIDTH[OH_NFIELD] = {'):]
    body = re.sub(r'/\*.*?\*/', '', body[:body.index('};')].split('{', 1)[1])
    return [int(x) for x in re.findall(r'\d+', body)]


WIDTH = _widths()
NFIELD, PACKED = len(WIDTH), 11
(CUTOFF, RESO, ENVMOD, ACCENT, DISTAMT, DTYPE, TONE, SLIDE, TRIM, PRESET, SCALE, ROOT, OCTAVE, NOTES, RANGE,
 D_ACC, D_SLIDE, D_OCT, D_REST, P2_ENV, P2_ACC, P2_DIST, RSTYLE) = range(23)
MUFFLER, FILTER, OVERDRIVE, DISTORTION = range(4)
MINOR, CHROMATIC = 1, 11
VEL_NORMAL, VEL_ACCENT, VEL_SLIDE, VEL_BOTH = 127, 104, 80, 48


def preset(i):
    """Preset i's 23 values, from oh_presets.h (OH_P(cut, res, env, acc, dist, type, tone, slide,
    trim, n): Natural Minor on C, octave C2, 3 notes over an octave, every density low)."""
    src = open(os.path.join(HERE, '..', 'src', 'common', 'oh_presets.h')).read()
    rows = re.findall(r'^\s*OH_P\(([^)]*)\)', src, re.M)
    types = {'OH_D_MUFFLER': 0, 'OH_D_FILTER': 1, 'OH_D_OVERDRIVE': 2, 'OH_D_DIST': 3}
    a = [types.get(x.strip(), None) if x.strip() in types else int(x) for x in rows[i].split(',')]
    return a[:9] + [a[9], 1, 0, 1, 1, 1, 1, 1, 1, 1, 16, 16, 16, 0]


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


def db(a, b):
    return 20 * math.log10(max(a, 1e-9) / max(b, 1e-9))


def f0(x):
    """Frequency from interpolated upward zero crossings."""
    t = [i + -a / (b - a) for i, (a, b) in enumerate(zip(x, x[1:])) if a < 0 <= b]
    return SR * (len(t) - 1) / (t[-1] - t[0]) if len(t) > 2 else float('nan')


def cents(f, ref):
    return 1200 * math.log2(f / ref)


def bright(x):
    """The share of the signal's energy in its sample-to-sample differences (a brightness
    measure: higher for more high harmonics)."""
    d = [b - a for a, b in zip(x, x[1:])]
    return rms(d) / max(rms(x), 1e-9)


class ThreeOh:
    """The image, built once per test class."""
    def __init__(self, tmp, extra=(), messages=()):
        self.stock = Image.load(require_image('v1.3.0'))
        self.sym = Symbols(SYMBOLS_V130)
        self.img, self.p = build_patched(self.stock, ['voice_map', 'voice_3_oh', *extra], tmp)
        self.cave = self.p.cave_symbols['m7host']
        names = sorted(['three-oh', 'three-oh-live', 'voice-map'] + list(messages))
        self.msg = {n: m7host.FIRST_MESSAGE + i for i, n in enumerate(names)}

    def machine(self, outs=(0,)):
        """Booted M7 with 3 Oh on `outs` (engine 5: map value 4)."""
        e = verify.boot(self.img, self.sym)
        self.remap(e, [4 if o in outs else o for o in range(4)])
        e.audio(960)            # let the mute-fade gain settle (it starts at 0 after boot)
        return e

    def remap(self, e, m):
        remap(e, m, self.cave, self.msg['voice-map'])

    def sound(self, e, out=0):
        """The sound output `out` plays (its 23 values)."""
        a = e.slot(out) + SOUND_OFF
        return [e.r8(a + i) for i in range(NFIELD)]

    def send_sound(self, e, v, out=0, held=0):
        e.send(2, self.msg['three-oh'], bytes([out | held << 4]) + pack(v))
        e.pump()

    def send_live(self, e, bpm=120, drive=(128, 128, 128, 128)):
        t = round(bpm * 10)
        e.send(2, self.msg['three-oh-live'], bytes([t & 255, t >> 8, *drive]))
        e.pump()

    def play(self, e, params, algo=ENV, mode=SAW, note=36, vel=127, n=4800, out=0, trig=True):
        """params: TUNE (cutoff), DECAY, PARAM1 (resonance), PARAM2; note: the step's (36: C2);
        vel: how it plays (127 plain, VEL_ACCENT, VEL_SLIDE, VEL_BOTH)."""
        voices = [{'params': [2048] * 4} for _ in range(4)]
        voices[out] = {'params': list(params), 'algo': algo, 'mode': mode}
        e.send_voice_state(voices)
        e.pump()
        if trig:
            am = [(0, 0)] * 4
            am[out] = (algo, mode)
            e.send_triggers([o == out for o in range(4)], am, vel, note)
        return e.audio(n)[out]
