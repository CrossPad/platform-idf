# Firmware: slots, the library, updates, the STM32

Contributor notes: A/B slots, signed packages, the Firmware app, the STM32 in-field update. The short version of what a change in this area
must not break; the pages it links to have the detail.

## Firmware Library and A/B Switch

Two resident OTA slots (A/B = `ota_0`/`ota_1`, 3.5 MB each) plus a library of
firmware packages on the SD card (`/sdcard/crosspad/firmware/<name>/`). Every
image carries its own `CPFI` descriptor (board rev, kit, `stm_proto_min`,
assets id) — the authority over any `manifest.json`. Installing writes only
the non-running slot; switching moves the boot partition and restarts, each
firmware keeping its own settings bar a shared list; the bootloader rolls a
firmware that never confirms itself back on its own. The shared `assets`
partition follows whichever slot is running. The Update app is now
**Firmware** (`main/app/update/`). Code: `main/fw_{slots,library,image_desc,
assets_sync}.*`; CDC verbs `FW_SLOTS|FW_LIST|FW_INSTALL|FW_SWITCH|FW_CONFIRM`;
host `tools/make_fw_package.py`. Wire formats, states, timings and every trap
found on hardware:
[docs/subsystems/firmware-library.md](../subsystems/firmware-library.md).

## Firmware Updates (catalog, signed packages)

CI builds every kit (`config/profiles/*.json`, community kits in
`config/profiles/community/`) for v2 (`tools/ci_matrix.py`; v1 only on request,
`-DCROSSPAD_FW_KIT=<kit>`, `-DPROJECT_VER=<tag>`) into a draft release and signs
each package's `manifest.json` (ECDSA P-256, `tools/fw_sign.py`). **The private
key is only in the Actions secret `FW_SIGNING_KEY`, set by a maintainer; no
agent or tool ever touches it** -- only public keys are committed
(`config/fw_signing/`, generated `main/fw_sign_pubkey.h`, max 2). On the
self-hosted runner the build jobs run in the `crosspad-ci` container
(root-owned build files), the release job (the key) on GitHub's runner unless
`CI_SECRETS_RUNNER` says otherwise, and core commits must be on GitHub first. crosspad.app
`/api/device/catalog?rev=v2` lists published releases, `/api/device/file` serves
them with `Range` (live since 2026-10-24). One tag rule everywhere (CI,
Worker, local test server; `tools/tests/fw_tag_rules.json`): a `-` = beta, never the
pre-release tick; at most 23 characters with the `v` (the manifest keeps it). The board checks in the
background (boot if *Check at startup*, daily when idle in the launcher;
silent unless it succeeds; abandoned on a pad, an app or the kit selector;
held on a trial boot until the image confirms) on a WiFi lease; downloads and
installs are manual from the Updates view (Firmware app; network mode is the
fallback on `no_mem`): `.part` resumed, SHA-256 vs the signed manifest,
`.complete` last, then *Run the new version now?*. Result in NVS `fwupd` →
orange glyph; the board's own check beats the hub's `FW_LATEST`. Cleanup keeps
the library ≤ 512 MB and 10 % of the card free, never touching slot or
`.pinned` packages. Job task: 8 kB PSRAM stack, never flash. Verbs
`FW_UPD_STATUS|FW_CHECK|FW_CATALOG|FW_DOWNLOAD|FW_UPD_CFG|FW_UPD_CHANNEL|FW_UPD_URL`
(replies built in static buffers: the 4 kB main task overflowed under
`FW_CHECK`); local test server `crosspad_hil.fw_server`, scenario `fw_update`.
Details, numbers, limitations: [docs/subsystems/firmware-updates.md](../subsystems/firmware-updates.md).

## Firmware App (homebrew launcher)

The **Firmware** app (crosspad-gui `src/fw/fw_app.cpp`, registered with the
Over USB block by `main/app/update/update_app.cpp`) is a launcher of kit
tiles in three tabs: **Library** (the card and slots A/B), **Store** (the
catalog B's check cached, Official then Community) and **Updates** (+ the Over
USB block). A tile opens a full-screen **kit card** (description, apps, notes,
older versions) whose buttons come from crosspad-gui's `fwCardPlan()`: a rolled-back
slot is offered both *Try this slot again* and *Reinstall*, Delete is absent
while pinned and refused for slot packages. Store/Updates hold a WiFi lease
while shown and check once a minute at most (not during a trial boot); offline
they show the cached catalog with its date; `no_mem` offers network mode, whose
screen hosts the same views (one encoder group per tab, tabs switched on
`LV_EVENT_CLICKED`: LVGL 9.3 sends no `VALUE_CHANGED` for a tab press). Views
read only the card: `.catalog.json`, `.details/<kit>.json`,
`.icons/<kit>-<version>.bin` and `<pkg>/icon.bin` (48x48 LVGL RGB565A8, 6 924 B,
flags 0, never a PNG), written by the catalog check. One PSRAM-stack worker
(`fw_view_data.cpp`) does all card work; icons bypass LVGL's image cache and
exist only for tiles in view. Grids are crosspad-gui's `ui_tile_grid` (rows
recycled). The views are crosspad-gui's (`crosspad-gui/fw/`) and reach the board
only through `IFwViewService` (`main/fw_view_service.cpp`, a forward to the
`fw_*` C APIs); the PC simulator registers the same app over a fake service. Budget: <= 128 kB (131072 B) of PSRAM over the launcher with 30
packages, the WiFi radio included, the kit card not (it costs ~47 kB open and
gives it back); measured 97-122 kB on the Store tab. At most 32 packages; the
image's PSRAM pages and the text headroom to the next one are budgeted in
`config/image_budget.json` (`python3 tools/check_image_budget.py --rev v2`;
LVGL's themes, observer and text entry are at `-Os` for it) -- check it after adding code. `SCREENSHOT` cannot
capture the overlay layer (cards, dialogs). `FW_UI` over CDC;
`crosspad-hil run firmware_app`. Details:
[docs/subsystems/firmware-app.md](../subsystems/firmware-app.md).

## STM32 In-Field Update

The ESP reflashes the STM32 over UART from `/assets/stm32_fw.bin`
(`main/stm32_fw/stm32_fw.bin`, staged by `collect_assets.cmake` — travels in the
`assets` partition, which OTA does not touch; use `tools/fs_transfer.py assets`).
Every image carries a `CPFW` descriptor at 0x100 (major, minor, protocol, PCB);
a bundle for another board is never offered. `stm32_fw_update_check()` runs right
after `bsp_stm32_init()`; an update is *due* when the bundle is newer or speaks
another protocol (never older). With a display the user decides
(`stm_fw_overlay`, once the launcher is up); only a headless board updates on
its own, late in boot, and defers below 20 % battery with no charger. Off-device:
`STM_FW_INFO`, `STM_FW_UPDATE [path] [FORCE]`; UI in `main/gui/stm_fw_overlay.cpp`;
browser flasher `tools/webflasher/`. Mechanism, the RAM flasher, the CFGBSY/IWDG
traps: [docs/subsystems/stm32-update.md](../subsystems/stm32-update.md).
