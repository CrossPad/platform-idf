# FX Pad: the board as a performance multi-effect

The **fxpad** firmware kit (`config/profiles/fxpad.json`) is one app, FX Pad
(`components/crosspad-fxpad`), with no sampler, song or kit scan started
(`"sample_engine": false`) and **no radio** (`"radio": false`: no WiFi lease,
no BLE, no remote session -- the radio held ~60 kB of internal RAM and time on
core 0 in a board paired to an account; updates come over USB). What comes in -- line 1/2, line 3/4, the mics, the
computer over USB -- goes through the effects the pads hold and out to the
phones, the line out and the computer. Kaoss-style: a pad held is its effect
on, a finger on the XY surface sets its two parameters, the pad's pressure
sets WET, two effects at once with LATCH.

```
idf.py -B build_v2 -DSDKCONFIG=sdkconfig.v2 -DCROSSPAD_FW_KIT=fxpad build
```

## The path

```
IN2 (line 1/2) ─┐
IN3 (line 3/4) ─┤ send (post- or pre-fader, pan kept)
IN1 (mics) ─────┤──────────► FX bus ──► FxEngine::process() ──► returns ──► Out 1/2, Out 3/4, USB rec
USB (host) ─────┘           (sum)       (in place, every block)            (before volume and limiter)
```

- **The mixer's FX bus** (crosspad-mixer 0.22, `AudioMixerEngine::setFxProcessor`,
  `setFxSend`, `setFxReturn`, `setFxPreFader`, `getFxLevel`): one insert bus
  beside the outputs. Channels send to it in the routing pass, the processor
  runs over the sum in the same pass, and its result is added to the outputs
  it returns to before their volume and limiter -- no block of latency. The
  processor runs every block once set, sound or not, so tails ring out. A
  muted channel sends nothing; nothing of the bus is saved with the desk.
- **`IFxBus`** (core 2.47, `PlatformServices::fxBus`): the app's view,
  sources (`FxSource`) and outputs (`FxOutput`) by name, not by mixer
  channel. `FxBusPort.cpp` implements it: Line 1/2 = IN2, Line 3/4 = IN3, the
  mics = IN1, USB = the host's playback channel; Out 1/2, Out 3/4 and the USB
  rec bus. **Attached, every source is taken off its own routes** (saved, put
  back on `detach()`), so the processor is the whole path and carries the dry
  signal too -- a parallel dry would comb against a delayed copy.
- **Two ADCs, three analog inputs**: the sends pick the input pair
  (`input_pairs_cover()`), and the input monitor counts an FX send as a send,
  so the codec's ADC is held on the input (`InputMonitorPort.cpp`). Sending
  all three leaves one unheard; ROUTE shows it "off" (`sourceAvailable()`).
  USB is available only in the USB audio profile.

## The effects (`FxEngine.cpp`)

Thirty-two in two banks of sixteen, a bank on the pads at a time (BANK on
FX; the encoder on FX walks both), in pad order (pad 1 bottom left), all
running at once if asked, in the order they were started. A pad held keeps
its effect across a BANK until it is let go. Bank A:

| | X | Y | release |
|---|---|---|---|
| ECHO, PING PONG | TIME | FDBK | tail |
| TAPE ECHO | TIME | WOW | tail |
| ROLL | RATE | DECAY | snap |
| HALL | SIZE (0.3-7.5 s) | DAMP | tail |
| SHIMMER | SIZE | PITCH (+0..24 st in the tail) | tail |
| FREEZE | TONE | LEVEL | tail (fades 1.5 s) |
| GATE | RATE | DEPTH | snap |
| LP / HP FILTER | CUTOFF (80 Hz-16 kHz) | RES | snap |
| BAND SWEEP | FREQ | WIDTH | snap |
| WAH | FREQ | RES | snap |
| FLANGER | SPEED (Hz) | DEPTH | snap |
| PHASER | SPEED (Hz) | FDBK | snap |
| BITCRUSH | BITS | RATE | snap |
| DRIVE | GAIN (0-36 dB) | TONE | snap |

Bank B cuts the sound up -- for building a track live out of what is
playing. They read one shared **tape**, the last ~3 s of the input (2^17
stereo frames, ~1 MB of PSRAM), which stops while an effect is looping out
of it, so a repeat holds what was there at the press. All release SNAP
(ECHO FRZ can be set to TAIL).

| | X | Y |
|---|---|---|
| STUTTER | SLICE (1/8 .. 1/64) | GATE |
| BEAT RPT | LEN (1/16 .. 1 bar) | STEP (-2 .. +2 st a pass) |
| GLITCH | SLICE | CHAOS |
| SLICER | PATTERN (#1-8, sixteenths) | DEPTH |
| REVERSE | LEN | TONE |
| TAPE STOP | TIME | CURVE |
| SPINBACK | TIME | BACK |
| SCRATCH | POSITION | LEN |
| GRAIN | SIZE (20-200 ms) | DENSITY |
| PITCH | SHIFT (-12 .. +12 st) | FDBK |
| AUTO FLT | RATE (a sweep every four) | RES |
| PUMP | RATE | DEPTH |
| LOFI | AGE | WOBBLE |
| RING MOD | FREQ | SHAPE |
| ECHO FRZ | LEN | FADE |
| PAN CHOP | SLICE | WIDTH |

SLICE and LEN are note values with SYNC, milliseconds without (125 .. 15 ms,
60 ms .. 2.5 s). TIME and RATE are note values with SYNC (1/32 .. 1/2, the transport's tempo,
TAP or a host's MIDI clock), milliseconds without. Delays, reverbs and freeze
**add** their wet signal to the dry; filters, modulation, distortion, gate and
roll **blend** it in place of the dry by WET. THRU off: the sources are heard
only through an effect (a blend effect on them is all wet, an added one leaves
the dry out); with nothing on, silence.

How the render keeps it clean, each measured in the host tests
(`components/crosspad-fxpad/tests`, Catch2: every effect at every corner of
X/Y finite and bounded, tails end, THRU, two at once):

- **No buffer is cleared on the render thread.** A delay line knows how much
  has been written since its effect started (`Delay::clear()` resets only that
  count) and reads further back as silence; clearing two seconds of PSRAM
  costs milliseconds.
- X, Y and WET follow over ~30 ms, a gate closing over ~10 ms, SNAP fades in
  ~30 ms, all ramped a sample at a time within the block.
- A tail ends after 0.4 s under -74 dBFS, or after 15 s whatever it is.
- ROLL listens all the time (a 2 s ring), so its loop is what came before the
  press; the ring stops while it loops.
- The reverbs run at half rate (a 2:1 decimation into power-of-two rings).
- Memory: ~4 MB of PSRAM (three 2 s stereo delays, the roll ring, four small
  reverbs, the tape), taken once when the processor is attached.

Measured (`RT_PROF`, 1451 us block, radio off): idle 194 us average / 489
worst; SHIMMER + HALL 528 / 1069; PITCH + GRAIN 518 / 959; GLITCH + SCRATCH
279 / 767; LOFI + AUTO FLT 384 / 799.

## The app

- **The pads start an effect from their own task**, straight into the
  engine's atomics (`cp_fxpad_pad()`): on within the block after the hit, not
  after a screen refresh. The screen and the pad LEDs follow from a 30 ms
  LVGL timer (`cp_fxpad_tick()`); one recursive mutex keeps the pads' task
  and LVGL off each other's state, briefly.
- **The XY pad redraws only what moved**: edge ticks, the cursor, five trail
  dots and three short labels, each set only when it changed (an LVGL style
  set invalidates its whole object even when the value is the same). Whole-pad
  crosshair lines and a trail polyline invalidated all of 312 x 120 a move --
  four of the display's 30-row SPI bands -- at a 33 ms refresh. The app sets
  the display refresh to 16 ms while it runs (`lv_display_get_refr_timer`)
  and puts LVGL's back on destroy; a drag measured 2.2 ms a refresh, ~325 px
  a flush (`LVGL_STATS`).
- **The XY pad owns a sideways drag** (`kDragsHorizontal`): without it
  crosspad-gui's screen-wide swipe took a quick finger run across the pad as
  back, which is KILL here, and every effect stopped mid-gesture.
- **USB REC** (ROUTE) switches USB to its audio profile (UAC2, class
  compliant: a phone or a computer records the USB output) and turns that
  output on; again, back to the data profile (CDC + MIDI). The profile
  persists like every other switch of it.
- Encoder: PAD → FX → EDIT → PAD on a click; FX picks an effect, EDIT a row,
  ROUTE a source's send. The power button's click is KILL.
- Kept in NVS (`fxpad` namespace): the sends, the outputs, PRE/POST, THRU,
  MONO, SYNC, the bank, each effect's release, curve and swapped axes.
- Soft keys: PAD -- HOLD, SYNC, WET (full wet instead of the pad's
  pressure), TAP, KILL; FX -- LATCH, BANK, CLEAR, MAIN FW (back to the full
  firmware in the other slot, `flowSwitchAndRestart()`); EDIT -- SWAP X/Y, LIN, EXP, SNAP,
  TAIL; ROUTE -- THRU, PRE (off = post-fader), MONO, USB REC, CLEAR.
- **SWAP X/Y moves the parameters, not their names**: the engine takes an
  effect's two parameters in its own order (`FxEngine::setSlot`), and the
  screen maps the finger to that order (`cp_fx_map()`, `FxDef.hpp`) -- a
  swapped effect takes the finger's Y first. Each parameter keeps its range,
  and the EXP curve stays on the effect's first parameter (a filter's CUTOFF)
  whichever axis it is on.
- **TAIL is there only for an effect that adds its wet signal to the dry**
  (`FxEngine::ringsOut()`: the delays, the reverbs, FREEZE, ECHO FRZ). The
  others are the signal itself and fade in ~30 ms whatever was asked, so on
  them EDIT shows SNAP and the TAIL key is dim.
