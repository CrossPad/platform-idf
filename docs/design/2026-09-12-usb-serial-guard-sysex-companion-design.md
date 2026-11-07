# USB serial guard + companion control over SysEx

**Date:** 2026-09-12 · **Status:** approved in chat, implementing

## Problem

The default USB profile (MIDI + UAC2, what a DAW user runs) has no CDC
endpoint, so the browser companion (`/device`, Web Serial) cannot see the
board. Getting kits onto the card means switching to the CDC profile, and
today the only way to ask for that is SysEx `F0 7D 1B 01 F7` — which any
host the board is plugged into can send, unattended.

## Goals

1. Companion works from the audio profile: sees the board over Web MIDI,
   can switch it to USB serial for bulk work and back to audio afterwards.
2. Light control (remote pads/encoder/apps, `STM_FW_INFO`, `KIT_*`,
   `SETTINGS_STATUS`) works **without** leaving the audio profile, next to a
   running DAW.
3. A host-originated switch to the CDC profile is confirmed **on the
   device**. Unknown computers do not get the console/OTA/file plane.

Non-goals: files or OTA over SysEx (7-bit MIDI at USB full speed is minutes
per megabyte — the switch exists for that), authenticating hosts.

## Design

### 1. USB serial guard (firmware)

- New setting `CrosspadSettings::usbSerialGuard` (core 1.12.0, NVS
  `cfg_audio/usb_guard`, default **on**, in the JSON backup).
- SysEx `0x1B 0x01` (host asks for CDC) with the guard on raises a modal on
  the device: *"USB serial mode? A computer asks for USB serial (files,
  updates, console). Allow only computers you trust."* — **Allow** /
  **Deny**, encoder-navigable, power click = Deny, 30 s → Deny. One prompt
  at a time; repeats while it is up are dropped. Allow →
  `usb_config_manager_set_mode(USB_MODE_DEFAULT)`.
- `0x1B 0x02` (to audio, the safe profile), the Settings dropdown and CDC
  `USB_DEFAULT` never prompt.
- CDC `USB_GUARD` / `USB_GUARD 0|1` reads / persists the setting.
  Settings → USB gets a switch "Ask before USB serial" (gui 1.8.0).
- Development and factory: a development board is set `USB_GUARD 0` once
  (persisted). `crosspad-hil usb-mode set default` waits 45 s (prompt 30 s +
  enumeration) and its timeout hint names the prompt.

### 2. Companion control over SysEx (firmware)

- `CROSSPAD_CMD_COMPANION 0x1F` in `SysExProtocol.h`;
  `Stm32MessageHandler::setCompanionCallback` (same shape as
  `AudioRouteCallback`).
- Request `F0 7D 1F 01 <ASCII line> F7`: one CDC verb, printable ASCII,
  ≤ 190 bytes (device `SysExMaxSize` is 256).
- Reply chunks `F0 7D 1F 02 <text> F7` (more follow) …
  `F0 7D 1F 03 <text> F7` (last). ≤ 200 payload bytes per chunk; the host
  concatenates. Text is the CDC reply verbatim (`\r\n` included).
- Same `BleControlMailbox` class as BLE, second instance in
  `main/companion_sysex.cpp`; same allowlist and dispatch
  (`hil_control_companion_handle()`, shared with `hil_control_ble_poll`):
  `CP_CAPS`, `STM_FW_INFO`, `UI_STATE`, `APP_LIST`, `LED_STATE`,
  `ENC_PRESS[ ms]`, `ENC_ROTATE n`, `APP_STOP`, `APP_START x`,
  `PAD_PRESS/RELEASE`, plus `KIT_LIST`, `KIT_LOAD n`, `KIT_STATUS`,
  `SETTINGS_STATUS`, `USB_GUARD` (read only). Anything else →
  `ERR unsupported_companion_command`. A request while one is in flight →
  `ERR busy` at once.
- Threads: RX callback (MIDI task `uartLoop`) → mailbox; main loop executes
  (`companion_sysex_poll()` beside `hil_control_ble_poll()`, woken through
  `app_queue`); the MIDI task sends the reply chunks (`companion_sysex_pump()`
  before it drains RX) so USB MIDI TX stays single-writer, then resets the
  mailbox.

### 3. Browser companion (crosspad `/device`)

- `app/lib/device/midi.ts`: `midiApi()`, `openMidi()` (Web MIDI with
  sysex; the ESP port is the one whose name contains "crosspad" and not
  "MIDI+Serial" — the STM bridge), `createMidiControl(send, subscribe)`
  → `Remote.command(verb)` with chunk reassembly and a 15 s timeout,
  `requestSerialMode()` (`F0 7D 1B 01 F7`), `requestAudioMode()`.
- Page: **Connect MIDI** (auto-reconnect when permission exists),
  status *"MIDI: <name> — audio profile, no USB serial"*, **Switch to USB
  serial** (sends the request, says *"Confirm on the device"*, polls
  `getPorts()` for the CDC port up to 45 s, connects Web Serial),
  **Back to MIDI + Audio** when on serial (CDC `USB_AUDIO`, expects the
  disconnect). A note that the DAW loses its MIDI input during the switch
  (FL under Wine: relink with `aconnect`). `RemotePanel` gets the MIDI
  `Remote` like the BLE one.
- Tests: unit (fake `MIDIAccess`: frame layout, reassembly, timeout, busy,
  port selection, switch flow with the CDC port appearing after N polls,
  guard timeout message), simulated Playwright e2e, hardware e2e
  (`CROSSPAD_HW_MIDI=1`: audio → switch → serial → back to audio; guard
  prompt path with `USB_GUARD 1` + `ENC_PRESS` … no — the prompt path is
  verified by hand, the guard must not be scriptable from the host).

### 4. Host tooling

- crosspad-hil: `knowledge/sysex.yaml` `companion:` section, `midi.py`
  builders/parser, CLI `crosspad-hil companion "<VERB>"`, `usbmode.py`
  45 s + hint, knowledge test. Verifies the firmware without a browser.
- Docs: `docs/subsystems/ble-midi.md` (rename section to "Companion
  control": BLE and SysEx fronts), `hil-testing.md` traps (guard), CLAUDE.md
  summary, crosspad `docs/companion.md`.

## Risks

- A guard on by default changes test behaviour: every unattended
  `usb-mode set default` on a fresh board stops at the prompt. Mitigated by
  the persisted `USB_GUARD 0` for development boards, the hint, and the factory
  script step naming it.
- USB MIDI TX from the main loop is avoided by design (see threads).
