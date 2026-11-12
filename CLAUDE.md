# CLAUDE.md — CrossPad ESP-IDF

## What This Is

CrossPad: ESP32-S3 pad controller. 320x240 LCD, 16 pressure-sensitive RGB pads, rotary encoder, touch, IMU, dual ES8388 audio codecs, DRV2605 haptics, ESP-NOW wireless, USB MIDI+CDC+UAC2, BLE MIDI, UART MIDI through the STM32. ESP-IDF 5.5.x (5.5.4 minimum, CI on v5.5.5; 6.x unsupported), LVGL v9.3, FreeRTOS.

**This repo = platform template.** Full BSP + drivers + crosspad-core + crosspad-gui. Apps are independent modules loaded as git submodules into `main/app/` or `components/crosspad-*/`. Business logic in `crosspad-core` (portable C++ library shared with Arduino repo `ESP32-S3` and `crosspad-pc`) with app build blocks in `crosspad-gui`.

**User documentation is the site, not this file: https://crosspad.app/docs/** —
[Start here](https://crosspad.app/docs/start-here/) (toolchain, board revision,
build, first flash, check), how-tos (`how-to/new-app/`, `install-apps/`,
`publish-app/`, `ota-update/`, `flash-uart/`, `flash-stm32/`, `hil-testing/`,
`debugging/`, `esp-idf-versions/`), [Architecture](https://crosspad.app/docs/concepts/architecture/),
[CLI reference](https://crosspad.app/docs/reference/cli/),
[BSP API](https://crosspad.app/docs/reference/bsp/) (generated from
`components/bsp/crosspad/include/`), [Troubleshooting](https://crosspad.app/docs/troubleshooting/).
`docs/subsystems/*.md` are published there as `subsystems/<name>/`.

**This file is the entry point, not the manual.** It holds the rules, how to
build, the shape of the firmware and the invariants that break a board when
they are ignored. Everything else is one step away: find your area in
[Where to read next](#where-to-read-next) and read that page before you change
anything in it.

## Rules

- **Read before modifying.** Always read a file before editing it. Understand existing patterns.
- **Minimize changes.** Only touch what's requested. No drive-by refactors, no bonus docstrings, no "while we're here" improvements.
- **No dead code.** Don't leave commented-out blocks, placeholder TODOs, or unused functions. If something is removed, remove it completely.
- **No ESP-IDF antipatterns.** Static constructors don't work for registration. Never use `__attribute__((constructor))` or static `AppRegistrar` objects. Use `REGISTER_APP` macro only.
- **Portability first.** Apps must depend on crosspad-core interfaces (`IEventBus`, `IPadLogicHandler`, `IKeyValueStore`, etc.), never on ESP-IDF APIs directly. This ensures they compile on both ESP-IDF and Arduino.
- **LVGL thread safety.** All LVGL mutations from non-LVGL tasks must go through `IGuiPlatform::postToGui()` / `lv_async_call_locked()` (raw `lv_async_call()` is itself not thread-safe). Deletion via `lv_obj_delete_async()`. Lock with `lvgl_port_lock(timeout_ms)` / `lvgl_port_unlock()`. A pad-logic callback or an event-bus handler must never block on the LVGL lock (the LVGL thread may be waiting for it) — post GUI work with `IGuiPlatform::postToGui()` / `lv_async_call_locked()`, which never make the caller wait.
- **Long-term solutions over quick hacks.** Even if it means more initial work (event system > raw callbacks, REGISTER_APP > manual wiring).
- **Build verification.** After code changes, build to confirm. Known harmless warnings: ArduinoJson deprecations (`DynamicJsonDocument`, `createNestedObject`). `python3 tools/check_sdkconfig.py` (Kconfig values a measured budget depends on, each with its reason in `config/sdkconfig_invariants.json`) and `python3 tools/check_conventions.py` (the rules above a compiler does not check) run in CI before anything compiles; run them after regenerating an `sdkconfig.v*`.
- **Docs move with the code.** A change in subsystem behaviour updates its page in docs/subsystems/ in the same PR; a change that breaks a docs page (command, output, path) opens an issue in CrossPad/crosspad-docs.

## Build Environment

Toolchain, board revision, build and flash, step by step with expected output:
[Start here](https://crosspad.app/docs/start-here/),
[Flash over UART](https://crosspad.app/docs/how-to/flash-uart/),
[OTA update](https://crosspad.app/docs/how-to/ota-update/); every `idf.py`
extension and `tools/*.py` flag: [CLI reference](https://crosspad.app/docs/reference/cli/).
What an agent must keep in mind on top:

- **No single `sdkconfig`.** Every build names a revision:
  `idf.py -B build_v2 -DSDKCONFIG=sdkconfig.v2 build` (v1: `build_v1` /
  `sdkconfig.v1`). A bare `idf.py build` is a CMake `FATAL_ERROR` naming both;
  `idf.py board` shows which revision applies (Hardware Revisions below).
- **`idf.py -B build_<rev> fullclean` is required after adding app
  directories.** An incremental build won't discover them (CMake `file(GLOB)`
  runs at configure time only).
- **OTA for iteration:** `python3 tools/ota_flash.py` (CDC `0x303A:0x3456`, no
  bootloader mode) takes `build_<rev>` of the board it flashes, refuses another
  revision's image, and needs `--board v1|v2` for a board whose revision it
  cannot read. With a board on USB prefer `tools/bench.py flash` (Driving Your Board).
- **UART flash port** on v2 is the STM32 bridge VCP `0483:5740` (`-p <PORT>`,
  esptool's auto-reset drives EN/IO0 through it), which also carries the
  console. `tools/requestBootloader.py --method cdc,midi` (ROM download mode
  over SysEx or CDC) is for v1 boards, whose STM firmware does not drive EN/IO0,
  and as a fallback.
- **Assets:** `python3 tools/fs_transfer.py assets` reflashes the whole
  `assets` partition (icons, on-device docs, bundled STM32 image) — app OTA does
  not touch it. Same tool does `push`/`pull`/`ls`/`rm` on `/assets`, `/spiflash`
  and `/sdcard`. See [docs/subsystems/file-transfer.md](docs/subsystems/file-transfer.md).
- **Factory provisioning:** `python3 tools/factory_provision.py` — flash over UART, `smoke`, `APP_VERSIONS`, writes `factory/<serial>.log`, exit 0/1/2 like `crosspad-hil`.

> **Windows + Git Bash (MSYS2):** ESP-IDF rejects MSYS shells. Wrap commands through `cmd.exe` with `set MSYSTEM=&& set PYTHONIOENCODING=utf-8&&` before calling `export.bat`.

## Architecture Map

```
main/
├── main.cpp              ← app_main(): the boot as named stages in order, the command loop
├── boot_power.cpp        ← soft-off gate, qualified wake, STM power states, restart into the bootloader
├── cdc_rx.cpp            ← CDC receive: TinyUSB callback → command queue → line dispatch
├── include/display.h     ← LVGL task, crosspad-gui bootstrap, app registry init
├── include/gui.h         ← InitializeAppRegistry(), AddApps(), LoadMainScreen()
├── gui/
│   ├── gui.cpp           ← Launcher UI, app lifecycle management
│   └── ota_overlay.cpp   ← OTA progress UI
├── app/
│   ├── CMakeLists.txt    ← Auto-discovers main/app/*/CMakeLists.txt, generates app_registry_init.cpp (core cmake/crosspad_app_registry.cmake)
│   ├── app_registry_init.cpp    ← AUTO-GENERATED — never edit manually
│   ├── example/          ← Template app (copy this for new apps; off by default)
│   ├── settings/         ← Settings app (7 categories: display/audio/pads/vibration/connectivity/midi/system)
│   └── update/           ← Firmware app: A/B slot switch, SD-card firmware library, OTA
├── ota_cdc.cpp           ← OTA-over-CDC protocol handler
├── midiCallbacks.cpp     ← MIDI event routing
└── usb_*.c/.cpp          ← TinyUSB descriptors and audio bridge

components/
├── crosspad-core/        ← [SUBMODULE] Portable C++ business logic. ZERO platform code.
│   ├── include/crosspad/
│   │   ├── app/          ← AppRegistry, AppEntry, REGISTER_APP macro
│   │   ├── event/        ← IEventBus, EventTypes, EventData
│   │   ├── pad/          ← PadManager, PadLedController, PadAnimator, IPadLogicHandler
│   │   ├── settings/     ← CrosspadSettings, IKeyValueStore
│   │   ├── stm32/        ← Stm32Manager, the STM32 register map
│   │   ├── protocol/     ← Stm32CommandSender, IStm32Transport, SysEx
│   │   ├── led/, midi/   ← ILedStrip, IMidiOutput
│   │   └── platform/     ← IClock, PlatformServices
│   └── src/              ← Implementation files
├── crosspad-gui/         ← [SUBMODULE] LVGL UI framework (AppBase CRTP, launcher, styles)
├── crosspad-platform-idf/ ← ESP-IDF adapters: EspClock, EspLedStrip, EspMidiOutput, CrosspadPlatform
├── bsp/crosspad/         ← Hardware BSP: display, audio, I2C, IMU, SD, touch, perf monitor
├── stm32_hal/            ← STM32 co-processor bridge: UART HAL, C bridge, MIDI transport
└── (other: midi libs, lsm6dso, ArduinoJson, lcd_touch, nvs, usb_device_uac, etc.)
```

## Init Flow (runtime)

1. `app_main()` → the `boot_*()` stages of `main/main.cpp`, called in order (NVS and settings, USB, the platform singletons, the STM32 link and the wake gate, the card, the display, the engines). `Boot` says what kind of boot it is -- transfer mode, network mode, a charge boot, headless -- and `boot.normal()` is the ordinary one
2. `CrosspadPlatform_Init(settings, status)` → Creates all crosspad-core singletons
3. LVGL FreeRTOS task starts → `setupLVGL()` → `crosspad_gui::bootstrap()` → `InitializeAppRegistry()` → `AddApps()` → `LoadMainScreen()`
4. User opens an app from the home screen -- the pad map (its pad, or More on pad 8; the default), the icon grid or the list, Settings → Display → Home screen (`CrosspadSettings::launcherStyle`, NVS `cfg_display/home_view`) -- → `app->start(container)` → `create_()` factory builds UI → `show_()` callback. With the pad map as home, MODE brings the map up over any app; with the grid or the list, MODE goes home. On the map the pads are buttons: a finger picks an app, plays nothing and is not recorded; a pattern and a host's MIDI keep sounding through the logic under it (`IPadLogicHandler::buttonsOnly()`, core 2.7.0).
5. User closes the app (power click without a `setOnBack` handler, or the app's own close) → `app->destroyAppAsync()` → `destroyApp()` → `destroy_()` callback → the home screen again (rebuilt in a style changed meanwhile)

## Singletons (available after CrosspadPlatform_Init)

```cpp
crosspad::IEventBus&         bus     = crosspad::getEventBus();
crosspad::PadManager&        pads    = crosspad::getPadManager();
crosspad::PadLedController&  leds    = crosspad::getPadLedController();
crosspad::PadAnimator&       anim    = crosspad::getPadAnimator();
crosspad::Stm32Manager&      stm32   = crosspad::getStm32Manager();
crosspad::IClock&            clock   = crosspad::getClock();
crosspad::ILedStrip&         strip   = crosspad::getLedStrip();
crosspad::IMidiOutput&       midi    = crosspad::getMidiOutput();
crosspad::CrosspadSettings*  cfg     = crosspad::CrosspadSettings::getInstance();
```

## Invariants (each one has broken a board or a build)

- **Every build names a revision** (`-B build_v2 -DSDKCONFIG=sdkconfig.v2`), and
  a new app directory needs `fullclean`. [Build and memory](docs/dev/build-and-memory.md)
- **The image is PSRAM.** `.flash.text` and `.rodata` are copied there in 64 kB
  pages, so a page the image grows is 64 kB the UI loses. Run
  `python3 tools/check_image_budget.py --rev v2` after anything big.
  [Build and memory](docs/dev/build-and-memory.md)
- **`sdkconfig.defaults` wins over `sdkconfig.v*` for a Kconfig choice.** The
  values a measured budget depends on are held by `python3 tools/check_sdkconfig.py`.
  [Build and memory](docs/dev/build-and-memory.md)
- **Nothing slow on the LVGL thread, nothing that waits for the LVGL lock in a
  pad-logic callback or an event-bus handler.** Storage work runs on a worker
  and hands its result back. [Writing an app](docs/dev/app-framework.md)
- **The RT task is the only I2S writer and reader**, and I2S0 RX is initialised
  before TX. [Audio](docs/dev/audio.md)
- **The engines share one PSRAM arena, one owner at a time**; the arena owner
  lock is taken before the loader lock. [Kits and engines](docs/dev/instruments.md)
- **Verbs match by prefix, in the order of `k_verbs`.** A new verb goes where its
  prefix cannot shadow another, and its replies are built in static buffers:
  every verb runs on the main task's 5120 B stack. `docs/dev/verbs.tsv` lists
  every verb in dispatch order with how it is matched and whether the
  companion allow-list names it; it is generated
  (`python3 tools/gen_verb_catalog.py --write`) and CI refuses a stale copy.
  [Bench](docs/dev/bench.md)
- **Do not take GPIO 8 into the RTC mux** (it breaks SD card reads), and branch
  on `BSP_HAS_*`, never on the board revision. [Hardware](docs/dev/hardware.md)
- **MIDI+UAC2 is the default USB profile and has no CDC endpoint.** A board that
  "does not answer" is usually in it: `python3 tools/bench.py ready`.
  [Bench](docs/dev/bench.md)
- **Flash writes need an internal-RAM stack.** A task with a PSRAM stack that
  writes flash asserts when the cache is disabled. [Firmware](docs/dev/firmware.md)

## Where to read next

Each page under `docs/dev/` is the short version of an area: what it does, the
numbers that were measured, and the traps. It links to the `docs/subsystems/`
page with the detail.

| You are touching | Read first | Detail |
|---|---|---|
| An app, the launcher, the app lifecycle, events, pad logic | [app-framework](docs/dev/app-framework.md) | [Create a new app](https://crosspad.app/docs/how-to/new-app/) |
| The power button, back, the top bar | [power-button](docs/dev/power-button.md) | [power-button](docs/subsystems/power-button.md) |
| A board on USB, flashing, HIL scenarios, CDC verbs | [bench](docs/dev/bench.md) | [hil-testing](docs/subsystems/hil-testing.md) |
| sdkconfig, memory, image size, LVGL build options, Doxygen | [build-and-memory](docs/dev/build-and-memory.md) | |
| Pins, board revisions, the IMU, the ULP core, CI per revision | [hardware](docs/dev/hardware.md) | [ulp-coprocessor](docs/subsystems/ulp-coprocessor.md) |
| crosspad-core or crosspad-gui versions, the submodule pins | [versioning](docs/dev/versioning.md) | |
| `idf.py app-*`, profiles, kits of apps | [app-manager](docs/dev/app-manager.md) | [app-manager](docs/subsystems/app-manager.md) |
| Codecs, inputs and outputs, USB audio, the sample rate | [audio](docs/dev/audio.md) | [audio-routing](docs/subsystems/audio-routing.md), [audio-engine-rate](docs/subsystems/audio-engine-rate.md) |
| Kits, the sampler engines, the pitched instrument, kit swaps | [instruments](docs/dev/instruments.md) | [sampler-engine](docs/subsystems/sampler-engine.md), [pitched-instrument](docs/subsystems/pitched-instrument.md), [kit-swap](docs/subsystems/kit-swap.md), [sample-tools](docs/subsystems/sample-tools.md) |
| Songs, Arrange, bounce, freeze | [song](docs/dev/song.md) | [audio-song](docs/subsystems/audio-song.md) |
| BLE MIDI, the companion service, WiFi, network mode | [radio](docs/dev/radio.md) | [ble-midi](docs/subsystems/ble-midi.md), [wifi](docs/subsystems/wifi.md) |
| MIDI clock, the TRS jacks | [midi](docs/dev/midi.md) | [midi-clock](docs/subsystems/midi-clock.md), [trs-midi](docs/subsystems/trs-midi.md) |
| DAW control, the MIDI hub | [daw-control](docs/dev/daw-control.md) | |
| The browser twin, the motion stream | [twin](docs/dev/twin.md) | [twin-protocol](docs/subsystems/twin-protocol.md) |
| A/B slots, the firmware library, updates, the Firmware app, the STM32 update | [firmware](docs/dev/firmware.md) | [firmware-library](docs/subsystems/firmware-library.md), [firmware-updates](docs/subsystems/firmware-updates.md), [firmware-app](docs/subsystems/firmware-app.md), [stm32-update](docs/subsystems/stm32-update.md) |
| USB transfer mode, assets, files over CDC | [usb-and-files](docs/dev/usb-and-files.md) | [usb-transfer-mode](docs/subsystems/usb-transfer-mode.md), [file-transfer](docs/subsystems/file-transfer.md) |
| Fonts, file names with diacritics | [fonts](docs/dev/fonts.md) | |
| Where a file is | [key-paths](docs/dev/key-paths.md) | |

## CrossPad Manifesto

Write once, run everywhere; platform repos thin, shared repos thick; hardware is
software you can touch; small team, big surface area; open by default;
documentation ships with the code. Full text: [docs/MANIFESTO.md](docs/MANIFESTO.md).

