# GUI map in crosspad-docs

Date: 2026-10-21. Repos touched: crosspad-gui, platform-idf (gui pin), crosspad-pc
(branch `feat/web-twin`), crosspad-web-twin, crosspad-docs.
The accepted HTML mock-up is referred to below as "prototype v2".

## Intent

A page in the docs site (crosspad.app/docs, User guide section) that shows **every
CrossPad screen**, so a reader sees what is in the device and where each thing
lives — mainly people curious what CrossPad contains (before buying), also players
looking for a control. It is a map: pan, zoom, and the amount of detail changes
with zoom (semantic zoom).

The screens are **real screenshots, refreshed automatically**, collected by an
automated walk through the GUI. The walk also records each control's rectangle on
screen; those rectangles are what the callout arrows point at. The control
descriptions are drafted from the code and the existing docs; a maintainer
corrects them afterwards.

**Source of the screens: the web twin** (crosspad-web-twin: the platform-idf
firmware — core, gui and every app — compiled to WebAssembly, run in headless
Chrome). Phase 1 is PC only, the rest through the bench; the twin
is the PC-side runner that already feeds the user guide's screenshots
(`crosspad-docs/web/scripts/guide-shots.mjs`) and crosspad.app's app clips, and its
CI (`live.yml`) rebuilds it from the current firmware every night. It shows the
firmware's own app set, which the crosspad-pc native simulator does not.

## Goals

1. Every app of the public twin build and its reachable sub-screens (tabs,
   categories, card/detail views, overlays reached by a button) appear on the map.
2. Three detail levels, switched by zoom and by a 3-way control (Apps / Screens /
   Controls):
   - **Apps** — one band per app: name and one-sentence summary; screenshots faint.
   - **Screens** — screenshots with titles; arrows "this control leads here" from the
     control's rectangle to the target screen, the control's label in a pill on the arrow.
   - **Controls** — numbered markers on the interactive controls of a screen,
     callout boxes in columns left/right of the screenshot (side chosen by the
     control's x centre, sorted by y), elbow leader lines, hovering a callout
     highlights its control and line.
3. Layout as in prototype v2: launcher at the left, one row (band) per app, a screen's
   children in the next column to the right (tidy tree; deeper levels keep going right).
   A side list of apps and their screens flies the view to a screen; clicking a
   screenshot zooms it to the Controls level.
4. Screenshots and coordinates refresh without a human: the twin's `live` CI produces
   them, the crosspad-docs generate pipeline pulls them in.
5. A screen or control without a written description still appears (the control with
   its on-screen label); the pipeline reports what lacks a description and which
   descriptions no longer match any captured control.

## Non-goals

- In phase 1, capturing from a real board. Phase 1 is the twin only; the page says
  the screens come from the firmware running in the browser. Device-only screens are
  phase 2 (section 8).
- An interactive emulator in the page — the map shows pictures, it does not run the UI
  (the `<crosspad-device>` embed already exists for that).
- Translation. The site is English; descriptions are English.
- Showing transient states (playing, recording, a loaded song's content) beyond what a
  fixed start state shows.
- Replacing `guide-shots.mjs` or the app clips; both stay as they are.

## Anti-goals (failure even if it "works")

- Coordinates or screen lists maintained by hand.
- The walk changing state it does not restore, or doing anything destructive: power
  off, firmware install/switch/delete, factory/settings reset, format, deleting kits,
  songs or recordings, network mode, USB transfer mode.
- The docs build depending on the twin being available — captured data is committed
  to crosspad-docs, the build only reads it.
- Screenshots churning on every run with no GUI change (non-deterministic start state,
  clock, battery, random content) — every regenerate would become a noisy commit.
- Stale data shown without saying so: the page states the firmware commit and the
  capture date.

## Design

### 1. Widget probe (crosspad-gui)

New shared function in crosspad-gui, same pattern as `transportbar_probe`
(`include/crosspad-gui/components/transport_bar.h`), usable by the twin now and the
firmware later. It walks `lv_screen_active()` and `lv_layer_top()` (overlays,
dialogs, cards live there) and returns, for every visible object (not
`LV_OBJ_FLAG_HIDDEN` up the parent chain, area intersects the 320x240 LCD,
effective opacity > 0):

- class name, absolute coords (`lv_obj_get_coords`), parent index;
- text: an `lv_label`'s own text, otherwise the first descendant label within 3
  levels (same rule as `widget_label()` in `platform-idf/main/hil_control.cpp`);
- `interactive`: `LV_OBJ_FLAG_CLICKABLE` set, or the object is in the encoder group
  currently on the navigation encoder indev;
- states: checked, disabled, focused.

No LVGL mutation. Host test in `crosspad-gui/tests`. MINOR version bump of
crosspad-gui; platform-idf bumps its `components/crosspad-gui` pin (the twin compiles
the gui platform-idf pins).

### 2. Twin export (crosspad-pc `feat/web-twin`, `src/wasm/wasm_bridge.cpp`)

`EMSCRIPTEN_KEEPALIVE const char* wasm_ui_tree()` beside `wasm_ui_apps()`: JSON
`{running, lcd:{w,h}, nodes:[…]}` in LCD pixels (the space `wasm_touch` takes), read
directly like `wasm_ui_app()` does. `web/simtwin.js` in crosspad-web-twin gains
`uiTree()` (parsed JSON; `null` on older builds without the export).

### 3. Capture walk (crosspad-web-twin, `tools/capture/`)

`gui_map.mjs` (Node + Playwright, like `capture.mjs`), on the same `page.html`
(exposes `window.uiTree`, `window.lcdPng` beside the existing steps).

- **Start state**: a fresh browser context per app (as `capture.mjs` does), the card
  from release `card-v1`, the twin's fixed clock. Two runs on the same firmware must
  give byte-identical PNGs; anything that still varies (battery, meters, a clock) is
  fixed in the twin or masked before saving.
- **Settle**: a frame is taken when `sim.lcd()` has not changed for 2 polls and the
  running app is the one expected (`sim.app()`).
- **Apps**: `sim.apps()` of the public build (`CROSSPAD_PUBLIC=1`, so unreleased apps
  never reach the site), minus "Power OFF" and "CITest".
- **Walk**: depth-first from each app's root screen, max depth 3 below the app root,
  a global cap on screens and time. On each screen take the PNG and `uiTree()`. Then
  activate each interactive node in reading order — touch tap at its centre; if
  nothing changes and it is in the encoder group, rotate the encoder to it and press
  — except:
  - value controls that change state in place: switch, checkbox, slider, arc, roller,
    dropdown, textarea/keyboard;
  - labels matching the destructive deny list (regex, case-insensitive: the words in
    the anti-goals plus "erase", "remove", "forget", "install", "switch", "restart",
    "reboot", "factory", "format", "delete", "reset", "power").
  If the screen fingerprint changed (running app + set of interactive class/label
  pairs + their geometry rounded to 8 px), it is a new screen: record it and the edge
  (from-screen, widget key, to-screen), recurse, then go back with the power button
  until the parent's fingerprint returns (3 tries); otherwise reopen the app (fresh
  context) and replay the path. A fingerprint seen before only adds an edge.
- **Not explored**: a control skipped by the deny list or the depth limit is still a
  control of its screen (marker + description), without an arrow; it carries
  `explored: false` and the reason.
- **IDs**: screen id `<app>/<slug of the labels clicked to reach it>`; widget key
  `<class>:<label-slug>`, `#n` suffix for duplicates, unlabelled
  `<class>#<reading-order index>`. Keys stay stable while labels stay stable —
  descriptions are keyed by them.
- **Output**: `dist/gui-map/map.json` (schema version, capture date, firmware commit
  as `capture.mjs` records it, `screens[{id, app, title, parent, png, widgets[{key,
  class, label, x, y, w, h, interactive, explored, skip}]}]`, `edges[{from, widget,
  to}]`) and `dist/gui-map/screens/<id>.png` (320x240, lossless).

### 4. CI (crosspad-web-twin `live.yml`)

In the "Embed, live screens, add-on" step, after `capture.mjs`: `node
tools/capture/gui_map.mjs`; the publish job adds `gui-map.zip` to release `live`. A
walk that cannot open an app fails the job; a sub-screen that the previous `live`
had but this run lacks is a warning in the log.

### 5. Pull into crosspad-docs

`scripts/fetch_gui_map.py`, called from `generate.yml`: download `gui-map.zip` from
release `live` of CrossPad/crosspad-web-twin (`gh`, `DOCS_PAT`), write PNGs to
`web/public/gui-map/screens/` and the JSON to `web/src/data/gui-map.json`; leave files
untouched when equal, so an unchanged GUI makes no commit. Then report, in the job
log: controls/screens without a description, descriptions whose key no longer
exists. The report never fails the job or the docs build.

### 6. Descriptions (crosspad-docs)

`web/src/data/gui-map-text.yml`: per app `summary`; per screen `title`, `summary`;
per widget key `text` (one or two sentences, what it does for the user), optional
`label` override. The first full set is drafted from the apps' code and the
existing guide pages; a maintainer edits it later.

### 7. Page (crosspad-docs)

`web/src/content/docs/guide/screen-map.mdx` (the guide's autogenerated sidebar picks
it up), using a new `web/src/components/ScreenMap.astro` with a client script
(vanilla TS, no new dependency), behaviour as prototype v2 plus:

- full content width (no right-hand TOC), Starlight colour variables in light and
  dark;
- touch: drag to pan, pinch to zoom; the side list becomes a select on narrow screens;
- deep link `#screen=<id>` flies to that screen on load and updates as you move;
- screenshots as SVG `<image>`, `image-rendering: pixelated`, loaded only when the
  Screens level is reached and the screen is near the viewport;
- below the map, a plain-HTML "All screens" section (collapsed) listing every screen
  with its controls and texts — works without JS and is what the site search indexes;
- footer line: firmware commit and capture date from `map.json`.

Layout (positions, tree columns, callout placement) is computed from `map.json` at
build time, not in the browser.

### 8. Phase 2: device-only screens from the bench

Screens the twin lacks or shows differently (WiFi, USB profiles, BLE, network mode,
STM update, transfer mode, charge screen…) come from a real board, after phase 1
ships; a separate plan:

- firmware verb `UI_TREE` in `main/hil_control.cpp`, same crosspad-gui probe, one
  `UI_TREE:` prefixed row per node (multi-line reply convention);
- the PNG from the existing `SCREENSHOT` (BMP on `/sdcard/crosspad/screen.bmp`),
  pulled with `tools/fs_transfer.py pull` and converted;
- a crosspad-hil scenario `gui_map` running the same walk over CDC verbs
  (`APP_START`, `TOUCH_TAP`, `ENC_*`, `PWR_GESTURE CLICK`), run by hand through
  `tools/bench.py test gui_map` — not CI;
- its output is a second `map.json` with `source: "device"`; the docs fetch merges it,
  a device screen with the same id as a twin screen replacing it, and the page marks
  device screens.

`SCREENSHOT` cannot capture the overlay layer (cards, dialogs) — phase 2 has to fix
that or mark those screens as not captured.

## Constraints

- crosspad-docs: Node 22, `npm run build` must stay green (link validator, facts);
  `check_site.py` rules apply to the new page.
- crosspad-gui stays portable (no twin- or ESP-specific code in the probe); LVGL
  9.3 API only.
- crosspad-web-twin `live.yml` has a 60 min budget for the whole job; the walk gets a
  time cap that leaves the existing steps their time.
- Only the publish job writes the release (the workflow's existing rule).

## Left to the builder

Settle polling interval, fingerprint details beyond the above, tidy-tree algorithm,
styling, PNG optimisation (lossless, deterministic), how the twin pins its clock and
battery if it does not already, exact JSON field names beyond the ones listed.
