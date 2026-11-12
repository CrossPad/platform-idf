# Build and memory budgets

Contributor notes: sdkconfig traps, PSRAM and internal RAM, image size, LVGL build choices, Doxygen. The short version of what a change in this area
must not break; the pages it links to have the detail.

## Build Gotchas

- **~1950 compilation units** — full build takes minutes. Use OTA flash for iteration.
- **`idf.py -B build_v2 fullclean`** (the revision's build dir) required after new app directories or CMakeLists.txt changes.
- **LittleFS venv mismatch:** If `build_<rev>/littlefs_py_venv/` was built with a different Python version, you get "circular import". Fix: delete `build_<rev>/littlefs_py_venv/` and rebuild.
- **WHOLE_ARCHIVE** is set on main component — required for REGISTER_APP functions to survive linking.
- **CMake variable scoping:** App `set(SOURCES ...)` must NOT use `PARENT_SCOPE`. Parent CMake auto-collects.
- **Compile definitions must be set BEFORE `project()`.** ESP-IDF adds every component as a subdirectory from inside `project()`, and directory compile properties only propagate into subdirectories created afterwards. `add_compile_definitions()` after `project()` reaches a few late targets in `compile_commands.json` and no component source, so a definition set there silently does nothing. Kconfig is not parsed that early, so anything revision-dependent reads the `sdkconfig` file directly.
- **`sdkconfig.defaults` silently wins a Kconfig `choice` over `sdkconfig.v*`.** `main/lvgl_mem_psram.c` is gated on `LV_USE_STDLIB_MALLOC == CUSTOM`, but `sdkconfig.defaults` sets `CONFIG_LV_USE_CLIB_MALLOC=y`, which wins in every build dir — `sdkconfig.v2`'s `CUSTOM` choice included — and every LVGL widget then costs ~48 B of internal RAM (internal RAM free: launcher 28 kB without the PSRAM allocator, 63 kB with it; Sampler+kit 4.7 kB against 63 kB). So `lv_malloc_core`/`lv_realloc_core`/`lv_free_core` are routed through `-Wl,--wrap` in `main/CMakeLists.txt`, outside the Kconfig guard — that holds regardless of how a regenerated sdkconfig lands the choice. Verify with `crosspad-hil` `mem --blocks`: the `<=64`/`<=128` used-block counts should match the launcher baseline, and the internal-RAM win should show up as `psram_blocks` growing instead. Other `sdkconfig.defaults` vs `sdkconfig.v*` choices may be in the same state — check before trusting one.
- **PSRAM, not internal RAM, is what the UI runs out of.** The heap pool is
  6.2 MB. The engines take PSRAM as a kit needs it (64 kB heads, a stream pool that grows with the voices
  playing and shrinks after 4 s of quiet, `engine_arena.cpp` floor 512 kB) --
  ~4.9 MB free at boot, ~4.0 MB with POP_ICONIC loaded. The status bar always
  shows `PS` (free) / `BLK` (largest block). Kit list, LVGL's image cache and
  every app share it with the engines. Three
  rules keep it working: the boot splash is never a decoded picture
  (crosspad-gui `src/components/boot_splash.cpp`: objects and a 6 kB alpha logo compiled in; a
  PNG logo needs a 461 kB decode buffer at a moment ~520 kB are free, and fails
  on every refresh); `LV_CACHE_DEF_SIZE` is
  192 kB (600 kB lets decoded thumbnails eat everything the selector needs —
  `bad_alloc` in the next pitched load, `abort()`, reboot); the kit selector
  is a virtual list (objects only for ±8 rows around the highlight, ~3 kB and
  ~50 allocations per row; 79 kits in a non-virtual list are 237 kB). `LoadMainScreen()` also
  drops the image cache, `lvgl_mem_psram.c` keeps a 64 kB pool of PSRAM only
  LVGL allocates from once PSRAM is gone (≤ 16 kB a block; handed to the
  general heap it would be eaten at once), an LVGL assert is a panic, not
  LVGL's `while(1)` (`main/lvgl_assert/`, `CONFIG_LV_ASSERT_HANDLER_INCLUDE`),
  and the log callback is budgeted to 20 lines/s. Measure
  with `crosspad-hil mem` at the launcher (2026-10-03, kit loaded, BLE on:
  ~275 kB free, largest ~225 kB) and with the Mixer open (~130 kB).
- **The render runs from PSRAM through the I-cache, which both cores share.**
  32 kB of it, not 16 (`CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB` in both
  `sdkconfig.v1` and `sdkconfig.v2`, 2026-10-23):
  sixteen streamed voices in the USB-audio profile went from 844 to 613 us
  average a block, worst 1202 -> 1036, and both cores' idle share from 67/61
  to 79/75 %. The 16 kB of internal RAM it takes were paid by moving tables
  and stacks nothing realtime touches to PSRAM (waveform cache table, OTA
  write buffer, `smpl_pad` and `audio_bg` stacks, launcher icon paths, a verb
  buffer). The render's per-block buffers (`dsp::AlignedVector`: the mixer's
  channel scratch, buses, pads tap) are internal RAM first, allocated for
  registered channels only. Check `MEM` int_min after a boot with WiFi up
  after anything that moves internal RAM.
- **The image size is PSRAM too.** `SPIRAM_FETCH_INSTRUCTIONS` + `SPIRAM_RODATA`
  copy `.flash.text` and `.rodata` into PSRAM at boot (`.ext_ram.dummy`, 64 kB
  pages), so every kB a feature adds to the image is a kB the UI loses: the
  A/B firmware library and the fonts took the launcher from 286 to 76 kB
  free, too little for the explorer and the Sampler. The firmware is
  built `-Os` for that reason, with the hot paths at `-O2` (`main/CMakeLists.txt`):
  LVGL (Settings list 1.91 µs/px against 2.05 at `-O2` everywhere; its themes,
  observer and text entry widgets stay at `-Os`, 2.2 kB) and the USB
  audio path — bridge, UAC, TinyUSB, `crosspad-platform-idf` (but its
  per-file list of non-RT files), mixer; at `-Os` a UAC2 loopback glitched
  ~10×/s. 256 kB free. Keep `crosspad-core` off that list: it costs a 64 kB
  page (141 kB free), too little for the explorer.
  `python3 tools/check_image_budget.py --rev v2` prints the pages the image
  takes and how far `.flash.text` is from the next one (crossing it moves
  `.rodata` a whole page), against `config/image_budget.json`; CI fails a full
  kit that needs more pages. Run it after anything big; raising the budget is
  a decision made in the pull request that needs it.
- **LVGL's software blend lives in flash, and only the renderers we draw with
  are built.** `LV_ATTRIBUTE_FAST_MEM_USE_IRAM` put ~68 kB of blend code in
  IRAM, and the AL88/L8/I1 renderers nothing produces cost 41 kB more. Both are
  off in `sdkconfig.v*`, and `sdkconfig.defaults` must never set
  `FAST_MEM_USE_IRAM` again: it wins over `sdkconfig.v*`. Together that is
  +67.8 kB internal RAM (launcher 31 → 99 kB) for a 4 % slower Settings scroll.
  RGB888 must stay: with `LV_COLOR_DEPTH=24` LVGL renders every opaque layer
  part in it, and `SCREENSHOT` snapshots it. ARGB8888 must stay too: every PNG
  decodes to it. XRGB8888, the roller, the menu and the grid layout are off
  too (nothing produces or creates them; ~8 kB of image). Measured numbers: the WiFi spec, Revision 2
  (`docs/design/2026-10-02-wifi-on-demand-design.md`).
- **LVGL's flush wait sleeps** (`bsp_flush_wait()`, `crosspad_s3.c`): without a
  `flush_wait_cb` LVGL spins on `disp->flushing` at priority 4 on core 0 for the
  ~3.8 ms each 30-row band takes over the SPI, and a screen redrawn whole every
  frame leaves nothing below it on core 0 any time -- a CDC verb took
  1.4 s. A tick's sleep a poll costs ~0.5 ms a band.
- **Nothing slow on the LVGL thread.** `KitOrchestrator::changeKit()` parses the
  kit off the SD card — inline in the selector's event handler that is
  seconds of frozen UI and a task watchdog — so it runs inside
  `kit_load_task_start(onlyModified, kitId)` with the result handed back through
  atomics and an `lv_timer`; platform follow-up work goes in
  `SamplerAudioCallbacks::onKitLoadFinished`. Anything touching storage belongs
  on that side of the boundary.
- **No unit tests on this side** — `crosspad-hil run <scenario>` against real
  hardware is the test suite (see [docs/subsystems/hil-testing.md](../subsystems/hil-testing.md)). No linting
  configured.

## API Documentation (Doxygen)

`doxygen` in the repo root writes browsable HTML to `docs/doxygen/html/` (open
`index.html`) for `main/` and `components/crosspad-platform-idf/` — dark theme,
sidebar, hyperlinked source. `.github/workflows/docs.yml` publishes it to
GitHub Pages (https://crosspad.github.io/platform-idf/) on every push to `main`
that touches those paths. crosspad-core and crosspad-gui have their own
Doxyfiles: run `doxygen` in each submodule root. The BSP is not in this
Doxyfile; its reference is generated on the docs site.
