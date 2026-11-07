# Firmware App (homebrew launcher)

> Source of truth for this subsystem. CLAUDE.md carries only the summary; when
> behaviour changes, update this file and the summary together.

Design: [2026-10-02-firmware-app-homebrew-design.md](../design/2026-10-02-firmware-app-homebrew-design.md)
(Revision 2 binding; its "As built" note lists where the code diverges). Builds on
[firmware-updates.md](firmware-updates.md) (catalog, downloads, signed packages),
[firmware-library.md](firmware-library.md) (packages, slots, switch) and
[wifi.md](wifi.md) (leases, network mode).

**Status: implemented, measured on v2** (crosspad-core 1.41.0, crosspad-gui
1.20.0). The numbers below are of commit `eac76e8` (2026-10-04/05); what
came after it (the Updates-view behaviours carried into these views, the
community warning by the version's tier, the image back to 41 PSRAM pages, the
tile grid's allocation checks, the delete and switch guards) is covered by host
tests and builds and not yet measured on a board. v1 builds and is not
measured.

## In one paragraph

The **Firmware** app (crosspad-gui's `src/fw/fw_app.cpp`; the board registers it
in `main/app/update/update_app.cpp`) is a launcher of kit tiles in three tabs: **Library** (packages on the SD card and the images in
slots A/B), **Store** (the catalog B's check cached on the card, Official then
Community) and **Updates** (kits on the card with a newer version, plus the
*Over USB* block). A tile opens a full-screen **kit card** (description, app
chips, notes, older versions) whose buttons come from crosspad-gui's `fwCardPlan()`.
Every view reads only the card: `.catalog.json`, `.details/<kit>.json`,
`.icons/<kit>-<version>.bin`, `<package>/icon.bin`, `manifest.json`. A shown
Store or Updates tab also holds a WiFi lease and asks B's updater to check, so
the cached catalog is refreshed while you look at it; offline it stays on screen
with its date. `no_mem` offers network mode, whose screen hosts the same two
views. One PSRAM-stack worker (`fw_view_data.cpp`) does all card work, icons
bypass LVGL's image cache, and the grids are crosspad-gui's portable
`ui_tile_grid` (rows recycled). `FW_UI` over CDC tells a test which tile is
selected without a screenshot.

**Where the code lives.** The views, the kit card, the actions, the icon cache
and the app's tabs are crosspad-gui's (`include/crosspad-gui/fw/`, `src/fw/`) and
call nothing of the platform's but `crosspad_gui::IFwViewService`: the card
(requests whose answers move in on `dataPoll()`), the slots and their flows, the
updater, the WiFi lease, network mode and the FW_UI fields. On the board
`main/fw_view_service.cpp` forwards each call to the C APIs named in this page
(`fw_view_data_*`, `fw_slots_get()`, `fw_updater_*`, `wifi_link_*`,
`wifi_fallback_offer()`, `fw_ui_state_*`), registered at boot by
`fw_view_service_install()`; the threading is theirs, unchanged. The board adds
the *Over USB* block and the launcher icon through `crosspad_gui::FwAppHooks`;
`fw_flow_overlay.cpp` (the install/switch tasks, the restart) stays in `main/`.
The views' static state is `CROSSPAD_GUI_PSRAM_BSS` (EXT_RAM_BSS_ATTR on the
board). crosspad-gui's `tests/test_fw_views.cpp` builds every view against a
fake service (`FW_VIEWS_SNAPSHOT_DIR=<dir>` writes PNGs); the PC simulator
registers the same app over fake data.

## Tabs, tiles and the kit card

**Hosts.** The same view code runs in two places and both call the same entry
points (`fw_library_view_*`, `fw_store_view_*`, `fw_updates_view_*`,
`fw_kit_card_*`, `fw_actions_*`):

| | Firmware app | Network-mode screen |
|---|---|---|
| File | crosspad-gui `src/fw/fw_app.cpp` (+ `main/app/update/update_app.cpp`) | `main/gui/net_mode_screen.cpp` |
| Tabs | Library, Store, Updates | WiFi, Store, Updates (no Library) |
| Pages | **one built at a time**: a page exists only while its tab is shown, the one left is closed (`show_tab()`) | all three built at once, for the whole boot |
| Encoder groups | one group; the tab buttons first, then the shown page | **one group per tab**; the tab buttons and Exit (the "tail") are moved into the shown tab's group behind its controls |
| Switching tabs | `LV_EVENT_CLICKED` on a tab button (see Traps) | the same; touch swipes are off in both |
| Power click | card -> its tab; tab -> app closed | card -> its tab; tab -> *Exit* (leaves network mode, a restart) |
| `FW_UI host=` | `app` | `net` |

The tab bar is reachable by encoder and touch. In the app the tabview's content
is not scrollable: a swipe would slide to a page that is not built.

**Tiles.** 3 per row, 2 rows at rest (`fw_grid_create()`, `kRowsShown`), 48 px
icon or, while it has none, the first letter of the name (ASCII upper-cased,
UTF-8 aware), the display name and a meta line. Focus is an outline drawn
inside the tile (a border would move the content; an outline outside is
clipped by the grid). A tile whose item cannot be used is dimmed. Meta line
badges, all from glyphs the fonts have:

| Badge | Symbol | Meaning |
|---|---|---|
| play mark + slot letter | `LV_SYMBOL_PLAY` `A`/`B` | the image is in that slot; the play mark = it is running (no mark: the other slot) |
| check mark | `LV_SYMBOL_OK` | (Store) this exact version is complete on the card; (kit card) same |
| warning sign | `LV_SYMBOL_WARNING` | community build: the tile's or card's version is community (no star glyph exists); a rolled-back slot reads `B <warning> rolled back` and is dimmed |
| save mark | `LV_SYMBOL_SAVE` | pinned |
| download mark and `n %` | `LV_SYMBOL_DOWNLOAD` | being downloaded now (Library: `incomplete` for a half download nobody is working on) |

**Library.** One tile per package directory on the card (at most **32**, what
the space cleanup handles, `kFwLibMax`) plus one **slot-only tile** for each
slot image whose package is not on the card (a board flashed over USB; named
`Slot A` when the image has no kit). At most 34 tiles. Order: the running
image, the other slot's image, then the rest by display name (ASCII
case-insensitive), newest version first. Footer: `SD: <library MB>/<limit MB>
· A ▶ · B`. A package whose verdict is not installable shows the verdict text
dimmed and still opens its card (Delete works).

**Store.** The same grid over the cached catalog, two sections, **Official**
then **Community** (`fwStoreOrder()`). A tile shows the kit's
`fwShownVersion()` in that kit's channel: the newest stable, or the newest beta
for a kit with no stable (so a beta-only kit is visible); the kit's Beta channel
(Settings -> Updates) sees every version. Status line above the grid: `Checking
the store...`, `Catalog of <server date>` (no wall clock offline, so a date and
never "n days ago"), `Never fetched`, `Store unavailable - catalog of another
board` / `- catalog unreadable`, then a second line with the WiFi refusal or the
updater's error in words (`fw_updates_error_text()`).

**Updates.** A status block (`Running <version>, checked <date>` -- `checked`
alone when the catalog's answer had no usable Date, `not checked yet` before the
first check that succeeded -- or the job in progress), **Check now**, a hidden **Run the new version** row (shown while the
updater is in `READY_TO_RESTART`), **Settings** (opens the Settings app on B's
Updates page; absent in network mode), then *For the kits on the card*: one row
`<kit> <have> -> <new>` with **Update** per owned kit that has a newer version
in its channel (`fwStoreUpdates()`). *Owned* = complete packages with a kit
**plus the kits in slots A and B** (a board flashed over USB holds its kit in a
slot only). A slot holding a newer build of the kit counts as owned at that
version, so the row disappears while a slot already has it: the view says
`Everything on the card is up to date.` while `FW_UPD_STATUS` may still say
`available` for the running slot. **Update** is *Install* from the Store card
(same questions). In the app, the **Over USB** block (CDC status, *Switch USB to
CDC*, auto-accept checkbox, `core · git`, *Close*) ends the Updates page,
unchanged from the one-page app; its focusables are the page's tail.

**Kit card.** A full-screen overlay on `getOverlayParent()` (the top layer),
opened from a Library, Store or older-versions row, with its own encoder group
and a `ui_back_push()` handler (power click closes it and returns the encoder to
the tile it came from; `lv_indev_restore_group()`, never a raw saved pointer).
Contents: Back, icon (or letter), display name, `<version>  <slot letter>
<check> <warning>`, description, **app chips**, `size · channel · tier · by
author`, *What's new* (the notes, hidden when empty), a status line, the action
buttons, and *Older versions (n)* (collapsed; each row opens that version's card,
and installing it asks first). Chips: the profile's app id (`pad-mixer`) matches
a running app (`Pad Mixer`) when both are equal ignoring case, spaces, `-` and
`_`; the first 4 matches show their launcher icon, the rest (and unmatched ids)
show a name. The card keeps all nine action buttons in its encoder group and
**hides** the ones the plan does not show, so `ENC_GROUP` lists hidden buttons;
what the encoder can reach is the truth (a test reads `ENC_FOCUS` while turning).
A card opened from the Library also loads the catalog (`fw_views_want_store`),
for "in the catalog?" and the older versions.

## Where the data comes from (card files, the check that fills them)

All under `/sdcard/crosspad/firmware/`. The scan, `fw_library_scan()`, and the
cleanup skip every name starting with `.`:

| Path | Written by | Read by | Notes |
|---|---|---|---|
| `.catalog.json` | B's check job | `do_store()` | streamed through an ArduinoJson filter (`fwStoreCacheFilter`): a cache that can be 160 kB keeps ~20 kB. The cache is `{"v":1,"fetched":<unix>,"rev":"v<N>","catalog":{...}}` |
| `.details/<kit>.json` | the check job (`fw_store_files.cpp`) | the kit card | `/api/device/kit/<kit>?rev=v<N>` verbatim, **at most 16 kB** (`kFwDetailMax`); description, apps and notes of up to 8 versions. Fetched when the kit's newest version is not in the file yet |
| `.icons/<kit>-<version>.bin` | the check job | tiles and the card | the icon of every Store tile's version (`fwShownVersion`) |
| `<package>/icon.bin` | the package download | tiles and the card | listed and hashed in the signed `manifest.json`, so it is signed |
| `<package>/manifest.json` | the package | the scan, the card | display name, tier; kit and version when there is no image |
| `<package>/.pinned` | the card's *Pin* | the scan, the cleanup | pin = keep: never evicted, Delete not offered |
| `.community_ok` | the card, after the first install of a community kit | the action chain | one line, space-separated kit ids, rewritten whole (temporary name, then rename); read back deduplicated |
| `<package>/.downloading`, `.complete` | B's download | the scan | `.downloading` without `.complete` = a download in flight (verdict `Downloading`, shown `incomplete` or with its progress) |

**The check fills the card.** After B's check job writes `.catalog.json` it
fetches, on the lease it already holds, the icon of each tile's version that the
card lacks (only entries whose size is exactly 6 924) and the detail file of each
kit whose newest version is not in its file (`fw_store_files_fetch()`); every
file goes through a temporary name and a rename. It stops between files when it
is abandoned, and prunes both directories to the catalog only after a complete
pass (a half pass deletes nothing). A failed icon or detail file is retried by
the next check; a tile without an icon keeps its letter. So a view with no WiFi
still shows icons and text.

**Icon format.** An LVGL v9 binary image: 12-byte header (magic `0x19`, colour
format `0x14` = RGB565A8, **flags 0**, width 48, height 48, stride 96, reserved
0) + 4 608 B of RGB565 + 2 304 B of alpha = **6 924 B**
(`kFwIconBytes`). `fw_icon_valid()` checks all of it before any buffer is shown:
a compressed, premultiplied or "allocated" file would make LVGL read the buffer
as something it is not, so flags must be 0. Never a PNG on the device. The
converter is `tools/fw_icon.py` (pure Python + `zlib`, so CI makes the same bytes
on every machine; input exactly 64x64 8-bit RGB/RGBA, area-averaged with
premultiplied alpha). The catalog, the Store model and the signed manifest accept
an icon of **at most 16 kB** (`kFwIconBytesMax`): over that, the unsigned catalog
drops just the icon and a signed manifest makes the whole package unparseable; the
device then still fetches only size 6 924 and re-validates what it got.

## WiFi: the tabs' lease, the minute between checks, no_mem and network mode

- A shown Store or Updates tab holds the lease `fw_views` (`fw_views_tab_shown`)
  until it closes (`fw_views_tab_closed`); the lease goes with the last tab that
  holds it. In the app a tab closes when you leave it; in network mode tabs are
  never closed before Exit, and the screen holds its own lease (`netmode`) beside it.
- On show, if the lease is held, the tab asks for a check **at most once a
  minute** (`kAutoCheckGapUs`, shared by both tabs: flipping tabs must not
  hammer the Worker). **Check now** always checks, and first asks again for a
  lease the show was refused (WiFi was off a moment ago).
- **A trial boot holds both.** While `fw_running_on_trial()` the tab neither takes
  the lease nor starts its automatic check: a crash in the WiFi connect, the radio
  or TLS would roll a good image back. The poll goes on by itself once the confirm
  has landed (a v2 board logged `boot check held` at 7.9 s and `slot A confirmed` at
  12 s). A *Check now* press is not held.
- A refusal (WiFi disabled, no network, wrong password, ...) leaves the cached
  catalog on screen with its date and the reason in words. A refused automatic
  check shows the same way, passively (`fw_views_wifi_reason()`), until a press
  or a job; `busy` is not shown.
- `no_mem` (the lease's own gate, or the radio's gate after the lease) calls
  `wifi_fallback_offer()` **once per showing** and never from a timer, a verb or a
  background path; a refused Download/Install press, and a check the user
  started, offer it too. **Nothing is ever offered in network mode** (`net_mode_active()`),
  from timers or from verbs. With a kit card open the offer gives the encoder back
  to the card.
- Network mode is the fallback, not a mode of its own: its Store and Updates
  pages are the same code, and its Download and Install work. A Sampler with a
  kit loaded is refused the radio by design (wifi.md), which is the usual `no_mem`.

## Actions and their questions (fwCardPlan, community, older, replace, run now)

`crosspad_gui::fwCardPlan(FwCardFacts)` (crosspad-gui, `FwCardActions.hpp`) decides which
buttons are **shown** and which **enabled**; `fw_kit_card.cpp` fills the facts
(catalog, card, slot, updater) at every refresh and applies the plan; the label
is the only thing the UI adds. Every state is a crosspad-gui host test (`tests/test_fw_card_actions.cpp`).

| Button | Shown when | Enabled | Does |
|---|---|---|---|
| **Run the new version** | this version was installed this boot and waits (`readyThis`) | always | `Run the new version now?` -> `fw_flow_switch_and_restart()` |
| **Pin** / **Unpin** | the package is on the card | not while this version is being downloaded or installed | `.pinned` written / removed on the worker |
| **Delete from card** | on the card **and not pinned** (unpin first: pin means keep) | only when the image is **in no slot** and not busy; shown disabled with `Installed in slot X - it cannot be deleted` while a slot has it | confirm, then the worker removes it; **the worker refuses a slot package too** (the line nothing crosses) |
| **Run** (`Try this slot again` when that slot is rolled back) | the image is in the **other** slot and was not just installed | updater idle | confirm (rolled back: `Slot X did not start last time. Try it once more?`), then `fw_flow_switch_and_restart()`; a rolled-back slot is cleared first |
| **Install and run** (`Reinstall` when the other slot rolled this image back) | a **complete** package on the card, no slot has the image (or the other slot rolled it back) | installable verdict and updater idle; disabled with `Cannot be installed: <verdict>` | question chain below, then `fw_flow_install_and_restart()` -- **no network** |
| **Resume download** | a package on the card that is **not** complete, in the catalog | updater idle and not this version | `fw_updater_download_start(kit, version, false)` |
| **Download** | not on the card, in the catalog, no slot has it | updater idle | download only; the tile shows `n %`, the card a status line |
| **Install** (`Reinstall` when rolled back) | as Download | updater idle | download **and** install into the other slot, then `Run the new version now?` |

Consequences worth knowing:
- **The running image offers nothing** except Pin/Unpin (when on the card): the
  status line says `Running now, in slot X`. An image already in the other slot
  is run, never installed again -- except one that slot rolled back, which is
  offered **both ways**: *Try this slot again* (what failed, a missing card or a
  flat battery, may have been fixed) and *Reinstall* (a rolled-back image may be
  broken; writing it fresh is the real fix).
- **Slot-only tiles** (no package on the card) have no Pin or Delete; they get
  *Run* for the other slot, nothing for the running one, and, when the catalog
  knows that version, the Store actions as above.
- Delete is **absent** (not just disabled) while a package is pinned.

**The questions before an install** (`fw_actions.cpp`, in this order; declining
any of them does nothing), for *Install and run* and *Install*:
1. **Community warning** -- before the first install of a community build:
   `CrossPad CI built this firmware from community code. The CrossPad team has
   not tested it. Continue?` Accepting is remembered per kit in `.community_ok`
   once the install has started (shared by both slots). Download alone does not ask.
   The tier is the **version's** (`FwStoreVersion::tier`), not the kit's newest: an
   older community build of a kit promoted to official warns too. The catalog is
   unsigned, so the download also refuses a signed manifest whose tier differs from
   the catalog's (`mismatch`), as it does for the channel.
2. **Older version?** -- when the version is below the newest known for the kit
   (the Store tile's version in the kit's channel, or any version on the card):
   `<v> is older than <newest>. Install it anyway?`
3. The final step: *Install and run* confirms (`Install "<pkg>" (<kit>, <version>) to
   slot X and restart into it.`; an image with no `CPFI` block is worded as unverifiable,
   an older STM protocol adds `Built for an older pad controller protocol - it may
   not work.`; a bootloader without rollback adds `Rollback unavailable: old
   bootloader.`). *Install* asks **Replace the other firmware?** only when the other
   slot is neither empty nor rolled back and holds another kit; otherwise it starts
   at once.

Every follow-up dialog and every flow starts one LVGL cycle later (`lv_async_call`),
because `ui_modal_confirm` restores the encoder group after its callback returns
(see Traps). An automatic check never asks anything; the outcome of a press the user
made is settled by that job's own end (the updater generation moved past the press's),
never by another job that ended meanwhile.

## Threads and memory (worker, icons outside the image cache, the budget)

**One worker** (`fw_view_data.cpp`, task `fw_views`): 8 kB stack **in PSRAM**,
priority 1, core 0, created `WithCaps` and deleted `WithCaps` (or every swap leaks
its stack). It exists only while there is work and **never touches flash** (no NVS,
no `fw_slots_get()`; the LVGL thread reads the slots). Requests (16) and results
(16) are small queues under one mutex: Scan, Store, Detail, Icon, Pin, Unpin,
Remove, Ack. Scan and Store requests are de-duplicated; a card write ends with one
more scan; a full queue drops the request and counts on the next sync or scan to
retry. Results reach widgets only through `fw_view_data_poll()` from the views'
timer (250 ms in the app); late results of a closed view are freed, not applied.
The reader yields to the sample streamer between chunks
(`sample_player_port_yield_to_streamer`), as every bulk reader of the card must.
The worker's static stack depth is ~4.8 kB of 8 kB (the loose-file fold's 4 kB name
array lives in one PSRAM allocation, not on the stack, which it would overflow).

**Icons** (`fw_icons.cpp`) are `lv_image_dsc_t` variable sources over 6 924 B PSRAM
buffers read by the worker, **never in LVGL's image cache** (an uncompressed
RGB565A8 variable source is drawn in place and never cached). Refcounted, shared by
content hash (identical files share one buffer), at most **8** buffers, only for
tiles at least half inside the viewport, trimmed when unheld; past 8 a tile keeps
its letter. An image is detached (`lv_image_set_src(img, nullptr)`) before its buffer
can go. The kit card's chips are the one exception: launcher PNGs decoded through
LVGL's cache, at most 4, dropped (`lv_image_cache_drop`) when the card closes.

**Budget: 131 072 B (128 kB) of PSRAM over the launcher, the WiFi radio included
while Store/Updates hold the lease, kit card not included.** It is measured with
`MEM` `psram_free`, 30 test packages on the card, a kit loaded, BLE on. ~50 kB
of the Store tab's cost is the radio itself, so the budget holds the radio
rather than being measured with WiFi off. The card is measured on its own (it costs what its chips cost) and must give it back on
close. 20 enter/leave cycles return to baseline (per-visit loss <= 64 B).

Measured 2026-10-04/05 on v2, `eac76e8` (`firmware_app` default and
`--switch` with `--tag v1.1.0-fwstore.4`, three runs); rows marked *b8b0229* are
from the build before it (launcher baseline then 490 352 B free):

| What | PSRAM over the launcher |
|---|---|
| Library open | 40 952-47 988 B |
| Library walked to tile 24 (six icons on screen) | 76 256-76 264 B |
| Store tab, radio up (`leases=1`), no card | **96 836-121 764 B** (the spread is the settle sampling; budget 131 072) |
| Kit card open over a tab | +47 436-47 452 B; +564-572 B kept after close |
| Store, card open after a download (what the old scenario measured, *b8b0229*) | 168 368-178 048 B -- over the budget by design, hence the card is separate |
| 30 opens (F1 soak, *b8b0229*) | -116 B against the baseline |
| `firmware_app` churn | `loss_per_visit` -2 960 / -3 102 B (no leak); `app_churn` 88 opens clean (*b8b0229*) |
| Network mode, Store | card +18 400-18 412 B, +1 060-1 076 B kept; 5 279 600 B free (a boot with no engine, *b8b0229*) |

Stacks seen on the board: check job 4.3 kB free, download 2.8-2.9 kB, install 2.9 kB,
`wifi_ctl` 2.2 kB.

**Image size.** The v2 image's `.flash.text` sits just under a 64 kB page
boundary (2 031 616 B). Core's Store cache filter is a constant JSON document
parsed by the `deserializeJson(const char*, size_t)` the catalog parser already
links, not 17 proxy chains (986 B of text, enough to cost a page). As of
2026-10-05 `.flash.text` is **2 030 904 B, 712 B under the boundary**:
`ext_ram.dummy` v2 2 686 944 (41 pages), v1 2 621 408 (40 pages; text 1 933 644 B,
~32 kB headroom). Check `xtensa-esp32s3-elf-size -A build_v2/CrossPad.elf | grep
-E 'flash.text|ext_ram.dummy'` after adding code to v2: the next 712 B cost the
launcher 64 kB.

## ui_tile_grid

`components/crosspad-gui/.../ui_tile_grid.{h,cpp}` is portable (LVGL only; the PC
simulator can use it). A **pool of rows** (viewport rows + 2, at most 8) is created
once; scrolling moves a row that left the window to one that entered it and calls
`fill` for the new items; `unused` frees per-tile state (an image buffer above all).
Up to 4 sections with optional titles (Official / Community), each starting a new row.
Encoder focus moves over **logical indices**: the pool's tiles sit in the caller's
group in item order, rearranged after every recycle with `lv_group_swap_obj()` only,
and the walk coming onto the grid from another object lands on item 0 going forward
and on the last item going back, whatever rows are built. The pool grows with a
resized grid (`LV_EVENT_SIZE_CHANGED`), never shrinks. Callbacks may call back into
the grid (the re-entry guard ignores the focus and scroll events they raise), but
move the selection with `ui_tile_grid_select()`, not `lv_group_focus_obj()`. A caller
sizes the grid **before** adding other focusables to the group: tiles added by a later
pool growth join the end of the group. Host tests (headless LVGL 9.3 + Catch2, with
snapshots as PNG) live in `components/crosspad-gui/tests/`.

## FW_UI and the firmware_app scenario

`FW_UI` (CDC, `fw_ui_state.cpp`; any task may ask) answers one line, so a test drives
the UI with `ENC_*` and reads this instead of a screenshot:

```
FWUI: tab=<library|store|updates|wifi|-> sel=<index|-> card=<kit@version|-> items=<n> selkey=<kit@version|-> host=<app|net|-> store=<ok|missing|format|other_board|-> busy=<0|1>
```

`items`, `sel`, `selkey` and `store` describe the **tab on screen**; network mode keeps
every tab built, so a view writes them only while `fw_ui_state_tab_is()` its own tab
and writes them again when it is shown (the WiFi tab, which has no `shown`, clears
them). `busy=1` while the view worker has requests or results pending **or B's
updater is busy** (`fw_updater_busy()`, cleared only after the check job has freed
its buffers, so a PSRAM reading at `busy=0` is not ~143 kB short): wait for 0
before reading. `store=` is the card cache's state, `-` until the Store view has loaded it. Reply format and its worst-case length are computed from the fields, so nothing
is ever cut.

`crosspad-hil run firmware_app [--switch]` (hil `scenarios/firmware_app.py`) pushes 30
test packages (a manifest and a distinct icon each, no image), then: Library list and
budget, a 24-tile walk, a card (Pin, Delete not offered, Unpin, Delete, power click),
the Store tab against the test server's catalog, the card's own budget (64 kB open, 4 kB kept),
a Store download, 20-cycle churn, and the same download in network mode. `--switch`
adds the Run step (the other slot's package; the board goes away and back). Needs a saved WiFi network on the host's LAN and a release with
icons and kit text (`v1.1.0-fwstore.4` or later; the default tag is the newest
release whose kit packages carry icons). Its teardown removes the test catalog's
`.icons/` and `.details/` and puts the card's own `.catalog.json` and
`.community_ok` back as they were before the run.

## Failure rules

| Situation | Outcome |
|---|---|
| No WiFi, WiFi refused, catalog unreachable | Store shows the last fetched catalog with its date and the reason; Library and Updates still work |
| No cache file | `Never fetched` |
| Cache written for another board revision | `Store unavailable - catalog of another board`; nothing of it is offered |
| Cache truncated (card pulled mid-write), `"v":2`, or no kit parses | `Store unavailable - catalog unreadable`; the Store grid is empty, the Library works |
| Icon missing, invalid or not fetched | the tile keeps its letter; the next check retries |
| Detail file missing or without this version | the card shows the manifest's text, else `No description.` |
| Package without `.complete` | `incomplete` / the live progress; the card offers *Resume download* and Delete |
| Delete requested for a package in a slot | disabled with the reason; refused again by the worker |
| Package changes under an open card (deleted over CDC, finished, installed) | the plan is recomputed from the next scan; actions act on the package name; deleting a vanished package is a no-op |
| Community or older-version question declined | nothing happens |
| Updater busy | other actions disabled; a refused press toasts `Busy - try again in a moment` |
| Library scan finds no memory or no card | retried up to 4 times with doubling waits, then `Could not read the SD card` |
| `no_mem` | once per showing, `wifi_fallback_offer()`; never in network mode |

## Traps

- **A dialog opened from a dialog's callback is undone** by the first one's group
  restore (`ui_modal_confirm` gives the encoder back after its callback returns).
  Every follow-up dialog and every flow overlay starts from an `lv_async_call`.
- **`lv_group_swap_obj()` moves the focus with the slot**, so a recycled tile under
  the encoder ends up as another item: the grid refocuses after reordering.
- **A tile's image must be detached before its buffer is released** (`lv_image_set_src(img, nullptr)`).
- **LVGL's `LV_EVENT_DELETE` reaches the grid before its tiles**: the grid clears its
  user data first, then the tiles free their own state.
- **LVGL 9.3 tab buttons send no `VALUE_CHANGED`.** `button_clicked_event_cb()` makes the
  tab active *before* it scrolls, so the scroll's end finds nothing changed;
  `VALUE_CHANGED` comes only from a swipe that ends on another page. Both hosts switch
  tabs on `LV_EVENT_CLICKED` of the tab buttons (the tabview's own handler was added
  with the button, so it has run by then).
- **A control focused on a hidden network-mode page slides the pages over to it**
  (scroll-on-focus), so FW_UI would say `tab=wifi` with the Store on screen. Hence one
  group per tab, pages made non-scrollable, and swipes off.
- **A Store page built in network mode must not take the encoder on its first
  catalog unless it is on screen** (`lv_obj_is_visible(grid)`), or its focus would
  slide the tab view.
- **Reading the slots on the worker is forbidden** (`fw_slots_get()` reads flash and
  the worker's stack is PSRAM); the LVGL thread reads them and hands copies to the views.
- **Kit-card buttons are hidden, not removed**: `ENC_GROUP` lists them all.
- **A card opened from the Library needs the catalog too**; `fw_views_want_store()` is
  counted, the last user drops the ~5.4 kB.
- **Teardown order matters:** `fw_actions_forget()` (an open confirm is answered "no" and
  hands the encoder back to the card), then `fw_kit_card_close()`, then the pages; leaving
  an app with a card open must not free the views' data only to load it again.
- **`SCREENSHOT` cannot capture the overlay layer** (it snapshots the active screen), so
  kit cards, dialogs and the flow overlay are proven through `FW_UI` / `ENC_GROUP` /
  `ENC_FOCUS`. It also needs one **230 400 B** contiguous block (320x240 RGB888): after a
  radio cycle the largest free block is ~225-229 kB and it answers `no memory`; it works
  reliably only on a fresh boot.

## Known limitations

- **32 packages.** The scan, the cleanup and the Library hold at most 32 package
  directories (`kFwGcMaxPackages`, 34 tiles with the two slot-only ones); `FW_LIST`
  prints 16. The 33rd package is on the card but not listed: its card keeps offering
  Download/Install although the catalog says `on_card`. Delete one package to see it.
- **Touch swipes between tabs are disabled** in both hosts (a swipe would show a page
  that is not built, or move to a page without moving the encoder there). The tab bar
  and the encoder are the only ways.
- **v2 is 712 B of text from its next 64 kB PSRAM page** (41 pages, see Threads and
  memory): crossing it costs the launcher 64 kB.
- **crosspad.app does not serve the catalog and kit routes yet** ([firmware-updates.md](firmware-updates.md), 2026-10-04: a
  page answers 404), so against the default URL a check ends `err=catalog:http`; the
  bench serves them with `crosspad_hil.fw_server`.
- The *Update* rows hide while a slot holds a newer build of the kit; a rolled-back slot
  holds it too.
- With WiFi off and an update waiting, the Updates tab shows the installed-in-the-other-slot
  state and *Run* without the WiFi reason; the row names the newest package on the card as
  the old side.
- The scenario's `--switch` step restarts the board: it must wait for the card's *Run* to
  verify the image and reset (~0.6 s after the press) before reopening the link.
