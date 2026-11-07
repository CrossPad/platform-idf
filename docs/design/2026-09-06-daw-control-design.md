# DAW Control — design

Date: 2026-09-06. Status: approved. Priorities: (1) USB audio
+ LED feedback now, (2) kit sync, (3) transport / step sequencer / clips later.

## Goal

CrossPad as a DAW controller: pads play the DAW's drum instrument, the DAW
paints the pads (instrument colours, hit feedback, beat), the LCD shows what
the DAW says, and the board's own UAC2 is the audio interface. First host is
FL Studio under Wine; Ableton follows on the same wire protocol.

## Constraints that shape it

| Fact | Consequence |
|---|---|
| FL and Live run their controller scripts in an embedded Python without pip; under Wine only USB MIDI reaches them, CDC does not | The device speaks one **generic SysEx protocol**; host scripts are self-contained files; anything needing CDC (kit sync) is a host-side `crosspad-hil` verb, not part of the DAW script. |
| Firmware must not know FL from Live | Protocol vocabulary is device-shaped (LED pad N, text row N, beat, transport bits, pad mode). No "pattern", "channel rack". |
| Apps are portable (core interfaces only) | The app uses `IMidiOutput` for notes/CC and a new `sendSysEx()` on it for replies; host SysEx arrives through `Stm32MessageHandler` like `0x1D` does. Both are core additions (MINOR bump). |
| Pad callbacks run on the pad thread, LVGL on its own | Pad logic records state atomically; an `lv_timer` in the app drains it for the screen. LEDs are set from the pad thread via `PadManager::setPadColor` (already thread-safe, like fishtank). |
| SysEx from the host arrives on the USB MIDI RX path | Handler runs on that task: it only writes pad colours and copies text into atomically-flagged buffers; never LVGL. |
| No new host repos | Host scripts live in the app repo under `host/<daw>/`; kit sync is `crosspad-hil` (`kits.py`, `kit push`). |

## Wire protocol: `F0 7D 1E <sub> … F7` (`CROSSPAD_CMD_DAW = 0x1E`)

All bytes 7-bit. Colours are sent as 3 bytes r g b each 0..127 (device
doubles them). Text is ASCII 0x20..0x7E, max 24 chars per row.

| sub | dir | payload | meaning |
|---|---|---|---|
| `0x01 hello` | host→dev | `daw_id` (1=FL, 2=Live, 0=other), `proto_ver` | host announces itself; device answers `hello` with `proto_ver, pads=16, rows=3, caps` (bit0 rgb, bit1 text, bit2 encoder, bit3 aftertouch) and starts showing "connected" |
| `0x01 hello` | dev→host | as above | reply; also sent unsolicited when the app starts, so a running host script re-syncs |
| `0x02 led` | host→dev | `pad r g b` | one pad's resting colour |
| `0x03 led_all` | host→dev | 16 × `r g b` | all pads at once |
| `0x04 text` | host→dev | `row` + chars | one LCD row (0..2); empty payload clears it |
| `0x05 beat` | host→dev | `0 off / 1 bar / 2 beat` | beat indicator (FL `OnUpdateBeatIndicator` values, Live sends the same) |
| `0x06 transport` | host→dev | bits: 0 playing, 1 recording, 2 loop | shown on the LCD |
| `0x07 mode` | host→dev | `0 pads / 1 steps / 2 clips` | what pads mean; v1 implements `pads`; others are accepted, echoed on screen, pads still send notes |
| `0x08 config` | host→dev | `base_note channel` | note layout the host wants (default 36 / ch 10) |
| `0x7F bye` | host→dev | – | host script unloading; device shows "no host" |

Device→host beyond `hello`: ordinary MIDI. Pads: NoteOn/NoteOff
`base_note + pad` on `channel`, velocity from the pad, aftertouch as
polyphonic key pressure. Encoder: CC 16 relative (65 = +1, 63 = −1) on the
same channel, encoder click CC 17 (127/0). The host script maps those.

Catalog of record: `crosspad-hil/crosspad_hil/knowledge/sysex.yaml`, section
`daw`. `SysExProtocol.h` and the host scripts carry the same numbers.

## Device: `components/crosspad-dawcontrol`

Generated from the crosspad-apps template (`idf.py app-new --app dawcontrol
--name "DAW Control"`, local, no remote yet), then rewritten:

- `DawControlPadLogic` (`IPadLogicHandler`): press → `sendNoteOn`, LED flash
  to a brighter shade of the pad's host colour (or a default palette when no
  host), release → `sendNoteOff` + restore, pressure → `sendAftertouch`.
  Holds `hostColor[16]`, `baseNote`, `channel` as atomics.
- `DawControlProtocol`: parses one SysEx payload (`sub, args, len`) into
  actions on the pad logic and a `DawScreenState` (3 rows, beat, transport,
  mode, daw_id, connected flag, `dirty` atomic). Pure functions, no LVGL,
  so it is the unit tested on the PC build.
- `DawControlApp.cpp`: LVGL screen (title bar with Back, "host: FL Studio /
  none", 3 text rows, transport line, beat dot), `lv_timer` 50 ms draining
  `dirty`; encoder catcher object in the app's group turning `LV_KEY_LEFT/
  RIGHT` and click into the CC above. Registers the SysEx callback on
  `getStm32MessageHandler().setDawCallback()` in create, clears it in
  destroy. On show sends `hello`; on hide/destroy clears pad colours.
- Settings: none persisted in v1 (base note / channel come from the host
  `config` sub or the defaults).

## Core changes (crosspad-core 1.5.0 → 1.6.0, MINOR)

- `SysExProtocol.h`: `CROSSPAD_CMD_DAW 0x1E`.
- `Stm32MessageHandler`: `DawCallback` + `setDawCallback()`, dispatch like
  `AUDIO_ROUTE` (pass `data+3, size-4`).
- `IMidiOutput`: `virtual void sendSysEx(const uint8_t* frame, size_t len) {}`
  (whole frame incl. F0/F7). `UsbMidiOutput` in `main/midiCallbacks.cpp`
  overrides it with `USB_MIDI.sendSysEx(len, frame, true)`. `getMidiOutput()`
  is the `MidiInputHandler` fan-out, so it overrides `sendSysEx` too and
  forwards to its USB and BLE outputs (not gated by the keypad send flags —
  found on hardware: without it the hello reply went nowhere).
- `PlatformServices::stm32MessageHandler`: the catalog decoder, published by
  the IDF platform so a portable app can hang `setDawCallback` on it without
  a platform include (null on the PC simulator).
- `main/include/crosspad_deps.h`: requires core 1.6.0.

## Host: FL Studio script (`host/fl_studio/device_CrossPad.py`)

`# name=CrossPad`. `OnInit` → hello; `OnDeInit` → bye. `OnMidiMsg`: notes
pass through untouched (FL routes them to the selected channel / FPC);
CC 16/17 → `ui.jog` / `transport.start`? — v1: CC16 selects the channel rack
channel (`channels.selectOneChannel`), CC17 toggles play; `event.handled` on
those. `OnRefresh` (and every 250 ms in `OnIdle` if names changed):
`text 0` = channel name, `text 1` = pattern name, `text 2` = BPM; pad
colours from `plugins.getPadInfo(chan, -1, 2, pad)` when the selected channel
hosts FPC, else the channel colour on every pad; `transport` bits.
`OnUpdateBeatIndicator(v)` → `beat v`. `OnSysEx`: device `hello` → resend
everything. Install path (Wine): `~/.wine/drive_c/users/<u>/Documents/
Image-Line/FL Studio/Settings/Hardware/CrossPad/device_CrossPad.py`, then
FL → MIDI settings → CrossPad port → controller type "CrossPad".

Ableton (`host/ableton/`, later): a Remote Script sending the same frames;
nothing on the device changes.

## Kit sync (`crosspad-hil` 1.1.0 → 1.2.0)

- `crosspad_hil/kits.py`: `build_kit(src_dir, out_dir, name, *, rate=44100)`
  → kit directory (`kit.json` + `SAMPLES/*.wav`): every `*.wav` in `src_dir`
  sorted by name → pads 0..15 (more than 16 = error, fewer = empty pads),
  converted to 16-bit PCM at the kit rate (stdlib `wave` + `audioop`-free
  linear resampling; stereo kept, mono duplicated). Pad colours from a fixed
  palette. `kit.json` in the shape `fixtures.write_sine_kit` writes.
- `crosspad_hil kit push <dir> [--name N] [--load]`: build into the work dir,
  `FS_MKDIR`, push files (`kit.json` last), reset through the console, wait
  for the scan, optionally `KIT_LOAD`. Reuses `scenarios/_audio` helpers
  (`_mkdir`, `_push_kit`, `_reboot_for_the_kit_scan`, `_wait_for_the_kit_scan`),
  which move to `kits.py` so a CLI verb does not import a scenario module.
- FPC preset (`.fst`) import: out of scope for v1.

## Test

- `crosspad-hil run daw_control`: `APP_START "DAW Control"`, hello over the
  ESP MIDI port → expect a hello reply with 16 pads; `led_all` with a known
  pattern → `LED_STATE` colours match (device doubles 7-bit values);
  `PAD_PRESS 3` → NoteOn 39 ch 10 seen on the host MIDI IN, `PAD_RELEASE`
  → NoteOff; `text 0 "HIL"` → `UI_STATE` (label text is not exposed, so the
  scenario checks the app is still running and hello still answers — text is
  verified by eye / the FL session). Exit 1 on any mismatch.
- Unit tests (pytest, fakes): `kits.build_kit` (rate conversion, pad count,
  kit.json shape), the SysEx builders/parsers in `midi.py`, the `kit push`
  CLI path with a fake serial, the scenario against a fake device + fake
  MIDI bus.
- Firmware: build v2.
