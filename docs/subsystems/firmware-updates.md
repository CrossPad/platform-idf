# Firmware Updates (catalog, signed packages, background checks)

> Source of truth for this subsystem. CLAUDE.md carries only the summary; when
> behaviour changes, update this file and the summary together.

Design: [2026-10-02-fw-catalog-updates-design.md](../design/2026-10-02-fw-catalog-updates-design.md)
(Revision 2 binding; its "As built" note lists where the code diverges). Builds
on [wifi.md](wifi.md) (the leased link, network mode as the fallback) and
[firmware-library.md](firmware-library.md) (packages, slots, switch).

**Status (2026-10-04): implemented, measured on v2** (crosspad-core 1.40.0). Every step of the
hardware run passed against a local copy of the server serving the
real CI-signed release `v1.1.0-fwstore.3`. The crosspad.app routes are live
since 2026-10-24 (`https://crosspad.app/api/device/catalog?rev=v2`). v1 builds
and is not measured.

## In one paragraph

CI builds every kit (a profile in `config/profiles/`) for every board revision,
signs each package's `manifest.json` with ECDSA P-256 and attaches the files to
a draft release. crosspad.app lists published releases per revision
(`/api/device/catalog`) and serves the files with `Range`. The board **checks**
on its own -- at boot when *Check at startup* is on, and daily when it has sat
idle in the launcher -- on a WiFi lease in a normal boot; a background check
shows nothing unless it succeeds, and its result lights the update glyph and
caches the catalog on the card. **Downloads and installs are manual**: the
Updates view (Firmware app, or the network-mode screen when WiFi was refused
for memory) downloads to `/sdcard/crosspad/firmware/<kit>-<version>/`
(resumable, every file SHA-256 checked against the signed manifest), installs
into the slot not running and asks *Run the new version now?*.

## Packages and signing

- `tools/make_fw_package.py --build build_v2 --name <kit>-<ver> --tag <tag> [--tier official|community] [--channel stable|beta] [--sign key.pem]` --
  manifest fields `name kit version board_rev stm_proto_min assets_id channel tier tag files{size,sha256}`;
  channel defaults to the tag's (see "Versions and channels"). The tag must be
  `v<maj>.<min>.<patch>[-pre][+build]` -- each number at most 65535, the
  pre-release and build of non-empty dot-separated `[0-9A-Za-z-]` identifiers,
  exactly core `fwVersionValid()`'s grammar (`make_fw_package.tag_valid()`,
  checked against core's own code by its test; `v1.2.3-.` or `v1.2.3-+` would
  sign and publish, and every board refuse the manifest) -- at most **23**
  characters: the board holds a version of at most `kFwVersionMax - 1` = 23 characters, and
  the manifest's version is the image's, i.e. the tag **with** its `v` (the
  catalog's drops it). A 24-character tag would build, sign and publish, be
  listed, and then fail every download with `err=mismatch`. crosspad.app and the
  local test server skip such a tag, and a malformed one, the same way
  (`tools/tests/fw_tag_rules.json`). A
  stale `manifest.sig` is removed before signing.
- `manifest.sig` = DER ECDSA P-256 over SHA-256 of the exact bytes of
  `manifest.json` (`openssl dgst -sha256 -sign`). One signature covers the
  package: the manifest carries every file's hash. `tools/fw_sign.py` does it
  all through the openssl CLI (no Python crypto package) and refuses any key
  that is not prime256v1.
- Device check: `main/fw_verify.cpp` (`mbedtls_pk_parse_public_key` +
  `mbedtls_pk_verify`) against each key in `main/fw_sign_pubkey.h`.
- `main/fw_sign_pubkey.h` is generated:
  `python3 tools/fw_sign.py pubkey-header -o main/fw_sign_pubkey.h config/fw_signing/*.pub.pem`
  (at most 2 keys). `tools/tests/test_fw_sign.py` fails if the committed header
  was made from any other key; the release job fails if no key in the header
  verifies the release's own signatures (`fw_sign.py verify-header`).
- A header with no key (`FW_SIGN_KEY_COUNT 0`) refuses every package.

## Keys and rotation

- **No agent, tool or script ever creates, reads or stores the private key.**
  It exists only in the Actions secret `FW_SIGNING_KEY` and the maintainer's
  offline copy. Only the public key is committed.
- How the key in use was set up (2026-10-03): the maintainer generated it
  offline, set the secret, took the private key off the disk, and committed
  `config/fw_signing/fw_sign_0.pub.pem` with the header generated from it
  (`FW_SIGN_KEY_COUNT 1`, fingerprint `30b21f3eebabb5d8` in the header's comment).
- Make one: `openssl ecparam -name prime256v1 -genkey -noout -out fw_sign_N.key.pem`,
  `openssl ec -in fw_sign_N.key.pem -pubout -out config/fw_signing/fw_sign_N.pub.pem`,
  `gh secret set FW_SIGNING_KEY < fw_sign_N.key.pem`, then move the `.key.pem`
  off the machine.
- Rotate: commit the new public PEM, regenerate the header (two keys), release
  once still signing with the old key; then set the secret to the new key, drop
  the old PEM, regenerate, release. A board that skipped the bridge release
  needs a package from the card or CP Tools.
- The release step writes the secret to `$RUNNER_TEMP`, uses it and removes it
  in one shell step with a `trap … EXIT`, so a self-hosted runner whose job dies
  there keeps no key on disk.

## CI and release files

- `tools/ci_matrix.py [--all]`: every `config/profiles/*.json` (official) and
  `config/profiles/community/*.json` (community) × `v1`,`v2`; kit names
  `^[a-z][a-z0-9_]{0,14}$`, never `v<digits>`, `name` equal to the file name.
  `--all` on tags, manual runs and PRs touching `config/profiles`; any other
  push builds only `full`. Every profile is validated either way. The root
  `CMakeLists.txt` refuses a bad `-DCROSSPAD_FW_KIT` the same way.
- Each cell: `python3 tools/app_manager.py profile apply <profile> --remove-extra`
  (pinned apps are protected and stay, as locally), then
  `idf.py … -DCROSSPAD_FW_KIT=<kit> [-DPROJECT_VER=<tag>]`.
- `tools/ci_release_assets.py` → per cell `CrossPad-<tag>-<rev>-<kit>.bin`,
  `-assets.bin`, `-manifest.json`, `-manifest.sig`; the `full` kit also under the
  old names (`CrossPad-<tag>-<rev>.bin`, `-assets.bin`, `-bootloader.bin`,
  `-partition-table.bin`, `.elf`); `CrossPad-<tag>-kits.json` =
  `{"v":1,"tag":…,"kits":[{"kit","tier","revs"}]}`. It refuses a cell whose image
  kit is not the cell's or whose version is not the tag. Files are staged in a
  temporary directory and moved only when every cell passed.
- `config/profiles/full.json` must equal the checked-in app set
  (`tools/tests/test_ci_matrix.py`).
- **A tag makes a draft release** (pre-release when the tag has a `-`). The
  catalog lists published releases only, so a release reaches boards when a
  person publishes it.
- **Self-hosted runner.** With the repository variable `CI_RUNNER` set
  (`tools/ci_runner.py on`) `matrix` and `build` run on the self-hosted
  runner, in the `crosspad-ci` container (`tools/ci/`), because the build
  leaves root-owned files (`build_*`) in the shared workspace that a checkout
  on the host cannot clean. A tag's build mounts no
  cache volume and compiles without ccache. **`release` runs on GitHub's
  runner** (same image): it holds `FW_SIGNING_KEY` (written to the
  container's `/dev/shm` for the one step) and a `contents: write` token, and
  on the self-hosted runner every job's container gets the host's Docker
  socket. Only while GitHub's runners cannot start jobs (Actions billing) does
  the repository variable `CI_SECRETS_RUNNER=self-hosted` put it on the
  self-hosted runner; unset it again after. It adds
  `git config --global --add safe.directory "$GITHUB_WORKSPACE"` before its
  own `git submodule status`: the container's root does not own the checkout,
  and git refuses it otherwise ("dubious ownership").
- **Submodule commits must be on GitHub.** CI checks out the core commit the
  platform pointer names; a core commit that exists only locally fails every
  build at checkout. Push the
  core branch before the platform tag or PR.
- Test releases: **`v1.1.0-fwstore.3`** (0d95d71), a draft with 30 files,
  channel beta, the v2 `full` manifest verified against
  `main/fw_sign_pubkey.h`; `fwstore.4` adds the icons and kit text the
  Firmware app's test uses.

## crosspad.app `/api/device/*`

| Route | Answer |
|---|---|
| `GET /api/device/ping` | `{"ok":true}`, `Cache-Control: no-store` |
| `GET /api/device/catalog?rev=v2` | `{"v":1,"generated","kits":[{"kit","versions":[{"version","tag","channel","tier","bin":{"name","size"},"assets":{…},"manifest","sig"}]}]}` -- published (non-draft) releases only, newest first, ≤ 16 kits × 8 versions, channel from the tag (a `-` = `beta`, GitHub's pre-release flag is not read); 400 without `rev=v<N>` |
| `GET /api/device/file/:tag/:name` | 200 / 206 + `Content-Range` / 416 (`bytes */<size>`); `ETag` = SHA-256, `Accept-Ranges: bytes`; 404 for a name the release index does not list or a draft release's file (the same releases the catalog lists) |

The catalog is not signed -- a broken Worker
can hide updates, never install one.

`/api/device/*` bypasses Cloudflare bot management: a board
is a bot by every measure and cannot solve a challenge. `/file` loads the whole
asset (up to 3.5 MB) per request, so it is the route to rate-limit or cache at
the edge if that bypass is ever abused.

## On the device

| Module | Does |
|---|---|
| `main/fw_http.cpp` | one GET into a buffer, one ranged GET into a sink; `https://` with the certificate bundle, `http://` allowed (integrity is the signature's) |
| `main/fw_catalog.cpp` | GET the catalog for `v<board_rev>` (body capped at 64 kB, PSRAM); write `.catalog.json` |
| `main/fw_verify.cpp` | the manifest signature against the compiled-in keys |
| `main/fw_download.cpp` | core `FwDownloader` on the board: card (stdio), `fw_http`, mbedtls SHA-256, `fw_verify`, `fw_gc`, the blocklist's lock and the log lines |
| `main/fw_gc.cpp` | the space cleanup (core `FwLibraryGc::select`) |
| `main/fw_updater.cpp` | state, NVS `fwupd`, the job task, background triggers |
| `main/fw_updater_verbs.cpp` | the verbs below |
| crosspad-gui `src/fw/fw_updates_view.cpp` | the Updates view (Firmware app; network-mode screen's Updates tab) |
| `main/app/settings/settings_updates.cpp` | Settings → Updates |

Pure logic is in crosspad-core `crosspad/fw/` (`Semver`, `HttpDate`, `FwKit`,
`FwSchedule`, `FwCatalog`, `FwManifest`, `FwLibraryGc`, `FwDownloader`),
unit-tested. `FwDownloader` is one version → package: manifest + sig, verify,
match, cleanup, `.part` files with `Range`, SHA-256, `.complete`, the
blocklist; it reaches the card, HTTP and the hash only through `IFwFiles`,
`IHttpStream` (`crosspad/net`) and `ISha256` (`crosspad/util`). The device
keeps a compact list of the last fetch (`FwCatalogEntry`, ~6 kB PSRAM), not the
~50 kB parsed catalog.

Tasks:

| Task | Stack | What runs there |
|---|---|---|
| `fw_upd` (job) | 8192 B **PSRAM**, prio 1, core 0 | HTTP, TLS, FATFS. Never flash: no NVS, no partition reads |
| caller of `start_job()` | the caller's (LVGL thread, main task, bsp_worker) | the compare-exchange that admits one job, the WiFi lease, the slot package names (`fw_slots_get()`) |
| `fw_upd_nvs` | 3072 B internal, one-shot | every NVS write of `fwupd`, and the hand-over to `host_info_set_latest()` (which writes NVS too) |
| `fw_upd_inst` | 6144 B internal, one-shot | `fw_install_start()` (image header off the card, OTA partition, the STM link); the slot write then runs on fw_slots' own install task |
| `fw_upd_idle` | a bsp_worker job, 1 s | the boot-check hold and the daily rule; reads state, at most starts a job |

The lease is taken on the caller's task, so a refusal answers at once
(`ERR no_mem`, a toast, the network-mode offer); the job releases it, and a
download releases it as soon as the package is whole -- install and the wait
for *Run* need no radio. A download takes the lease up front only for a
package this boot's last fetch saw missing from the card. One it saw whole, or
one it does not know (the list is RAM, empty after a reboot), may install with
WiFi off: the job verifies the card first and leases only when it must fetch;
a refusal there ends the job `failed` with the lease's reason as `err=` (the
views offer network mode on `no_mem` as for a check, `FW_DOWNLOAD` has
already answered `OK`).

## When it checks, how it installs

- **Background** (normal boot only, never in network or USB transfer mode):
  - the boot check, once the launcher has painted, when *Check at startup*
    (`boot_check`) is on;
  - daily when `last_check` is ≥ 24 h old or unknown and the launcher has been
    in front for 2 min with no pad press (core `fwDailyCheckDue`);
  - "launcher in front" = GUI up, no running app, **no kit selector open** (the
    gate a kit-less Sampler opens has no running app), no firmware flow
    overlay, not in USB transfer or network mode.
- **Background checks are silent.** A refused lease, no network, a failed
  fetch: logged, nothing shown, `last_check` and the cache untouched, the next
  try waits an hour (`kFwRetryAfterFailS`). Only a completed check publishes
  (state, `latest`, glyph, cache). A background check never offers network mode
  and never restarts. A user's `FW_CHECK` while it runs answers `ERR busy`.
- **Abandoned on activity**: a pad press (any source, the sequencer's
  included), an app opening, the kit selector opening. The previous state stays,
  nothing persists, the attempt counts as failed (an hour). The cancel flag is
  read between the link wait's 250 ms slices, between HTTP reads and before
  persisting.
- **Held on a trial boot.** On the first boot of a freshly installed or
  switched-to image (`fw_running_on_trial()`, PENDING_VERIFY until the confirm
  task marks it valid, ~12 s) no background check starts: a crash in the radio
  or TLS would roll a good image back. The boot check is held and started by
  the idle job on its first tick after the confirm, under the same
  launcher-in-front rule (dropped if the board is in use by then; the daily
  check covers it). Measured: held at 7.5–7.8 s, confirm at ~12.0 s, lease at
  12.5–12.8 s, after an OTA trial and after an `FW_SWITCH` alike.
- **Held until the boot's kit scan and kit load are done**
  (`g_kit_scan_done`, `kit_load_task_is_loading()`): otherwise the radio and
  the scan's file handles take internal RAM at the same moment and newlib
  aborts on a FILE lock it cannot have (`kit_scan`, `lock_init_generic`, about
  one boot in five). The idle job starts it as for a trial boot; a kit load still
  running is waited out, not taken for a player. Measured: held at ~5.0 s,
  `Discovered 205 kits` at ~7.7 s, lease at ~8.6 s; 27 boots, no abort.
- **Held for a card that mounts late, and never beside a rescan** (2026-10-30):
  a boot that mounted no card holds the boot check until the SD hotplug probe
  has tried once more (`g_sd_probed`, ~1 s after `All systems operational`).
  A card the probe mounts later is rescanned (`kit_rescan`) only while the
  radio is off (`wifi_link_radio_idle()`) and no updater job runs; until then
  the kit list reads as scanning, which holds the boot and daily checks in
  turn. Without this a card that came up a second late was rescanned under the
  boot check's radio and aborted the same way, in `kit_rescan`.
- **User**: Settings → Updates → *Check now*; the Updates view checks when it is
  shown (at most once a minute, not while a job runs or an install waits for
  *Run*, not on a trial boot) and on *Check*; `FW_CHECK`. A press or a verb is
  never held on a trial boot.
- **Install** (manual): *Install update* -- a *Replace / Cancel* question first
  when the slot not running holds another kit (an empty or rolled-back slot,
  or one with this kit, is replaced without asking) -- download → verify →
  install → *Run the new version now?* (*Run* = `fw_flow_switch_and_restart()`;
  *Later* keeps the *Run the new version* row).
- **Already installed.** fw_slots keeps a RAM copy of the other slot's kit and
  version while it can boot (installed, trial, confirmed);
  `fw_slots_other_holds()`. A check that finds an update the other slot already
  holds ends `ready_to_restart`, and so does `fw_updater_init()` when the
  persisted `latest` is in the other slot: the view offers *Run*, not *Install*,
  also after a restart. A rolled-back slot does not count.
- **A version that did not start is not offered again.** When fw_slots marks
  the other slot rolled back, `fw_updater_init()` records its kit and version
  (`rb_kit` / `rb_version`, persisted); a check skips exactly that version of
  that kit and offers the next newer one when it appears (an older one still
  newer than the running image is offered meanwhile). Only checks skip it:
  `FW_DOWNLOAD` can still install it. A `latest` persisted before the rollback
  stays until the next check. The record is dropped (NVS through `persist()`)
  when it no longer applies: an Install of that kit and version through the
  updater succeeds (the user wants it: checks offer it, and *Run*, again); at
  init the other slot is no longer rolled back (overwritten, or that build
  installed again some other way -- the Firmware app's package list,
  `FW_INSTALL` -- which takes effect at the next boot); or the running image
  *is* that kit and version and has confirmed itself -- at init on a normal
  boot, or by the idle job once a trial boot's confirm has landed.
  `fw_updater_forget()` clears it too.
- **Fallback**: a user's check or install refused with `no_mem` (the lease,
  the radio's own memory gate after it, or a job's own heap -- any `no_mem*`)
  offers *Restart in network mode* on the Updates tab
  (`wifi_fallback_offer(NET_TAB_UPDATES)`), where the same view runs and checks
  when the tab is shown; *Exit* returns to a normal boot.
- **Refusals the caller answers leave the state alone.** A press or a verb gets
  the refusal back (`busy`, `disabled`, `no_mem`, …) and answers it as a toast,
  the network-mode offer or an `ERR` line; the status rows keep showing the
  last result. The view's automatic check on opening passes a refusal buffer
  too and shows it passively: a lease refused there (WiFi off, `no_mem`)
  puts the reason in the status line (`fw_updates_error_text()`, e.g. "WiFi is
  off - see Settings > WiFi") until a check or install runs, and offers
  nothing -- only a press leads to a question. A failure after the lease was granted
  (no network, wrong password, the radio gate) is a `failed` state with `err=`.

## Persisted state

- NVS `fwupd`: `latest` (newest update of the running kit, `""` = up to date),
  `last_check` (server `Date` of the last good check, unix s; left as it was
  when a response carries no usable `Date`), `checked` (1 once any check has
  succeeded, dated or not; a board saved before it reads `last_check > 0`), `boot_check`
  (default on), `url`, `beta_kits` (space-separated kits on the beta channel --
  one key because NVS keys are ≤ 15 characters), `rb_kit` / `rb_version` (what
  the other slot last rolled back from, while that still applies, see above). Both slots share it. Factory
  reset erases it (`NvsSettingsStore::eraseAll()` lists the namespace,
  `onFactoryReset()` and `SETTINGS_RESET` call `fw_updater_forget()`, which
  clears the RAM copies under the updater's lock and makes a running job's
  result stale).
- **The board's own check is authoritative.** After every completed check the
  writer hands `latest` to `host_info_set_latest()`, which replaces the known
  version (`""` included, so the glyph also goes out after beta → stable or a
  pulled release) and recomputes `status.fwLatest` / `fwUpdateAvailable`
  (orange USB glyph, Settings → Info). The hub's kit-agnostic `FW_LATEST` goes
  through `host_info_offer_latest()`, which only raises the version and is
  **ignored once `checked` is set** (`fw_updater_checked()`), not on
  `last_check`: a check whose response had no `Date` still counts, so a hub
  cannot relight the glyph over it. The hub informs only a board that has
  never checked.
- `/sdcard/crosspad/firmware/.catalog.json` =
  `{"v":1,"fetched":<unix>,"rev":"v<N>","catalog":<the Worker's JSON>}`, written
  after every good fetch (temporary name + rename, in 4 kB pieces yielding to
  the streamer). A dot name: the scan and the cleanup skip it. The Firmware
  app's views read it when WiFi is unavailable.
- RAM only: the state, the error, `ready_to_restart` (rebuilt at boot from the
  other slot, see above), `gc_mb=`, the blocklist.

## Versions and channels

`crosspad::Semver`: `major.minor[.patch][-pre]`, `v` ignored, `-dirty` and
`+build` ignored for order (`+build` must still be dot-separated
`[0-9A-Za-z-]`, like a pre-release), a `git describe` suffix = commits after the tag (above that
tag, below the next version or pre-release); pre-release < release;
`rc9 < rc10`. An update is a version of the running kit and revision, strictly
newer, in the kit's channel (`beta` sees `stable` too), not blocked this boot.
An unparseable running version counts as 0.0.0. The catalog's channel is
checked against the signed manifest's: a beta build cannot pass for stable.

**One channel rule: a tag containing `-` is `beta`, any other is `stable`.**
CI signs that channel into the manifest (`make_fw_package.tag_channel()`),
crosspad.app lists it in the catalog and the local test server serves it; none of
them reads GitHub's pre-release tick, so promoting an `-rc` or leaving a final
release ticked cannot make the catalog disagree with the signature (the board
would answer every download `err=mismatch`). `tools/tests/fw_tag_rules.json`
holds the cases; the Worker and crosspad-hil test the same file, copied.

Every version from outside -- a catalog entry, a manifest, the hub's
`FW_LATEST`, a verb argument -- must pass core `fwVersionValid()`: a `Semver`
that is also an `fwSafeName` (`A-Za-z0-9._+-`, no leading `.`) of at most 23
characters (`kFwVersionMax - 1`; a manifest's version keeps the tag's `v`, so
that bounds the tag too). Anything else is skipped or refused before it can
reach NVS `latest`, `status.fwLatest`, a verb line or the LCD.

## Card cleanup

Library ≤ 512 MB (`FW_UPD_CFG gc_mb=` overrides until reboot), card ≥ 10 % free.
Never deleted: the packages named in slot A/B (fw_slots `a_pkg`/`b_pkg`, by name
-- the hash fw_slots stores is the partition's, not the file's), those with
`.pinned`, the download in progress. Oldest version within a kit first, then the
oldest directory across kits; at most 32 candidates, everything else (dot files,
past the cap) still counts against the limit. Unreachable → `card_full`, nothing
deleted. Runs before a download (for what it still needs) and after it --
**only after the manifest's signature and match passed**: the catalog's sizes
are unsigned and must not decide what gets deleted.

## Package markers

`.downloading` is written before the first file, every file is `<file>.part`
until its SHA-256 matches and it is renamed, `.complete` comes last and then
`.downloading` goes. A directory with `.downloading` and no `.complete` is a
download to resume (`FW_LIST verdict=downloading`), never installable. A
complete package that no longer verifies is re-downloaded file by file: only
its `.complete` goes (after the fresh manifest verifies and the cleanup found
room, so `card_full` leaves it complete), so a package a slot holds is never
emptied.

## Verbs

| Verb | Reply |
|---|---|
| `FW_UPD_STATUS` | `FWUPD: state=… running=… kit=… latest=…\|- channel=… progress=… last_check=… err=…\|- stack_free=… inst_stack_free=…` |
| `FW_CHECK` | `OK` / `ERR busy` / `ERR <lease refusal: no_mem, disabled, …>` |
| `FW_CATALOG [<kit>]` | `FWCAT: count=<shown> total=<matching> \| <kit> <ver> <channel> <tier> <on_card>…` (≤ 448 B; this boot's last fetch) |
| `FW_DOWNLOAD <kit> <ver> [INSTALL]` | `OK` / `ERR busy\|blocked\|bad_args\|<lease refusal>`; `v1.3.0` = `1.3.0` |
| `FW_UPD_CFG [boot=0\|1] [gc_mb=<n>]` | `OK` (boot persisted, gc_mb RAM only); a typo changes nothing |
| `FW_UPD_CHANNEL <kit> stable\|beta` | `OK` (persisted) / `ERR bad_kit` |
| `FW_UPD_URL <url>\|DEFAULT` | `OK` (persisted, `http(s)://` with a host) / `ERR bad_url` |

All work in a normal boot and in network mode. `FW_UPD_STATUS`, `FW_CATALOG`
and `FW_CHECK` are companion-allowed (BLE / SysEx `0x1F` / WiFi remote;
`FW_CHECK` answers at once and leases WiFi on the job task, `FW_DOWNLOAD`
stays CDC-only). **Over BLE, `FW_CHECK`'s WiFi lease stops BLE while the check
runs:** the link may drop (see [ble-midi.md](ble-midi.md)); over SysEx on USB
nothing does. The companion mailbox is 481 B,
hence `FW_CATALOG`'s 448 B cap and its kit filter. `stack_free=` is the last
job's unused stack, `inst_stack_free=` the last install-start task's -- the
measurements below.

## Failure rules

| Situation | Outcome |
|---|---|
| Background: lease refused / no network / board in use | nothing shown; next trigger (an hour later after a failure) |
| User: lease refused with `no_mem` | the *Restart in network mode* offer (`ERR no_mem` from a verb) |
| User: lease refused otherwise (WiFi off, …) | a toast / `ERR disabled`; state unchanged |
| The view's automatic check: lease refused | the reason in the status line, no offer, no toast; state unchanged |
| User: no network / wrong password / link timeout | `failed`, `err=no_network\|auth\|wifi_timeout`; nothing persisted changes |
| Catalog unreachable / refused / unparseable | `err=catalog:network\|http\|parse\|no_mem`; cache untouched |
| `FW_DOWNLOAD` of a version the catalog does not have | `err=not_in_catalog` |
| Connection dropped mid-file | resumed twice in the job (2 s apart); else `err=network`, `.part` kept |
| Server ignores `Range` (200) / a 206 for another range / 416 | that file starts over, never appended |
| SHA-256 mismatch | `.part` deleted, one fresh retry, then `err=sha`, the package removed (unless protected), version blocked until reboot |
| Bad signature | nothing written to the card, `err=signature`, blocked until reboot |
| Missing manifest or signature on the server | `err=http` (or `network`) |
| Manifest ≠ catalog entry / board / channel / file sizes | nothing written, `err=mismatch` |
| A catalog name unsafe for a path or URL | `err=mismatch` |
| Card full after cleanup | `err=card_full`, card unchanged |
| `fw_install_start()` refuses / the install fails | `err=install:<reason>`, package kept |

## Measured numbers (v2, 2026-10-04)

| What | Value |
|---|---|
| Job stack (8192 B PSRAM), check over **TLS** to crosspad.app (a 404, so handshake + response, no catalog parse) | 3160–3192 B free |
| Job stack, check against the local test server (http) | 4188–4380 B free |
| Job stack, download / resume / no-range / install | 3092–3132 B; with a cleanup 2804 B (the lowest seen); signature/mismatch refusals 3948–3980 B |
| Job stack, abandoned background check | 5004 B |
| `fw_upd_inst` (6144 B internal) | 2880–2892 B free |
| `wifi_ctl` | 1172–1208 B free at 4096 (~150 B above the 1024 B line) → **2196–2204 B** at 5120 (`kTaskStackHeadroom`, +1 kB; peak is `esp_wifi_init()`, see wifi.md "Stacks") |
| Main task at `wifi_link_acquire()` under `FW_CHECK` | 2220 B free (28 B with the verbs' buffers on the stack, see Traps); high-water 620–668 B |
| PSRAM at radio start, Firmware app open, kit loaded, BLE on | `psram_init=449000` against the 116736 B gate (+332 kB); never `no_mem` in that run |
| Same on an earlier, larger image | `psram_init=101592` → `no_mem` on most opens: that image could not run the check from the Firmware app |
| MEM, launcher, kit, BLE on | psram_free 535312 (largest 270324); Firmware app open 497456 (286708); radio up 446948 |
| Download, bin 3086384 B + assets 1048576 B over WiFi | ~150–165 kB/s (~25–28 s), all 200, no resume |
| Install into the other slot | ~16.5 s |
| Boot check after an OTA / a switch (trial) | lease ~0.6–0.9 s after the confirm, link up ~3.5 s later |
| `ext_ram.dummy` (2026-10-05) | v2 2686944 (41 pages, **712 B** text headroom); v1 2621408 (40 pages, ~32 kB) |

## Known limitations

- **crosspad.app does not serve the routes yet**: until the Worker is deployed
  every check against the default URL ends `err=catalog:http`. Tests use
  `FW_UPD_URL` with the local test server.
- **A two-part version is accepted**: `Semver` reads `9.9` as 9.9.0 on purpose,
  so `FW_DOWNLOAD full 9.9` answers `OK` and the job then fails on its own
  (`not_in_catalog`). If the catalog has `x.y.0`, the job continues with the
  catalog's own string (`x.y.0`) for the package directory, the install and
  `FW_UPD_STATUS`'s target.
- **The HTTP connect / header phase cannot be abandoned**: the cancel flag is
  read between reads, so a pad press during a TLS handshake takes effect after
  it (seen: an abandon logged ~6.5 s after the kit selector opened).
- **An abandoned background check leaves the radio connecting**: it comes up
  and lingers its 30 s with `leases=0`.
- **A press is not held on a trial boot** -- background checks and the
  Updates view's automatic check are. A view opened in the first ~12 s after a
  switch does not check on its own until it is shown again; *Check* checks at
  once.
- **WPA3-SAE CacheError panic, seen once.** On the release image
  `v1.1.0-fwstore.3` (0d95d71), in its trial boot, the boot check's SAE commit
  panicked in the WiFi task (`cause=71` CacheError in
  `mbedtls_zeroize_and_free` ← `mbedtls_mpi_grow` ← … ← `sae_prepare_commit`,
  a free of a valid PSRAM address; no flash operation in progress, the wifi
  task 3220 B from its stack end). The bootloader rolled the slot back as
  designed. Not reproduced on the current image: 12 of 12 cold SAE connects
  (`WIFI_UP` / `WIFI_DOWN` cycles, each a fresh commit because
  `esp_wifi_deinit` drops the PMKSA cache, `COREDUMP_INFO` seq unchanged). Cause
  not found; the only WiFi config difference was SAE-PK (on in that image, off
  now, not on the stack). The trial-boot hold above is the structural answer.
- **`SCREENSHOT` needs one free 230400 B PSRAM block.** After a radio cycle the
  largest block drops to ~225 kB and stays, so screenshots answer `no memory`;
  `SCREENSHOT` also never captures `lv_layer_top` (dialogs, toasts). Prove
  dialogs with `ENC_GROUP` / `ENC_FOCUS` instead.
- **No "fresh slot" marker.** IDF 5.5.4's `esp_ota_begin()` calls
  `esp_ota_invalidate_inactive_ota_data_slot()` with rollback enabled
  (`app_update/esp_ota_ops.c`), which erases the otadata entry that marked the
  slot invalid; after a reinstall `esp_ota_get_last_invalid_partition()` no
  longer names it, so a reinstalled rolled-back slot is never re-marked
  RolledBack and no false "did not start" toast appears.
- No touch-injection verb: the views were driven by encoder only.
- v1: built, not measured.

## Release automation

An official app's version bump becomes an installable, signed beta without a
human step; stable stays manual.

**The chain**

1. An app repo pushes a new `crosspad-app.json` version; the shared workflow
   `CrossPad/.github/.github/workflows/app-release.yml@v1` (`GITHUB_TOKEN`
   only, no bot key in app repos) tags `vX.Y.Z` and publishes a prerelease.
   The announcer Worker relays it as `app-released` to crosspad-apps and here.
2. `app-bump.yml` (relay, hourly cron `17 * * * *` as backstop) keeps one PR
   on `auto/app-bump`: `chore(deps): <component> X→Y, …`, moving app (and,
   via `deps_resolve.py`, core/gui) gitlinks and the profile pins. Bot-signed.
3. `bump-gate` judges that PR; once green, auto-merge (`APP_BUMP_AUTOMERGE`).
4. `auto-tag.yml` tags the next `vX.Y.Z-rcN` after the bump merge's green main
   run (`tools/next_rc_tag.py`, `tools/auto_tag.py`); only bump merges.
5. `main.yml` builds the tag, the `release` job (environment `firmware-release`,
   provenance check first) signs, verifies the upload and publishes an rc;
   then `retention` and `catalog-check` (crosspad.app lists it). Stable and non-`-rcN` tags stay drafts.

**Kill switches** (platform-idf repository variables; unset = step skipped)

| Variable | Values | Off means |
|---|---|---|
| `APP_BUMP_AUTOMERGE` | `on` | the bump PR waits for a human merge |
| `AUTO_TAG` | `on` | merged bumps are not tagged (dispatch "Auto tag" by hand) |
| `AUTO_PUBLISH` | `on` | the signed rc stays a draft |
| `RETENTION` | `on` / `dry-run` | no unpublishing / plan only |
| `APP_BUMP_HOLD` | app ids, comma list | listed apps are left out of the next bump |

Turning one off strands nothing: the next run resumes from repo state.

**Alerts** go to the CI alerts channel (`DISCORD_CI_ALERTS_WEBHOOK`; unset = log only):
bump PR red / stuck, needs-a-human, auto-tag failed, main red, cancelled
bump-merge run, tag build red, provenance refused, upload incomplete,
retention over `--max`, catalog missing.

**What a human still does**

- Tags stable (`vX.Y.Z`), starts a new minor/major line, publishes stable drafts.
- Merges bump PRs while `APP_BUMP_AUTOMERGE` is off; closes a foreign PR on
  `auto/app-bump` (the reconciler stops, by design).
- Sets `APP_BUMP_HOLD=<id>` for a bad app version.
- Unpublishes a bad rc: `gh release edit <tag> --draft=true` (never delete).
- Dispatches "Auto tag" with the newest main sha when a bump merge's run was cancelled.
- Owns the keys, rulesets and the variables above.

**Not proven yet:** end-to-end timing (the design budgeted ~30 min; unmeasured
until the first auto-tagged rc) and `release_check` phase B (install, switch,
switch back), which has never run on hardware.

## Tests

- Core: `components/crosspad-core/tests` `[fw]` (`test_fw_semver`,
  `test_fw_catalog`, `test_fw_kit`, `test_fw_schedule`, `test_fw_gc`,
  `test_fw_downloader` with fakes for the card, HTTP and the hash). Host:
  `python3 -m pytest tools/tests`. Worker: `pnpm vitest run test/unit/device-api.test.ts`.
- Local test server (serves a real release's files, cached under
  `~/.cache/crosspad-hil`, answering exactly like the Worker):
  `python -m crosspad_hil.fw_server --tag v1.1.0-fwstore.3 --kit full --rev v2 --port 8765 --host-ip <host-ip> [--fault drop|flip|ignore_range]`
  (from the crosspad-hil venv), then `FW_UPD_URL http://<host-ip>:8765` (persisted; `FW_UPD_URL DEFAULT` after).
  `<host-ip>` is the host's address on the network the board joins; the board needs a saved
  network (`WIFI_ENABLE 1`, `WIFI_SET` with credentials from `CROSSPAD_WIFI_SSID` /
  `CROSSPAD_WIFI_PASS` in the environment, never in a repo).
- Scenario: `python3 tools/bench.py test fw_update` (tls, catalog, resume,
  signature, mismatch, no_range, cleanup, install, sha, then the download in
  `NET_MODE 1 TAB=updates` and the boot check after it; host IP from the route
  to the board, `--host-ip` overrides; `--switch` adds the `FW_SWITCH` round
  trip); the scenario is crosspad-hil's
  `fw_update`.
- To see an update on a development build (always newer than a release): build with
  `-DPROJECT_VER=v0.9.0` and set `FW_UPD_CHANNEL full beta` for a pre-release
  tag; then `cmake -U PROJECT_VER .` **inside** `build_v2` (run from the
  worktree root it leaves a stray `app_versions.cpp` and `generated/` there).
- `RESET_LOG`'s ring is frozen at n=255 (earlier code): use `COREDUMP_INFO seq=`
  as the panic witness across a run.

## Traps found on hardware

- **Any verb that reaches `wifi_link_acquire()` from the main task keeps its
  buffers off the stack.** The main task has 4 kB: under
  `hil_control_handle_cdc()`'s 1216 B frame the lease's `ESP_LOGI` needs
  1552 B, so with `fw_updater_handle()` at 544 B (inlined status line, refusal
  buffers, `FW_UPD_CFG`'s parse buffer) and `start_job()` at 256 B (an inlined
  `FwSlotInfo[2]`) 28 B were left and `FW_CHECK` / `FW_DOWNLOAD` over CDC
  overflowed it (`task=main cause=65` = the end-of-stack watchpoint, the
  backtrace a printf deep inside `wifi_link_acquire()`'s log line). Every reply
  line and refusal is built in the verbs' static `s_line`; `reply_status()`,
  `apply_cfg()` and `read_slot_packages()` are `noinline` → 96 B and 64 B
  frames, +640 B at the acquire point, 0 B internal RAM. `WIFI_UP` takes the
  same lease from a 656 B shallower frame.
- **The job's PSRAM stack must not touch flash**: NVS through `fw_upd_nvs`,
  slot names and the lease from `start_job()`, `fw_install_start()` on
  `fw_upd_inst`, the other slot's kit/version from fw_slots' RAM copy.
- **A lease granted is not a radio started**: the control task's own memory
  gate can still fail, and the job then ends `err=no_mem` -- the views offer
  network mode for that too.
- **A crash in the boot check of a trial image is a rollback.** Hence the hold
  until the confirm.
- **The kit selector is not an app**: with no kit loaded `APP_START Sampler`
  opens the selector with `running=-`, which would look like an idle launcher,
  so `kit_selector_is_open()` is part of the rule. After a reset the board comes
  up with no kit.
- **Applying a profile dirties the tree**: without `-DPROJECT_VER=<tag>` the
  image calls itself `<tag>-dirty`, a version no catalog names (the release job
  refuses it).
- **The `lite` kit keeps `instructions`**: pinned apps are protected from
  `--remove-extra`, in CI as locally.
- **A kit named like a revision** (`v3`) would break the release file names --
  `ci_matrix.py` and CMake refuse it.
- **v2 is 712 B from its next 64 kB PSRAM page** (`ext_ram.dummy`, 2026-10-05;
  v1 is in its 40th page with ~32 kB): check
  `xtensa-esp32s3-elf-size -A build_v2/CrossPad.elf | grep -E 'flash.text|ext_ram.dummy'`
  after adding code.
