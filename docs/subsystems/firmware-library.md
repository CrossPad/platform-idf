# Firmware Library and A/B Switch

> Source of truth for this subsystem. `CLAUDE.md` carries only the summary; when behaviour changes, update this file and the summary together.

CrossPad keeps several firmwares on the SD card (sampler, DAW-only, a future
kit-specific build, …), installs one into a resident OTA slot in a few
seconds, and switches which slot boots next with nothing more than a restart.
Each firmware keeps its own settings; a firmware that does not start cannot
strand the device — the bootloader rolls back on its own.

Design intent: `docs/design/2026-09-21-firmware-library-ab-switch-design.md`
(carries an "As built" note at the top listing where the shipped code diverges
from that design). Code: `main/fw_image_desc.{h,c}`, `main/fw_library.{h,cpp}`,
`main/fw_slots.{h,cpp}`, `main/fw_assets_sync.{h,cpp}`, `main/app/update/update_app.cpp`
(the Firmware app), `main/hil_control.cpp` (`FW_*` verbs), `main/collect_assets.cmake`
(assets id), `tools/make_fw_package.py`, `tools/fw_image_desc.py`,
`components/crosspad-core/include/crosspad/settings/SettingsScope.hpp` (shared
settings), `components/stm32_hal/stm32_fw_update.c` (never-downgrade gate).

## What an image says about itself

Every image carries a custom descriptor, `CPFI`, placed right after ESP-IDF's
own `esp_app_desc_t` — a fixed file offset of **0x120** (`FW_DESC_FILE_OFFSET`,
`main/include/fw_image_desc.h`):

| Field | Type | Meaning |
|---|---|---|
| `magic` | `uint32_t` | `'CPFI'` (`0x49465043`) |
| `desc_version` | `uint8_t` | layout version (`1` today) |
| `board_rev` | `uint8_t` | 1 or 2, from the board revision the build was configured for |
| `stm_proto_min` | `uint16_t` | the STM32 protocol this firmware needs |
| `kit_id[16]` | `char[]` | firmware kit name (`"full"` — no per-kit builds exist yet, `CROSSPAD_FW_KIT_ID` is a compile define with that fallback) |
| `assets_id[32]` | `uint8_t[]` | SHA-256 over the staged asset files this image was built with |

The linker places `.rodata_custom_desc` right after `.rodata_desc`; a
`-Wl,-u,s_fw_image_desc` force-retain in `main/CMakeLists.txt` is required or
`--gc-sections` drops the whole struct silently (nothing in the retained
startup path calls `fw_image_desc_running()`, unlike `esp_app_get_description()`).
An image without the block (anything built before this feature) reads as
`FwPkgVerdict::UnknownBuild` — installable only with an explicit second
confirmation (`FORCE` over CDC, a stronger-worded dialog in the Firmware app).

**The image's own `CPFI` block is the authority**, always, over anything a
`manifest.json` claims. `tools/fw_image_desc.py` (host) and
`fw_image_desc_read_file()` (device) both read the same 56-byte struct at the
same offset, so host and device always judge an image by the same evidence.

## Package layout on the card

```
/sdcard/crosspad/firmware/<name>/
    CrossPad.bin      required
    manifest.json     name, kit, version, board_rev, stm_proto_min, assets_id, per-file sha256
    assets.bin        optional — the assets partition image this firmware was built with
    settings.json     optional — this firmware's exported settings (SettingsIo format)
    icon.bin          optional — the kit's icon, 48×48 LVGL RGB565A8 (6 924 B); listed and hashed in manifest.json
    .pinned           optional — the package is kept: the space cleanup never evicts it, the Firmware app offers no Delete
    .downloading      while a download runs; with no .complete the directory is a download to resume
    .complete         the download finished and every file matched the signed manifest
```

`icon.bin` comes from CI (`tools/fw_icon.py`, `make_fw_package.py`), so it is
signed with the rest; the Firmware app draws it on the package's tile and kit
card (format, validation and the 16 kB cap: [firmware-app.md](firmware-app.md)).

Beside the package directories, `firmware/` holds the library's own **dot
entries**, which `fw_library_scan()` and the cleanup skip because a name starting
with `.` is never a package:

| Entry | What it is | Written by |
|---|---|---|
| `.catalog.json` | the catalog cached by the last check | B's check job |
| `.icons/<kit>-<version>.bin` | the icon of each Store tile's version | the check job (`fw_store_files.cpp`), pruned to the catalog |
| `.details/<kit>.json` | `/api/device/kit/<kit>` verbatim (description, apps, notes), at most 16 kB | the check job, pruned to the catalog |
| `.community_ok` | kit ids whose community warning was accepted (one line, space-separated; shared by both slots) | the Firmware app's view worker |

A card without them is fine: the Store then says `Never fetched` and tiles keep
their first letter until the next check.

`tools/make_fw_package.py --build build_v2 --name <name> [--out DIR]` builds
`CrossPad.bin` + `assets.bin` + `manifest.json` from a build directory; it
refuses when the image carries no `CPFI` block or when the build dir's
`assets_id.txt` doesn't match the id baked into the image (a stale
`assets.bin` sitting next to a rebuilt `CrossPad.bin`). It never writes
`settings.json` — that file only ever comes from a device export during a
switch (see below), or a user editing one by hand.

**A bare `*.bin` dropped straight into `firmware/`** (USB Transfer mode, a
card reader, `fs_transfer.py push`) is folded into its own directory on the
next scan: `<stem>/CrossPad.bin`. `fw_library_scan()` (`main/fw_library.cpp`)
does this in **two passes**, deliberately: `fold_loose_files()` first
collects every loose `.bin` name via its own `opendir`/`readdir`/`closedir`,
fully closed out, and only then does `mkdir()`+`rename()` on each one; the
counted, directory-only scan opens `FW_PKG_ROOT` fresh afterwards. Folding and
listing inside one `readdir()` pass is unsafe — ESP-IDF's FATFS `readdir()` is
position-based, so a directory created mid-scan can be handed back a second
time in the same pass, inflating the count and duplicating a row.

A directory whose name does not fit `FW_PKG_NAME_LEN` is **skipped** with an
`ESP_LOGW`, never truncated: the scan judges a package through the real path
it walked, but every later call — the install included — finds it again by
the stored name, so a truncated one could install a different image than the
one that was listed.

`manifest.json` is a convenience for a host reading the package without
parsing a binary; the device never trusts it over the image's own `CPFI`
block, and `fw_install_task()` refuses an install when the manifest disagrees
with what it read straight out of `CrossPad.bin` (board_rev, kit, and — when
present — the file's own SHA-256).

**Markers.** A package downloaded by the updater (`main/fw_download.cpp`) gets
`.downloading` before its first file, is written file by file as `<file>.part`,
each renamed once its SHA-256 matches the signed manifest, and gets `.complete`
last, after which `.downloading` is removed. A directory with `.downloading`
and no `.complete` is a download to resume: `fw_library_scan()` reports it with
verdict `downloading`, never as installable. `.pinned` (any content) keeps a
package out of the space cleanup (`main/fw_gc.cpp`), which also never touches
the two slots' packages or the download in progress. Packages made by
`make_fw_package.py` and pushed by hand carry no marker and are judged as
before.

## Slot states and what moves them

Two resident OTA slots, `ota_0` (**A**) and `ota_1` (**B**), 0x380000 (3.5 MB)
each — a fixed mapping to partition subtype, independent of which one is
running (`partitions_16mb.csv`). One NVS namespace `fw_slots`; per slot:
`<label>_pkg` (package name, empty = unnamed), `<label>_sha` (running image's
SHA-256), `<label>_state`.

```
Empty → Installed → Trial → Confirmed
                       ↓
                   RolledBack
```

| State | Wire text | Set by |
|---|---|---|
| `Empty` | `empty` | never installed (the `fw_slots_init()` SHA-mismatch reconciliation only clears the package *name* — it leaves the state alone) |
| `Installed` | `installed` | `fw_install_task()` after `esp_ota_end()` succeeds |
| `Trial` | `trial` | `fw_switch_request()`, right after moving the boot partition, only when the target was `Installed` (a switch between two already-`Confirmed` slots has no trial); also `fw_slots_init()` on any boot where the running image is still `ESP_OTA_IMG_PENDING_VERIFY` — a plain OTA over CDC is a trial as much as a switch is |
| `Confirmed` | `confirmed` | `fw_confirm_running()` (10 s timer or `FW_CONFIRM`) once the running image is no longer `ESP_OTA_IMG_PENDING_VERIFY`; also `fw_slots_init()` promotes a *running* slot's own stale `Trial`/`RolledBack` on every boot — a slot that is demonstrably executing right now cannot be the one that "did not start last time" |
| `RolledBack` | `rolledback` | `fw_slots_init()`, when `esp_ota_get_last_invalid_partition()` names the *other* (non-running) slot as `INVALID`/`ABORTED`. Not a dead end: the Firmware app offers that slot again as **Try X again**, which calls `fw_slot_clear_rollback()` (back to `Installed`) before the normal switch — a firmware can fail on something since fixed, and the alternative is a slot the device can never use again |

**Installing never touches the boot slot.** `fw_install_start()` always
targets `esp_ota_get_next_update_partition(nullptr)` — the slot that is not
currently running — and only writes its registry row after `esp_ota_end()`.
An OTA that lands outside `fw_install_start()` (a plain `ota_flash.py`/CDC
`OTA_BEGIN`) is reconciled on the next boot: `fw_slots_init()` re-hashes the
running partition, and on a SHA mismatch clears that slot's package name
(state is left as whatever it was — an "unnamed" slot, see the assets section
for the cost of that).

`fw_install_start()` runs its whole body under `op_lock()` (see Switching
below), so the "is anything running?" read and the `Verifying` that arms this
install are one decision across CDC and the GUI. Its refusals, in order:
*no such package*, the package's own verdict, *battery low, no charger*, and
**kit loading** — an install started mid-load would stop the sample engine
between two of the loader's guarded per-pad calls and tear the library down
underneath it. Every refusal but *install already running* also writes its
reason into the install status, which is where the GUI's poll timer reads it;
that one deliberately does not, because the status then belongs to the
install that *is* running. Both callers print the reason the call itself
returned.

**The install stops the sample engine, so it puts the kit back.**
`marcel_engine_stop()` is a `DeInit` plus an arena reset, and
`marcel_engine_start()` brings the engine back empty — every other stop/start
pair in the tree (`components/crosspad-platform-idf/SamplerPlatformPort.cpp`) sits *inside* the kit reload loop, which
is what normally reloads the pads. `fw_install_task()` has no such loop, so
after restarting the engine (on the success path and on any abort that got as
far as stopping it) it asks for one: `kit_load_task_start(false, <current kit
id>)`, hopped onto the LVGL thread with `lv_async_call_locked()` because that
is the only thread that call may be made from. Without it the pads would be
silent after an install until the next kit change.

**Switching** (`fw_switch_request()`, `main/fw_slots.cpp`), the whole body
under the module mutex `op_lock()` that `fw_install_start()` also takes — CDC
and the GUI each have their own busy flag, so only that mutex can decide
between two simultaneous requests:
1. refuse if an install is currently writing the target slot (checked *before*
   either state check — mid-install the stored state is still whatever it was
   before the install started, which could otherwise read as switchable while
   the partition is being overwritten), if the other slot is `Empty` or
   `RolledBack`, or if the **running** slot is itself still
   `ESP_OTA_IMG_PENDING_VERIFY` (`running slot not confirmed yet`) — leaving a
   trial behind means the bootloader's next look at it marks the image that
   was working `ABORTED`, and the device gains a `RolledBack` slot it had no
   reason to distrust; and if the other slot's image carries a CPFI block for
   another board revision (`other slot is for another board` -- an image that
   came over USB was never judged like a card package);
2. **export** the running firmware's settings into its own package's
   `settings.json` on the card, when the running slot has a package name and
   that directory exists (best-effort — a failure is logged, not fatal; an
   unnamed/plain-OTA-flashed slot has nowhere on the card to export to, so
   nothing happens, silently);
3. `esp_ota_set_boot_partition(target)` — **first of the three commits**, so a
   failure here leaves nothing behind;
4. write NVS `pending` = the target slot's package name (**even when step 2
   was skipped or failed** — the arriving firmware would rather import
   nothing and keep its own last-saved settings than have `pending` point at
   a file that was never written, and a later boot's `fw_switch_finish_boot()`
   simply finds no matching file and does nothing), and move an `Installed`
   target to `Trial`.

`fw_switch_request()` itself never restarts — **the restart happens in the
caller** (the CDC `FW_SWITCH` handler, or the Firmware app's switch task),
after the host/UI has been told, because this module has no CDC of its own to
flush first. This is a deliberate deviation from the design spec's literal
wording (see the design doc's "As built" note).

**First boot of the arriving firmware** (`fw_switch_finish_boot()`, called
once NVS and the SD card are up, before anything reads a per-firmware
setting): reads `pending`; if it equals the *running* slot's own package name
and `<pkg>/settings.json` exists on the card, imports it with
`settingsImportPerFirmware()` (shared settings excluded — see next section)
and saves the merged result back to NVS. `pending` is erased in every case,
whether or not an import happened.

## Shared settings

Some settings belong to the device, not to whichever firmware is running:
`crosspad::SharedSettingsSnapshot` (`components/crosspad-core/include/crosspad/settings/SettingsScope.hpp`,
core 1.18.0) lists them —

```cpp
struct SharedSettingsSnapshot {
    Stm32Settings stm;                 // STM32 power/charger behaviour
    uint8_t LCDbrightness;
    bool usbAudioEnabled;
    bool usbSerialGuard;
    bool enableBLE;                    // wireless.enableBLE
    decltype(AudioEngineSettings::sampleRate) sampleRate;
};
```

`settingsImportPerFirmware()` is `settingsImportAll()` (the existing full-file
`SettingsIo` import) wrapped so the shared snapshot is captured first and
restored afterward — a per-firmware import can never move pad calibration,
STM32 power settings, brightness, the two USB flags, BLE enable, or the audio
engine's sample rate, even though those fields live in the same JSON file and
the same `settings.json` the other firmware wrote. On import failure nothing
is restored, because nothing changed — `settingsImportAll()` already leaves
`s` untouched on a bad/missing file. The list lives in crosspad-core (not the
platform layer) so the PC simulator and the Arduino build share it.

## Rollback

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` (`sdkconfig.defaults`, `sdkconfig.v1`,
`sdkconfig.v2`) — a freshly switched-to slot boots `ESP_OTA_IMG_PENDING_VERIFY`.
Two independent mechanisms end that trial:

**Confirmation.** `fw_confirm_running()` (`main/fw_slots.cpp`) marks the image
valid and cancels the pending rollback — but only when the running partition
is still `PENDING_VERIFY` (a no-op otherwise). It never writes flash on the
caller's own stack (see Traps); the actual write always happens on a
dedicated internal-RAM-stack task. Called from two places: a **periodic**
`esp_timer` armed in `app_main()`, first tick **10 seconds** after
`bsp_stm32_init()` succeeds and the LVGL task is created (`kConfirmDelayMs`,
`main/main.cpp`), and `FW_CONFIRM` over CDC (fire-and-forget — `OK` only
proves the command was received, not that the confirm task's disable has
landed yet). Confirming also promotes the slot's registry state to
`Confirmed`, and logs `slot X confirmed`.

**Periodic, not one-shot, and the timer deletes itself.** Creating the
confirm write task can fail — internal RAM is tight with BLE up — and a
one-shot would drop the whole trial on that failure: nothing confirms, the
30 s watchdog below resets the board, and a perfectly good slot is quarantined
as `RolledBack`. So every tick retries while the image is still
`PENDING_VERIFY`, including after an `esp_ota_mark_app_valid_cancel_rollback()`
that returned an error (logged, and the registry is *not* moved to
`Confirmed`). The confirm task hands the timer back with
`esp_timer_stop()`+`esp_timer_delete()` as soon as the image is no longer
`PENDING_VERIFY` — which is on the very first tick of a boot that had nothing
to confirm, such as a switch between two already-`Confirmed` slots. The stop
happens on that task, never inside the timer callback: `esp_timer` re-arms a
periodic timer after its callback returns.

**An OTA over CDC is a trial boot too, not just a switch.** `ota_flash.py`,
`bench.py flash` and the browser flasher all end in
`esp_ota_set_boot_partition()`, so with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`
the freshly written image boots `PENDING_VERIFY` exactly like a switched-to
slot: any reset in the first ~10 s — a scenario that reboots the board, a
replug, `console --reset` — makes the bootloader mark it `ABORTED` and boot the
*other* slot. The failure is silent and looks like "flashed, nothing changed",
because everything after it runs against the previous firmware. Both tools
say so: `ota_flash.py` prints the running slot's state and, on `trial`, *"do
not reset the board for ~10 s"*; `bench.py flash` polls `FW_SLOTS` every 2 s
for up to 25 s (`await_confirm()`), sends one `FW_CONFIRM` if the bound
expires, prints *"confirmed after N s"* / *"confirmed by FW_CONFIRM"*, and only
then runs `smoke`. Firmware with no `FW_SLOTS` verb gets one warning line and
the old behaviour. `bench.py flash` then compares the board's own `APP_SHA`
(`APPSHA: <64 hex>`, the app descriptor's `app_elf_sha256`) against
`sha256sum build_<rev>/CrossPad.elf` and fails the flash on a mismatch —
`LIBVER`/`APP_VERSIONS` report the commit and read identical across two builds
of it, and so does the descriptor's compile time, since ESP-IDF does not
recompile `esp_app_desc.c` for every build.

**An image with no `CPFI` block cannot confirm itself.** It predates this
whole mechanism, so nothing in it ever calls `fw_confirm_running()`: the
bootloader stages it `PENDING_VERIFY` like any other trial and rolls it back
after one boot. `FwSlotInfo::hasDesc` carries the fact out of `fw_slots_get()`,
and the Firmware app's switch dialog says so before the restart.

**The trial RTC watchdog** (`fw_trial_watchdog_arm()`) is the safety net for
everything the 10-second timer never reaches: a hang, a crash loop, or
`bsp_stm32_init()` failing outright (so the confirm timer is never armed).
Called as the **literal first statement of `app_main()`** — no NVS, no heap,
no scheduler dependency, raw `wdt_hal` register writes, the same shape
ESP-IDF's own `esp_restart_noos()` uses. It **unconditionally clears** the RTC
watchdog (both the enable bit and `flashboot_en`) before checking anything,
then arms stage 0 for **30 seconds** (`kTrialWatchdogTimeoutS`) only if the
running image is still `PENDING_VERIFY`. `fw_confirm_running()` disables it
(same unconditional-clear helper) right before marking the image valid, well
inside the 30 s window on a healthy boot.

**Why the watchdog must be cleared unconditionally, not just disabled.**
`wdt_hal_disable()` clears only the enable bit — `rwdt_ll_disable()`'s own doc
says as much: it "does not disable the flashboot mode... a timeout can still
occur if flashboot mode is simultaneously enabled." A trial armed with
`flashboot_en` would therefore confirm successfully — logged, watchdog
"disabled" — and still reset ~30 s later, because the independent
`flashboot_en` trigger keeps counting underneath. So `fw_trial_watchdog_arm()`
never sets `flashboot_en` (it is a bootloader flash-boot-phase mechanism, not
for guarding app execution), and the disable path clears and feeds *both*
trigger paths unconditionally, before the `PENDING_VERIFY` check — closing off
any state a previous boot's failed disable might have leaked forward,
regardless of whether the exact RTC_CNTL register survives an
`esp_restart()`. Verified with three separate 150–200 s console soaks past
the 30 s deadline (fresh trial, rewrite-triggering trial, already-confirmed
slot) with zero resets, and a `while(true){}` hang still rolling back at 31.4 s
(`RTCWDT_SYS_RST`, matching the configured stage-0 action).

`fw_trial_watchdog_feed()` resets the countdown without disabling it (a no-op
unless this boot actually armed one). `app_main()` calls it once, right after
`fw_assets_sync()` — that call can spend 4.2–4.4 s erasing and writing the
assets partition, inside the same 30 s window, long before anything can
confirm.

**Residual gap, by design, not a bug**: a hang *before* `fw_trial_watchdog_arm()`
itself executes — before `app_main()` reaches its own first line — is not
covered by anything in this module. Measured over ~8.5 minutes with the task
watchdog off (`CONFIG_ESP_TASK_WDT_PANIC` is not set in this project — it logs,
never resets) and the sabotage placed as the literal first statement: neither
side reset the board on its own.

**`fw_rollback_supported()`** answers whether the *flashed bootloader* — not
this device's history — can actually do any of this: it reads
`esp_ota_get_bootloader_description()` and checks `desc.version >=
kMinRollbackBootloaderVer` (2). `CONFIG_BOOTLOADER_PROJECT_VER=2` travels with
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` in the same three sdkconfig files —
bumped together, so a bootloader reporting version ≥2 is one built with
rollback support. It reads the bootloader rather than NVS history on purpose:
a raw `esptool` flash never goes through `esp_ota_write()`, so a freshly
flashed unit never sees `NEW`/`PENDING_VERIFY`, and a flag set by observing
one would read `false` regardless of the bootloader — useless for factory
gating.

**Updating an old bootloader.** `tools/bench.py recover [--build DIR]` and
`tools/factory_provision.py` both flash the bootloader over UART (through the
STM bridge) as part of their normal flash sequence — no separate step. Both
check the result: `bench.py check` prints one **advisory** line —

```
[warn] bootloader rollback: not supported — flashed bootloader lacks rollback
       fix: tools/bench.py recover   (reflashes the bootloader with app rollback support)
```

never a hard failure. `factory_provision.py` is stricter: `FW_SLOTS`'
`rollback=` trailer feeds `rollback_supported()`, and `smoke_rc == 0 and not
rb_ok` produces `verdict = "FAIL (no bootloader rollback)"`, exit code 1 — a
unit does not leave the factory on an old bootloader.

## Assets

The `assets` LittleFS partition (icons, fonts, the bundled STM32 image) is
shared by both slots. **The identity is a hash of the staged asset *files*,
not of `assets.bin`** — a file cannot hold the hash of the image it sits
inside, so `collect_assets.cmake` hashes every staged file (relative path +
content, `file(GLOB_RECURSE ...)`, sorted) at configure time, writes the
64-hex-char result both into the image (`FW_ASSETS_ID_BYTES`, compiled into
`fw_image_desc_running()->assets_id`) and as a marker file inside the staged
tree, `/assets/.assets_id`.

At boot (`fw_assets_sync()`, after `fw_switch_finish_boot()`, before
`bsp_display_start()`/`AudioTasks_Start()`, never in USB transfer mode), the
running image's own `assets_id` is compared against the marker on the
partition:

- **Match** — nothing to do.
- **Mismatch, source found** — the *running* slot's own package on the card
  has an `assets.bin` whose declared `assets_id` matches: read it whole into
  a PSRAM buffer, unregister LittleFS, rewrite the partition
  (`fx_assets_write_image()` — erase, chunked write with page 0 held back and
  committed last, whole-partition CRC readback), re-register, re-read the
  marker to confirm. Result: `Rewritten` — on which `app_main()` re-runs
  `stm32_fw_update_check()`, because the bundled STM32 image lives in
  `/assets` and the first check judged the *previous* firmware's bundle.
  Should the remount ever have to format the partition to come back at all,
  that is one `ESP_LOGE`: icons, fonts and `stm32_fw.bin` are gone until
  `fs_transfer.py assets`.
- **Mismatch, no source** (`MismatchNoSource`) — the running slot has no
  package name, or that package has no `assets.bin`, or its `assets_id`
  doesn't match. The partition is **left alone** (no destructive rewrite it
  couldn't complete) and one toast fires: *"Icons belong to the other
  firmware - insert the card with `<pkg>`"*.

**The known cost of an UNNAMED slot.** A slot flashed by `bench.py flash` (a
plain OTA over CDC, no package involved) has `pkg=-` in the registry. Switch
into it: `fw_assets_sync()` correctly reports `Match`/`Rewritten` for
*whichever* firmware it came from, but switching back to the unnamed slot has
no source package to rewrite from — the *other* firmware's assets stay on the
partition (missing icons on the launcher, `fs_open: Could not open file`
warnings in the log) until the next `bench.py flash --assets`. It is exactly
the scenario `MismatchNoSource` exists to name rather than mask.

**Measured rewrite time**: consistently **4.2–4.4 s** end to end (1 MB read
off the 1-bit SD card, 1 MB erase, 1 MB chunked write, 1 MB verify readback) —
comfortably inside the 30 s trial-watchdog budget. Runs on its own 8 KiB
internal-RAM-stack task (`fw_assets_sync_task`); measured high-water mark:
**4608 bytes free** of that 8192-byte stack.

## STM32 never downgraded

Switching firmware must not drag the STM32 co-processor backwards.
`stm32_fw_update_check()` (`components/stm32_hal/stm32_fw_update.c`) gates
`due` on `!older` in addition to the existing `newer`/protocol-differs
checks — a bundle *older* (major.minor) than what's running is never `due`,
even when its protocol differs from the running one; `STM_FW_UPDATE ... FORCE`
still bypasses `due` entirely on request.

At install time (not update time), `fw_install_start()` compares the
package's `stmProtoMin` against the STM32's *currently running* protocol
(`stm32_hal_get_link_info()`) and, when the package expects an older
protocol, replies `OK warn=stm_proto_older` instead of a plain `OK` over CDC.
`fw_install_stm_proto_older()` exposes the same fact after a
`fw_install_start()` call for that reply; the GUI needs the answer *before*
committing to an install (its confirm dialog), so it uses a separate pure
query, `fw_package_stm_proto_older(const FwPackage&)` — the single source of
truth both call into. The dialog appends: *"Built for an older pad controller
protocol — it may not work."* This is informational only: it never refuses
the install, and says nothing about whether the bundled STM image would
actually be applied (still `due`, above).

## The Firmware app

> The app is a launcher of kit tiles in tabs (Library, Store, Updates) with a kit
> card; see [firmware-app.md](firmware-app.md). What follows describes the slot
> cards, the install and switch flows and their overlay, which the app still uses
> (`fw_flow_overlay.cpp`) -- the one-page layout is gone.

`main/app/update/` — renamed from **Update** to **Firmware** (`REGISTER_APP_PL`,
same file, same `lv_CreateUpdateApp`/`lv_UpdateApp_destroy` function names).
Its launcher icon is chosen at registration time by the currently running
slot (`firmware_app_icon()`: `fw_a.png` when slot A is running, else
`fw_b.png` — both 64×64 RGBA, `main/app/update/assets/`).

Screen, top to bottom: two slot cards (`A` / `B ▶ running` head, package,
`kit · version`, state coloured — Confirmed green, Trial yellow, Rolled back
orange, Empty grey; the running one gets the theme-primary border; **never
added to the focus group** — informational only), a full-width **Switch to
A/B** button, a rollback-unavailable row (hidden unless
`!fw_rollback_supported()`), then **On the SD card**: one row per scanned
package — the **whole row is the control** (name, `kit · version`, a `›` on
the right; press = install *and restart*), a package that cannot be installed
is the same row dimmed, not focusable, with the verdict text where the arrow
would be. Below that **Over USB**: one status line, the *Switch USB to CDC*
button (hidden in the CDC profile), the auto-accept checkbox, `core · git`,
Close. The encoder group follows that order: the rows are built after the
tail focusables exist, so `rebuild_package_rows()` re-appends the tail behind
them each time it runs.

**Both flows run in `main/gui/fw_flow_overlay.cpp`**, a full-screen panel on
`lv_layer_top()` shaped like the OTA and STM32 overlays (title, *Do not power
off*, bar, phase line; the pads mirror the bar through `PadAnimator`):

- `fw_flow_install_and_restart(name, allowUnknown)` — *Starting… →
  Verifying… → Writing to slot X… NN% → Restarting into X…* →
  `esp_restart()`. A refused or failed install shows the reason in red for
  3.5 s and closes. When the install succeeded but `fw_switch_request()`
  refused (the running image has not confirmed itself yet, say) the panel
  says *Installed to X. \<reason\> Use Switch to X later.* in green — the
  package is in the slot, only the restart is postponed.
- `fw_flow_switch_and_restart(clearRollback)` — the Switch / Try again
  button; the same panel from *Restarting into X…* on.

While the panel is up the encoder holds an **empty group** (the app's own
rows sit underneath and must not answer a press) and the power-button click
is swallowed through `ui_back_push()` — nothing to go back to while the
flash is being written. `ui_modal_confirm` restores the encoder group *after*
its callback returns, so the app starts either flow through `lv_async_call`
one cycle later, or the dialog's restore would undo the empty group and leave
the app's rows with focus during the install.
`fw_flow_active()` is what the app polls: it refreshes the slot cards on the
true→false edge (a successful flow never comes back).

The switch button reads **Slot X is empty** and is disabled only for `Empty`.
A `RolledBack` slot is offered as **Try X again** and asks in its own words
("Slot X did not start last time. Try it once more?"); on confirm the flow's
switch task calls `fw_slot_clear_rollback()` before `fw_switch_request()`,
whose own `RolledBack` refusal stays exactly as it is. The ordinary switch
dialog appends the *cannot confirm itself* sentence when the target slot's
image has no `CPFI` block (`FwSlotInfo::hasDesc`).

**Every `fw_library_*`/`fw_install_start`/`fw_switch_request` call runs off a
dedicated task, never the LVGL thread** — the app's `fw_scan_task` (SPIRAM
stack, the scan touches the SD card), the overlay's `install_task` (SPIRAM —
the *lookup* alone needs a dedicated stack, as on `FW_INSTALL`'s CDC path) and `switch_task` (**internal** stack —
`fw_switch_request()` writes flash directly). `lv_timer`s poll the published
results back onto widgets; the app's is deleted unconditionally in
`lv_UpdateApp_destroy()`, the overlay's when the panel closes. **The switch
restarts from its own task**, ~200 ms after `fw_switch_request()` returns
success — the same delay and shape the CDC path uses, so "Restarting into
X…" has a chance to paint before the board disappears.

## CDC verbs

| Verb | Reply |
|---|---|
| `FW_SLOTS` | two rows `FWSLOT: slot=A\|B running=0\|1 state=empty\|installed\|trial\|confirmed\|rolledback pkg=<name>\|- ver=<ver>\|- kit=<kit>\|-`, then one trailer `FWSLOT: rollback=0\|1` |
| `FW_LIST` | one row per package `FWPKG: name=<name> kit=<kit> ver=<ver> rev=<n> size=<bytes> assets=0\|1 settings=0\|1 verdict=<text>`, then `FWPKG: count=<n>` (or `FWPKG: ERR busy`/`ERR no task`) |
| `FW_INSTALL <name> [FORCE]` | `OK` or `OK warn=stm_proto_older` (install started asynchronously) or `ERR <reason>` |
| `FW_INSTALL` (bare) | `FWINST: phase=idle\|verifying\|writing\|done\|failed percent=<0-100> error=<text>\|-` |
| `FW_SWITCH` | `OK reboot=1` then the board restarts ~200 ms later, or `ERR <reason>` |
| `FW_CONFIRM` | `OK` (fire-and-forget) |
| `APP_SHA` | `APPSHA: <64 hex>` — the running image's `app_elf_sha256`, to compare with `sha256sum build_<rev>/CrossPad.elf` |

None of these is on the companion (SysEx/BLE) allow-list — changing firmware
needs the user at the device or a trusted CDC host.

## Hardware traps

- **Flash writes from a PSRAM-backed stack assert/coredump the moment the call
  frame reaches the write's critical section** (it disables the cache the
  stack itself needs) — `fw_switch_cmd_task`/`fw_switch_gui_task`
  (`main/hil_control.cpp`, `main/app/update/update_app.cpp`) and
  `fw_switch_finish_task` (`main/fw_slots.cpp`) all use
  `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`, never `MALLOC_CAP_SPIRAM`, for this
  reason.
- **`fw_library_find()` alone overflows the CDC drain loop's own stack** —
  `fill_package()`'s `esp_app_desc_t` (256 B) plus path buffers is enough;
  `FW_INSTALL`/`FW_SWITCH` never call into `fw_slots.cpp` directly from that
  loop, always through a dedicated task (`main/hil_control.cpp`, the comment
  above `fw_install_cmd_task`).
- **`std::ifstream` + `ArduinoJson` `JsonDocument` measured near 7 kB of
  stack** for even a small manifest — more than `fw_install_task()`'s whole
  6 KiB internal stack, so it reads through a plain `FILE*`/`fread()` buffer
  instead (`main/fw_slots.cpp`).
- **Do not poll the board through the STM bridge during a trial window.**
  `crosspad-hil devices` (and anything that calls it) polling every few
  seconds during a trial looks like "never confirms"; a passive console tail
  or direct CDC reads do not. The likely cause (observed, not proven) is
  pyserial's default `Serial.open()` asserting DTR/RTS on the STM bridge VCP,
  which resets the ESP ([hil-testing.md](hil-testing.md)).
- **A reset inside the trial window rolls the new image back** (see *An OTA
  over CDC is a trial boot too* above). `smoke` resets the board at ~7–10 s of
  uptime, before the confirm timer's first tick: run straight after an OTA, the
  bootloader marks the image `ABORTED`, boots the other slot, and `smoke`
  passes against the **previous** firmware while the new slot reads
  `rolledback`. The only symptom is that nothing you changed is on the board.
  `bench.py flash` waits for the confirm and compares `APP_SHA`; by hand, after
  `ota_flash.py`, leave the board alone ~20 s before resetting it.
- **`readdir()` while the same pass mutates the directory** duplicates rows —
  ESP-IDF's FATFS `readdir()` is position-based, so `fw_library_scan()` folds
  loose `.bin` files in a fully-closed first pass before ever opening
  `FW_PKG_ROOT` for the counted scan (`main/fw_library.cpp`).

## Measured numbers

| What | Value |
|---|---|
| Install, `OK` reply (lookup + verify + task spawn) | ~1 s |
| Install, full write (2.15 MB image, 4096 B chunks, `vTaskDelay(1)` every 16 chunks) | ~4–5 s |
| Install, end to end (`FW_INSTALL` to `phase=done`) | ~5–6 s |
| `fw_install_task` stack | 6144 B internal RAM (4 KB read buffer separately in PSRAM) |
| Assets rewrite (1 MB read + erase + write + verify) | 4.2–4.4 s |
| `fw_assets_sync_task` stack high-water mark | 4608 B free of 8192 B |
| Confirm timer delay | 10 s after `bsp_stm32_init()` + LVGL task creation |
| Boot to `fw_slots: slot A confirmed` on a trial boot | **11.8 s** of uptime (console timestamp `I (11808)`; the timer is armed ~1.8 s in) |
| Trial watchdog timeout | 30 s (observed reset at 31.4 s wall-clock) |

## Test

Bench procedure (through `tools/bench.py`; a new `crosspad-hil` scenario
`fw_switch` covers 1–3 and 5, the rest are manual bench checks recorded here):

1. `make_fw_package.py` a build, push it to the card, `FW_INSTALL` it →
   `FW_SLOTS` shows it in the *other* slot, `state=installed`.
2. `FW_SWITCH` twice (A→B→A): each time `FW_SLOTS` proves the running slot
   flipped and `state` goes `trial` → `confirmed` within ~10–16 s; `bench.py
   test smoke` passes on both sides.
3. Settings round trip: a per-firmware setting and a shared one, changed on A,
   survive a switch to B and back — the per-firmware value returns to A's
   own, the shared one carries through unchanged from whatever B had live.
4. An image that panics/hangs in `app_main()`: install, switch, `FW_SLOTS`
   shows the automatic return and `state=rolledback` on the bad slot;
   `FW_SWITCH` against it then answers `ERR other slot did not start last
   time`; `RESET_LOG` names the reset (`COREDUMP` for a panic, `WDT` for the
   30 s trial-watchdog case).
5. A v1 image installed on a v2 board (or the reverse): `FW_LIST` shows
   `verdict=built for another board revision`, refused before any erase.
6. A package with a corrupted/mismatched `manifest.json` sha256: refused with
   `manifest disagrees with the image` / `image does not match its manifest`.
7. Two packages with different `assets.bin`: `FS_LIST /assets` follows the
   running firmware across a switch (see Assets above for the `MismatchNoSource`
   case).
8. No package on the card for the running slot (unnamed/plain-OTA slot):
   listing and switching still work; installing needs a card.
9. `app_churn --rounds 5`: the Firmware app itself churns cleanly (0 B/visit,
   no leak) — it is no longer skipped by name, see the crosspad-hil note
   below.

Verbs `crosspad_hil/verbs.py` + `knowledge/cdc.yaml` (a separate repo) need,
with their reply prefixes: `FW_SLOTS` (`FWSLOT:`, two rows + a `rollback=`
trailer — multi-line), `FW_LIST` (`FWPKG:`, N rows + a `count=` trailer —
multi-line), `FW_INSTALL` (`OK`/`ERR` on success/refusal, `FWINST:` when
polled bare), `FW_SWITCH` (`OK reboot=1`/`ERR`), `FW_CONFIRM` (`OK`). None of
the `FW_*` verbs exist in `cdc.yaml` today, so a generic `bench.py hil cdc
"<verb>" --expect "<prefix>"` only ever sees the *first* matching line for a
multi-line reply — reading the rest needs a raw multi-line read
(`crosspad_hil.cdc.CdcLink.transact_multi`).

**crosspad-hil's own `app_churn`/`back_churn` `DEFAULT_SKIP` tuples still say
`"Update"`** (`crosspad_hil/scenarios/app_churn.py`, `back_churn.py`, and the
matching test assertions) — the app is now named `Firmware`, so it is no
longer skipped by either churn scenario. It churns cleanly (0 B/visit, no
leak, no crash), so this is not currently harmful, but whatever made the
original skip entry there may still apply, and the entry should be updated to
`"Firmware"` in crosspad-hil.
