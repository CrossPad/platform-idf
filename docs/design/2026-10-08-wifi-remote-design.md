# Managing a CrossPad over WiFi, from anywhere — design

Status: approved in conversation section by section; **amended after the
implementation and the final review to describe what was built** (sections 0-7; the
amendments are the "as built" text, the earlier wording they replace is gone). Builds on the account pairing
(`2026-10-07-account-sync-design.md`, P1: a paired board holds a
64-hex device token, the Worker keeps its SHA-256 in D1).

## 0. Decisions

| question | decision |
|---|---|
| from where | **anywhere**, through crosspad.app (the board behind any router, the page on https) |
| what, in v1 | **the commands that work over BLE and MIDI today** (status, settings, kits list/load, apps, remote pads/encoder/screen) minus WiFi changes (§3.5); files and firmware stay on their own paths (kits from the account: sync P3; firmware: the existing updater the board pulls). `ENC_*` and `APP_START` reach the whole UI, Settings included, as over BLE/MIDI: the owner's chosen scope |
| when reachable | **always while WiFi is on**, with a "Remote management" switch in Settings (default on) |
| transport | **MQTT** (3.1.1) |
| broker | **on the Cloudflare account we already have** — no VPS, no new DNS |
| BLE vs remote | **WiFi wins**: while remote management holds WiFi, BLE is off (the radios are exclusive, `wifi_link.h`). Turning BLE on in Settings turns Remote management **off** (saved, with a toast); turning Remote on while BLE is on is allowed and the WiFi page says BLE is off while remote runs |
| who | **per account**: several accounts may hold a pairing row for one serial (a serial is a label); each account's sessions are isolated from the others' in the one per-serial broker |

## 1. Architecture

```
browser (play.crosspad.app)               board (ESP32-S3)
  mqtt.js over wss                          esp-mqtt over wss
  /api/device/boards/:serial/mqtt           crosspad.app/api/device/mqtt/:serial
        │  account session                        │  serial + device token
        └──────────►  Durable Object  ◄────────────┘
                      BoardBroker, one per board
                      (MQTT 3.1.1 subset over WebSocket)
```

- Cloudflare has no MQTT broker we can use (Pub/Sub never left beta; Containers take
  no raw TCP), so the broker is a **Durable Object per board** speaking MQTT 3.1.1 over
  WebSocket (subprotocol `mqtt`). It implements only what we use: CONNECT/CONNACK,
  SUBSCRIBE/SUBACK, PUBLISH QoS 0 and 1 (PUBACK), PINGREQ/PINGRESP, DISCONNECT, a last
  will, and one retained message (`state`). Everything else closes the connection.
- The board uses the stock ESP-IDF `esp-mqtt` client with its WebSocket transport, so
  moving to a real broker later changes only the URL in the firmware.
- The board reaches the broker through the existing route `crosspad.app/api/device/*`
  (Worker `crosspad-device`); the browser through `play.crosspad.app` (Worker
  `crosspad`). The DO class lives in `crosspad-device`; `crosspad` binds it with
  `script_name: "crosspad-device"`. Both ends address the object by the board serial
  (`idFromName(serial)`), so they meet in the same instance.
- **One DO per serial, sessions per account.** A board session is bound to the account
  whose pairing row its token matched; an owner session to the account of its login.
  Requests, replies, retained state (`state:<userId>` in DO storage) and the peers count
  travel only within one account; a board login replaces only the same account's board.
  The serial itself is not stored (it rides in each socket's attachment).
- **QoS:** browser ↔ broker QoS 0 (a request is refused at once when the browser is not
  connected, never replayed later); broker ↔ board QoS 1 (`req` to the board, `rsp` from
  it, each PUBACKed).
- Hibernation API: an idle WebSocket costs no duration; each incoming message wakes
  the object (billed 20 messages : 1 request).

## 2. Topics and messages

| topic | publisher | subscriber | payload |
|---|---|---|---|
| `cp/{serial}/req` | browser | board | `<id> <command line>` |
| `cp/{serial}/rsp` | board | browser | `<id> <reply lines>` (`\r\n`-separated, as over CDC) |
| `cp/{serial}/state` | board (retained; last will `offline`) | browser | `online fw=<ver> rev=v<n>` or `offline` |
| `cp/{serial}/peers` | the broker | board | number of browsers connected (`0`..), for the status-bar icon |

- `id`: 1–8 characters `[0-9a-z]`, chosen by the browser; the board echoes it.
- Command lines are the existing CDC text verbs; the board runs them through the same
  allowlist as BLE and MIDI (`hil_control_companion_handle`).
- Limits: a command ≤ 512 B, a reply ≤ 4 KiB (BLE/MIDI: 190 B / 480 B). Longer replies
  are cut after the last whole line and end with `ERR truncated`.
- Error replies (`<id> ERR <token>\r\n`): from the broker `too_long` (request over
  9 + 512 B), `rate` (over 20/s on that socket), `offline` (no board of this account
  connected, or its pairing found gone); from the board `not_remote` (denied over the
  remote, §3.5), `unsupported_companion_command` (not on the companion allowlist),
  `busy` (queue full), `truncated` (the end of a cut reply).
- The `<id> <line>` framing, its limits and its parser live in **crosspad-core**
  (portable, Catch2 tests), next to `FwAccount`.

## 3. Security

1. **Board login.** CONNECT `username` = serial (12 hex, factory MAC), `password` =
   device token. The DO checks `sha256(token)` against `device_pairings` in D1 (the
   pairing table). Wrong → CONNACK `5` (not authorized) and close; the board treats it
   like a 401 from the catalog: forgets the token, shows "Unpaired on crosspad.app".
   The matched row's account binds the session. The board's row is **re-verified** in
   D1 at most every 60 s on activity (a request forwarded to it); a row found gone ends
   that account's sessions — an unpair whose kick was lost stays effective ≤ 60 s.
   Failed CONNECTs are counted per address (IPv4, or IPv6 /64 -- one subscriber's link, so
   a /48 neighbour cannot lock a board out; from `CF-Connecting-IP` only; `::ffff:a.b.c.d`
   is its IPv4 address) in KV: 60 per 10 min. Over it the route answers the upgrade 429 and the DO
   itself (it may run on a Worker whose KV is not the route's) answers a board CONNECT
   with CONNACK `3` (server unavailable — never `5`: the address may be shared with the
   board behind a NAT, and `5` would unpair it) without a D1 read.
2. **Browser login.** The upgrade to `play.crosspad.app/api/device/boards/:serial/mqtt`
   carries the account session; the Worker checks the board is paired to **this**
   account and checks `Origin` when present (a browser always sends it: it must be the
   page's own origin, no cross-site WebSocket hijacking), then forwards the upgrade to the
   DO marked as the owner of that account. A non-browser client (the probe script,
   crosspad-hil) sends no `Origin` and authenticates with a Bearer mobile JWT instead of
   the cookie. CONNECT credentials from the browser are ignored.
3. **Topic ACL in the DO, by role.** Board: subscribe `req` and `peers`, publish `rsp`
   and `state`. Browser: publish `req`, subscribe `rsp` and `state`. Any other topic, or
   another serial, closes the connection. Peers are counted per account.
4. **Unpairing** (`DELETE /api/device/boards/:serial`) tells the DO to close **that
   account's** board and browser sessions at once and deletes that account's retained
   state; other accounts' sessions for the serial are untouched. A failed kick does not
   fail the unpair (the 60 s re-verify closes the gap).
5. **On the board:** only the companion allowlist (no firmware upload, no file writes,
   no bootloader, no USB drive mode), and over the remote a **deny list**: every `WIFI_*`
   verb but `WIFI_STATUS` and `WIFI_LIST` answers `ERR not_remote` (no WiFi changes).
   `HOST_INFO` / `FW_LATEST` stay allowed (host notifications; they change no protected
   setting). `ENC_*` and `APP_START` reach the whole UI, as over BLE/MIDI — the owner's
   chosen scope, documented, not denied. The "Remote management" switch; a status-bar
   icon while a browser is connected. At most **20 requests/s per browser socket** (over
   → `<id> ERR rate`); the board runs one command at a time and queues up to 4 more,
   beyond that it answers `<id> ERR busy`; no board of the account connected → the
   broker answers `<id> ERR offline`. Replies are published by a sender task
   (`cp_remote_tx`), never by the main loop.
6. **Who:** only the account(s) the board is paired with, each isolated: several
   accounts may hold rows for one serial (pairing again replaces only the same account's
   token), and each sees only its own board session, state and peers. Sharing one session
   between accounts is out of scope.
7. Tokens are never logged, on the server or the board.
8. **Denial of service (per broker = per serial).** A socket that has not completed
   CONNECT is closed after **10 s**. Board sockets not yet logged in are anonymous and
   pooled **per prefix (4)** -- an IPv4 address or an IPv6 **/48** (a host is commonly
   routed a /56 or /48, so per /64 it could fill the pool alone): a new one evicts that
   prefix's oldest idle one, never one whose CONNECT is being verified; past **32** in all
   a newcomer takes the place of the most crowded prefix's oldest idle socket (that prefix
   holding at least two more), else it is refused (503). **8 owner sockets per account**
   (only that account's 9th is refused, 429); one board session per account, so an
   account holds at most 9 sockets. A **2048-socket** ceiling is a safety valve, above
   128 accounts paired to the serial at their caps plus the pool (1184): at it an idle
   anonymous socket of a prefix holding at least 2 more than the newcomer's gives way
   first (to an owner newcomer -- whose prefix counts 0 -- only one at least **2 s** old:
   a real board sends CONNECT one round trip after its upgrade, so a board's fresh socket
   is never displaced unless its own prefix holds 2+ anonymous sockets), then the newest
   socket (an owner's before the board's) of the account holding the most sockets, **all of an account's sockets
   counted, boards included**, only if it holds more than the newcomer's own account (an
   anonymous board counts 0; ties: the account that arrived last gives way); else the
   newcomer is refused (503). An account is harmed only by an attacker filling the broker
   with about 2000 / k accounts (k: the sockets it holds). A logged-in socket evicted
   there is closed like an unclean close (its will stored, its account's peers told).
   Below the ceiling an upgrade reads only the anonymous pool (a board; an in-memory index
   of pending boards) or its account's own owner sockets (an owner), never every socket;
   the sweep for late logins runs at most once a second. Owner sockets are tagged with
   their account (`owner:<userId>`) and connected boards indexed by account (rebuilt from
   the `board` tag after hibernation), so a message's fan-out (req, rsp, state, peers,
   kick) reads only its own account's sockets. At most **32 MQTT packets per WebSocket
   message** (more closes 4000). A board's state publishes take tokens like a browser's
   requests (20/s) and a repeated state is not written again. A board **silent for
   more than 90 s** (no packet, not even PINGREQ) is closed as an unclean close by an alarm that
   runs every 60 s while a board and an owner are both connected (armed on whichever
   CONNECT completes the pair, re-armed only while both remain) (state `offline` for that account, its
   browsers told). The DO answers a client's close frame with its own.

## 4. Firmware (platform-idf)

- **`main/cp_remote.cpp`** (new, small): the `esp-mqtt` client
  (`wss://<account base>/api/device/mqtt/<serial>`; esp-mqtt sends no custom WebSocket
  headers, so the serial is in the path and the token in CONNECT), a small queue like
  `companion_sysex`, and `cp_remote_poll()` called from the main loop, which runs
  `hil_control_companion_handle()` — the same thread and allowlist as BLE and MIDI; no
  new thread runs commands. The finished reply goes to a small sender task
  (`cp_remote_tx`, created with the client) that publishes it on `rsp` with a blocking
  QoS 1 publish (a frame handed to the MQTT task waited up to 1 s for its socket poll).
- **When it runs:** paired, WiFi enabled and "Remote management" on → it takes a WiFi
  lease `"remote"` (`wifi_link_acquire`) and keeps the radio up. Not in transfer-mode or
  network-mode boots. A memory-gate refusal → reason `no_mem`, retry every 60 s; any
  other refused lease or failed/lost link backs off **60 s doubling to 30 min**, reset
  when online or when the WiFi settings page is opened. The page shows the board
  offline with the reason.
- **Connection:** MQTT keepalive 60 s (Cloudflare closes a WebSocket idle for 100 s).
  esp-mqtt pings at half the keepalive, so **every ~30 s**: 2880 incoming messages a day,
  **≈ 144 billed requests per board per day** (20:1); the broker's 60 s alarm runs only
  while a browser is connected too (§3.8), so an unwatched board costs the pings alone. Reconnect backoff 5 s → 5 min with
  0–50 % random jitter. `not authorized` → unpaired: drop the token, stop.
- **Music first:** messages are small and nothing is written to the SD card; the
  `rt_glitch` bench with `WIFI_UP` held measured 0 clicks. To verify in the plan: a
  Sampler kit load while the `"remote"` lease holds the WiFi memory — the kit must
  load; remote may release WiFi for it.
- **UI and verbs:** the switch in Settings next to WiFi; the status-bar icon;
  `REMOTE_STATUS` for HIL: `REMOTE: state=<off|waiting|no_wifi|connecting|online|backoff|unlinked>
  reason=<r> peers=<n> queued=<n> done=<n> busy=<n> uri=<uri|->` (no stack free: there
  is no handle to the esp-mqtt task).
- **Image size — measured 8.10:** `esp-mqtt` with the WebSocket transport adds 17 132 B
  of `.flash.text` and 6 192 B of `.flash.rodata`; `.flash.text` then ends at 0x421ED848
  (limit 0x421F0000), leaving **~9.9 kB** for `cp_remote` and the core framing.
  **As built:** `CONFIG_MQTT_TRANSPORT_SSL` stays **on** (esp-mqtt compiles the `wss`
  transport only with it). Measured after the final fixes (v2): `.flash.text` ends at
  0x421EED70, **4752 B** of headroom; `.ext_ram.dummy` 2686944 (unchanged).
- **Sampler:** with a kit loaded the WiFi memory gate refuses by design (`no_mem`);
  remote shows offline with that reason and retries every 60 s.

## 5. Server and web (crosspad.app)

- `server/lib/device-remote/`: the MQTT packet codec and the broker logic as plain TS
  (no Cloudflare APIs, unit-testable), and a thin `BoardBroker` Durable Object class.
- Routes (all in the one `deviceApi` under `/api/device`): `GET /mqtt/:serial` (board,
  reached on `crosspad.app` → `crosspad-device`), `GET /boards/:serial/mqtt` (browser,
  reached on `play.crosspad.app` → `crosspad`: session or Bearer JWT, owner, same-origin
  `Origin` when present).
- `wrangler.jsonc`: the DO binding and a `new_sqlite_classes` migration in env
  `device` (Worker `crosspad-device`) and `device-staging` (its own Worker
  **`crosspad-device-staging`**, workers.dev only, dev D1, its own BoardBroker — it must
  never share the production name); in `prod` a binding with `script_name:
  "crosspad-device"`. No D1 migration.
- `DELETE /api/device/boards/:serial` kicks the DO's sessions.
- Web: `app/lib/device/mqtt.ts` — `createMqttControl()` with the same `Remote`
  interface as `midi.ts` and `ble.ts` (`mqtt.js` loaded on connect); "WiFi" joins
  USB/Bluetooth/MIDI in the connections list when the board is paired and online;
  `/device` shows online/offline and the reason. The remote panel stops polling
  `LED_STATE` while the tab is hidden.

## 6. Tests

- **vitest:** codec (remaining length, boundaries, malformed packets); broker (login,
  ACL, last will, retained state, peers count, kick on unpair, 20/s limit, offline
  answer, per-account isolation, re-verify, the §3.8 limits: CONNECT deadline, pools,
  owner cap, ceiling, packets per message, silent-board alarm, state writes, close
  reply, failed-login limit in the DO); routes (someone else's board, wrong `Origin`, no
  session, address from `CF-Connecting-IP` only, IPv6 /64 for failures, /48 for the pools).
- **Catch2 (core):** the `<id> <line>` framing and limits.
- **Bench, fully local first** (`wrangler dev --local`, the board built with the LAN
  address, a Node script with `mqtt.js` playing the browser): commands and latency;
  reconnect after a Worker restart and after WiFi loss; unpair kicks the session;
  memory (`REMOTE_STATUS`, `MEM`); `live_jam` with a remote session open; a Sampler
  kit load with remote on.

## 7. Rollout

1. Image-size measurement of `esp-mqtt` (stop and revisit if it does not fit).
2. Codec, broker and routes — fully local.
3. Firmware on hardware against the local Worker.
4. Web client.
5. Staging: a deploy script deploys the Worker **`crosspad-device-staging`**
   (workers.dev; no D1 migration) and refuses to deploy a build with any other name --
   a staging deploy under the production name replaces production `crosspad-device`;
   then the bench test against staging.
6. Production: the PR with deploy and rollback steps. **Deploy order:
   `crosspad-device` (env `device`, it owns the BoardBroker class) before the site
   (`crosspad`, env `prod`, which binds it by `script_name`).** A maintainer merges the
   CrossPad PRs (core, platform-idf).

A firmware with this feature and no server behind it fails to connect and backs off to
5 min, but it still holds the WiFi lease while paired with Remote management on: the radio
stays up and BLE stays off. Release the firmware only after `crosspad-device` (with the
BoardBroker) is live in production.

## 8. Out of scope (v1)

Files and firmware over WiFi (sync P3 and the updater cover them), sharing a board with
other accounts, the PC simulator.
