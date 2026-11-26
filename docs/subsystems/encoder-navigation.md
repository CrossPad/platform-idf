# Encoder Navigation

> Source of truth for this subsystem. `CLAUDE.md` carries only the summary; when
> behaviour changes, update this file and the summary together.

The knob is one indev and one rule: **a detent is a decision somebody made with
their fingers**, so nothing here auto-repeats — not the list scrolling while the
knob sits between detents, and not a step that keeps going after the hand stopped.
What the board does *not* do is pretend a decision is smaller than the hand that
made it: turn fast and the step grows (ISSUES.md 9), turn carefully and one detent
is still one unit.

## The chain, end to end

| stage | where | what it carries |
|---|---|---|
| the knob itself | the STM32 scans A/B and the button; it sends its detent index (0..15, so **sixteen detents to a turn**) and the quadrature steps | `crosspad-core` `Stm32MessageHandler::input.encoderTop`, `encoderPhase` |
| one step per detent | `Stm32MessageHandler::takeEncoderSteps()` — the steps are counted as the reports arrive and drained once per read. The raw count used to gain about seven counts a click, and a step read from it was two: both were corrected before this point, and the handler's own tests pin the decoding | `crosspad-core/include/crosspad/protocol/Stm32MessageHandler.hpp:121-139`, `crosspad-core/tests/test_stm32_uart_frames.cpp:116-131` |
| the read callback | `encoderTop_read()` — LVGL calls it every read window (**33 ms**, `CONFIG_LV_DEF_REFR_PERIOD`), it drains the accumulated steps, applies the accelerator, and reports `enc_diff` | `components/bsp/crosspad/include/indev/encoder.h` |
| what LVGL does with `enc_diff` | **one event per unit**: in edit mode `LV_KEY_LEFT`/`LV_KEY_RIGHT` once per step (`lv_indev.c:1092-1108`), in navigate mode one `lv_group_focus_next/prev` per step (`:1110-1126`). A slider therefore moves by its own step times the diff | `managed_components/lvgl__lvgl/src/indev/lv_indev.c` |
| who gets it | the focused widget of the group the current screen set: the launcher, an app's own group, the drawer, the top bar's keys. Apps set and restore it (`lv_indev_set_group` / `lv_indev_restore_group`) | `main/include/display.h:216-221`, every app's create/destroy |

Two LVGL rules worth knowing because they explain what a hand feels:

- **Steps are dropped while the button is held** (`lv_indev.c:930-933`: a non
  released `data->state` zeroes `enc_diff`). Turning the knob while pressing its
  click does nothing on purpose — that combination is a click, not a move.
- A click is formed by a press and release inside `LV_INDEV_DEF_LONG_PRESS_TIME`;
  a long press is what enters and leaves edit mode (`:1078-1083`). Holding the
  click on an edited widget repeats keys at `long_press_repeat_time` — that is a
  different mechanism from this accelerator, and it is why "hold and wait" is not
  how anyone adjusts a fader here.

## The accelerator

`components/bsp/crosspad/include/indev/encoder_accel.h` — the rule alone, no LVGL
and no ESP in it, so it is tested against hand-shaped timings instead of by feel
(`main/tests/test_encoder_accel.cpp`, 11 cases / 332 assertions, fed the 33 ms
windows a hand makes).

Feed it one read window (the detents drained in it, signed) and it returns the
multiplier to report that window with:

| part | value | why |
|---|---|---|
| rate basis | detents/s, exponential average over **every** window including the empty ones, τ = 200 ms (~6 read windows) | averaging only over windows that moved reads a slow turn as a fast one — one detent every 300 ms lands in a 33 ms window as 30/s — and a wrong fast step costs the user a value they have to wind back. Six windows is also why **one hard flick does not jump to the cap**: a single 3-detent window reads ~90/s for itself and lands around ×2, while a hand that keeps cranking reaches ×8 inside a fifth of a second |
| tiers | ×2 from 7/s, ×4 from 18/s, ×8 from 36/s, capped at 8 | 16 detents a turn: 2/s is careful, 8/s a normal turn, 24/s and up a fast one. At ×8 one fast turn covers a 0..100 fader twice; past that a step is bigger than the gap between two detents |
| hysteresis | comes down only under 3/4 of the tier's rate | a hand at a boundary otherwise flickers between two step sizes mid-move, which is worse than one that is consistently a little coarse |
| **a direction change** | the reversing window reports one unit whatever the rate is, and the average starts over | overshoot is the price of a big step; reversing is how you correct it, and a correction has to be exact. This is the rule that makes a big step safe to have. If the hand really is sweeping the other way, the ramp restarts from the next window |
| a pause | after 500 ms with no detent, the next move starts at ×1 | the first detent after a pause is a new, deliberate move |
| a stalled read | the window's `dt` is clamped to 1..500 ms | a card read or a big redraw must not make one detent look like a fast turn, nor two reads in one tick divide by zero |

Switches and read-out, over CDC/HIL:

| verb | what |
|---|---|
| `ENC_ACCEL` | what the last windows did: on/off, the multiplier now, the smoothed rate and its maximum, the tier constants, the window counters and a histogram of detents per window (0, 1, 2-3, 4-7, 8+) |
| `ENC_ACCEL 0\|1` | switch the rule, without a reboot and without losing the counters — the A/B a person needs to judge it |
| `ENC_ACCEL INJECT 0\|1` | let an injected `ENC_ROTATE` join the window the knob's own detents make, so a ladder can walk the tiers at a scripted rate. Off by default: the injection means exactly what it says, and the scenarios built on it count on that |
| `ENC_ACCEL RESET` | zero the counters, so one turn of a real hand can be read on its own |
| build with `-DCROSSPAD_ENCODER_DEBUG=1` | the status bar shows `d<index> <steps> x<multiplier>` while you turn |

**Tuning it against a hand, on the bench** (this is how the constants get moved to
what a real hand actually does, and it is the one part of this page that a
maintainer should repeat before merging a change to the tiers):

```text
ENC_ACCEL RESET          # then turn the knob the way you normally would,
                         # a few times: to a fader, back, to another fader
ENC_ACCEL                # read hist= and rate_max=
```

If the histogram's weight sits in the `2-3` bucket while `mult` is still 1, the
hand is faster than the first tier assumes and `kRate2` is too high; the other way
round, the user is being jumped when they meant to be fine. The counters distinguish
`up`/`down` (the tiers being crossed) from `reversed` (a correction) and `idles`
(the pause reset) — which is what to look at when the feel is wrong.

## What was not used, and why

- `lv_indev_set_rotary_sensitivity()` and the `rotary_sensitivity` style: LVGL
  applies those to *scrollable* objects' wheel handling (`lv_indev.c:1558-1571`),
  and this board's widgets change through keys; a fixed sensitivity is also a
  scale rather than an accelerator, which is the thing being asked for.
- A per-widget step ladder: every fader, dropdown and list on the board would need
  its own rule, and the complaint is about the hand, not about a widget.
- A user setting in Settings → Display/Pads: the constants are not settled yet
  (they are reasoned from 16 detents a turn, not measured across hands), and a
  setting is a promise. `ENC_ACCEL 0|1` is the honest interim, and the read-out is
  what turns the argument into numbers.

## HIL

`ENC_ROTATE <n>`, `ENC_PRESS [ms]`, `ENC_STATE`, `ENC_RAW`, `ENC_FOCUS`,
`ENC_GROUP` (`main/hil_control.cpp`, catalogued in crosspad-hil
`knowledge/cdc.yaml`). Injection goes through the same read callback as the knob —
`encoderTop_hil_diff()` — because `lv_group_send_data()` only sends an
`LV_EVENT_KEY`: focus never moves that way, and widgets that gate on
`lv_indev_active()` see no encoder at all. `UI_TREE` and `UI_STATE` are how a test
sees what the focus and the values became.
