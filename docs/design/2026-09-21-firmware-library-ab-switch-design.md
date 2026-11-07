# Firmware library on the SD card and A/B switching

Date: 2026-09-21. Status: implemented (Tasks 1–11). See
[docs/subsystems/firmware-library.md](../../subsystems/firmware-library.md)
for the shipped subsystem doc.

## As built — deviations from this design

- **The restart lives in the caller, not in `fw_switch_request()`.** §5 reads
  as if the switch function itself restarts; it never does — the CDC
  `FW_SWITCH` handler and the Firmware app's switch task both restart
  (~200 ms later) after telling the host/UI, because `fw_switch_request()`
  has no CDC/UI of its own to flush first.
- **Installing checks "another install already running" before the two
  `FwSlotState` refusals**, not in the order §5 implies — a mid-install
  target slot's registry state is still whatever it was *before* the install
  started, which could otherwise read as switchable while the partition is
  being overwritten.
- **A trial RTC watchdog was added**, beyond §8's confirm timer: 30 s, armed
  only when the running image is `PENDING_VERIFY`, as the literal first
  statement of `app_main()` — the confirm timer alone does not cover a hang
  or a `bsp_stm32_init()` failure that leaves the timer never armed.
- **`fw_rollback_supported()` reads the flashed bootloader's own descriptor
  version** (`CONFIG_BOOTLOADER_PROJECT_VER`), not device/NVS history as §8
  implies — an NVS-history flag reads `false` on every freshly-flashed unit
  regardless of bootloader, which made it useless for factory gating.
- **No quick-settings tile** — §10 already named this as future work (the
  drawer has no extension mechanism yet); still true.
- **`FW_INSTALL` can answer `OK warn=stm_proto_older`** — a warning §11 does
  not mention, added alongside §9's never-downgrade STM32 rule so a host (and
  the GUI confirm dialog, via a separate pure query) knows before the switch
  that would apply it.

## Goal

CrossPad is moving to separate *firmware kits* per use (sampler first). A user
should be able to keep several firmwares on the SD card, drop new ones onto it
through USB Transfer mode, install one in about twenty seconds, and flip between
the two firmwares resident in flash with nothing more than a restart. Each
firmware keeps its own settings. A firmware that does not start must not strand
the device.

## Decisions taken with the user

1. **Config is per firmware.** Switching saves the leaving firmware's settings
   into its package and loads the arriving firmware's.
2. **Bootloader rollback is enabled.** A new image boots on trial and confirms
   itself; without the confirmation the bootloader returns to the previous slot.
   Boards already built need one UART flash of the bootloader.

## Approach

Two resident slots plus a library on the card. `ota_0` and `ota_1` (3.5 MB each,
the image is 2.25 MB today) always hold one firmware each; installing writes to
the slot that is not running. Rejected: a third app slot (2.3 MB each leaves no
room to grow and needs a new partition table), and running from the card (the
ESP32 executes from flash).

## 1. What an image says about itself

Today an image does not carry its board revision: `tools/ota_flash.py` reads it
from the `sdkconfig.json` beside the binary. On the device there is nothing
beside the binary, so the image gets a custom description placed directly after
`esp_app_desc_t` (the IDF `esp_custom_app_desc` pattern, section
`.rodata_custom_desc`), at a fixed file offset of 0x120:

| Field | Meaning |
|---|---|
| `magic` | `CPFI` |
| `desc_version` | layout version of this struct |
| `board_rev` | 1 or 2, from the revision the build was configured for |
| `kit_id[16]` | firmware kit name (`sampler`, `daw`, …; `full` until kits exist) |
| `stm_proto_min` | STM32 protocol the firmware needs |
| `assets_id[32]` | SHA-256 over the staged asset files (sorted names and contents) this image was built with |

`esp_app_desc_t` already gives project name, version and build date. An image
without `CPFI` (anything built before this change) is listed as *unknown* and
installs only after an explicit second confirmation.

`tools/ota_flash.py` switches to the same field when it is present, so host and
device judge an image by the same evidence.

## 2. Package layout on the card

```
/sdcard/crosspad/firmware/<name>/
    CrossPad.bin      required
    manifest.json     name, kit, version, board_rev, stm_proto_min, sha256 of each file
    assets.bin        optional — the assets partition image this firmware was built with
    settings.json     optional — this firmware's settings (SettingsIo format)
    icon.png          optional — 64x64, shown in the list
```

A bare `*.bin` dropped into `firmware/` is accepted too: on the next scan it is
moved into a directory named after the file and a manifest is derived from the
image descriptions. `manifest.json` is a convenience for listing; the image's
own description is the authority, and a manifest that disagrees with it marks
the package *inconsistent* and blocks installation.

`tools/make_fw_package.py --build build_v2 --name <name> [--kit sampler]`
produces the directory from a build dir; CI can call the same tool.

Packages reach the card through USB Transfer mode, `tools/fs_transfer.py`, or a
card reader.

## 3. Slot registry

One NVS namespace `fw_slots`, two records (`a`, `b`): package name, image
SHA-256, assets id, install time, and `state` (`empty`, `installed`,
`trial`, `confirmed`, `rolled_back`). The registry is a cache of what was
installed; on boot the running slot's record is reconciled with the running
image's SHA so an OTA over CDC (which knows nothing about packages) shows up as
*unnamed* rather than as a stale name.

## 4. Installing

Preconditions, all checked before the first erase: chip is ESP32-S3, project name
matches, `board_rev` equals the running firmware's, file SHA-256 matches the
manifest when one exists, the image fits the slot, battery at least 20 % or
charger present (same gate as the STM32 update).

The install runs on its own task (never LVGL, never `bsp_worker`): stop the
sample engines (`marcel_engine_stop()` — it also frees the card from the
streamer), `esp_ota_begin` on the non-running slot, stream the file in 4096-byte
chunks, `esp_ota_end` (validates the image hash), write the slot record, restart
the engines. Progress and result use the existing global OTA overlay
(`main/gui/ota_overlay.cpp`). Installing does not change the boot slot.

## 5. Switching

`FW_SWITCH` / the GUI button:

1. export the running firmware's per-firmware settings into its package's
   `settings.json` when that package is on the card (skipped silently otherwise);
2. write a pending-import marker naming the arriving package;
3. `esp_ota_set_boot_partition(other)` and restart.

First boot of the arriving firmware: import its `settings.json` if the marker
names it and the file exists, then clear the marker. Then the assets check (§6).

## 6. Assets

The `assets` partition is shared by both slots. The identity is a hash of the
staged asset *files*, not of `assets.bin`: the image must contain its own marker,
and a file cannot hold the hash of the image it sits in. `collect_assets.cmake`
computes it at configure time, writes it to `/assets/.assets_id` and to a
generated header the firmware compiles in (§1). At boot, when the two differ and
the running slot's package on the card declares an `assets.bin` with the wanted
id, the partition
is rewritten through the existing staged path in `main/file_transfer.cpp` (whole
image in PSRAM, CRC-checked before the erase) — before the sample engine takes
its arena. Without the card the firmware runs on the other firmware's assets and
raises one toast saying so; missing icons are the known cost.

## 7. Settings: shared and per firmware

Per-firmware export/import uses `SettingsIo`. Hardware and bench facts must not
travel between firmwares, so core gets an explicit list of **shared keys** that
the per-firmware import never overwrites: pad calibration and per-pad tuning,
STM32 power settings, display brightness and timeouts, `usbSerialGuard`,
`usbAudioEnabled`, BLE enable, audio engine sample rate. Everything else is per
firmware. The list lives beside `SettingsIo` in crosspad-core so the PC simulator
and Arduino share it.

## 8. Rollback

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` in `sdkconfig.defaults` and both
revision files. A freshly installed or freshly OTA-flashed image starts in
`ESP_OTA_IMG_PENDING_VERIFY`. The firmware calls
`esp_ota_mark_app_valid_cancel_rollback()` once all of these hold: LVGL task is
running, the STM32 answered `bsp_stm32_init()`, and ten seconds passed without a
panic. `FW_CONFIRM` does the same on demand for the bench. A switch between two
already-confirmed slots involves no trial.

When the bootloader rolls back, the surviving firmware sees the other slot as
`ESP_OTA_IMG_INVALID`/`ABORTED`, sets that record to `rolled_back`, and shows
one toast: "*name* did not start — still on *name*".

Existing boards: the bootloader is not updatable over OTA. `tools/bench.py
recover` and `tools/factory_provision.py` already flash it over UART; both gain a
check that reports a bootloader built without rollback, and the Firmware app
shows "rollback: unavailable (old bootloader)" — detected by the running image
never being in `PENDING_VERIFY` after an install. Installing still works there,
unprotected, behind a confirmation that says so.

OTA over CDC gains the same protection without any change to its protocol.

## 9. STM32

Switching firmware never downgrades the STM32. The auto-update after
`bsp_stm32_init()` keeps its "newer only" rule and loses "or protocol differs"
for the case where the bundled image is older. A package whose `stm_proto_min`
is above what the STM32 runs is installable (its own assets carry the newer STM
image); one whose firmware is older than the STM32's protocol shows a warning at
install time, because nothing will bring the STM32 back down.

## 10. GUI: the Firmware app

The existing Update app (`main/app/update/`) becomes **Firmware** rather than
gaining a sibling. Its launcher icon is chosen at registration from the running
slot — `REGISTER_APP` evaluates its icon argument at run time, so a function
returning `fw_a.png` or `fw_b.png` is enough; both PNGs ship in the app's
`assets/`.

Screen, top to bottom: two slot cards A and B (package name, version, kit, build
date, state; the running one highlighted), **Switch to A/B** with a confirm
dialog, the card's package list with *Install to slot X* per row (and the reason
when a package is not installable), then the Update app's present content (USB
mode, OTA auto-accept) unchanged.

A quick-settings tile is left out: the drawer has no way yet for an app or the
platform to add one (open item in the project TODO), and that mechanism is its
own piece of work.

`crosspad-hil` and docs that name the app `Update` are updated with it.

## 11. CDC verbs

`FW_SLOTS` (two `FWSLOT:` rows), `FW_LIST` (`FWPKG:` rows with installable flag
and reason), `FW_INSTALL <name>` (answers `OK` and reports progress as
`FWINST:` rows, final `FWINST: done|error <reason>`), `FW_SWITCH`, `FW_CONFIRM`.
None of them is on the companion (SysEx/BLE) allow-list: changing the firmware
needs the user at the device or a trusted CDC host.

## 12. Code layout

| Unit | Responsibility | Depends on |
|---|---|---|
| `main/fw_image_desc.{h,c}` | the `CPFI` struct, its instance in the image, parsing it from a file | esp_app_format |
| `main/fw_library.{h,cpp}` | scan the card, validate packages, derive manifests | fw_image_desc, FATFS |
| `main/fw_slots.{h,cpp}` | slot registry, install task, switch, confirm, rollback detection | esp_ota_ops, NVS, fw_library |
| `main/fw_assets_sync.{h,cpp}` | assets id compare and rewrite | file_transfer staging |
| crosspad-core `settings/SettingsScope.hpp` | shared-key list, scoped export/import | SettingsIo |
| `main/app/update/` | Firmware app UI | fw_slots, fw_library |
| `main/hil_control.cpp` | verbs | fw_slots, fw_library |
| `tools/make_fw_package.py` | build a package | — |

## 13. Testing (hardware, through `tools/bench.py`)

1. Package a build, push it, `FW_INSTALL`, `FW_SLOTS` shows it in the other slot.
2. `FW_SWITCH` twice: A → B → A, each time `LIBVER`/`FW_SLOTS` prove the slot, smoke passes.
3. Settings round trip: change a per-firmware setting and a shared one on A,
   switch to B and back; the per-firmware value returns, the shared one never moved.
4. An image built to panic in `app_main`: install, switch, observe the automatic
   return and the `rolled_back` record.
5. A v1 image on the v2 board: refused before any erase.
6. A truncated `CrossPad.bin`: refused by SHA, and by `esp_ota_end` when there is no manifest.
7. Packages with different `assets.bin`: the partition follows the running firmware (`FS_LIST /assets`).
8. No card: the app lists slots, switching works, installing is unavailable.
9. `app_churn`, `kit_churn` after a switch: no regression from the engine stop/start around installs.

New scenario `crosspad-hil run fw_switch` for 1–3 and 5; the rest are manual
bench checks recorded in the subsystem doc.

## Out of scope

Signed packages and secure boot; downloading packages over a network; more than
two resident slots; per-firmware kits or samples (the card's `kits/` is shared);
updating the bootloader from the device.

## Open risks

- **Internal RAM during install.** The install task needs an internal stack
  while the UI is up; measure with `MEM` before fixing its size.
- **NVS schema drift between kits.** Two firmwares sharing one NVS can disagree
  about a key's meaning. The shared-key list is the contract; anything else a
  firmware reads from NVS must tolerate values written by another kit.
- **Rollback and the STM32 watchdog.** A firmware that hangs without panicking
  is rebooted by the STM32, not by the ESP; the trial state must survive that
  path and still end in a rollback. To be proven in test 4 with a hang variant.
