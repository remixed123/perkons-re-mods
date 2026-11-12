"""Akords tests: a voice's sound (src/common/ak_sound.h, read from the header itself): its
fields, packing, the presets in fields, and the M7's units."""
import os
import re

_H = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'src', 'common', 'ak_sound.h')
_SRC = open(_H).read()


def _table(name):
    body = re.search(name + r'\[[^\]]*\](?:\[[^\]]*\])? = \{(.*?)\n\};', _SRC, re.S).group(1)
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    rows = re.findall(r'\{([^{}]*)\}', body)
    if rows:
        return [[int(x) for x in r.split(',') if x.strip()] for r in rows]
    return [int(x) for x in body.split(',') if x.strip()]


BITS = _table('AK_BITS')
MAX = _table('AK_MAX')
MIN = _table('AK_MIN') + [0] * (len(BITS) - len(_table('AK_MIN')))
PRESET_FIELDS = _table('AK_PRESETS')
NFIELD = len(BITS)
PACKED = 16
(WAVE, OCT, DET, LEV, PW) = range(5)
OSC_N = 5
FMODE, CUT, RES, ENVAMT, KEYTRK = 10, 11, 12, 13, 14
AENV, FENV, GLIDE, TRIM = 15, 19, 23, 24
E_ATT, E_DEC, E_SUS, E_REL = range(4)
W_SINE, W_TRI, W_SAW, W_PULSE = range(4)
F_LP24, F_HP12, F_FORMANT = range(3)
O_STRUM_UP, O_STRUM_DOWN, O_SPREAD, O_DROP, O_MUTATE, O_OWN = 1, 2, 4, 8, 16, 0x80
assert NFIELD == 25 and sum(BITS) <= 8 * PACKED and len(PRESET_FIELDS) == 16


def ak_pack(f):
    out = bytearray(PACKED)
    at = 0
    for i, x in enumerate(f):
        assert MIN[i] <= x <= MAX[i], (i, x)
        for b in range(BITS[i]):
            if x >> b & 1:
                out[at >> 3] |= 1 << (at & 7)
            at += 1
    return bytes(out)


def ak_unpack(data):
    f, at = [], 0
    for i in range(NFIELD):
        x = 0
        for b in range(BITS[i]):
            x |= (data[at >> 3] >> (at & 7) & 1) << b
            at += 1
        f.append(min(MAX[i], max(MIN[i], x)))
    return f


def seconds(x):
    return 0.001 * 2 ** (x / 5)
