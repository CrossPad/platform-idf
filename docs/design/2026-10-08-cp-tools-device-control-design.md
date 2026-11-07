# CP Tools: device control and tracers, through the hub

Date: 2026-10-08. Repos: crosspad-hil, crosspad-mcp, crosspad-apps (CP Tools
core), platform-idf (this spec; its CP Tools copy refreshes itself).

## Goal

From CP Tools (`idf.py app-manage`, `crosspad_app_manager.py`), without a
browser:

- control the board's ESP the way crosspad.app `/device` does: pads, encoder,
  apps, files, settings backup, USB profile, WiFi, BLE;
- run the ESP traces (`HEAP_TRACE`, `TOUCH_TRACE`);
- start the existing STM32 SWD tracer (crosspad-mcp `tracer/`), a shortcut
  only.

Hard requirement: **every access respects who already holds the board.**
Nothing opens a port behind another client's back; a busy board says who
holds it, for what and since when. The MIDI hub becomes the arbiter.

## Today

- CP Tools talks to the board with its own pyserial (`AppManager._cdc_exchange`,
  `APP_VERSIONS`), no lock, no hub.
- crosspad-hil owns device I/O by decision (2026-08-25): `CdcLink` per process
  under a `PortLock` file (`locks.py`, pid + purpose). Two processes cannot
  share the CDC; the second gets `PORT_BUSY`.
- `midi_hub` (systemd --user, Linux) owns the board's MIDI ports, bridges them
  to `CrossPad Hub` for DAWs and sends `HOST_INFO`/`FW_LATEST` companion notify
  frames. `crosspad-hil companion` opens the board's MIDI itself, not the hub's.
- The SWD tracer's web UI (`trace-webui.ts`, :7373) starts only from inside the
  MCP server; `crosspad_trace` is not in `src/registry.ts` since the v10
  refactor.
- crosspad.app has no tracer and no screen view.

## Two layers of "busy"

- **Bench lease** (crosspad-hil 1.6.0, `bench.py`, already shipped): one
  holder per board for a whole test, a queue behind it, a TTL, and the
  firmware left on the board. `serve` and the CLI check it before every board
  op and refuse with `BENCH_BUSY`; `CROSSPAD_BENCH_MODE` = enforce|warn|off.
  It stays client-side and unchanged.
- **Hub** (this spec): below the bench lease. It shares the port between
  clients and grants a short **exclusive op** (OTA, file transfer, USB profile
  switch, a resetting scenario); a client that meets one gets `PORT_BUSY`.

A client checks the bench lease first, then goes through the hub. CP Tools
and `crosspad-trace` honour both.

## 1. The hub as arbiter (crosspad-hil)

### Process and IPC

`midi_hub` gains a local control socket: a Unix socket in the lock dir
(`$XDG_RUNTIME_DIR/crosspad-hil/hub.sock`) on Linux/macOS, `127.0.0.1:<port>`
plus a random token in a 0600 file in the same dir on Windows. Protocol is the
`serve` NDJSON: request `{id, op, args}`, response `{id, ok, result|error}`,
events `{ev, ...}`. Each connection says who it is on connect
(`hello {client, purpose, pid}`); that is what `PORT_BUSY` reports.

Systemd keeps the hub alive on Linux as today. A client that finds no socket
starts `crosspad-hil midi hub run` detached and waits up to 3 s for the socket,
on every OS. On Windows the hub has no virtual DAW ports (no ALSA); arbitration
of the board's own ports works the same.

### What the hub owns

- **Board MIDI** — as today. Client companion verbs (`0x1F 01`) join the same
  queue as the hub's own notify frames, one at a time, so a client never meets
  `ERR busy` caused by the hub. Replies (`0x1F 02/03`) go back to the requester
  only; DAW frames keep flowing to `CrossPad Hub` untouched.
- **Board CDC** — lazily. Opened on the first request that needs it, under a
  `PortLock` with purpose `hub`; closed 5 s after the last client disconnects.
  While closed, Chrome (crosspad.app Web Serial) can open the board; while the
  hub holds it, Chrome fails to open and hil tools say who holds it.

### Access modes

- **Shared**: `transact`, `transact_multi`, `burst`, `send`. Serialised in one
  queue; replies matched per request as `CdcLink` does today. Many clients at
  once (CP Tools' pad screen, an MCP query, a scenario's verbs).
- **Exclusive op**: `lease {purpose}` / `release` on the wire. For anything that leaves
  line mode, resets the board or opens its own direct link: OTA, file
  transfer, kit push, USB profile switch, bootloader, capture, record, stim,
  every scenario (a scenario may opt out), `console --reset`.
  While an exclusive op runs, every other request answers
  `PORT_BUSY {holder, purpose, pid, since}` at once; no unbounded waiting. A
  lease whose connection drops is released. The hub does not enforce the bench lease; clients check it before they reach the hub.

### Transport choice

Inside the hub, by the board's USB profile: CDC profile → CDC; MIDI+UAC2 →
the same verb as SysEx `0x1F` (190-char limit). Verbs that need CDC (FS,
OTA, multi-kB replies) answer `NO_CDC_IN_AUDIO_MODE` in the audio profile.

### Client side

`HubLink` implements `CdcLink`'s surface (`transact`, `transact_multi`, `send`,
`burst`, `unmatched`, `close`); the exclusive op is `hub_link.exclusive(purpose)`,
and inside it `open_link()` gives the holder a direct link. `CdcLink.open()`'s callers get a
`HubLink` by default; `--no-hub` (and `CROSSPAD_HIL_NO_HUB=1`) keeps today's
direct path under `PortLock`. Scenarios, `serve` (MCP), `bench.py` and CP Tools
do not change above the link. A scenario declares whether it needs a lease
(attribute on the scenario class, default: lease, so nothing silently
interleaves with an old scenario).

### Out of scope

- STM bridge console VCP (`0x0483:5740`): DTR/RTS resets the ESP; stays on
  `PortLock` only.
- ST-Link: one probe, one pyOCD; the tracer handles it (section 3).

## 2. ESP screens in CP Tools (crosspad-apps)

### Connection

CP Tools starts one `crosspad-hil serve` for the life of the screen (found as
`bench.hil_path()` finds it: PATH, repo `.venv`, a sibling crosspad-hil checkout's `.venv`,
`~/.venvs/crosspad-hil`) and talks NDJSON to it; `serve` goes through the hub
by section 1. Without crosspad-hil the screen says how to install it and shows
nothing else. `_cdc_exchange` / `cdc_verb` / `query_device_versions` move onto
the same path, so CP Tools never bypasses the hub.

### Layout

New Developer tools entry **CrossPad** (`_dev_entries()`, reachable from the
`/` palette). Four tabs (`Tab` / `1`–`4`) under a status line:

`v2 · CDC · hub: 2 clients · lease: kit_churn (pid 4121, 3 min)`

1. **Pads** — 4×4 grid coloured from `LED_STATE` (24-bit ANSI background,
   nearest 256-colour fallback), app name and focus from `UI_STATE`, refreshed
   at 5 Hz over the shared queue.
   - Keys `1234 / qwer / asdf / zxcv` are the pads. A terminal has no key-up:
     a key is a hit (`PAD_PRESS i 100`, `PAD_RELEASE i` after 120 ms);
     `Shift`+key holds the pad until pressed again.
   - `←` `→` = `ENC_ROTATE ∓1/±1`, `Enter` = `ENC_PRESS`, `a` = app list
     (`APP_LIST`, `APP_START`, `APP_STOP`).
2. **Trace** — `HEAP_TRACE START|STOP|DUMP`, `TOUCH_TRACE ON|OFF|CLEAR|DUMP`;
   scrollable output pane; `s` saves it under `.crosspad/traces/<ts>.txt`.
3. **Files** — `/assets`, `/spiflash`, `/sdcard`: browse, pull, push, rm,
   mkdir (crosspad-hil `fs.py`); settings backup (`SETTINGS_EXPORT`,
   `SETTINGS_IMPORT`, `SETTINGS_STATUS`, `/sdcard/crosspad-settings.json`).
   Transfers take a lease for their duration.
4. **Connections** — USB profile CDC ↔ audio (`crosspad-hil usb-mode`, with
   the note that a board with the serial guard asks on its LCD), WiFi
   (`WIFI_STATUS|LIST|SET|FORGET`), BLE (`BLE_ENABLE`, `BLE_STATUS`).

### Busy board in the UI

The status line shows both layers:
`v2 · CDC · bench: pidf (firmware_app, 12 min left) · hub: 2 clients · op: -`.

CP Tools does not claim the bench on its own: every op it sends carries holder
`cp-tools` (or `CROSSPAD_BENCH_HOLDER`), and works while nobody holds the
board. A claim key (`c`, `b` on the Pads tab where `c` is a pad) claims or
releases the bench as `cp-tools`; while held, CP Tools renews it before half
the TTL and releases it when the screen closes. A queued claim shows its place
in the queue. A board held by someone else shows holder, purpose, expiry and
queue in place of the tab content, polled once a second.

While another client holds an exclusive op, every tab shows the holder and age in
place of its content and polls once a second; it comes back by itself when the
lease is released. Shared clients do not block each other: Pads stays live
while an MCP query or a second CP Tools uses the shared queue. In
the audio profile, Files and Trace say "needs the CDC profile — switch in
Connections".

CP Tools calls named verbs (`cdc.verb`), not raw lines; crosspad-hil adds only
those still missing (settings backup), and uses its existing `fs_*` verbs.

## 3. STM tracer shortcut (crosspad-mcp) and the Tracer menu

- New bin `crosspad-trace` (`dist/trace-cli.js`, `package.json` `bin`): runs
  `trace-doctor`, starts `TraceSession` and `Dashboard` on :7373, opens the
  browser, stops cleanly on `Ctrl+C`. Composes existing modules only.
- If :7373 already answers (an MCP session or another `crosspad-trace`), it
  opens the browser on that session and starts no second pyOCD.
- Before starting a new trace it checks the bench lease as the MCP
  `crosspad_trace start` does (holder `CROSSPAD_BENCH_HOLDER`, else
  `crosspad-trace`); a board held by someone else prints the holder and exits
  2 without touching the probe.
- CP Tools Developer tools gets **Tracer**:
  - **STM (SWD)** → runs `crosspad-trace` (PATH, then
    a sibling crosspad-mcp checkout's `dist/trace-cli.js` via node). Missing binary or a
    failing doctor (no ST-Link, no venv, no Debug ELF) shows the doctor's
    verdict and its fix command.
  - **ESP (CDC)** → opens the CrossPad screen on the Trace tab.

Re-registering `crosspad_trace` in the MCP registry is a separate fix, not part
of this work.

## Testing

- **crosspad-hil (pytest, no hardware, `tests/fakes.py`):** shared queue
  ordering and reply matching across two clients; `PORT_BUSY` with holder
  details under a lease; lease released on disconnect; CDC closed 5 s after the
  last client; client autostart; CDC vs `0x1F` by profile; companion replies
  never cross to another client or to the DAW side.
- **CP Tools (crosspad-apps `tests/`):** screens rendered against a fake
  `serve`: pad grid colours from a `LED_STATE` fixture, greyed actions under a
  lease, audio-profile notices, missing-hil message.
- **crosspad-mcp (vitest):** `crosspad-trace` attaches to a running :7373
  instead of starting a second session.
- **Bench** (take the bench lease before touching the board):
  1. `bench.py test smoke` and `kit_churn` through the hub while CP Tools
     Pads is open → both PASS, Pads shows the lease and recovers.
  2. OTA from CP Tools with Pads open in a second CP Tools → lease shown, then
     back.
  3. 5 s after the last client, crosspad.app connects over Web Serial.
  4. Audio profile: Pads works over `0x1F` with a DAW on `CrossPad Hub`.
  5. STM: `crosspad-trace` with the ST-Link; a second start attaches.

## Order of work

1. crosspad-hil: hub control socket, `HubLink`, leases, wrappers.
2. crosspad-mcp: `crosspad-trace`.
3. crosspad-apps: CrossPad and Tracer screens; `_cdc_exchange` onto hil.

platform-idf picks up CP Tools through `tools/app_manager.py`'s hourly
refresh; `tools/bench.py` needs no change beyond the hil it runs.
