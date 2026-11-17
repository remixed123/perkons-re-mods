# Unquantized

The trigger keys and MIDI notes record your hits where you played them, not snapped to the
nearest step.

Based on official Pērkons firmware **1.3.0**. Unofficial; not affiliated with Erica Synths.

## Getting it

Build Pērkons firmware with **UNQUANTIZED** ticked in the **Mod Matrix**, from your own
official firmware 1.3.0: follow [Get modded firmware](https://github.com/kiteless/perkons-re#get-modded-firmware)
in perkons-re. No firmware files are provided here. To remove it, flash the official
firmware again.

## Turning it on

1. Hold **SHIFT** and press **MOD** to open the config menu.
2. Press **step 13 on the third row** of step buttons. LED on: unquantized. LED off (as
   it starts): recording snaps to the grid, as on the factory firmware.
3. Leave the config menu as usual; the choice is saved on the SD card.

## Using it

Record as usual: press **REC** while the pattern plays and tap the **trigger keys** or play
notes into **MIDI in**.

* Each hit goes into the step that was playing when you played it, with how late into that
  step it came. It plays back there, every time round.
* The timing is kept as a share of a step (24 positions per step, as fine as 96 PPQN), so
  it follows tempo changes, an external clock and each track's DIVIDE / MULTIPLY.
* A step holds one hit per track: a second hit in the same step replaces the first.
* The timing is stored where the pattern keeps shuffle, so it's saved with the pattern and
  the official firmware plays it the same way. Setting a track's **SHUFFLE** again puts
  that track's steps back on the shuffle grid.
* MIDI notes keep their velocity (with MIDI velocity on in the config menu) and, in multi
  channel mode, their note, as when recording on the grid.
* Steps you program with the step buttons stay on the grid.
* While it's off, recording snaps to the nearest step as on the factory firmware.
