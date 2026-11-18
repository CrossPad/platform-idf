# Key paths

Contributor notes: where each thing lives. The short version of what a change in this area
must not break; the pages it links to have the detail.

## Key Paths

| What | Path |
|------|------|
| BSP API (headers; reference generated at https://crosspad.app/docs/reference/bsp/) | `components/bsp/crosspad/include/` |
| Component dependencies and versions (LVGL, TinyUSB, ArduinoJson, littlefs, esp-now, …) | `main/idf_component.yml`, `components/bsp/crosspad/idf_component.yml` |
| Architecture / pipeline diagrams (components and data flow; notes, MIDI and apps end to end) | `docs/architecture.html`, `docs/pipeline.html` |
| Doxygen config / Pages workflow | `Doxyfile`, `.github/workflows/docs.yml` |
| Audio codec docs | `components/bsp/crosspad/AUDIO_ES8388.md` |
| Performance monitor docs | `components/bsp/crosspad/PERFMON.md` |
| Example app (template) | `main/app/example/` |
| Settings app | `main/app/settings/` |
| Platform adapters | `components/crosspad-platform-idf/` |
| LVGL→PSRAM allocator wrap | `main/lvgl_mem_psram.c` |
| Audio task creation | `main/audio/audio_task.cpp` |
| Factory provisioning | `tools/factory_provision.py` |
| Auto-generated registry | `main/app/app_registry_init.cpp` (never edit) |
| Audio routing / USB capture | `main/audio_route_control.cpp`, `main/include/audio_route_control.h` |
| Audio engine rate (BSP clock, UAC rate, HIL tone kit) | `components/bsp/crosspad/i2s_driver.c`, `main/audio/audio_control.cpp`, `tools/make_tone_kit.py` |
| Kit-swap audio fixture (two interleaved sine kits, `crosspad-hil run kit_swap_audio`) | `tools/make_sine_kits.py` |
| Bench entry point (check / ready / flash / test) | `tools/bench.py` |
| Board revision resolver (`idf.py board`) | `tools/crosspad_board.py` |
| ULP RISC-V program / BSP API | `components/bsp/crosspad/ulp/ulp_background.c`, `components/bsp/crosspad/include/bsp_ulp.h` |
| HIL command set (device side) | `main/hil_control.cpp` (reply path, dispatch table `k_verbs`, companion allow-list), `main/hil_verbs_<domain>.cpp` (the verbs), `main/hil_verbs.h` |
| Assets flashing / file transfer (device) | `main/file_transfer.cpp`, `main/include/file_transfer.h` |
| Assets flashing / file transfer (host) | `tools/fs_transfer.py` |
| USB transfer mode (boot flag + MSC callbacks, screen, descriptors) | `main/usb_transfer.cpp`, `main/gui/usb_transfer_screen.cpp`, `main/usb_descriptors_transfer.c` |
| Firmware library / A/B switch (image descriptor, package scan, slots + rollback, assets resync, Firmware app) | `main/fw_image_desc.c`, `main/fw_library.cpp`, `main/fw_slots.cpp`, `main/fw_assets_sync.cpp` (headers in `main/include/`), `main/app/update/update_app.cpp`, `main/gui/fw_flow_overlay.cpp`, `tools/make_fw_package.py` |
| Firmware updates (catalog, download, cleanup, triggers, verbs, Updates view, Settings page) | `main/fw_updater.cpp`, `main/fw_updater_verbs.cpp`, `main/fw_download.cpp`, `main/fw_catalog.cpp`, `main/fw_http.cpp`, `main/fw_gc.cpp`, crosspad-gui `src/fw/fw_updates_view.cpp`, `main/app/settings/settings_updates.cpp`, [docs/subsystems/firmware-updates.md](../subsystems/firmware-updates.md) |
| Firmware signing and CI kits | `tools/fw_sign.py`, `tools/make_fw_package.py`, `tools/ci_matrix.py`, `tools/ci_release_assets.py`, `config/fw_signing/`, `main/fw_sign_pubkey.h`, `main/fw_verify.cpp` |
| STM32 in-field update | `components/stm32_hal/stm32_fw_update.c`, UI `main/gui/stm_fw_overlay.cpp`, image `main/stm32_fw/stm32_fw.bin` |
| Firmware app views (tiles, card, actions, Store/Updates/Library, icons, the app shell; portable, host tests) | `components/crosspad-gui/{include/crosspad-gui,src}/fw/` (`IFwViewService.h`, `fw_*_view.cpp`, `fw_kit_card.cpp`, `fw_actions.cpp`, `fw_views_common.cpp`, `fw_icons.cpp`, `fw_app.cpp`), `components/crosspad-gui/tests/test_fw_views.cpp` |
| Firmware app on the board (service, worker, FW_UI) | `main/fw_view_service.cpp`, `main/fw_view_data.cpp`, `main/fw_ui_state.cpp`, `main/fw_store_files.cpp`, [docs/subsystems/firmware-app.md](../subsystems/firmware-app.md) |
| Kit card action table (portable, unit-tested) | `components/crosspad-gui/include/crosspad-gui/fw/FwCardActions.hpp`, core `crosspad/fw/FwStore.hpp` |
| Virtual tile grid (portable, host tests) | `components/crosspad-gui/src/components/ui_tile_grid.cpp`, `components/crosspad-gui/tests/` |
| Mixer engine (render, sends, renderTap; host tests over the scalar and the esp-dsp path) | `components/crosspad-mixer/src/AudioMixerEngine.cpp`, `components/crosspad-mixer/tests/` — in the mixer: `cmake -S tests -B tests/build && cmake --build tests/build && ctest --test-dir tests/build` |
| Kit icons (PNG -> LVGL RGB565A8) | `tools/fw_icon.py`, `config/profiles/icons/` |
| Browser flasher (ESP OTA / ESP ROM / STM DFU) | `tools/webflasher/index.html` |
| Pitched instrument (engine core, platform port, fixture) | `components/crosspad-core/include/crosspad/instrument/`, `components/crosspad-platform-idf/PitchedKitPort.cpp`, `tools/make_pitched_kit.py` |
| Shared engine PSRAM arena / prebuilt streamer start-stop | `components/crosspad-platform-idf/engine_arena.cpp`, `main/audio/engine_handoff.cpp`, `main/include/engine_handoff.h` |
| Sample-player thread arbitration | `components/crosspad-platform-idf/include/SamplePlayerAccess.hpp` |
| Kit load worker | `components/crosspad-sampler/src/logic/kit_load_task.cpp` |
| Kit browser (UX 2000: PERF / DRUMS / SLICE / PITCH / SONGS / USER, filed by folder (core `KitFolders`), LOAD, EDIT card, pads play the highlighted kit via a `kit_peek` worker) and the kit picker | `components/crosspad-sampler/src/ui/kit_selector.cpp`, `components/crosspad-sampler/src/logic/kit_gate.cpp` |
| HIL suite (host side) | `crosspad-hil` package; `tools/hil_*.py` are shims |
| Recorder / Slicer (sample tools: takes, audition, loops, kits from slices) | `components/crosspad-platform-idf/SampleToolsPort.cpp`, `crosspad/sampletools/`, [docs/subsystems/sample-tools.md](../subsystems/sample-tools.md) |
| WiFi (link, saved networks, verbs, page, fallback) / network mode (boot flag, screen) | `main/wifi_link.cpp`, `main/wifi_creds.cpp`, `main/wifi_verbs.cpp`, `main/wifi_ping.cpp`, `main/gui/wifi_page.cpp`, `main/gui/wifi_fallback.cpp`, `main/net_mode.cpp`, `main/gui/net_mode_screen.cpp`, [docs/subsystems/wifi.md](../subsystems/wifi.md) |
| Partition table | `partitions_16mb.csv` |
| Subsystem deep-dives (source of truth for the summaries above) | `docs/subsystems/*.md`, `docs/MANIFESTO.md` |
| SDK config defaults | `sdkconfig.defaults` |
| Repository checks (Kconfig invariants, image budget, conventions) | `tools/check_sdkconfig.py` + `config/sdkconfig_invariants.json`, `tools/check_image_budget.py` + `config/image_budget.json`, `tools/check_conventions.py` |
| Open work (short list) | `TODO.md` |
