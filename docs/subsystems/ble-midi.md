# BLE MIDI

> Source of truth for this subsystem. `CLAUDE.md` carries only the summary; when behaviour changes, update this file and the summary together.

NimBLE, wired to the same `MidiInputHandler` router every other transport uses.
`components/crosspad-platform-idf/EspBleMidi.{hpp,cpp}` implements
crosspad-core's `IBleMidi`; `main/midiCallbacks.cpp` owns the wiring and the
`bleMidi_start/stop/boot` entry points.

- **Both roles.** Server (peripheral) advertises the BLE MIDI service and is
  what a PC, phone or DAW connects *to* — the default. Host (central) scans for
  BLE MIDI peripherals and subscribes. `Settings → BLE MIDI` switches live; the
  choice persists (`CrosspadSettings::wireless.bleMidiMode`).
- **Inbound notes play the instrument.** A note the pad grid owns
  (`PadManager::getPadForMidiNote`, and a kit redefines those) goes through
  `handlePadPress()` — the same entry point a physical pad and the STM32 bridge
  use, so whichever app owns pad logic sounds it. Anything outside the grid goes
  to `MidiInputHandler`'s external-input path (LEDs, EventBus, BLE2Midi thru).
- **Outbound is routed, not hardcoded**: pad hits reach BLE through
  `keypad.send2BLE`, USB-MIDI thru through `esp32Routing.Midi2BLE`.
- **Send transpose.** BLE is the output most likely to face a host with its own
  note map, so it has its own transpose: `Settings → BLE MIDI → Send transpose`,
  a picklist from `crosspad/midi/BleMidiOffsets.hpp` (one table shared by the UI
  and the transport, so a label cannot select a different value than it names).
  Default sends exactly what USB MIDI is sending at that moment; the other
  entries are octaves and the drum-rack bases — `+36` puts a grid that starts at
  note 0 on C1, where Ableton's Drum Rack and FL's FPC expect it. The shift is
  applied in `EspBleMidi::sendNote*`/`sendAftertouch`, and a note that would
  leave 0..127 is dropped rather than wrapped (it shows up as `tx_drop`).
  Inbound has its own separate offset (`wireless.bleMidiNoteOffset`).
- **Status bar.** The bar's Bluetooth glyph, next to battery and SD, follows
  `status.bluetoothConnected`, which `midiCallbacks.cpp` sets from the
  connection callback, so the glyph appears when a peer is connected — one
  icon, in the system row, rather than a second one in the app-icon zone.
- **Sending never blocks the caller.** `sendNoteOn()` &c. push onto a queue that
  a small `ble_midi_tx` task drains, coalescing everything that piled up during
  the last connection interval into one packet with 13-bit timestamps. A pad hit
  costs one queue send whether BLE is connected, idle, or compiled out.
- **Compiled out cleanly**: the whole file falls back to a no-op stub when
  `CONFIG_BT_NIMBLE_ENABLED` is off, and `EspBleMidi::isSupported()` reports it
  (the Settings category then never registers).

### What it costs, and what had to move

The BT stack takes **~40 kB of internal RAM while running** plus ~36 kB of
static DRAM, on a board that had ~76 kB free. Three things follow, and all three
are load-bearing:

- **BLE starts before the display, LVGL and audio** (`main.cpp`, right after
  `midiCallbacks_init()`). The controller needs one large contiguous DRAM block;
  started last it fails with `BLE_INIT: Malloc failed` and then asserts in
  `emi.c` — a boot loop. Everything after it asks for small blocks, which fit
  around it.
- **USB-audio mode's two UAC tasks keep their stacks in PSRAM**
  (`components/usb_device_uac/usb_device_uac.c`). They are copy loops between
  TinyUSB FIFOs and the I2S bridge — nothing about them needs internal RAM, and
  with BLE running two 4 kB internal stacks make entering audio mode fail
  (`Failed to create usb_spk task`).
- **LittleFS allocates from PSRAM** and `SPIRAM_MALLOC_ALWAYSINTERNAL` is 128,
  so only genuinely small allocations stay internal.

Measure rather than guess: `MEM` reports free/largest/min internal and PSRAM,
`MEM_BLOCKS` gives the block-size histogram plus every block over 8 kB. "18 kB
free but the largest block is 4 kB" is a different problem from "out of memory",
and only the histogram tells them apart.

**With BLE up, the rest of the board is short of internal RAM.** NimBLE up
leaves ~9–12 kB internal free at boot, and the next allocations fail: I2S DMA
buffers, then `sdmmc` read buffers. Measured A/B on one board, one commit
(2026-08-31): **non-BLE 72.5 kB internal free early in boot, codecs up, USB
audio OK; BLE 9.4 kB free, no audio, no UAC2.** (The launcher after a full
boot -- kit engine, LVGL, codecs -- sat at ~39.5 kB internal free; use `MEM`
at the launcher as the reference figure.) So **BLE is off by default** — off
at runtime, not compiled out: NimBLE stays in the image (`CONFIG_BT_ENABLED=y`
on both `sdkconfig.v1` and `sdkconfig.v2`) and the `wireless.enableBLE`
setting defaults to off, so the stack never starts until it is switched on
(Settings or `BLE_ENABLE 1`). NimBLE footprint reduction (controller
`BT_CTRL_BLE_MAX_ACT`, ACL/ADV buffer sizes, `BT_NIMBLE_MAX_CONNECTIONS=1`,
`NIMBLE_MEM_ALLOC_MODE_EXTERNAL`) is not done yet.

### Off-device control

`BLE_ENABLE 0|1` (persists `wireless.enableBLE` and applies it — a bench
board goes back to the default with `crosspad-hil ble enable off`;
`BLE_START`/`BLE_STOP` are runtime only), `BLE_START [0|1]`, `BLE_STOP`, `BLE_STATUS`, `BLE_SCAN`, `BLE_DEVICES`,
`BLE_CONNECT <addr>`, `BLE_DISCONNECT`, `BLE_SEND <note> [vel]`,
`BLE_TXOFF <semitones>` (transport only, not persisted) over CDC. `UI_STATE`
reports the settings that decide what the screen shows (drawer arrow,
brightness, theme, running app) — "the arrow is gone" is a setting as often as
it is a bug.
`BLE_STATUS` reports state, role, peer, negotiated interval and the tx/rx
counters — enough to tell a dead radio from an unrouted pad hit. `PAD_NOTES`
dumps the note→pad map, which anything driving the device with musical MIDI
needs, since a kit redefines it.

The status bar is rebuilt on every return to the launcher, so no file-static
handle may outlive the tree it points into. `lv_CreateStatusBar` clears the
drawer handles (a stale `openButton`, the quick-settings drawer arrow, makes
`create_open_button()` return early on a dangling pointer and the arrow
disappears after the first app exit) and re-creates registered icons
(`statusbar_add_icon`'s slot table would otherwise write through labels that
died with the bar). Encoder telemetry in the icon zone (`indev/encoder.h`) is
off unless `CROSSPAD_ENCODER_DEBUG=1` — it shares the zone with real status
icons and overlaps the centred logo.

Verified on hardware (2026-08-20, kit DRUMS, 15 ms connection interval): 181 of
181 notes sent over BLE reached the sample engine while the same USB cable
streamed the audio back; device→host latency 6–30 ms, median ~18 ms;
`crosspad-hil run ble_midi --kit 8 --host-mode` 15/15 checks, both roles.

Host mode needs something to connect *to*: on Windows the test script
advertises a BLE MIDI peripheral from the host radio itself
(`--host-mode`, WinRT GATT server), so the central path is exercised without a
second device.

## On a Linux host, BlueZ takes the service — test through ALSA, not bleak

bluetoothd (5.72) ships a MIDI GATT driver: when a device with the BLE
MIDI service connects, it claims the service itself and creates an ALSA
sequencer client (`'CrossPad' → 'CrossPad Bluetooth'`, pid = bluetoothd).
A service claimed by an internal profile is **not exported over D-Bus**, so
bleak sees only GATT 0x1801 and `start_notify` fails with
`BleakCharacteristicNotFoundError` — while `gatttool --characteristics` shows
the MIDI characteristic at 0x000f. That is not a firmware fault. To check:
`bluetoothctl connect <board address>`, then `aseqdump -p <client>:0`
prints the pad's notes (`tx_msg` counts them on the device) and a note sent
to that port with rtmidi bumps `rx_msg`. FL Studio under Wine sees the port
like any ALSA MIDI input. The `ble_midi` scenario must therefore drive the
link through ALSA (rtmidi, which the package already uses), with bluetoothctl
doing the connect; bleak stays useful only on a host without the driver.


## Browser companion control (protocol 1)

The peripheral also exposes service `7e400001-b5a3-f393-e0a9-e50e24dcca9e`.
The advertisement keeps the BLE MIDI UUID to retain MIDI discovery; Web Bluetooth
requests the control UUID through `optionalServices`. This works directly between
the browser and ESP32, without a host application or crosspad-hil daemon.

- RX `7e400002-…`: acknowledged GATT writes, printable ASCII command terminated
  by LF, at most 190 characters. Browsers split writes into 20-byte chunks for
  the default ATT MTU. One command may be pending at a time.
- Reply `7e400003-…`: GATT read/long-read. Byte 0 is `0` while pending, or `1`
  followed by the complete text response (at most 480 bytes). A new request
  clears the old response before its last chunk is accepted. The client polls
  with a 15-second deadline and disconnects on timeout to discard stale state.
- `CP_CAPS` returns `CP_CAPS 1 control=1 files=0`. The allowlist
  (`hil_control_companion_handle()`, shared with the SysEx front below) is
  `UI_STATE`, `STM_FW_INFO`, `APP_LIST`, `APP_START <name>`, `APP_STOP`,
  `PAD_PRESS`, `PAD_RELEASE`, `ENC_ROTATE <n>`, `ENC_PRESS [ms]`, `LED_STATE`,
  `KIT_LOAD <n>`, `KIT_STATUS`, `SETTINGS_STATUS`, `USB_GUARD` (read
  only), `FW_CHECK` (starts a catalog check; `FW_DOWNLOAD` stays CDC-only),
  and `HOST_INFO` / `FW_LATEST` (a host reporting in — see below) —
  what the companion's remote panel needs. **Over BLE, `FW_CHECK` stops
  BLE:** the check leases WiFi and BLE gives way while WiFi runs, so the link
  may drop while the check runs (the reply `OK` comes first). Over SysEx on
  USB nothing drops. Files, OTA, settings
  writes and arbitrary HIL verbs answer `ERR unsupported_companion_command`.
  Every allowed verb replies synchronously from the handler and
  in one line; a multi-line verb such as `APP_VERSIONS` or `KIT_LIST` would
  overflow the 480-byte reply, so it stays CDC-only. While the USB serial
  guard prompt is on the screen, `ENC_*` answers `ERR guard_prompt_active`:
  the host that asked for the serial profile must not be the one that
  presses Allow.
- GATT callbacks only copy into a bounded mailbox. `hil_control_ble_poll()` runs
  on the existing main command loop, reuses HIL handlers, and captures replies
  only from that task. Concurrent CDC and asynchronous HIL replies keep their
  USB destination. Disconnect increments a generation counter so a completed
  old request cannot leak to a new peer. A completed write (the chunk ending
  in LF) calls the wake registered with `bleCompanionSetWake()`: `main.cpp`
  posts an empty message to `app_queue`, so the loop answers at once instead
  of on its periodic pass (1 s, 10 s while an OTA or file stream is active).
- No extra task or independent Bluetooth stack is created. Enable BLE in
  peripheral/server mode using the existing device setting. This service uses
  the existing BLE connection policy; enable BLE only when companion access is
  intended. No erase/flash/settings-reset commands are permitted by this API.

The browser companion negotiates CP_CAPS and shows status,
Start Sampler and Stop app controls. Firmware without the service stays usable
as BLE MIDI, with an explicit firmware-upgrade message.

Validation: host C++ mailbox tests (fragmentation, overflow, concurrent requests,
disconnect generation); companion-side ATT chunking/timeout/injection tests;
Chromium simulated GATT control test. On a v2 board (2026-09-11, bleak
from the host's radio, MTU 23): `CP_CAPS` 240 ms on the
first exchange, then `UI_STATE`/`APP_START`/`APP_STOP`/`PAD_*` 55–65 ms,
`STM_FW_INFO` 135 ms, `APP_LIST` 111 ms; `MEM` and a 189-character command
answer `ERR unsupported_companion_command`, a 200-byte write is refused at the ATT
level. `hil_control_ble_poll()` keeps its buffers static: 673 bytes of them
on the main task's stack, above a handler that reads a file through LittleFS
(`STM_FW_INFO`), overflow it (a StackCanary in `main`). `COREDUMP_INFO` is the
check after any change on this path.

## Companion control over SysEx (0x1F) and the USB serial guard

The same verb set, over USB MIDI, for the profile a DAW user actually runs:
MIDI + UAC2 has no CDC endpoint, but its MIDI port is there in both profiles.
Spec: `docs/design/2026-09-12-usb-serial-guard-sysex-companion-design.md`;
catalog of record: crosspad-hil `knowledge/sysex.yaml` `companion`.

- Request `F0 7D 1F 01 <ASCII verb line> F7`, at most 190 bytes (the device's
  `SysExMaxSize` is 256). Reply `F0 7D 1F 02 <text> F7` chunks with `03` on
  the last one, ≤ 200 payload bytes each, the CDC reply text verbatim
  (`\r\n` included; a byte ≥ 0x80 becomes `?`). A second request while one is
  in flight answers `ERR busy` at once; a malformed one `ERR bad_request`.
- Threads (`main/companion_sysex.cpp`): the MIDI task's SysEx callback fills a
  second `BleControlMailbox` and wakes the main loop through `app_queue`; the
  main loop executes the verb (`companion_sysex_poll()` beside
  `hil_control_ble_poll()`); the MIDI task sends the chunks
  (`companion_sysex_pump()` at the top of its loop, before it drains RX) so
  USB MIDI TX keeps one writer and a finished reply always leaves before the
  next request can overwrite it. Static buffers, as on the BLE path.
- Host side: `crosspad-hil companion "<VERB ...>"` (works in both profiles),
  and the browser page's **Connect MIDI** (Web MIDI with sysex; the port whose
  name contains "crosspad" and not "MIDI+Serial" or "Hub").

**USB serial guard.** SysEx `0x1B 0x01` is any host's way to ask for the CDC
profile — the console, OTA and file plane. With `CrosspadSettings::usbSerialGuard`
on (the default; Settings → USB → "Ask before USB serial"; CDC `USB_GUARD [0|1]`,
persisted) the request raises `ui_modal_confirm` on the device
(`main/gui/usb_serial_prompt.cpp`): **Allow** switches, **Deny**, the power
click and 30 s do nothing, one prompt at a time. A request that arrives while
the board is already on CDC, a request for the audio profile (`0x1B 0x02`),
the Settings dropdown and CDC `USB_DEFAULT` never prompt; before the launcher
is up the request is dropped and logged. The modal's timeout goes through
`ui_modal_confirm_dismiss()` (gui 1.8.0) so every exit runs the same teardown.
`crosspad-hil usb-mode set default` waits 45 s for that reason and its timeout
hint names the prompt. **A development board is set `USB_GUARD 0` once** — the
factory script and every unattended `bench.py ready` depend on it.

Validation (2026-09-12, bench, core 1.12.0 / gui 1.8.0): the fourteen
allowed verbs over `crosspad-hil companion` in the CDC profile and again in the
audio profile (`CP_CAPS`, `UI_STATE`, `APP_LIST`, `LED_STATE` 130 B, `STM_FW_INFO`,
`KIT_STATUS`, `SETTINGS_STATUS`, `USB_GUARD`, `ENC_ROTATE 1`, `PAD_PRESS 3 100`,
`PAD_RELEASE 3`, `APP_START Sampler`, `APP_STOP`, `KIT_LOAD 0`); `USB_GUARD 0`
and `FS_LIST /sdcard` refused; unguarded `usb-mode set default` from audio in
2 s. Guard on, request from audio: prompt on the LCD, `companion ENC_PRESS`
= `ERR guard_prompt_active`, `UI_STATE` still answers, a repeat request is
absorbed, `usb_serial_prompt: USB serial request timed out` after 30 s on the
console, profile still audio. Browser: simulated Playwright run of connect →
remote → switch → back, and the hardware run (`CROSSPAD_HW_MIDI=1`, 10.7 s)
— see crosspad `docs/companion.md`.

## A host reporting in: HOST_INFO and FW_LATEST

The companion verbs also carry the other direction's news — what the computer
on the other end of the cable is doing. No protocol of its own: two allowed
verbs (`main/host_info.cpp`), on all three fronts.

- `HOST_INFO cpu=<pct> ram=<used_mb>/<total_mb> time=<unix_s> tz=<min_east>`,
  every 2 s. Sets `CrosspadStatus::hostCpuPct`/`hostRamUsedMb`/`hostRamTotalMb`
  and `hostSeenUs`, and the board clock (`settimeofday` only past 2 s of drift,
  `TZ` as `UTC∓hh:mm`). `hostAlive()` holds for 6 s — three missed reports.
  Optional tail `daws=<installed>/<running>`: bitmasks by DAW-protocol
  `daw_id` (bit 1 FL Studio, 2 Ableton, 3 REAPER) of the DAWs the host can
  start and of those running → `hostDawsInstalled`/`hostDawsRunning`; absent =
  0/0. DAW Control offers a launch button for each installed, not running DAW
  with no script answering; the press goes back as DAW SysEx `0C launch
  <daw_id>`, which the hub consumes (`crosspad_hil/daw_launcher.py`: Wine DAWs
  through their prefix's runner — `bottles-cli` for a bottle — REAPER native,
  nothing without `DISPLAY`/`WAYLAND_DISPLAY`, one start per DAW per 30 s).
- `FW_LATEST <x.y.z>`: the newest firmware the host knows of. Persisted in NVS
  (`host_info` / `fw_latest`, loaded by `host_info_init()` at boot) and compared
  with `esp_app_get_description()->version` (a describe string
  `v1.0.1-284-g…` compares as 1.0.1). `fwUpdateAvailable` survives a reset
  with no host attached.
- Bare `HOST_INFO` / `FW_LATEST` only report: `HOST: alive=… age_ms=… cpu=…
  ram=U/T time=… daws=I/R`, `FW_LATEST: latest=… running=… update=0|1`.
- Where it shows: the status bar shows `LV_SYMBOL_USB` beside the charging
  bolt while the host is alive (orange when an update waits); the two are
  separate icons, so a charging board shows both;
  DAW Control has a host row (CPU, RAM, the board's clock) and a firmware row;
  Settings → Info adds "Update: vX at crosspad.app/device" whenever
  `fwUpdateAvailable` is set.
- Stack: this runs on the main task's 5 kB stack when it comes in over SysEx
  or BLE, so the reply text is built in `noinline` helpers, not in the frame
  every CDC verb passes through.

Sender today: the crosspad-hil MIDI hub (`crosspad_hil/midi_hub.py`,
`host_report.py`), which already holds the board's port from login for FL
under Wine. While a board is attached it sends `HOST_INFO` every 2 s and
`FW_LATEST` every minute, one frame per 1 s poll, as **companion notify**
frames: `F0 7D 1F 04 <verb> F7` runs the verb and never answers — not even
an error; a notify that finds the mailbox busy is dropped. Replies go to every
listener of the port, so a periodic sender answering `0x01` requests would
land its `HOST:` rows in a browser's or crosspad-hil's own exchange. The
newest version comes from `~/.config/crosspad/latest.json` until the release
endpoint beside the release bot serves the same `{"version": "x.y.z"}`.
Bench (2026-09-12): hub restarted with the reporter, `HOST: alive=1
age_ms=89`; ten `crosspad-hil companion UI_STATE` during the reports all
answered `UI:`, none `ERR busy` or a stray `HOST:`.
Bench (2026-09-26): `daws=10/0` (FL in Bottles + REAPER), DAW Control showed
two buttons; FL pressed by encoder → hub `launched FL Studio` → `daws=10/2`
and `DAW_STATE connected=1 daw=1` 22 s after the press.

