# The pad grid: which index is which pad

Four rows of four, sixteen pads, one WS2812 under each. The order everything speaks in --
the STM's SysEx pad messages, `PadManager`, the LED strip, `LED_HOST`, `PAD_PRESS`, the
kit pads, the DAW Control notes -- is:

```
index = row * 4 + column        row 0 is the BOTTOM row, columns counted left to right
printed label on the case = index + 1
```

So pads 0, 1, 2, 3 are the bottom row from left to right (printed `PAD 1 2 3 4`), and
pads 12, 13, 14, 15 are the top row (`PAD 13 14 15 16`). The bottom-left pad is index 0 /
printed PAD 1; the top-right is index 15 / printed PAD 16.

That is not a convention someone chose once and nobody wrote down. It is said, in
matching words, in five places:

| where | what it says |
|---|---|
| the case | `PAD 1 2 3 4` along the bottom row, `PAD 13 14 15 16` along the top |
| the Sampler's own grid on the LCD | `1 2 3 4` in its bottom row, `13 14 15 16` in its top row, and the header names the selected pad ("PAD 1") |
| `crosspad-gui/src/virtual_device/VirtualPadGrid.cpp:28` (the PC simulator) | `const int padIdx = (3 - row) * 4 + col; // bottom-left = 0` and `displayNum = padIdx + 1` |
| `crosspad-gui/src/components/mode_overlay.cpp:426` (the home screen's MODE map) | `const uint8_t pad = (3 - row) * 4 + col + 1;` |
| the manuals | the printed manual and the on-device instructions: "pad 1 is in the lower left corner, pad 16 in the upper right" |

## How to re-prove it, in three commands and one photograph

Do not prove it with pads 0, 5 and 10: those three lie on the diagonal, where this
mapping and its transpose give the same three squares. An earlier measurement note drew
"index = column × 4 + row from the bottom" from exactly that test, and it stood for a
week before it was caught.

Paint two pads that are **not** on the diagonal, and read the case:

```
crosspad-hil --no-hub -d <PORT> cdc LED_HOST ALL 8 8 8
crosspad-hil --no-hub -d <PORT> cdc LED_HOST SET 1 255 0 0     # red
crosspad-hil --no-hub -d <PORT> cdc LED_HOST SET 4 0 0 255     # blue
```

Measured on a v2 board on 28.11.2026: index 1 lit the square printed **PAD 2** (bottom
row, second from the left) and index 4 lit **PAD 5** (second row from the bottom, left
edge). Under the transposed reading those two squares would have been swapped, so this
one photograph separates the two. The numbers behind this were taken on a bench and kept in the maintainer's own
notes; the three commands above reproduce them on any board.

Two traps when you run it, both found the hard way:

- **The host-LED lease lasts 5 s.** `LED_HOST STATUS` reports `lease_ms=5000`; paint the
  pads and photograph a minute later and the board has long since handed the colours back
  to the app. Re-send the paint just before the shot, or shoot inside the window.
- **A webcam hands back a buffered frame.** On Windows, `dshow` keeps a real-time buffer,
  so `ffmpeg -frames:v 1` taken right after the paint can deliver a scene from before it.
  Cap the buffer (`-rtbufsize 65536`), ask for no buffering (`-fflags nobuffer`) and take a
  frame at >= 1 s into a short capture.
- Photograph the board the way you hold it. On this bench the webcam sees it **upside
  down**; a frame has to be rotated 180 degrees before the printed labels read, and that
  matters for anything that maps pixels to pads (see below).

## Why the "crosspad" pad show came out mirrored

Two independent flips, each harmless alone and wrong together. The show tool
(`tools/ledshow.py`) builds its 3×4 font with bitmap row 0 at the top of the letters --
the way a person reads -- and it used to append those rows to the pad array in that same
order, which put the top of every letter on pads 0..3: the **bottom** row. So the word was
painted vertically flipped on the pads. The webcam then saw the board upside down, and a
180-degree rotation is a vertical flip plus a horizontal flip: the two vertical flips
cancelled, and the horizontal one survived. On the recording, the word read **backwards**.

The fix is at both ends, and `tools/ledshow.py` now feeds its rows bottom-up
(`for r in (3, 2, 1, 0)`) and prints its own layout on paper: `python3 tools/ledshow.py
--list` shows each letter row, the pads that row lands on, and the whole window drawn the
way the board is held. If a future change gets the order wrong,
`tools/tests/test_ledshow_grid.py` fails: it asserts the top of a glyph is pads 12..15,
the bottom is 0..3, a lit cell at the left edge is pad 12 and not 15, and that the stem of
the letter `p` -- the one letter in the font with a left-hand side -- stays on the left.
Run against the old loop, those tests fail exactly as they should; the check was done.

## Still open: the DAW host scripts count the rows the other way

`components/crosspad-dawcontrol/host/fl_studio/device_CrossPad.py:85` and
`.../host/ableton/CrossPad/crosspad.py:75` both carry the comment "pad index = row * 4 +
column, **row 0 at the top**", and the control-mode map under that reading (PLAY / STOP /
RECORD / LOOP on pads 0..3, the mute block on 8..15) therefore sits on the row opposite to
the one a DAW grid shows. That is two readings of one board inside one product, and it is
an open question for whoever owns the `crosspad-dawcontrol` repo: either the scripts mean what the case and the device
screen mean, in which case their layout is wrong, or control mode is deliberately mirrored
to the DAW's screen, in which case the comment needs to say so and the mapping needs to be
pinned by a test. Settle it with the photograph above: paint pads 0..3 and see which row
the grid in FL Studio highlights.
