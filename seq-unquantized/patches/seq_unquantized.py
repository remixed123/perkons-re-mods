"""seq-unquantized: the trigger keys and MIDI notes record where they were played, off the
grid (config row 3 step 13). Built by the framework's M4 host; hook sites are in
../symbols/<version>-m4.json (merged into the framework's symbols)."""
import os

from perkons import m4host

TARGET = 'v1.3.0'
SUMMARY = 'Unquantized: trigger keys and MIDI notes record off the grid, as played (config row 3 step 13)'

MOD = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(MOD, 'src', 'm4')
KEY = 44                    # config row 3 step 13


def apply(p, arg=None):
    h = m4host.host(p)
    h.add([os.path.join(SRC, 'unquantized.c')], [SRC])
    h.toggle(KEY, 'unquantized', 'pk_cfg_unquantized')
    h.replace_word('keys.handler_lit', 'pk_uq_key_event', 'key events: which trigger key is handled')
    h.redirect_call('uq.playing.081039ec', 'pk_uq_playing', 'trigger key while recording: the step and its delay as played')
    h.replace_word('uq.midi_note_lit', 'pk_uq_midi_note', 'MIDI notes while recording: the step and its delay as played')
