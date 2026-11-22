"""BREAK MAKER tests: the shared rig (a booted M7 running BREAK MAKER on voice-map slots, a break
written into the AXI SRAM as the M4 would), test breaks, measures. Needs arm-none-eabi-gcc."""
import math
import random
import struct

from fixtures import SYMBOLS_V130, build_patched, require_image
from perkons import m7host, verify
from perkons.emu import Symbols
from perkons.fwimage import Image
from perkons.voicemap import remap

SR = 48000
POS1, POS2, POS3 = 1, 0, 2          # switch values for panel positions 1, 2, 3
RUN, GEN, CHOP = POS1, POS2, POS3   # ALGO
ROLL, MANGLE, STUTTER = POS1, POS2, POS3    # MODE: PARAM1's job
MID = 2048
GATE_OFF = 4095                     # DECAY at the top: no gate
BM_BASE = 0x24020100
BM_PCM = BM_BASE + 0x100
MAGIC = 0x524b4d42
STATUS = 0xc0                       # bm_t: st_slice, st_count, st_step, st_kind
KICK, SNARE, HAT, GHOST = range(4)
VEL = {'plain': 127, 'ghost': 104, 'reverse': 88, 'roll2': 72, 'roll3': 56, 'roll4': 40, 'revroll': 16}
PADS = ['roll8', 'roll16', 'roll32', 'reverse', 'freeze', 'half', 'tapestop', 'gate', 'random', 'filter',
        'stutter']


def p2_of_slice(s, n=16):
    """PARAM2's value for slice s of n: the middle of its zone (bm_shared.h)."""
    return ((2 * (s + 1) + 1) * 4096) // (2 * (n + 1))


def p2_zone(x, n=16):
    return max(0, min(n, x * (n + 1) // 4096))


def make_break(rate=44100, bpm=140.0, seed=1, marks=True):
    """A 1-bar test break: 16 slices, each a distinct sound so its slice can be told from the
    output: a tone at 200 + 50 s Hz (s the slice) under an exponential decay. With marks, each
    slice starts with a click (a hit). Returns (int16 list, slice starts)."""
    n = int(round(rate * 240.0 / bpm))
    g = n / 16
    out = [0] * n
    starts = [int(round(i * g)) for i in range(16)]
    for s in range(16):
        a, b = starts[s], starts[s + 1] if s < 15 else n
        f = 200.0 + 50.0 * s
        for i in range(a, b):
            t = (i - a) / rate
            out[i] = int(12000 * math.sin(2 * math.pi * f * t) * math.exp(-t * 6))
    return out, starts


DRUMS = {0: KICK, 10: KICK, 4: SNARE, 12: SNARE, 7: GHOST}     # the rest: hats


def drum_break(rate=44100, bpm=140.0, seed=3, offset=0, only=None):
    """A 1-bar break of synthetic drums on 16ths: kick on 1 and the "and" of 3, snare on 2 and 4,
    a ghost snare on step 8, hats (high-passed noise) elsewhere. offset: every hit that many frames
    late (off the grid); only: the steps with a hit (the rest silent). Returns (pcm, kind per step,
    where each hit begins)."""
    rnd = random.Random(seed)
    n = int(round(rate * 240.0 / bpm))
    g = n / 16
    out = [0.0] * n
    kinds, hits = [], []
    for s in range(16):
        k = DRUMS.get(s, HAT)
        kinds.append(k)
        a = int(round(s * g)) + (offset if s else 0)
        hits.append(a)
        if only is not None and s not in only:
            continue
        prev = 0.0
        for i in range(int(g)):
            t = i / rate
            if k == KICK:
                ph = 2 * math.pi * 55 * (t + (2 / 40) * (1 - math.exp(-t * 40)))
                v = 20000 * math.sin(ph) * math.exp(-t * 8)
            elif k in (SNARE, GHOST):
                lv = 1.0 if k == SNARE else 0.12
                v = lv * (9000 * math.sin(2 * math.pi * 190 * t) + 11000 * rnd.uniform(-1, 1)) * math.exp(-t * 18)
            else:
                w = rnd.uniform(-1, 1)
                v = 5000 * (w - prev) * math.exp(-t * 60)     # a first difference: bright noise
                prev = w
            if a + i < n:
                out[a + i] += v
    return [max(-32767, min(32767, int(v))) for v in out], kinds, hits


def wav(pcm, rate=44100, cues=None, channels=1, bits=16, cue_after=True):
    """A WAV file's bytes: PCM, cue points (frame offsets) after the data (as most editors write
    them) or before it."""
    data = struct.pack(f'<{len(pcm)}h', *pcm) if bits == 16 else bytes(len(pcm))
    fmt = struct.pack('<HHIIHH', 1, channels, rate, rate * channels * bits // 8, channels * bits // 8, bits)
    chunks = [b'fmt ' + struct.pack('<I', len(fmt)) + fmt]
    cue = b''
    if cues:
        body = struct.pack('<I', len(cues)) + b''.join(
            struct.pack('<IIIIII', i + 1, c, 0x61746164, 0, 0, c) for i, c in enumerate(cues))
        cue = b'cue ' + struct.pack('<I', len(body)) + body
    d = b'data' + struct.pack('<I', len(data)) + data
    chunks += [d, cue] if cue_after else [cue, d]
    body = b'WAVE' + b''.join(chunks)
    return b'RIFF' + struct.pack('<I', len(body)) + body


def rms(x):
    return (sum(v * v for v in x) / len(x)) ** 0.5 if x else 0.0


def peak(x):
    return max(map(abs, x)) if x else 0


def db(a, b):
    return 20 * math.log10(max(a, 1e-9) / max(b, 1e-9))


def freq(x, sr=SR):
    """Frequency from interpolated upward zero crossings."""
    t = [i + -a / (b - a) for i, (a, b) in enumerate(zip(x, x[1:])) if a < 0 <= b]
    return sr * (len(t) - 1) / (t[-1] - t[0]) if len(t) > 2 else float('nan')


def slice_of(x, rate=44100, sr=SR, speed=1.0):
    """Which make_break slice a stretch of output is (its tone, played at `speed`)."""
    f = freq(x, sr) / speed
    return round((f - 200.0) / 50.0)


class BreakMaker:
    """The image, built once per test class."""
    def __init__(self, tmp, extra=(), messages=()):
        self.stock = Image.load(require_image('v1.3.0'))
        self.sym = Symbols(SYMBOLS_V130)
        self.img, self.p = build_patched(self.stock, ['voice_map', 'voice_break_maker', *extra], tmp)
        self.cave = self.p.cave_symbols['m7host']
        names = sorted(['break-maker', 'break-maker-step', 'voice-map'] + list(messages))
        self.msg = {n: m7host.FIRST_MESSAGE + i for i, n in enumerate(names)}

    def machine(self, outs=(0,), pcm=None, starts=None, rate=44100, kinds=None, steps=None):
        """Booted M7 with BREAK MAKER on `outs` (engine 5: map value 4), and a break in RAM."""
        e = verify.boot(self.img, self.sym)
        if pcm is not None:
            self.load(e, pcm, starts, rate, kinds, steps)
        self.remap(e, [4 if o in outs else o for o in range(4)])
        e.audio(960)
        return e

    def load(self, e, pcm, starts=None, rate=44100, kinds=None, steps=None):
        """The break as the M4 leaves it: starts (frames) and the step each slice starts on
        (default: 16 equal slices, one a step)."""
        n = len(pcm)
        starts = starts or [n * i // 16 for i in range(16)]
        ns = len(starts)
        steps = steps or [i * 16 // ns for i in range(ns)]
        e.uc.mem_write(BM_PCM, struct.pack(f'<{n}h', *pcm))
        sl = list(starts) + [n] * (17 - ns)
        hdr = struct.pack('<6I', MAGIC, 0, 1, n, rate, BM_PCM) + struct.pack('<17I', *sl)
        hdr += bytes((kinds or [HAT] * ns) + [0] * (16 - ns)) + bytes([0, 1, ns, 0])
        hdr += bytes(list(steps) + [0] * (16 - ns))
        e.uc.mem_write(BM_BASE, hdr)
        e.uc.mem_write(BM_BASE + 4, struct.pack('<I', 1))         # ready

    def remap(self, e, m):
        remap(e, m, self.cave, self.msg['voice-map'])

    def status(self, e, out=0):
        a = e.slot(out) + STATUS
        return struct.unpack('<4i', bytes(e.uc.mem_read(a, 16)))

    def settings(self, e, out=0, keep=False, pads=(), char=0, aud=0):
        bits = sum(1 << PADS.index(p) for p in pads)
        e.send(2, self.msg['break-maker'], bytes([out, 1 if keep else 0, bits & 255, bits >> 8, char, aud]))
        e.pump()

    def step(self, e, step, out=0, first=0, last=15, playing=True, on=False, bpm=140.0, hit=None):
        """A step message; hit: a PARAM2 value (0-4095) for the trigger that follows (a hit message)."""
        t = round(bpm * 10)
        flags = (1 if playing else 0) | (2 if on else 0) | (4 if hit is not None else 0)
        p2 = hit or 0
        e.send(2, self.msg['break-maker-step'], bytes([out, step, first, last, flags, t & 255, t >> 8,
                                                       p2 & 255, p2 >> 8]))
        e.pump()

    def knobs(self, e, params, algo=RUN, mode=ROLL, out=0):
        """params: TUNE, DECAY, PARAM1, PARAM2."""
        voices = [{'params': [2048] * 4} for _ in range(4)]
        voices[out] = {'params': list(params), 'algo': algo, 'mode': mode}
        e.send_voice_state(voices)
        e.pump()

    def trig(self, e, out=0, algo=RUN, mode=ROLL, vel=127, note=36):
        am = [(0, 0)] * 4
        am[out] = (algo, mode)
        e.send_triggers([o == out for o in range(4)], am, vel, note)

    def run_bar(self, e, bpm=140.0, out=0, steps=range(16), on=(), trig_at=None, first=0, last=15,
                algo=RUN, mode=ROLL, vels=None):
        """Play steps at the tempo (64-frame blocks: a step's length rounded to them); trig_at:
        {step: velocity} triggers right after their step message. Returns the output and where
        each step began (sample index)."""
        step_len = SR * 15.0 / bpm
        out_buf, at = [], []
        t = 0.0
        for s in steps:
            self.step(e, s, out=out, first=first, last=last, on=s in on, bpm=bpm)
            if trig_at and s in trig_at:
                self.trig(e, out=out, algo=algo, mode=mode, vel=trig_at[s])
            at.append(len(out_buf))
            t += step_len
            n = int(round(t)) - len(out_buf)
            n = max(64, n - n % 64)
            out_buf += e.audio(n)[out]
        return out_buf, at
