# Firmware app as a homebrew launcher (sub-project C)

Date: 2026-10-02. Status: design agreed, revised (Revision 1, Revision 2), implemented.

## Revision 2 — Store and Updates live in normal mode; network mode as fallback (supersedes Revision 1 where they differ)

WiFi runs on demand in normal mode again (A and B, Revision 2):
- The Firmware app's **Store** and **Updates** tabs fetch the live catalog in
  normal mode with a WiFi lease (held while the tab is visible, released on
  leave). Without WiFi or with WiFi refused they show the cached catalog with
  its age; on `no_mem` they offer *Restart in network mode* on that tab.
- The network-mode screen hosts the same Store/Updates views as the fallback —
  shared code, as in Revision 1.
- Kit card *Download* / *Install* work in normal mode.
- HIL `firmware_app`: all steps in normal mode; one Store download repeated in
  network mode.

### As built (2026-10-04)

Subsystem doc: [firmware-app.md](../../subsystems/firmware-app.md). Where the code
differs from, or settles, what the design left open (decisions 1-18, then
what the bench changed):

1. Icon: 48x48 RGB565A8 is 6 924 B (12 B header + 4 608 B colour + 2 304 B alpha), not "~4.6 kB"; `tools/fw_icon.py` is pure Python + `zlib`, input exactly 64x64.
2. Icon location: a profile's `icon` is `icons/<kit>.png` next to it (community kits stay inside `community/`); required like `display_name` and `description`; `author` required under `community/`; text checked against the firmware's glyph set.
3. The catalog stays slim: per-kit `display_name`/`author`, per-version `icon`; description, apps and notes come from `GET /api/device/kit/<kit>?rev=v<N>`.
4. The catalog check fills the card once per change: tile icons into `.icons/`, kit text into `.details/`, both pruned to the catalog (no lazy fetch while scrolling).
5. One implementation per view, two hosts: a shown Store/Updates tab holds the `fw_views` lease and checks at most once a minute, *Check now* always; refusals show the cached catalog; `no_mem` offers network mode once per showing, never in network mode. During a trial boot the lease and the automatic check wait for the confirm.
6. Budget measured on the Library with 30 packages (it holds 32) and on the Store with a test catalog; icons only for tiles at least half in view.
7. Badges from glyphs the fonts have: `LV_SYMBOL_PLAY`, `_OK`, `_DOWNLOAD`, `_WARNING` (community), `_SAVE` (pinned).
8. `FW_UI`: `tab sel card items`, then `selkey host store busy`; `tab=wifi` exists in network mode and the fields describe the tab on screen.
9. Community warning before the first install of each community kit, remembered per kit in `.community_ok`, written by the view worker.
10. `ui_tile_grid` is host-tested in crosspad-gui's own `tests/` (headless LVGL 9.3 + Catch2), not in crosspad-pc, whose pins stay.
11. `settings_ui_open_next(category)`: the Updates tab's *Settings* opens Settings on B's Updates page.
12. Versions: crosspad-core 1.41.0; crosspad-gui 1.20.0, not 1.19.0 (A had shipped 1.19.0 first).
13. The kit card's action table is core (`FwCardActions.hpp`), unit-tested per state; two installs (*Download*/*Install* from the catalog, *Install and run* from a complete package, no network).
14. B's minimal Updates view was replaced from the inside, entry points kept; `fw_updates_view_close()` and the Store's four entry points added.
15. `fw_updater_target_kit()` added (two kits can share a version).
16. App chips: id matched to a running app ignoring case, spaces, `-`, `_`; the first 4 matches show their launcher icon (bounded, dropped when the card closes), the rest names.
17. "Kits on the card" for Updates = complete packages plus the kits in slots A and B.
18. No wall clock offline: the cached catalog shows the date of its fetch.

After the bench: the budget is 131 072 B with the radio inside and the kit card
measured on its own (see section 3); a rolled-back other slot is offered both
*Try this slot again* and *Reinstall*; Delete is absent while a package is pinned
and refused by the worker for slot packages; slot images with no package on the
card are Library tiles of their own; the tab buttons switch tabs on `CLICKED`
because LVGL 9.3 sends no `VALUE_CHANGED` for a press; network mode keeps one
encoder group per tab and, like the app, has no touch swipes between tabs;
a package with no image takes kit and version from its manifest in
`fw_library_scan()`, so `FW_LIST` and the views agree; `APP_STOP` before the
launcher exists answers `ERR no_launcher`. Known limits: 32 packages, no
`SCREENSHOT` of the overlay layer, 712 B of v2 image text headroom before the
next PSRAM page (41 pages, 2026-10-05).

## Revision 1 — Store and Updates run in network mode (superseded in part by Revision 2)

WiFi runs only in the network-mode boot (sub-projects A and B, Revision 1):

- **Normal mode:** the Firmware app's **Library** works fully offline (card,
  slots, kit card actions that need no network: *Run*, *Install and run* from
  the card, *Pin*, *Delete*). The **Store** and **Updates** tabs show the
  catalog cached on the card (with its age, or "never fetched") read-only, and
  a *Start network mode* button (confirm: "CrossPad restarts without sound to
  connect to the network") that enters network mode on that tab. *Download* on
  a kit card does the same.
- **Network mode:** its screen (sub-project A) hosts the live **Store** and
  **Updates** views — the same widgets and kit card as here, built from the
  live catalog, with *Download*, *Install*, *Run the new version*. The code of
  the views is shared; only where they run differs.
- The *Over USB* block stays in the normal-mode Updates tab.
- **§4 HIL `firmware_app`:** the Library steps run in normal mode; the Store
  download step runs after `NET_MODE 1 TAB=store`.
- **Kept unchanged:** layout, kit card, data (§2), threads and memory (§3),
  `ui_tile_grid`, failure rules (§5).
Depends on sub-project B ([2026-10-02-fw-catalog-updates-design.md](2026-10-02-fw-catalog-updates-design.md)):
the catalog, `fw_updater_*`, downloads, signed manifests, `.complete`/`.part`/`.pinned`.

## Goal

The Firmware app becomes the place where a CrossPad owner sees every firmware
the board can run — what is on the SD card, what is installed in slots A and B,
what the store offers, what can be updated — and acts on it with the encoder or
touch: download, install and run, switch, pin, delete, roll back to an older
version. It looks like a homebrew launcher: a grid of kit tiles.

Done means: on a 320×240 screen the three tabs work with encoder and touch; a
kit card shows description, apps, notes, meta and older versions; every action
goes through B's API or the existing install/switch flow; the app stays within
its PSRAM budget with 30 packages on the card; `crosspad-hil run firmware_app`
passes.

## Where things stand today

- `main/app/update/update_app.cpp` (registered as **Firmware**, icon
  `fw_a.png` / `fw_b.png` by running slot): two slot cards, a Switch button,
  one row per SD package (press = install and restart), an *Over USB* block
  (CDC switch, auto-accept, `core · git`). Scans run on `fw_scan_task`
  (SPIRAM stack); results come back through an `lv_timer`.
- `main/gui/fw_flow_overlay.cpp`: `fw_flow_install_and_restart()`,
  `fw_flow_switch_and_restart()` — full-screen, empty encoder group while up,
  power click swallowed.
- The kit selector in crosspad-sampler is a virtual list (objects only for
  rows near the highlight) — the pattern that keeps PSRAM flat.
- LVGL's image cache is 192 kB and shared by all UI; PNG decoding needs large
  PSRAM buffers (the boot splash failure, CLAUDE.md "PSRAM, not internal RAM").

## 1. Structure

The app name stays **Firmware** (HIL and docs keep `APP_START Firmware`). The
screen is an `lv_tabview` with three tabs; the tab bar is reachable by encoder
and touch; a power click from a kit card returns to its tab, from a tab closes
the app.

### Library
A virtual grid of tiles, 3 per row, 2 rows visible, scrolling vertically. One
tile per package on the card: 48 px kit icon, display name, version, badges —
`▶A` / `B` (installed in that slot, ▶ = running), `★` community, pin mark for
pinned. A download in progress shows `⇣ 42 %` on its tile. Footer: `SD: <used
by library>/<limit> MB · A ▶ · B`.

### Store
The same grid over B's catalog, in two sections: **Official**, then
**Community**. A tile whose exact version is already on the card carries ✓.
Sub-project D adds a "Build your own" tile here; C leaves no placeholder for it.

### Updates
A list of kits on the card for which the catalog has a newer version in the
kit's channel, each with *Update*. Above it: last check time, *Check now*, and
a shortcut to Settings → Updates (B). At the bottom, the **Over USB** block
moved unchanged from today's app.

### Kit card
Full screen, opened from a Library or Store tile:
- header: icon, display name, version; the description; app chips (icon from
  `/assets` when the app is in the running firmware, otherwise its name);
- meta row: size · channel · tier · author (community);
- *What's new*: the first ~6 lines of the release notes;
- actions, shown by state: *Download* / *Install and run* / *Run* (the image
  is in the other slot — a plain switch) / *Pin* or *Unpin* / *Delete from
  card* (disabled for an image in a slot);
- *Older versions* (collapsed): catalog versions of the kit older than the
  newest, each downloadable; installing an older version asks first.

Community images: before the first install of a community package, a dialog —
*"CrossPad CI built this firmware from community code. The CrossPad team has
not tested it. Continue?"*.

## 2. Data: icons, descriptions, apps, notes

- **Profile** (`config/profiles/<kit>.json`) gains `display_name`,
  `description` (1–2 sentences), `icon` (PNG 64×64 at
  `config/profiles/icons/<kit>.png`), `author` (required under
  `config/profiles/community/`). `apps` exists already.
- **Package** (`tools/make_fw_package.py`, B's signing): the icon is converted
  in CI to an LVGL binary image 48×48 (`icon.bin`, RGB565 + alpha) — never a
  PNG on the device. The manifest gains `display_name`, `description`,
  `author`, `apps[]`, and `icon.bin` among the hashed files, so the icon is
  covered by the signature.
- **Catalog** (Worker, B's `/api/device/catalog`): per version also
  `display_name`, `description`, `author`, `apps[]`, the icon asset name, and
  `notes` — the first ~6 lines of the GitHub release body. Notes are written by
  a person after CI builds, so they cannot be in the signed manifest; they are
  display text only.
- **Device**: catalog icons are cached on the card under
  `/sdcard/crosspad/firmware/.icons/<kit>-<version>.bin` (~4.6 kB each),
  fetched lazily as Store rows scroll into view (through B's download path,
  with a held WiFi lease). A bare `.bin` dropped on the card has no icon: its
  tile shows the first letter of its name.

## 3. Threads and memory

- Card scans, manifest and icon reads, catalog and downloads run off the LVGL
  thread: B's `fw_updater_*` plus the app's scan task (SPIRAM stack, as today).
  Results reach widgets through an `lv_timer`.
- Install and switch keep using `fw_flow_overlay` unchanged.
- Leaving the app while a download runs leaves the download running (B owns
  it); the tile shows its progress on return.
- **Virtual grid**: LVGL objects exist only for the visible rows ±1; the rest
  is data. A new crosspad-gui widget `ui_tile_grid` (portable, reusable by D)
  owns this: rows recycled on scroll, a callback fills a tile from an index,
  encoder focus moves over logical indices.
- Icons are loaded into `lv_image_dsc_t` buffers in PSRAM owned by the grid
  and freed with their row — they bypass LVGL's shared image cache.
- **Budget**: the app open on the Store with ≥ 30 entries costs ≤ 128 kB
  (131072 B) of PSRAM over the launcher baseline (`crosspad-hil mem`),
  **including the WiFi radio** while the Store or Updates tab holds its lease;
  20 enter/leave cycles return to baseline. Every cache stays bounded (the
  icon cache keeps at most 8 icons).

  *As built (2026-10-04):* the spec said ≤ 60 kB (61440 B). That figure dates
  from a launcher with ~150 kB of PSRAM free; after the image-size savings the
  launcher has ~508 kB free (kit loaded, BLE on), and the bench measured
  what the old number left out: the Store with the radio up costs 96860 B
  (46348 B with WiFi off, so ~50 kB is the radio's), and the Library walked to
  tile 24 with 30 distinct test icons costs 75784 B (six ~6.9 kB icons on
  screen over a 33.5 kB view). The budget was raised to 131072 B with the
  radio inside it, rather than measured with WiFi off.

## 4. Verbs and tests

- New verb `FW_UI`: `FWUI: tab=<library|store|updates> sel=<index|-> card=<kit@version|-> items=<n>`
  so tests do not infer the UI from screenshots. Existing `ENC_*`, `UI_STATE`,
  `APP_START/STOP` drive it.
- crosspad-hil scenario `firmware_app`: open the app; walk the tabs with the
  encoder; open a kit card from Library; pin and unpin; delete an unprotected
  package; from Store download a package (B's local test server); *Run* the
  image in the other slot (switch, confirm, switch back — as `fw_switch`);
  `mem` against the budget; 20 enter/leave cycles without growth.
- Before/after evidence with the `before-after` skill (simulator screenshots
  of the widget + `LVGL_STATS` on the board) in the PR.
- `ui_tile_grid` runs in the PC simulator; its behaviour (recycling, focus
  over indices, empty grid) is checked there.

## 5. Failure rules

| Situation | Outcome |
|---|---|
| No WiFi / catalog unreachable | Store shows the last fetched catalog with its age, or "Store unavailable — <reason>"; Library and Updates still work |
| Icon download fails | tile keeps the letter placeholder; retried on the next visit |
| Package on the card without `.complete` | shown as downloading (with progress if B is on it) or "incomplete — Delete / Resume" |
| Delete requested for a protected image | action disabled, with the reason ("installed in slot A") |
| Community install declined | nothing happens |
| Older version install declined | nothing happens |

## Out of scope

Choosing apps and building to order (D); payments; ratings or reviews;
uploading binaries.
