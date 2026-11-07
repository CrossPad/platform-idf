# A CrossPad on its owner's crosspad.app account, synced over WiFi — design

Status: draft. It ships as a firmware update over WiFi (the device API on
crosspad.app is live).

**Builds on what exists (cloud-builder line, not merged, not deployed):**
- crosspad `feat/cloud-builder`: `POST /api/device/pair/start`
  (6-character code, 10 min, single use), `/pair/token` (board polls, gets a 64-hex
  device token, stored as SHA-256, bound to the factory MAC), `/pair/claim` +
  `/device/link` page (signed-in account), `GET /api/device/boards`,
  `DELETE /api/device/boards/:serial` (unpair), KV rate limits, tables
  `device_pair_codes` / `device_pairings` (`drizzle/0011_device_pairing.sql`, not applied).
- platform-idf `feat/cloud-builder-device`: `cp_account.cpp` (token in NVS, pairing, 401 → unlink),
  `acct_pair_overlay` on the WiFi page, `ACCT_*` CDC verbs incl. `ACCT_CODE`.
Sections 1–2 below describe the same model; where they differ, **the existing code
wins** (64-hex token bound to the MAC, `/device/link`, `/pair/token`). P1 is a port of
that pairing onto main / `release/device-api` without the cloud-build parts.

Decisions: pairing **both** ways (code on the screen, and USB from
Hardware Studio); sync **kits account → board**, **backup board → account**, and a
**device list** on the account; sync runs **automatically** once WiFi is up.

## 1. Identity and trust

- A board is identified by a **device token** (32 random bytes, base64url), issued at
  pairing and kept in NVS (`account/token`). The Worker stores only `sha256(token)`.
  Every device call after pairing sends `Authorization: Bearer <token>`.
- The serial (`device_serial()`: PCB serial `CP1234` or MAC) is a **label, not a
  credential**: anyone can send any serial. Ownership is the token; the serial only
  shows which board it is. Pairing a board again **within the same account** issues a
  new token and revokes that account's old one; other accounts' rows for the same
  serial are not touched (revoking them by pairing would let anyone who knows a serial
  revoke its real owner) -- they stay isolated until their owner unpairs (see
  `2026-10-08-wifi-remote-design.md` §3).
- Unpairing: from the account (revokes the token; the board learns on its next call,
  401 → forgets the token) or from the board (Settings → Account → Unpair: calls
  `DELETE /api/device/me`, then forgets the token).
- Rate limits on the unauthenticated pairing calls (per IP, Cloudflare rate-limit
  binding); authenticated calls are bounded per token. Nothing unauthenticated writes
  per-serial state (an unauthenticated write per serial lets a
  caller walking the serial space burn the KV/D1 write budget).

## 2. Pairing

### 2.1 Code on the screen (main path, no cable)

1. Board (Settings → Account → Pair, or the first time WiFi is up and no token):
   `POST /api/device/pair/start {sn, rev, fw}` → `{code, poll, expires_in}`.
   `code`: 6 characters from `23456789ABCDEFGHJKLMNPQRSTUVWXYZ` (no 0/O/1/I), shown
   big on the screen with "crosspad.app/pair"; `poll`: 32 random bytes (secret).
2. Owner, logged in on crosspad.app/pair, types the code → `POST /api/devices/claim
   {code}` (session auth) binds the pending pairing to the user. The page shows the
   serial and model so the owner sees which board it is.
3. Board polls `POST /api/device/pair/poll {poll}` every 3 s (≤ 10 min):
   `pending` → `{token, user: {name}, device_id}` once claimed (only once: the
   pairing row is consumed). The screen shows "Paired with <name>".
4. Codes expire after 10 min; 5 wrong codes per session per 10 min lock the claim.

### 2.2 USB from Hardware Studio (shortcut)

The logged-in Hardware Studio page (WebSerial, already connected) reads `DEVICE_INFO`
(serial, rev, fw), calls `POST /api/devices/usb-pair {sn, rev, fw}` (session auth) →
`{token, device_id}` and writes it to the board with the CDC verb
`ACCOUNT_SET <token> <device_id>` (CDC is local and physical: no further check).

## 3. Server (crosspad.app Worker, D1 + R2)

New tables (migration `0010_devices`):

| table | columns |
|---|---|
| `devices` | `id` (uuid), `user_id`, `serial`, `name` (owner's label, default serial), `rev`, `fw`, `token_hash`, `paired_at`, `last_seen`, `revoked_at` |
| `device_pairings` | `code`, `poll_hash`, `serial`, `rev`, `fw`, `user_id` (null until claimed), `device_id`, `created_at`, `expires_at`, `ip` |
| `device_backups` | `id`, `device_id`, `user_id`, `kind` (`sequences`, `mixer`, `settings`, `song`), `r2_key`, `size`, `sha256`, `created_at` |

Device API (Bearer token unless noted):

| route | what |
|---|---|
| `POST /api/device/pair/start`, `/pair/poll` | pairing (no token; rate-limited) |
| `GET /api/device/me` | owner name, device name, sync settings; updates `last_seen`/`fw` (≤ 1 write / 10 min / device) |
| `DELETE /api/device/me` | unpair from the board |
| `GET /api/device/sync/kits?rev=` | manifest of the owner's kits for this board: `[{id, name, updated, files: [{path, size, sha256}]}]` |
| `GET /api/device/sync/kits/:id/file/*path` | a kit file: `kit.json` built from the kit's pads/samples (the inverse of `kit-import.ts`), samples streamed from R2, Range supported |
| `PUT /api/device/backup/:kind` | upload (≤ 4 MB each; last 10 per kind kept) |
| `GET /api/device/backup/:kind[?id=]` | restore (latest or a given one) |

Account API (session auth): `POST /api/devices/claim`, `POST /api/devices/usb-pair`,
`GET /api/devices` (list), `PATCH /api/devices/:id` (rename), `DELETE /api/devices/:id`
(unpair), `GET /api/devices/:id/backups`.

Web: `/pair` (code entry), a **Devices** section on the profile (list: name, serial,
firmware, last online, backups, rename, unpair), and the USB pair button in Hardware
Studio.

## 4. Firmware

- `account.cpp`: token in NVS, `ACCOUNT_STATUS` / `ACCOUNT_SET` / `ACCOUNT_CLEAR`
  CDC verbs, `DEVICE_INFO` verb (serial, rev, fw) for Hardware Studio.
- Settings → **Account** page: not paired → "Pair" (shows the code, polls); paired →
  owner, last sync, "Sync now", "Unpair".
- **Sync engine** (`account_sync.cpp`), one worker task on PSRAM stack, reusing
  `fw_http` (esp_http_client, Range, resume) and the WiFi lease:
  1. `GET /me` (401 → token forgotten, Account shows "Unpaired on crosspad.app").
  2. **Kits down**: manifest vs `/sdcard/crosspad/account/kits.json` (last synced
     ids/sha256); new/changed kits download to `/sdcard/crosspad/kits/.incoming/<id>/`,
     each file sha256-checked, then renamed into `/sdcard/crosspad/kits/<name>/` and
     the kit scan refreshed. Kits deleted on the account are **not** deleted on the
     board (marked "not on your account" in the kit list; the owner deletes).
  3. **Backup up**: `sequences.json`, mixer presets + desk, settings export, the open
     song project's `song.json` — uploaded when their sha256 differs from the last
     upload.
  4. Restore is manual (Settings → Account → Restore backup), never automatic.
- **Automatic, but never under the music** (SD writes during play give
  "card too slow"; the WiFi radio next to USB audio is the known underrun risk): a
  sync starts when WiFi comes up for any reason and at most every 6 h, and only while
  the board is **idle** — no USB audio stream, sequencer/song stopped, no pad hit for
  60 s, Sampler not recording. A pad hit, transport start or USB audio open pauses it
  at the next file boundary; it resumes later from where it stopped (`.part` files,
  Range).
- Size: about 12–18 kB of image (JSON + HTTP glue; esp_http_client is already in) —
  fits the 34.9 kB headroom of the 41-page v2 image; measured before merge.


## 4a. Patterns taken from a tap fleet's cloud sync

A fleet of beer taps already syncs content from a cloud to a card in the field;
what carries over:

1. **One manifest, all or nothing.** The kits sync is a manifest
   `{rev, kits: [{id, name, updated, files: [{path, sha256, bytes, url}]}]}`;
   a kit is taken whole or not at all, and a rejected kit leaves the one on the
   card playing, untouched (the taps: a rejected manifest revision leaves the
   previous playlist, GC included).
2. **Content-addressed files.** `sha256` names the file in
   `/sdcard/crosspad/account/blobs/<sha256>` and verifies the download; a kit
   folder is assembled from blobs, so a sample shared by several kits is fetched
   once, and `bytes` catches a file cut short by a reset (size must match, else
   the item is "absent").
3. **An acknowledgement channel.** After applying a manifest the board posts
   `POST /api/device/sync/ack {rev, applied: [ids], rejected: [{id, reason}],
   free_bytes}`; the account's device list shows per board what is on it and
   why something is not (the taps' `devices/{serial}/media/ack` + their timeline).
4. **"Retained" over HTTP.** The taps' MQTT retained topics land at boot; here
   the board asks `GET /api/device/sync/kits` with `If-None-Match: <rev>` when
   WiFi comes up — 304 costs nothing — instead of a broker.
5. **GC only what the manifest owns.** Blobs no manifest references are deleted;
   kits the owner made on the board are never touched (the taps GC only their cache
   dir).
6. **Never one queue for everything.** The taps' single download queue let
   showcase images wedge the main graphic for 45 s; here the kits download is
   its own low-priority job, paused by playing, and never in the way of the
   firmware updater or the catalog check.
7. **Identity from the factory, not from the network.** The taps' serial comes
   from eFuse/`fctry` NVS; the CrossPad's pairing binds to the ESP factory MAC
   and shows the STM OTP PCB serial — never to anything the network supplies.

## 5. Phases (each shippable alone)

| # | what | repos | needs the board |
|---|---|---|---|
| P1 | port the pairing (web: pair/start, token, claim, boards, `/device/link`, migration 0011; firmware: `cp_account`, pair overlay, `ACCT_*`) without cloud builds; add `last_seen`/`fw` to boards on authenticated calls | both | yes (pair a development board to a test account on staging) |
| P2 | device list on the profile (name, serial, firmware, last online, rename, unpair) + USB pairing button in Hardware Studio (`ACCT_SET` over WebSerial) | both | yes (WebSerial) |
| P3 | kits down: server `kit.json` export + manifest; firmware downloader + idle gate | both | yes (+ live_set: no underruns from a sync paused by play) |
| P4 | backup up + restore | both | yes |

## 6. Open points

- Which kits sync: own kits only, or also liked/collaborated ones (`kit_collaborators`)?
  Draft: own + collaborated; liked ones later.
- Backups of recordings (`/sdcard/crosspad/recordings`, large WAVs): left out (size).
- The production D1 migration and the Worker deploy are run by a maintainer with
  access to the Cloudflare account, from scripts prepared beforehand, as for the
  device API.
