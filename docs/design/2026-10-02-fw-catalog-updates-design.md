# Firmware catalog, updates and smart A/B (sub-project B)

Date: 2026-10-02. Status: design agreed, revised twice (Revision 2 binding), implemented.

## Revision 2 — WiFi in normal mode, background check only (supersedes Revision 1 where they differ)

With the LVGL IRAM change of sub-project A Revision 2, WiFi runs on demand in
normal mode again; network mode is the fallback when memory is short. The user
chose **background check only**:

- **Triggers return, for checking only:** at boot when `boot_check` is on
  (after the launcher paints) and daily (`last_check` ≥ 24 h) when the board has
  sat in the launcher for 2 min with no pad press, no recording, no app. Checks
  take a WiFi lease in normal mode; `no_mem`/no network = try at the next
  trigger, never a restart into network mode. A check in progress is abandoned
  when activity starts.
- The result sets `status.fwLatest`/`fwUpdateAvailable` (orange glyph) and the
  cached catalog on the card, as in Revision 1.
- **Downloads and installs stay manual** — from the Updates tab or the Store,
  in normal mode with a lease, or in network mode when normal mode is refused
  with `no_mem` (the dialog offers *Restart in network mode* on that tab).
  `auto_install`, the power policy and the launcher's restart dialog stay
  dropped; after a manual install the user is asked *Run the new version now?*.
- **Settings → Updates**: *Check at startup* (switch, `boot_check`), *Channel*,
  status row, *Check now* (in normal mode with a lease; on `no_mem`, the
  network-mode offer).
- **Verbs:** `FW_CHECK`, `FW_CATALOG`, `FW_DOWNLOAD` work in normal mode again
  (`ERR no_mem` when the lease is refused); `FW_UPD_CFG [boot=0|1] [gc_mb=<n>]`.
- **HIL `fw_update`** runs in normal mode; one step repeats the download inside
  `NET_MODE 1` to cover the fallback.

## As built — deviations from this design

Implemented on `feat/fw-catalog-updates-r2` (crosspad-core 1.40.0), measured on
v2 on 2026-10-04. The subsystem doc,
[firmware-updates.md](../../subsystems/firmware-updates.md), describes what the
code does; where it and this design differ, these are the decisions.

Decided while planning:

- Catalog and manifest parsing, the cleanup selection, kit ids and channels,
  the daily-check rule and HTTP-date parsing live in crosspad-core
  (`crosspad/fw/`), unit-tested; `main/fw_catalog.cpp` only fetches and
  caches. The device keeps a compact list of the last fetch (`FwCatalogEntry`,
  ~6 kB), not the ~50 kB catalog.
- Each release also carries `CrossPad-<tag>-kits.json`
  (`{"v":1,"tag":…,"kits":[{"kit","tier","revs"}]}`): the Worker learns each
  kit's tier from one file per release.
- Per-kit channels are one NVS string `fwupd/beta_kits` (NVS keys are ≤ 15
  characters); `fwupd` also holds `latest`, `last_check`, `boot_check`, `url`.
- The WiFi lease is taken by `start_job()` on the caller's task, so verbs answer
  `ERR no_mem` / `ERR disabled` at once and a view can offer network mode; the
  job releases it. A download of a package already whole takes no lease.
- The job task has a PSRAM stack and touches no flash: NVS writes and the
  hand-over to host_info go through a one-shot internal-stack writer; slot
  names are read by `start_job()`.
- The catalog cache is the Worker's JSON inside an envelope
  `{"v":1,"fetched":<unix>,"rev":"v<N>","catalog":…}`.
- B's Updates UI is one minimal view (`main/gui/fw_updates_view.cpp`) in the
  Firmware app and on the network-mode screen's Updates tab; it checks when
  shown, at most once a minute. Only a press can lead to a question.
- The network-mode screen's tab rows gain a `shown` callback; the tab bar joins
  the encoder group.
- A refused or failed background check changes nothing the user sees; the next
  attempt waits an hour, a good check without a `Date` header a day.
- A few more C functions (`fw_updater_error()` copies, `_busy`, `_latest`,
  `_target`, `_ready_version`, `_stack_free`, …). A user's job starts through
  `fw_updater_check_start()` / `fw_updater_download_start()`, which hand back
  the refusal; `fw_updater_check_now()`, `fw_updater_download()`,
  `fw_updater_install_latest()` and the copying `fw_updater_catalog()` had no
  caller in B or C and were removed (`fw_updater_catalog_each()` reads the list).
- `FW_CATALOG [<kit>]` filters and caps its line at 448 B (`count=` / `total=`)
  for the companion's 481 B mailbox; `FW_CHECK` / `FW_DOWNLOAD` answer
  `ERR busy`; `FW_UPD_STATUS` ends `stack_free=… inst_stack_free=…`.
- Slot protection in the cleanup matches the package **name** fw_slots records
  (`a_pkg`/`b_pkg`), not an image hash.
- `Semver` also reads `git describe` names and `-dirty`, compares `rc9 < rc10`,
  and accepts a two-part `x.y` as `x.y.0`.
- `config/profiles/full.json` is exactly the checked-in app set; the default
  build is kit `full` and keeps the old release file names.
- The kit reaches the image as `-DCROSSPAD_FW_KIT=<profile>` (root
  `CMakeLists.txt`, before `project()`); CI pins `-DPROJECT_VER=<tag>` because
  applying a profile dirties the tree.
- The `fw_update` scenario's `FW_SWITCH` round trip is opt-in (`--switch`).

Decided during execution and on hardware:

- **The board's own successful check is authoritative** for its kit and
  channel: it may lower or clear `latest` (the glyph goes out after beta →
  stable or a pulled release). The hub's `FW_LATEST` only raises, and only on a
  board that has never checked (`last_check` = 0). `CrosspadStatus::fwLatest`
  widened to `kFwVersionMax` (core 1.40.0).
- **Downloads write `.downloading` first** and remove it after `.complete`: a
  directory with the marker and no `.complete` is a download to resume
  (`FW_LIST verdict=downloading`), never installable.
- **The cleanup runs only after the signature and the manifest match**: the
  catalog's sizes are unsigned. The catalog's channel must equal the signed
  manifest's. A complete package that no longer verifies loses only its
  `.complete`, after the cleanup found room, and is re-downloaded file by file.
- **`fw_install_start()` runs on its own 6144 B internal-stack task**
  (`fw_upd_inst`), not on the PSRAM job stack: it reaches the OTA partition and
  the STM link. The lease is released before the install.
- `kJobStackBytes` stays **8192**: the lowest `stack_free` measured was 2804 B
  (download with a cleanup), 3160–3192 B over TLS.
- **Background work needs the launcher in front, and the kit selector counts as
  activity** (`kit_selector_is_open()`): the gate a kit-less Sampler opens has
  no running app.
- **No background check on a trial boot**: the boot check is held until the
  image confirms itself, then started by the idle watch. The Updates view's
  automatic check is skipped on a trial boot too; a *Check* press is not held. A check
  during a trial boot can panic in WPA3-SAE, and the image is then rolled back.
- **A refusal the caller answers leaves the state alone** (a press's toast, a
  verb's `ERR`); only failures after the lease become `failed`.
- **An update the other slot already holds is offered as *Run***, also after a
  restart (fw_slots keeps a RAM copy of the other slot's kit and version).
- **A version the other slot rolled back from is not offered again** by a
  check; its kit and version are persisted in `fwupd` (`rb_kit` /
  `rb_version`), dropped when the other slot holds something else or that build
  runs here and has confirmed itself, and cleared by `fw_updater_forget()`.
- **`FW_CHECK` / `FW_DOWNLOAD` overflowed the 4 kB main task** over CDC: reply
  lines and refusals are built in static buffers, the helpers `noinline`
  (2220 B free at the lease, was 28 B).
- **`wifi_ctl` +1 kB** (5120): `esp_wifi_init()` left ~150 B above the 1024 B
  line; 2.2 kB now.
- **No "fresh slot" marker**: IDF 5.5.4's `esp_ota_begin()` invalidates the
  rolled-back slot's otadata entry, so a reinstalled slot is never re-marked.
- **CI**: the `matrix` and `release` jobs run in the `espressif/idf` container
  (the self-hosted runner's workspace holds root-owned build files), the
  release job adds the workspace to `safe.directory`, and the core commit must
  be on GitHub. Releases are drafts; the catalog lists published ones.
- **Worker not deployed** at the time of writing; crosspad.app answers 404.

## Revision 1 — network mode, manual updates (superseded in part by Revision 2)

WiFi does not fit beside the audio engine in internal RAM (sub-project A,
Revision 1), so WiFi — and with it the catalog and downloads — runs only in
the **network-mode boot**. The user chose manual updates plus the hub:

- **Dropped from §4:** the boot and daily background checks, `boot_check`,
  `auto_install`, `last_check`, the power/idle conditions, background install,
  the launcher's "restart now?" dialog, the activity-abandons-the-check rule.
- **Checking** happens when the Updates tab of the network-mode screen opens
  (and on its *Check now*). The result — newest version of the running kit in
  its channel — is persisted in NVS `fwupd` and drives `status.fwLatest` /
  `fwUpdateAvailable` in normal mode (the same fields the hub's `FW_LATEST`
  writes; the newer version wins), so the orange glyph survives the restart
  back to normal mode. The fetched catalog is cached on the card
  (`/sdcard/crosspad/firmware/.catalog.json`, with its fetch time) so normal
  mode can show it, aged, in C's Store/Updates tabs.
- **Installing** an update or a store package happens in network mode:
  download → verify → `fw_install_start()` into the non-running slot →
  *Run the new version* (restart into it) or *Exit* (restart into normal mode,
  the new version waits in the other slot). The "never overwrite another kit's
  slot in the background" rule becomes a confirmation dialog when the
  non-running slot holds a different kit.
- **§6 UI:** Settings → Updates in normal mode keeps *Channel* for the running
  kit and the status row, and its *Check now* enters network mode on the
  Updates tab. The two switches are gone.
- **§7 verbs:** `FW_CHECK`, `FW_CATALOG` (live), `FW_DOWNLOAD` answer
  `ERR not_network_mode` in normal mode; `FW_UPD_STATUS`, `FW_UPD_CHANNEL`,
  `FW_UPD_URL` work in both. `FW_UPD_CFG` keeps only the bench knob `gc_mb=`.
  A new `NET_MODE 1 [TAB=updates|store|wifi]` (sub-project A) enters network
  mode on a tab.
- **§9 HIL `fw_update`** runs inside network mode (`NET_MODE 1`, then the
  steps), and finishes with `NET_MODE 0`.
- **Kept unchanged:** packages and signing (§1), Worker endpoints (§2),
  `fw_catalog`/`fw_download` (§3), version order, channels, cleanup (§5), the
  failure rules of §8 that concern downloads and installs.
Depends on sub-project A ([2026-10-02-wifi-on-demand-design.md](2026-10-02-wifi-on-demand-design.md)):
the WiFi lease, `wifi_link_wait_up()`, and `/api/device/ping`'s place on the Worker.

## Goal

A CrossPad on WiFi learns that a newer firmware of the kit it runs exists,
downloads it to the SD card (resumable, checked), verifies that CrossPad CI
signed it, installs it into the slot it is not running from, and asks the user
to restart. The old firmware stays in the other slot as the fallback. The same
machinery lists every image the board could run, for the store UI of
sub-project C.

Done means: a tag on platform-idf produces signed packages per profile and
board revision; `FW_CHECK` on a board running an older build of that kit finds
the new one; `FW_DOWNLOAD` interrupted half way resumes; a flipped byte or a
bad signature never reaches a slot; with auto-install on, the board installs in
the background on mains power and offers the restart; the card never holds more
than the library limit.

## Where things stand today

- **Packages already exist** (`tools/make_fw_package.py`, `docs/subsystems/firmware-library.md`):
  `/sdcard/crosspad/firmware/<name>/{CrossPad.bin, manifest.json, assets.bin}`.
  `manifest.json` has `name, kit, version, board_rev, stm_proto_min, assets_id,
  files{size, sha256}`. The device never trusts the manifest over the image's
  own `CPFI` block (`kit_id`, board rev, `stm_proto_min`, assets id).
- **Install and switch exist**: `fw_install_start(name, allowUnknown, &err)`
  writes the non-running slot from a library package; `fw_switch_request()`
  moves the boot partition; the bootloader rolls back an image that never
  confirms itself (`main/fw_slots.cpp`).
- **Build profiles exist**: `config/profiles/{full,lite}.json` (app set +
  feature flags), applied by CP Tools / `idf.py app-profile`.
- **CI** (`.github/workflows/main.yml`) builds v1 and v2 of one configuration
  and drafts a release with `CrossPad-<tag>-<rev>[-assets|…].bin`.
- **Worker** (crosspad.app website repo, `server/api/firmware/index.ts`,
  `server/lib/firmware/index.ts`): `/api/firmware` index of the private repo's
  releases via the CrossPad GitHub App, assets cached in R2, index in R2 with a
  10 min TTL. Assets are served whole from memory — **no `Range`**.
- **Update-available plumbing**: `CrosspadStatus::fwLatest` / `fwUpdateAvailable`,
  set from the hub's `FW_LATEST` (`main/host_info.cpp`, persisted in NVS), drawn
  as the orange USB glyph. `parse_semver()` there ignores pre-release suffixes.
- **mbedTLS has no Ed25519**; it has ECDSA P-256 and SHA-256 (the overview's
  "Ed25519" becomes ECDSA P-256 here — same role, no new library).

## 1. Packages from CI

- **Matrix `profile × revision`**: every `config/profiles/*.json` is one kit;
  a tag builds each profile for v1 and v2. The profile's `name` is the kit id
  (`CPFI kit_id`). Applying a profile in CI reuses the app manager
  (`tools/app_manager.py` / `idf.py app-profile`) so a CI kit is exactly what a
  developer gets locally.
- `make_fw_package.py` gains `--channel stable|beta` (beta when the tag has a
  `-`), `--tier official|community` and `--sign <pem>`. The manifest gains
  `channel`, `tier`, `tag`. `--sign` writes `manifest.sig`: ECDSA P-256 over the
  exact bytes of `manifest.json` (DER signature), key from the Actions secret
  `FW_SIGNING_KEY`. The manifest already carries every file's SHA-256, so one
  signature covers the package.
- Release assets per cell: `CrossPad-<tag>-<rev>-<kit>.bin`,
  `…-<kit>-assets.bin`, `…-<kit>-manifest.json`, `…-<kit>-manifest.sig`. The
  existing per-revision files of the default profile stay as they are (the
  companion and CP Tools read them).
- **Community kits** are profiles too, submitted by PR to a `config/profiles/community/`
  directory (apps from public repos via `external-apps.json`). The same CI
  builds and signs them with `tier: community`. CrossPad never hosts a binary
  it did not build.
- **Keys**: one P-256 key pair, generated once. Private key → Actions secret
  `FW_SIGNING_KEY` (added by a maintainer, not by tooling). Public key →
  `main/fw_sign_pubkey.h`, an array of up to **2** keys so a key can be rotated
  by shipping the new public key one release before signing with it.

## 2. Worker: `/api/device/*`

New Hono router in the website repo's `server/api/device/`, mounted beside
`/api/firmware`, reusing `getIndex()` / R2:

| Route | Answer |
|---|---|
| `GET /api/device/ping` | `{"ok":true}` — sub-project A's reachability check |
| `GET /api/device/catalog?rev=v2` | compact JSON, below |
| `GET /api/device/file/:tag/:name` | the asset from R2, honouring `Range` (206 + `Content-Range`; 416 past the end), `ETag` = the asset's SHA-256 |

Catalog (only packages of the requested revision, newest first, a few kB):

```json
{"v":1,"generated":"2026-10-02T18:00:00Z",
 "kits":[{"kit":"full","versions":[
   {"version":"1.3.0","tag":"v1.3.0","channel":"stable","tier":"official",
    "bin":{"name":"CrossPad-v1.3.0-v2-full.bin","size":2568208},
    "assets":{"name":"…-assets.bin","size":1048576},
    "manifest":"…-manifest.json","sig":"…-manifest.sig"}]}]}
```

The catalog is not signed: it is a pointer list. Every package it points to is
verified on the device against the signed manifest, so a compromised Worker can
hide or withhold updates but cannot install anything.

Draft releases are excluded; pre-releases appear with `channel: beta`.

## 3. Device modules

### `main/fw_catalog.{h,cpp}`
`fw_catalog_fetch(FwCatalog* out)` — needs a held WiFi lease; GETs the catalog
for this board's revision into a PSRAM buffer, parses with ArduinoJson into a
fixed-capacity structure (≤ 16 kits × 8 versions), records the response's
`Date` header as the check time. Base URL `https://crosspad.app` unless
`FW_UPD_URL` overrode it (NVS `fwupd/url`; `http://` allowed there — integrity
comes from the signature).

### `main/fw_download.{h,cpp}`
Downloads one catalog version into `/sdcard/crosspad/firmware/<kit>-<version>/`:
1. Space check and cleanup (§5) for the sum of the file sizes.
2. `manifest.json` and `manifest.sig` first; verify the signature against each
   key in `fw_sign_pubkey.h`. No valid signature → delete the directory, block
   this version until reboot, stop.
3. The manifest's `board_rev`/`kit` must match the catalog entry and this board.
4. Each file to `<file>.part` with `Range: bytes=<size of .part>-`; SHA-256 is
   computed over the whole file (the existing part re-hashed first, then the
   stream); match against the manifest → rename to the final name. Mismatch →
   delete the `.part`, one fresh retry, then block the version until reboot.
5. Write `.complete` last. `fw_library_scan()` treats a directory without
   `.complete` but with `.part` files as "downloading", not as a package.

Runs on one worker task with a PSRAM stack (it writes the SD card through
FATFS, not internal flash — same as `fx_worker`'s file work, which must yield
to the sample streamer: call `sample_player_port_yield_to_streamer()` between
chunks, CLAUDE.md "Changing Kits While Playing"). Chunk 4 kB, progress as
bytes done / total through an atomic.

### `main/fw_updater.{h,cpp}`
The policy (§4), its NVS namespace `fwupd`, the verbs, and the C-facing API:

```c
typedef enum { FWU_IDLE, FWU_CHECKING, FWU_UP_TO_DATE, FWU_AVAILABLE,
               FWU_DOWNLOADING, FWU_INSTALLING, FWU_READY_TO_RESTART,
               FWU_FAILED } fwu_state_t;
fwu_state_t fw_updater_state(void);
bool fw_updater_check_now(void);                         /* user: may offer reclaim (A) */
bool fw_updater_download(const char* kit, const char* version, bool install);
uint8_t fw_updater_progress(void);                       /* 0..100 */
size_t fw_updater_catalog(FwCatalogEntry* out, size_t cap); /* last fetched, with on_card flag */
const char* fw_updater_error(void);
```

### Version order
A shared `crosspad::Semver` (core, unit-tested) replaces `host_info.cpp`'s
local parser: `major.minor.patch[-pre]`, a pre-release sorts below its release
(`1.3.0-rc1 < 1.3.0`), pre-release identifiers compare numerically where both
are numeric. `host_info.cpp` moves onto it.

## 4. Policy

**Settings** (NVS `fwupd`, outside `CrosspadSettings`, shared by both slots):
`boot_check` (default on), `auto_install` (default off), `channel.<kit>`
(`stable` default / `beta`), `last_check` (unix time from `Date`), `url`.

**An update** is a catalog version of the running kit (`CPFI kit_id`) and this
board revision, in the kit's channel (`beta` sees `stable` too), strictly newer
than the running version. Older versions are never offered as updates.

**Triggers**
- Boot, when `boot_check` is on: once the launcher has painted (same moment as
  `bleMidi_boot()`), not before.
- Daily: when `last_check` is ≥ 24 h old (or unknown) and the board has sat in
  the launcher for 2 min with no pad press, no recording and no running app.
- User: Settings → Updates → *Check now*, the store (C), `FW_CHECK`.

Background triggers never reclaim memory: a WiFi `no_mem` or no network means
"try at the next trigger".

**On an update**
1. `status.fwLatest` / `fwUpdateAvailable` are set (the orange glyph appears;
   the hub's `FW_LATEST` and this path write the same fields — the newer wins).
2. Auto-install proceeds only if **all** hold: `auto_install` on; charger
   connected or battery ≥ 50 %; the non-running slot is empty, rolled back, or
   holds the same kit. A slot holding another kit (say the DAW firmware) is
   never overwritten in the background — the package is downloaded to the card
   and the user is asked.
3. Download (§3) → `fw_install_start()` into the non-running slot →
   `FWU_READY_TO_RESTART`.
4. In the launcher only (never over a running app), a dialog: *Update 1.3.0 is
   ready. Restart now?* — *Restart* calls the existing switch flow
   (`fw_flow_switch_and_restart`), *Later* leaves the glyph and the Settings
   row. The board never restarts on its own.

Nothing in the background runs while a pad is pressed, an app other than the
launcher is in front, or the Recorder/Sequencer is recording; a check in
progress is abandoned (lease released) when that changes.

## 5. Card cleanup (space only)

- Limits: the library (`/sdcard/crosspad/firmware`) stays ≤ **512 MB**, and the
  card keeps ≥ **10 %** free.
- Protected: the packages installed in slot A and slot B (matched by the
  image SHA-256 recorded at install), packages marked pinned (`.pinned` file in
  the package directory), and the download in progress.
- Eviction: oldest first by the manifest's version order within a kit, then by
  directory mtime across kits; loose bare `*.bin` files count like packages.
- Runs before a download (for its known size) and after it. If the limits
  cannot be met without touching a protected package, the download is refused
  with "card full" — nothing protected is ever deleted.

## 6. UI in this sub-project

Settings → new category **Updates** (`main/app/settings/settings_app.cpp`,
`settings_row_*`): *Check at startup* (switch), *Install automatically*
(switch), *Channel* for the running kit (dropdown stable/beta), *Check now*
(action), a status row (`Up to date: 1.2.0` / `Downloading 1.3.0… 42 %` /
`Ready — restart to update` / `Error: …`). The ready-to-restart dialog of §4.
Everything else (library, store, update list) is sub-project C.

## 7. Verbs

| Verb | Reply |
|---|---|
| `FW_UPD_STATUS` | `FWUPD: state=<s> running=<ver> kit=<kit> latest=<ver\|-> channel=<c> progress=<0-100> last_check=<unix\|0> err=<text\|->` |
| `FW_CHECK` | `OK` (result via `FW_UPD_STATUS`) |
| `FW_CATALOG` | `FWCAT: count=<n> \| <kit> <ver> <channel> <tier> <on_card 0\|1> \| …` |
| `FW_DOWNLOAD <kit> <ver> [INSTALL]` | `OK` / `ERR <reason>`; progress via `FW_UPD_STATUS` |
| `FW_UPD_CFG [boot=0\|1] [auto=0\|1] [gc_mb=<n>]` | `OK`; `gc_mb` overrides the 512 MB library limit until reboot (bench only, not persisted) |
| `FW_UPD_CHANNEL <kit> stable\|beta` | `OK` |
| `FW_UPD_URL <url>\|DEFAULT` | `OK` (persisted) |

`FW_UPD_STATUS` and `FW_CATALOG` are allowed over the companion; the rest are
CDC only (they start work that outlives the companion's reply).

## 8. Failure rules

| Situation | Outcome |
|---|---|
| WiFi refused (`no_mem`, disabled, no network) | background: try at the next trigger; user: A's reclaim dialog |
| Catalog unreachable / unparseable | `FWU_FAILED`, `err=catalog`, next trigger retries |
| Download interrupted (WiFi lost, power, card pulled) | `.part` kept; next attempt resumes with `Range` |
| SHA-256 mismatch | `.part` deleted, one fresh retry, then version blocked until reboot |
| Bad / missing signature | package deleted, version blocked until reboot, `ESP_LOGE` |
| Manifest disagrees with catalog or board | package deleted, `err=mismatch` |
| Card full after cleanup | refused, `err=card_full` |
| Install refused by `fw_install_start` | its own reason in `err`, package kept on the card |
| Power lost while installing | existing behaviour: slot invalid, boot partition unchanged |

Nothing here restarts the board except the user's *Restart*.

## 9. Tests

- **Core** (Catch2): `Semver` ordering incl. pre-releases; cleanup selection
  as a pure function (`FwLibraryGc::select(packages, limits, protected)`).
- **Host** (pytest, `tools/`): `make_fw_package.py --sign` round trip; a
  flipped manifest byte fails verification; channel/tier fields.
- **Worker** (vitest, website repo): catalog built from a fixture index
  (revision filter, drafts excluded, pre-release → beta); `Range` → 206 with
  correct `Content-Range`, open-ended range, 416 past the end, no `Range` → 200.
- **HIL** (crosspad-hil scenario `fw_update`): a local HTTP server serving real
  signed packages from a release (cached under the hil data dir),
  `FW_UPD_URL` pointed at it. Steps: catalog lists the kit; download completes;
  download killed half way (server drops the connection) resumes and verifies;
  a served byte flipped → rejected, nothing installed; a bad `manifest.sig` →
  rejected; `FW_DOWNLOAD … INSTALL` → `ready_to_restart`, `FW_SWITCH` → the
  new image confirms (as `fw_switch`); cleanup with a temporarily lowered
  limit (`FW_UPD_CFG` test knob `gc_mb=`) evicts the oldest unprotected
  package only. Finally `FW_UPD_URL DEFAULT`.

## 10. Documentation

`docs/subsystems/firmware-updates.md` (source of truth: package signing, key
rotation, catalog format, policy, cleanup, verbs, traps), a short section in
`CLAUDE.md`, and an update to `docs/subsystems/firmware-library.md` for the
`.complete`/`.part`/`.pinned` markers.

## Out of scope

Store UI and library tabs (C); choosing apps and building to order (D);
STM32 firmware as a separate download (it travels inside `assets.bin`);
delta updates; downgrades from the catalog (a user can still install any
package on the card through the Firmware app, as today).
