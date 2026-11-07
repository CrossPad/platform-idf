# The board ↔ host protocol for the browser twin and the live stage

The CrossPad mirrors what it does to a page on the host over USB MIDI SysEx:

- the **web twin** ([crosspad-web-twin](https://github.com/CrossPad/crosspad-web-twin)):
  this firmware compiled to WebAssembly, fed the board's inputs in lockstep, so
  the page shows the board's screen without sending a pixel;
- **mucha's live stage** ([mucha](https://github.com/CrossPad/mucha)): a 3D
  CrossPad with a fly, driven by the same inputs plus the IMU.

This page is the one description of that protocol. The code is the authority
where they disagree. Fix this page in the same commit.

| Side | Code |
|---|---|
| Board | `main/twin_mirror.cpp`, `main/motion_stream.cpp`, `main/companion_sysex.cpp`, the `TWIN_*` verbs in `main/hil_control.cpp`, `main/include/twin_state.h` |
| Host | crosspad-web-twin `web/boardlink.js`, crosspad-pc `src/wasm/wasm_bridge.cpp`, mucha `stage/live.js` |

## Versions: `TWIN_HELLO`

The twin is only right when it runs the sources the board was flashed from.
Two numbers say whether it does.

- **`proto`**: `twin_state::kProtocol` in `main/include/twin_state.h`. It goes
  up when a frame or a verb changes shape, so that the old reading is wrong.
  Additions do not count.
- **`src`**: the *source id*, 12 hex digits. It is a hash of the files that
  the board and the twin both compile into the screen. Both builds run
  `tools/twin_source_id.cmake` on every build:
  - the board's build bakes the id into the firmware;
  - crosspad-pc's build bakes it into the twin, against `CROSSPAD_FIRMWARE_DIR`.

  What goes into the hash:
  - the `crosspad-*` components, except `crosspad-platform-idf`;
  - `main/app/settings` and `main/app/update`;
  - `twin_state.h`;
  - the fonts;
  - the LVGL version;
  - `apps.json`.

  Only code and build files are hashed, and the files themselves rather than
  git, so the same sources give the same id from any clone. Board-only code
  (drivers, the mirror itself) is not in the hash: changing it needs no new
  twin.

`TWIN_HELLO` (request) answers:

```
TWIN_HELLO proto=1 src=9984cd8cab64 ver=v1.1.0-rc1-63-g98ec582-dirty
```

The twin carries the same pair (`wasm_twin_hello()`: `proto=1 src=…`). On
every lock, `boardlink.js` compares the two: on the first one, after a reboot
and after a replug. The result goes to `onHello({ board, twin, match })` and
to `link.hello()`.

- **`match: false`**: the twin keeps running, and says so:
  - `twin.html` puts it in its status line;
  - mucha shows it in its panel;
  - the lockstep test exits 2.
- **`match: null`**: a firmware or a twin from before `TWIN_HELLO`; it cannot
  tell.

To see why two ids differ, run the script on both checkouts with
`-DDUMP=<file>` and diff the two dumps.

## Framing

Everything is `F0 7D 1F <sub> … F7`: 7D is the non-commercial manufacturer
id, 1F the companion channel.

| sub | Direction | What |
|---|---|---|
| `01` | host → board | request: an ASCII verb, answered with `02`…`03` |
| `02` / `03` | board → host | reply, in chunks of up to 200 bytes; `03` is the last. The text ends in `\r\n` |
| `04` | host → board | notify: a verb never answered, not even with an error |
| `05` | board → host | touch (legacy mirror, before lockstep): `st x_hi x_lo y_hi y_lo` |
| `06` | board → host | power button (legacy): `1` pressed, `0` released |
| `09` | board → host | lockstep frames (below) |
| `0A` | board → host | motion: IMU, its events, OTA progress (below) |

**Replies carry no id.** A host matches a reply to its request by sending one
request at a time, which `boardlink.request()` does.
- A reply that comes after the host gave up answers the next request.
  `TWIN_FW` therefore gets 7 s (the card scan); everything else gets 1.5 s.
- Two pages talking to one board at once cross each other's replies. Use one
  page per board, or close the other one.

**The mailbox holds one request.** A request that finds it busy gets
`ERR busy`; a notify that finds it busy is dropped. The keepalives (`TWIN`,
`MOTION`) are therefore handled on the MIDI task, outside the mailbox.

## Keepalives (notify `04`)

| Verb | Effect | Lapses |
|---|---|---|
| `TWIN 1` / `TWIN 0` | the board sends `09` lockstep frames | 6 s after the last `TWIN 1` |
| `MOTION 1` / `MOTION 0` | the board sends `0A` motion frames | 6 s after the last `MOTION 1` |

The host repeats them every 2 s. `0` stops at once. No host means no cost.

## `09`: lockstep frames

```
F0 7D 1F 09 <kind> <t3 t2 t1 t0> <payload> F7      t = lv_tick_get(), 4 x 7 bit
```

| kind | What | Payload |
|---|---|---|
| 0 | touch, one LVGL read | state, x hi, x lo, y hi, y lo |
| 1 | encoder, one LVGL read | state, diff + 64 |
| 2 | pad as it enters the PadManager | pad (0–15), velocity (0 = release) |
| 3 | power button, the resolved gesture | 0 click (back), 1 hold (quick settings) |
| 4 | a MIDI message from the USB host, as its handler got it | 8-bit bytes in 7-bit groups: one byte of high bits, then up to 7 low bytes |
| 5 | clock | nothing before this tick is still to come |
| 6 | lost | the board's ring (256 events) overflowed, or an input it will not mirror (below): resync |
| 8 | status | what the status bar and the host rows show that no input decides (`twin_mirror.h`) |

The twin never runs its LVGL past a tick the board has vouched for. Every
time-dependent decision (a long press, the kit list's acceleration) therefore
comes out the same.

Frames carry no sequence number, so a frame lost on USB is caught only by the
safety net below.

**A text prompt's keyboard is never mirrored.** A touch that starts while
the keyboard (the WiFi password) is up, and every encoder step or press on
it, goes out as a `6` lost frame instead of its coordinates or diff; the twin
resyncs from `TWIN_STATE`, which carries no text. `touch_trace_text_entry_open()`
is the one check the touch mirror and the twin mirror share.

The pad frames are also the exact pad and velocity for the live stage. Note
numbers depend on the kit's note map.

## `0A`: motion

| kind | Frame | What |
|---|---|---|
| 0 | `F0 7D 1F 0A 00 <t3..t0> <8 × (hi lo)> F7` | an IMU sample, ~50 Hz |
| 1 | `F0 7D 1F 0A 01 <t3..t0> <ev lo> <ev hi> <d6d> F7` | what the IMU recognised (`bsp_imu_event_t` bits: tap, double tap, free fall, …) |
| 2 | `F0 7D 1F 0A 02 <t3..t0> <phase> <percent> F7` | an OTA over CDC: 1 receiving, 2 written (restarting), 3 failed. On every change, at most 10 a second |

- `t` is `esp_timer` milliseconds, 4 × 7 bit.
- A sample's eight values are 14 bit each, stored as `v + 8192`, high 7 bits
  first:
  - accel x, y, z in mg (the sensor's axes; it sits upside down under pad 1);
  - gyro x, y, z in dps × 4;
  - pitch and roll in degrees × 40 (the fused attitude).

## Request verbs the twin and the stage use

All of them are on the companion allowlist (`hil_control_companion_handle`)
and on CDC. **The twin and the motion stream are a USB host's:** over BLE MIDI
`TWIN`, `TWIN_*` and `MOTION` answer `ERR twin_usb_only`, over the WiFi remote
`ERR not_remote` -- a peer there must not switch the mirror on, and the frames
go out on USB MIDI only. `TWIN_FW L 0` starts one card scan at a time; while
one runs it answers `ERR TWIN_FW busy`.

| Verb | Reply |
|---|---|
| `TWIN_HELLO` | `TWIN_HELLO proto=<n> src=<id> ver=<app version>` (above) |
| `TWIN_STATE` | `TWIN_STATE tick=… app=… cfg=… hash=… geo=… ovl=… focus=… scroll=… lvl=… st=… ar=… sc=… host=… fwl=… song=… set=… ble=… imu=…`: the safety net (below) |
| `TWIN_SETTINGS <offset>` | `TWIN_SETTINGS <total> <offset> <text>`: the settings as NVS holds them, 400 bytes at a time |
| `TWIN_INFO P` / `C` / `<i>` | Settings → Info's board-only rows: the platform, the STM32 firmware, then row i of n |
| `TWIN_FW M` / `S 0` / `S 1` / `L <i>` | the Firmware app: mode, the two slots, row i of the card's library. `L 0` scans the card first, which takes up to 5 s |
| `LED_STATE` | the pad LEDs as driven, `bri=` the strip brightness |
| `PAD_NOTES` | `PADNOTES: 0:<note> 1:<note> …` (a kit redefines them) |
| `UI_STATE` | the running app (for a firmware without lockstep) |

## The safety net: `TWIN_STATE`

Every second, and at once when a `09` status frame arrives, the twin asks
`TWIN_STATE` and compares it with its own line (`wasm_twin_state()`). Both
sides build the screen part of the line with the same code
(`twin_state.h describe()`):

- the object tree under the LCD root: classes, the hidden flag, child counts,
  never text;
- the encoder group's focus;
- scroll offsets;
- the overlay layer.

The fields:

| Field | What |
|---|---|
| `hash` | structure |
| `geo` | coordinates, compared only on a still screen |
| `ovl` | the overlay layer |
| `cfg` | the settings the screen is drawn from |
| `set` | a hash of all settings |
| the rest | state that no input decides: levels, telemetry, audio route, the song open |

A difference that holds over two still checks with no input in between is put
right, in this order:
1. the settings (`TWIN_SETTINGS`);
2. the app;
3. the focus and scroll;
4. reopening.

The twin gives up after the same fix twice (`gaveUp` in `wasm_twin_stats()`).

**The line's budget is 470 bytes:** the companion reply holds 480, and the
line ends in `\r\n`. On a deep screen it has been close.
- New fields go at the end.
- Count the worst case before adding one.
- The board logs `TWIN_STATE cut at N bytes` when snprintf cut the line; the
  tail fields are then missing on that check.

## Changing the protocol

- **Additive and backward-compatible:** a new frame kind, a new verb, or a new
  field at the end of a line. A reader skips what it does not know. Do not
  bump `kProtocol`.
- **Anything else** (a payload reordered, a field's meaning changed): bump
  `kProtocol`, and update this page and both readers in the same change.
- A change to `twin_state.h` changes the source id. The board needs a reflash
  and the twin a rebuild.
