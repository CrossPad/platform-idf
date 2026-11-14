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

Sixteen, in pad order (pad 1 bottom left), all running at once if asked, in
the order they were started:

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
| FLANGER | SPEED (Hz) | DEPTH | tail |
| PHASER | SPEED (Hz) | FDBK | tail |
| BITCRUSH | BITS | RATE | snap |
| DRIVE | GAIN (0-36 dB) | TONE | snap |

TIME and RATE are note values with SYNC (1/32 .. 1/2, the transport's tempo,
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
- Memory: ~3 MB of PSRAM (three 2 s stereo delays, the roll ring, four small
  reverbs), taken once when the processor is attached.

## The app

- **The pads start an effect from their own task**, straight into the
  engine's atomics (`cp_fxpad_pad()`): on within the block after the hit, not
  after a screen refresh. The screen and the pad LEDs follow from a 30 ms
  LVGL timer (`cp_fxpad_tick()`); one recursive mutex keeps the pads' task
  and LVGL off each other's state, briefly.
- Encoder: PAD → FX → EDIT → PAD on a click; FX picks an effect, EDIT a row,
  ROUTE a source's send. The power button's click is KILL.
- Kept in NVS (`fxpad` namespace): the sends, the outputs, PRE/POST, THRU,
  MONO, SYNC, each effect's release and curve.
- Soft keys: PAD -- HOLD, SYNC, WET (full wet instead of the pad's
  pressure), TAP, KILL; FX -- LATCH, CLEAR, MAIN FW (back to the full
  firmware in the other slot, `flowSwitchAndRestart()`); EDIT -- SWAP X/Y, LIN, EXP, SNAP,
  TAIL; ROUTE -- THRU, PRE, POST, MONO, CLEAR.
