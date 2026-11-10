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
`docs/subsystems/*.md` are published there as `subsystems/<name>/`. This file
keeps what a contributor or agent needs on top: architecture, conventions,
gotchas, driving a board.

## Rules

- **Read before modifying.** Always read a file before editing it. Understand existing patterns.
- **Minimize changes.** Only touch what's requested. No drive-by refactors, no bonus docstrings, no "while we're here" improvements.
- **No dead code.** Don't leave commented-out blocks, placeholder TODOs, or unused functions. If something is removed, remove it completely.
- **No ESP-IDF antipatterns.** Static constructors don't work for registration. Never use `__attribute__((constructor))` or static `AppRegistrar` objects. Use `REGISTER_APP` macro only.
- **Portability first.** Apps must depend on crosspad-core interfaces (`IEventBus`, `IPadLogicHandler`, `IKeyValueStore`, etc.), never on ESP-IDF APIs directly. This ensures they compile on both ESP-IDF and Arduino.
- **LVGL thread safety.** All LVGL mutations from non-LVGL tasks must go through `IGuiPlatform::postToGui()` / `lv_async_call_locked()` (raw `lv_async_call()` is itself not thread-safe). Deletion via `lv_obj_delete_async()`. Lock with `lvgl_port_lock(timeout_ms)` / `lvgl_port_unlock()`. A pad-logic callback or an event-bus handler must never block on the LVGL lock (the LVGL thread may be waiting for it) — post GUI work with `IGuiPlatform::postToGui()` / `lv_async_call_locked()`, which never make the caller wait.
- **Long-term solutions over quick hacks.** Even if it means more initial work (event system > raw callbacks, REGISTER_APP > manual wiring).
- **Build verification.** After code changes, build to confirm. Known harmless warnings: ArduinoJson deprecations (`DynamicJsonDocument`, `createNestedObject`).
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
├── main.cpp              ← app_main(), hardware init, FreeRTOS tasks
├── display.h             ← LVGL task, crosspad-gui bootstrap, app registry init
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
│   │   ├── stm32/        ← Stm32Manager, Stm32CommandSender
│   │   └── platform/     ← IClock, ILedStrip, IMidiOutput, IStm32Transport
│   └── src/              ← Implementation files
├── crosspad-gui/         ← [SUBMODULE] LVGL UI framework (AppBase CRTP, launcher, styles)
├── crosspad-platform-idf/ ← ESP-IDF adapters: EspClock, EspLedStrip, EspMidiOutput, CrosspadPlatform
├── bsp/crosspad/         ← Hardware BSP: display, audio, I2C, IMU, SD, touch, perf monitor
├── stm32_hal/            ← STM32 co-processor bridge: UART HAL, C bridge, MIDI transport
└── (other: midi libs, lsm6dso, ArduinoJson, lcd_touch, nvs, usb_device_uac, etc.)
```

## Init Flow (runtime)

1. `app_main()` → BSP hardware init, WiFi/ESP-NOW, USB TinyUSB, NVS
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

## Creating a New App

Step by step, with the expected output: [Create a new app](https://crosspad.app/docs/how-to/new-app/)
(`idf.py app-new --app <id> --name "<Name>"`, then `fullclean` + build, OTA,
`fs_transfer.py assets` for the icon); `--private` (also creates the GitHub
repo and installs it back as a submodule; `--public` likewise) and the other flags:
[CLI reference](https://crosspad.app/docs/reference/cli/); publishing:
[Publish an app](https://crosspad.app/docs/how-to/publish-app/). In the TUI:
`idf.py app-manage` → [4] Developer tools → New app.

`app-new` renders the template from [CrossPad/crosspad-apps](https://github.com/CrossPad/crosspad-apps)
(fetched anonymously, `gh` only as a fallback) into `components/crosspad-<id>/`
(pad handler, LVGL buttons, slider, animation timer, `REGISTER_APP_PL`). The
manual path is the same shape, in `main/app/<id>/` with a `CMakeLists.txt` that
only does `set(MY_APP_SOURCES "app/my_app/my_app.cpp")` — **no `PARENT_SCOPE`**,
the parent collects it. `main/app/example/` is the in-tree reference. An app
kept in its own repo can also be added by hand —
`git submodule add <url> main/app/<id>`, then `fullclean` + build — and runs on
the Arduino platform too if it depends only on crosspad-core interfaces.

The skeleton every LVGL app follows:

```cpp
class MyPadLogic : public crosspad::IPadLogicHandler { /* onPadPress/onPadRelease… */ };
static std::shared_ptr<MyPadLogic> s_padLogic;
static lv_group_t* s_group = nullptr;

lv_obj_t* lv_CreateMyApp(lv_obj_t* parent, App* a) {
    lv_obj_t* c = lv_obj_create(parent);                     // root, 100 % x 100 %
    if (lv_indev_t* enc = crosspad_gui::getGuiPlatform().getNavigationEncoder()) { // null on a platform without an encoder
        s_group = lv_group_create(); lv_indev_set_group(enc, s_group);
    }
    if (!s_padLogic) {                                       // register once; activation is
        s_padLogic = std::make_shared<MyPadLogic>();         // framework-owned (push/pop on
        crosspad::getPadManager().registerPadLogic("MyApp", s_padLogic); // start/destroy)
    }
    return c;                                                // no setOnHide: it is never called
}
void lv_MyApp_destroy(lv_obj_t* app) {
    if (s_group) { lv_group_delete_deferred(s_group); s_group = nullptr; }  // framework restores the encoder group
    // root is null when the app's screen was already wiped: free what is ours, delete nothing.
    if (app) lv_obj_delete_async(app);                       // never lv_obj_delete() here
}
REGISTER_APP_PL(MyApp, nullptr, "play.png", lv_CreateMyApp, lv_MyApp_destroy,
                nullptr, nullptr, nullptr, 0, "MyApp")
```

Rules baked into that skeleton: do **not** call `setActivePadLogic()` when the
app declares a `padLogicName` (AppBase pushes on `start()`, pops on
`destroyApp()`); the framework restores the encoder group after destroy (`onHide` is never called), so delete your own group with `lv_group_delete_deferred()` and nothing more; ship
the icon as `assets/<name>.png` in the app (staged by `main/collect_assets.cmake`
— an icon that exists only in another repo is a blank tile); and
**`idf.py fullclean` is mandatory** for a new app directory, `idf.py build`
alone will not discover it.

## REGISTER_APP Macro

```cpp
REGISTER_APP(appName, logicFunc, iconPath, createLVGL, destroyLVGL, initSimple, refreshFunc, cleanupFunc, priority)
```

| Param | Type | Description |
|-------|------|-------------|
| `appName` | identifier | Stringified as display name, suffixed as `_register_<name>_app()` |
| `logicFunc` | `void(*)(App*)` | Called on start. Usually `nullptr` for LVGL apps. |
| `iconPath` | `const char*` | Icon filename (resolved via LittleFS `A:` prefix) |
| `createLVGL` | `lv_obj_t*(*)(lv_obj_t*, App*)` | Factory — builds UI, returns root widget |
| `destroyLVGL` | `void(*)(lv_obj_t*)` | Teardown — free your resources, then `if (app) lv_obj_delete_async(app)` |
| `initSimple` | `void(*)()` | Non-LVGL init. `nullptr` for LVGL apps. |
| `refreshFunc` | `void(*)()` | Periodic GUI refresh. Usually `nullptr`. |
| `cleanupFunc` | `void(*)()` | App cleanup. Usually `nullptr`. |
| `priority` | `int` | Higher = more preferred for auto-selection. 0 = normal. |

**Declarative pad logic:**
```cpp
REGISTER_APP_PL(appName, logicFunc, iconPath, createLVGL, destroyLVGL, initSimple, refreshFunc, cleanupFunc, priority, padLogicName)
```
`padLogicName` names an `IPadLogicHandler` the app registers via `registerPadLogic()` in `createFn`. Activation is framework-owned: `AppBase::start()` calls `pushPadLogic(padLogicName)` and `destroyApp()` calls `popPadLogic()` before teardown — apps declaring `padLogicName` must NOT call `setActivePadLogic()` themselves. Pass `nullptr` for `padLogicName` (or use plain `REGISTER_APP`) for apps that manage pad logic manually.

## Event System

Subscribe/post via `crosspad::getEventBus()`:

```cpp
auto& bus = crosspad::getEventBus();

// Subscribe
auto handle = bus.subscribe(crosspad::EventType::PadPressed,
    [](const void* data, crosspad::EventType type, void* ctx) {
        auto* d = static_cast<const crosspad::PadPressedData*>(data);
        ESP_LOGI("APP", "Pad %d pressed vel=%d", d->padIdx, d->velocity);
    }, this);

// Post (async, thread-safe)
bus.postNoteOn(channel, note, velocity, crosspad::EventSource::App);
bus.postPadPressed(padIdx, true, velocity, crosspad::EventSource::App);

// Unsubscribe
bus.unsubscribe(handle);
```

**Event types:** `NoteOn`, `NoteOff`, `PadPressed`, `PadReleased`, `AppChanged`, `TempoChanged`, `LiveModeChanged`, `GuiUpdateRequired`, `SamplePlayback`, `MotionGesture` (tap, double tap, free fall, wake-up, orientation, still — posted from the IMU, `MotionGestureData`)

**Event sources:** `PadManager`, `MidiInput`, `Cli`, `App`, `AppOther`, `System`, `Gui`, `SequencerCallback`, `SequencerInput`, `SamplePlayback`, `Imu`

## Pad Logic Handlers

Custom per-app pad behavior:

```cpp
class MyPadLogic : public crosspad::IPadLogicHandler {
    void onActivate(crosspad::PadManager& pm) override { /* set pad colors */ }
    void onDeactivate(crosspad::PadManager& pm) override { /* clear */ }
    void onPadPress(crosspad::PadManager& pm, uint8_t padIdx, uint8_t velocity) override { /* handle */ }
    void onPadRelease(crosspad::PadManager& pm, uint8_t padIdx) override { /* handle */ }
    void onPadPressure(crosspad::PadManager& pm, uint8_t padIdx, uint8_t pressure) override { /* aftertouch */ }
};

// Register once in createFn. With REGISTER_APP_PL/REGISTER_LVGL_APP_PAD, that's
// it — AppBase activates/restores it via pushPadLogic()/popPadLogic() on
// start()/destroyApp(). Only call setActivePadLogic() manually for plain
// REGISTER_APP apps that don't declare a padLogicName.
crosspad::getPadManager().registerPadLogic("MyApp", &myHandler);
crosspad::getPadManager().setActivePadLogic("MyApp");   // onShow (manual pattern only)
crosspad::getPadManager().setActivePadLogic("");          // onHide (manual pattern only)
```

## App Lifecycle (LVGL variant)

The order the code actually runs (`AppBase`, `AppOrchestrator`):

```
register   _register_<App>_app() from AppRegistry_InitAll()   ← REGISTER_APP / REGISTER_APP_PL
populate   one App per entry (defaultAppFactory), setPadLogicName(entry.padLogic)
start      user selects the app (MODE → More on the board; the PC also has a launcher grid)
  ├── encoder group snapshot (priorEncoderGroup_)
  ├── createFn(parent, app)   → builds the LVGL widget tree, returns root
  ├── show_ callback          ← setOnShow set inside createFn runs right here
  └── pushPadLogic(padLogicName) → handler onActivate()   (REGISTER_APP_PL only)
back       power button short click → setOnBack fn (true = consumed, false = framework closes the app)
close      app->destroyAppAsync()  → deferred through lv_async_call → destroyApp()
  ├── popPadLogic() → handler onDeactivate(), previous/base logic comes back
  ├── destroyFn(root)         ← delete timers/subscriptions/group, end with `if (root) lv_obj_delete_async(root)`
  └── encoder group restored (restoreEncoderGroup)
```

**Key points for createFn / destroyFn:**
- Self-close with `destroyAppAsync()`, never `destroyApp()` from an event callback (it deletes the tree the event belongs to).
- Create the app's own encoder `lv_group` in createFn (take the indev from `getGuiPlatform().getNavigationEncoder()`, null on a platform without an encoder); delete it in destroyFn with `lv_group_delete_deferred()`. The framework restores the previous encoder group after destroyFn, so the app does not snapshot or restore it.
- (SDK 2.0; harmless on 1.x) destroyFn may be called with `root == nullptr`, synchronously, from `LV_EVENT_DELETE`, when the app's tree was already deleted by someone else (e.g. `lv_obj_clean` when the screen is rebuilt). It must still free its own resources (timers, subscriptions, groups, transport claims, back tokens, dialogs) and delete no object of the app's tree; on 1.x it is never called with null.
- `setOnShow` is called once, right after createFn; there is nothing to resume.

**Not driven on the firmware:** `IApp` hooks (`onAppInit` / `onAppDeinit` / `onAppUpdate` / `onAppActivated`), `pause()`/`setOnHide` and `AppChanged` are not driven on the firmware. Nothing calls `pause()`, so `hide_` never fires, not even before destroy. The `logicFunc`, `initSimple`, `refresh` and `cleanup` arguments of `REGISTER_APP` are discarded by the macro.

**`started_` is honest even when someone else deletes the root.** A root can be
freed outside `destroyApp()` — e.g. `lv_obj_clean()` during a launcher rebuild
tearing down an overlay that never became `runningApp_`, such as a gate started
from the pre-launch hook (left alone, `started_` stays `true` with a dangling
root and every later `start()` is refused with "App already started" until
reboot). `AppBase` hooks `LV_EVENT_DELETE` on the
root: a deletion that didn't come from `destroyApp()` rolls the app back to
not-started, balances `popPadLogic()` and calls the app's destroy function
with `nullptr` (from inside the deletion, children still alive): timers,
subscriptions and groups go, nothing is deleted -- so a destroy function
deletes its root only when it gets one, and never `lv_obj_delete_async()`s
a widget inside its own tree. `destroyApp()` removes the hook
before its own async delete, so a restart that races the pending delete
doesn't roll back the fresh instance. Consequence for app authors: an overlay
started from a pre-launch hook never stays "started" against a tree LVGL
already tore down.

## Power Button

Timing lives in `main/power_button.cpp`; resolution in crosspad-gui
`components/power_gesture.cpp` (`powerBack()`, `powerToggleQuickSettings()`),
portable to the PC simulator.

| Gesture | Threshold | Effect |
|---------|-----------|--------|
| Click | release before 0.5 s | Contextual back, one step: dropdown → modal/overlay → encoder edit → drawer → top-bar focus → running app (its own levels first; Arrange: window → region → MOVE → keys) → the app it was opened from (`AppOrchestrator::historyBack()`) → home → nothing |
| Hold | 0.5 s | Open drawer: close it. Transport bar on screen: jump the encoder to its keys and back. Status icons on screen: toggle the quick-settings drawer |
| Hold | 2.4 s | Power off (STM, `REG_CTL_LONG_PRESS`, pushed from `Stm32Settings::longPressMs`): ship mode on battery, soft-off on a charger |
| Hold | 4.2 s | Ship mode at once (battery only) |
| Hold at boot | 1.5 s | Qualified wake |

**The top bar** (crosspad-gui `transport_bar.cpp`) is one row of two pages,
the transport bar and the status bar: a finger drags it sideways (LVGL scroll,
snapped a page at a time, and it vetoes the screen-wide back/forward swipe
over it); the encoder reaches the battery as the last key and its click swaps
to the icons, where the status bar's arrow takes the keys' place and swaps
back. A downward swipe from the top edge opens the quick-settings drawer
(Settings -> Display) under either page.

**App authors:** an app with internal levels calls `a->setOnBack(fn)` and pops one
level, returning `true`; a dynamically created overlay pushes its own handler with
`ui_back_push()` / `ui_back_remove()`. **Never restore a saved encoder group
unguarded** — use `lv_indev_restore_group()` (a freed group left on the indev
panics). HIL: `PWR_GESTURE CLICK|HOLD`.
Details and the STM 1.4 changes: [docs/subsystems/power-button.md](docs/subsystems/power-button.md).

## Submodule Workflow

Two git submodules:

| Submodule | Location in repo | Purpose |
|-----------|-----------------|---------|
| **crosspad-core** | `components/crosspad-core/` | Portable C++ business logic (shared with Arduino + PC) |
| **crosspad-gui** | `components/crosspad-gui/` | LVGL UI framework (shared with Arduino) |

**Updating submodules after upstream changes:**
```bash
cd components/crosspad-core
git fetch origin && git checkout origin/main
cd ../..
git add components/crosspad-core
```

**Arduino counterpart repo** (`ESP32-S3`) shares crosspad-core and uses Arduino adapters in `lib/crosspad_lib/src/platform/`.

### Submodule version gate

Both submodules carry a semver triple (`crosspad/CrosspadCoreVersion.hpp`,
`crosspad-gui/CrosspadGuiVersion.hpp`). Exact pairing -- a consumer declares the
triple it was written against and includes `crosspad/RequireVersion.hpp`
(major mismatch `#error`, minor/patch drift `#warning`; update the triple only
after building *and running* against the new version, never to silence the
warning) -- is how crosspad-gui pairs with core. **This firmware does not pair
exactly: it declares floors** (next bullet), so never re-add exact pins here.

- This firmware declares **floors** in `main/include/crosspad_deps.h`: another
  MAJOR or anything below is an `#error`, anything above builds quietly (the
  bump bot moves core/gui to what the apps' `requires` need and cannot edit
  code). The ranges themselves are checked by `tools/deps_resolve.py check` in
  every official build cell. crosspad-gui declares the core it needs in
  `CrosspadGuiVersion.hpp` (`CROSSPAD_GUI_REQUIRES_CORE_*`, exact pairing) and
  in `library.json` `dependencies` (the range the resolver reads).
- Bump rules: MAJOR when an interface a consumer implements or calls changes
  shape, MINOR when something is added, PATCH for fixes.
- Runtime: `crosspad_deps_check_runtime()` (after `bsp_stm32_init()`) logs the
  *linked* core/gui versions against the headers — stale objects the
  preprocessor cannot see — and the STM32 protocol against
  `STM32_PROTO_VERSION`. `APP_VERSIONS` over CDC ends with a
  `LIBVER: core=… gui=… stm_proto=have/want` line, `!` after anything that
  does not match; Settings → Info shows the same.
- The STM32 register map has one copy, `crosspad/stm32/reg_map.h` in core;
  `stm32_hal/include/stm32_reg_map.h` only includes it.

## Code Patterns

```cpp
// Logging
static const char* TAG = "MyModule";
ESP_LOGI(TAG, "Info: %d", value);
ESP_LOGW(TAG, "Warning");
ESP_LOGE(TAG, "Error");

// LVGL from non-LVGL task (thread-safe, never makes the caller wait)
crosspad_gui::getGuiPlatform().postToGui([](void* data) {
    lv_label_set_text(label, "Updated");
}, nullptr);

// LVGL mutex (when calling LVGL API from non-LVGL task)
if (lvgl_port_lock(3000)) {
    lv_label_set_text(label, "Updated");
    lvgl_port_unlock();
}

// Safe object deletion
lv_obj_delete_async(obj);  // NEVER lv_obj_delete() from callbacks

// Hardware access — always through BSP, never direct registers
bsp_display_brightness_set(80);
bsp_i2s_es8388_start(BSP_I2S_INTERFACE_0);

// Periodic housekeeping — a bsp_worker job, not a task of its own.
// Short, non-blocking body; return 0 to keep the period or a new period_ms
// (backoff). No flash writes (PSRAM stack); long work spawns a one-shot task.
static uint32_t my_job(void*) { /* … */ return 0; }
bsp_worker_job_t* job = bsp_worker_add("my_job", 50, my_job, nullptr);
bsp_worker_remove(job);   // returns once the job is not executing
```

`WORKER` over CDC lists every job with `runs`, `last_us`, `max_us`, `overruns`
— a `max_us` near the period means the body belongs in a task after all.
Design: `docs/design/2026-09-10-bsp-worker-design.md`.

## ULP RISC-V Coprocessor

The S3's RTC-domain RV32IMC core (not a C6/P4 "LP core") runs
`components/bsp/crosspad/ulp/ulp_background.c` every 100 ms beside the main CPU,
started by `bsp_ulp_start()` in `app_main()`: a heartbeat. It reaches only RTC
GPIO/ADC/touch/RTC I2C (GPIO 0–3), never the board buses. Builds and flashes with
the app image; check with `ULP` over CDC. `ulp` must be in REQUIRES, and
`BSP_HAS_*` needs a runtime `if`, not `#if`. **Do not take GPIO 8 into the RTC
mux**: as the "back cover" pin it breaks SD card reads (8 of 8 boots). Capabilities, workflow, traps: [docs/subsystems/ulp-coprocessor.md](docs/subsystems/ulp-coprocessor.md).

## Hardware Quick Reference

| Component | Chip/Interface | Notes |
|-----------|---------------|-------|
| SoC | ESP32-S3, dual Xtensa, 240MHz | 8MB PSRAM (OCT), 16MB Flash |
| Display | ILI9341, 320x240, SPI | ST7789 controller variant |
| Touch | FT6x36, I2C | Capacitive |
| Audio | 2x ES8388, I2S0 (master) + I2S1 (slave) | 44.1 kHz stereo (engine rate from Settings → Audio; kits are 44.1 k and the engine does not resample), anti-pop sequences |
| IMU | ST LSM6DS3TR-C (WHO_AM_I 0x6A), I2C, own driver (`bsp_imu.cpp`, bits from ST's `lsm6ds3tr-c_reg.h`) | gyro 52 Hz, accel 416 Hz with LPF2 ODR/50, BDU; single tap/free fall/wake-up/6D recognised in the part (`bsp_imu_set_event_callback`, `IMU_THS`, raw `IMU_REG`/`IMU_REGW`), **double taps paired in the driver** (two knocks within 500 ms, 150 ms to settle — the part's own double-tap machine answered a hand on this case unpredictably, whatever DUR/QUIET were); **the wake-up threshold must stay above the tap threshold (125 mg default) and the sources must not be latched, or no tap arrives at all**; a pad hit shakes the case like a knock, so `main.cpp` drops tap gestures for 400 ms after every `PadPressed`; gestures reach apps as `EventType::MotionGesture` on the event bus, not through any IMU call; gyro sleep only in LIGHT sleep, where motion wakes the board (`REG_CMD_USER_ACTIVITY`); the `imu` task wakes on the gyro data-ready pulse on INT2 (v2 GPIO 48, attached after `bsp_display_start()`), reads 0x1B..0x2D in one burst, Kalman on the ESP; polls where `BSP_HAS_IMU_INT2` is 0 |
| LEDs | WS2812B, 16 addressable | One per pad, via RMT |
| Pads | 16 capacitive, pressure-sensitive | Via STM32 co-processor (UART SYSEX) |
| Encoder | Rotary + momentary switch | GPIO, LVGL input device |
| SD Card | SDMMC | 4-bit bus |
| Haptics | DRV2605 | Vibration motor driver, probed and started by `CrosspadPlatform_StartVibration()` |
| Wireless | ESP-NOW, WiFi, BLE | The S3 has no Bluetooth Classic — no A2DP/SPP, BLE only |
| USB | MIDI + CDC + UAC2 (TinyUSB) | CDC profile VID 0x303A, PID 0x3456; MIDI+UAC2 profile PID 0x3457 (no CDC) |
| MIDI | USB, BLE, UART (via the STM32) | |
| Storage | SD card (FAT), LittleFS `assets`, FAT `storage` | Partitions: `partitions_16mb.csv` |
| Co-processor | STM32 | UART (+ I2C at 0x42 on v2), pad scanning + charger management |
| Battery | Li-Ion, 4000 mAh | From the old README's spec table; not readable from code |

BSP API reference: https://crosspad.app/docs/reference/bsp/ (generated from `components/bsp/crosspad/include/`)
Audio codec details: `components/bsp/crosspad/AUDIO_ES8388.md`
Performance monitoring: `components/bsp/crosspad/PERFMON.md`
Hardware revisions: `components/bsp/crosspad/README.md#hardware-revisions`
Schematic: in the Arduino repo (`CrossPad/ESP32-S3`) under `docs/sch_cp.pdf`

## Hardware Revisions

CrossPad has multiple PCB revisions. The active one is selected at **build time**
via Kconfig (`CONFIG_BSP_BOARD_REV_V1` / `CONFIG_BSP_BOARD_REV_V2`). One firmware
binary per revision — no runtime probe.

**Which revision to build/flash is decided by `tools/crosspad_board.py`, not by
memory.** Order: `--board v1|v2` explicit → the connected board (hardware
revision from the STM bridge `bcdDevice` = `0x3000 | BCD(PCB)`, else firmware
revision from the ESP `bcdDevice` = `0x0500 | rev<<4 | profile`) → `"board"` in
`crosspad.local.json` → none (non-interactive callers exit 2). `idf.py board`
shows the decision and its source, `idf.py board --set v2` remembers one. CP
Tools, bench.py, ota_flash.py, factory_provision.py and crosspad-mcp all go
through this resolver, and `ota_flash.py` refuses an image built for another
revision than a connected board. Full design:
[docs/design/2026-09-14-board-selection-design.md](docs/design/2026-09-14-board-selection-design.md).

- **Pinout headers:** `components/bsp/crosspad/include/pinout/pinout_v*.h`. Each
  revision has its own header with all GPIOs and capability flags
  (`BSP_HAS_STM32_I2C`, `BSP_HAS_EEPROM`, `BSP_HAS_JACK_ADC`, `BSP_HAS_BACK_COVER_DETECT`, …).
- **Selector:** `components/bsp/crosspad/include/crosspad_s3.h` — single
  `#if defined(CONFIG_BSP_BOARD_REV_V*)` chain picks the right pinout header.
  **Never add `#ifdef CONFIG_BSP_BOARD_REV_*` anywhere else in the codebase.**
  Branch on `BSP_HAS_*` capability macros instead so consumers stay
  rev-agnostic.
- **Pins missing on a rev** are defined as `GPIO_NUM_NC`, not `#undef`-ed —
  capability flags derive automatically (`BSP_HAS_JACK_ADC = (ADC_PIN != GPIO_NUM_NC)`).
- **Per-revision build dirs** `build_v1` / `build_v2` (each with its
  `sdkconfig.v*`, see Build Environment) avoid a full reconfigure when
  switching; `build_*/` is gitignored so they cohabit. PCB r16/r19 = v1,
  PCB r20 = v2.
- **Adding a new rev:** copy `pinout_v2.h` → `pinout_v3.h`, add Kconfig choice
  entry + matching `BSP_BOARD_REV_STR` default, add `#elif` branch in
  `crosspad_s3.h`. Full procedure: `components/bsp/crosspad/README.md#adding-a-new-revision`.
- **`sdkconfig.v1`/`sdkconfig.v2` are regenerated with the local IDF (5.5.4),
  not CI's** — CI builds the matrix inside `ghcr.io/crosspad/crosspad-ci:v5.5.5-1`
  (ESP-IDF v5.5.5, `tools/ci/`), a patch-level difference. `v1` also builds without a flash core dump
  (`ESP_COREDUMP_ENABLE_TO_FLASH` off); `reset_log.cpp` guards the
  `esp_core_dump_get_summary()` path behind `RSTLOG_COREDUMP` so it still
  builds — `COREDUMP_INFO` just answers "none" on v1. CI builds **v2 only**
  (`-B build_$rev -DSDKCONFIG=sdkconfig.$rev`; v1 cells are off by default --
  a manual run with input `revs: v1,v2` still builds it, `tools/ci_matrix.py --revs`) on
  `main`. **Optional self-hosted runner:** `tools/ci_runner.py install`
  + `service` registers a machine as a self-hosted runner; `on`/`off` flips
  the repo variable `CI_RUNNER` the workflow reads (`off` before that machine
  goes down -- GitHub never falls back to a hosted runner). **CI image:** every
  job runs in `ghcr.io/crosspad/crosspad-ci:<IDF>-<rev>` (`tools/ci/Dockerfile`,
  published from main by `ci-image.yml`), the image the other CrossPad repos'
  CI uses too; a change to `tools/ci/` bumps `CI_IMAGE_REV`, then each consumer
  moves its pin (tag@digest, from the publish job's summary) in a commit of
  its own. The `release` and Pages jobs stay on GitHub's runner (secrets) --
  unless the repo variable `CI_SECRETS_RUNNER=self-hosted` is set, only while
  GitHub's hosted runners cannot start jobs.

## App Package Manager

Apps are git submodules under `components/crosspad-*/`, managed by `idf.py app-*`
(registry: [CrossPad/crosspad-apps](https://github.com/CrossPad/crosspad-apps),
fetched anonymously over HTTPS with `gh` only as a fallback, cached 1 h).
Using it: [Install and remove apps](https://crosspad.app/docs/how-to/install-apps/);
every command and flag: [CLI reference](https://crosspad.app/docs/reference/cli/);
the one-line installer: [Start here, step 1](https://crosspad.app/docs/start-here/1-toolchain/).
`idf.py app-manage` opens the TUI: **What's next** on the first line,
`[1] Update my CrossPad` (one log in `.crosspad/last-update.log`), `[2] Add or
remove apps`, `[3] Something's wrong`, `[4] Developer tools` (Workspace,
Configure, Profiles, Board, Build & Flash, New app). An app *follows the
latest release* (`track=registry`, newest `v*` tag), *follows development*
(`track=branch`) or *stays on a version* (`track=pinned`); the UI never names
the modes. Spec: `docs/design/2026-09-15-app-manager-tui-redesign-design.md`.
`tools/app_manager.py doctor | support | status --json`; `?` on every screen; a
checkout that equals a published release flashes its image instead of building.
After `app-install`/`app-remove`: `fullclean` + build of the revision's build dir.
VS Code: `.vscode/settings.template.json` puts **CP Tools** and **OTA** buttons
in the status bar (needs the "VsCode Task Buttons" extension; tasks in
`.vscode/tasks.json`).

`crosspad.config.json` = intent (track policy + feature flags), `apps.json` = state,
`crosspad.local.json` = personal overrides (gitignored). Nothing destructive runs
over local work; feature flags never touch submodule headers (deviations go to
`.crosspad/build_flags.cmake`). `APP_VERSIONS` over CDC says what the device
actually runs. Details, policies, flag catalog: [docs/subsystems/app-manager.md](docs/subsystems/app-manager.md).

## Audio Routing (USB Audio capture path)

**MIDI+UAC2 is the default USB profile** (`usbAudioEnabled`, fallback `true` in
`CrosspadSettings::load()` — the constructor's value never reaches a boot). It
carries no CDC endpoint, so a board that has never been switched has no console,
no OTA and no `fs_transfer`: control goes over SysEx on USB MIDI
(`crosspad-hil usb-mode set default`, or `bench.py ready`). Every way of
switching now persists the choice — the Settings dropdown, the CDC command and
the SysEx one all write `usbAudioEnabled` — so a development board stays on CDC across
reboots and `factory_provision.py` flips to CDC for its checks and back to audio
before it logs a serial.

`main/audio_route_control.{h,cpp}` is the one implementation behind the GUI
(Settings → USB), SysEx `F0 7D 1D …` and CDC `AUDIO_*`. Presets name real signal
paths: `MICS` = codec0 LINE2, `LINE_IN` = codec1 LINE2, `SPEAKER_BUS` /
`HEADPHONE_BUS` = LINE1 on codec 0/1 (DAC→ADC loop), `MIX` = both codecs'
LINE2 summed in the USB bridge (`mic_src 2`; a 4-channel UAC2 stream does not
fit the S3's USB FIFO beside MIDI — TinyUSB refuses it). **Every boot records
the Mixer's USB bus** (`SAMPLER`): ROUTE decides what a host gets; the ADC
presets are for testing and last until the next boot.
**The RT task is the only I2S writer and reader, also in the USB-audio
profile**: host playback enters the mixer as the "USB" channel and capture is a
tap in the render (`UsbAudioPort`), the UAC tasks only move bytes between
TinyUSB's FIFOs and two rings. The feedback endpoint is CPU cycles between host
SOFs, and the render takes the host's frames through a resampler steered by
the ring's level (`UAC_STATS asrc_ppm`).
**Both are shaped per host computer, found by itself**
(`main/usb_host_profile.cpp`: Auto reads the first string request after a bus
reset through a `--wrap` of TinyUSB's `dcd_event_handler`; Linux asks for 255
bytes, macOS for 2. Settings -> USB host, `USB_HOST AUTO|MAC|LINUX`,
`USB_ENUM`, NVS `cfg_usb/mode`; Linux until placed):
Linux a feedback over 128 SOFs and ±1000 ppm, macOS a feedback per interval
(a steady one made macOS fill its own stream with silence after minutes,
`UAC_FLOW zh`) and ±5000 ppm. **The USB
interrupt runs on core 1** (`CONFIG_TINYUSB_INIT_IN_DEFAULT_TASK=y`): on core
0 a PSRAM heap walk (`heap_caps_get_largest_free_block`, the status bar's
`BLK`, 1.3-1.6 ms with interrupts off) made the DWC2 arm the wrong frame
parity and drop ~2 speaker packets a second (`UAC_PKT skip`, `UAC_LATE`). A DAW
monitoring its input while the preset is a DAC→ADC bus closes an analog loop
that rails on codec 1 — that is a host-side loop, not a firmware fault.
Adding a preset = enum entry + `k_presets` row + `kRoutePresetOptions` entry, in
step. **An input the mixer sends anywhere (the Mixer's MONITOR: `Out 1/2` =
phones and speakers, `Out 3/4` = line out) holds its codec's ADC on its input over the preset**
(`audio_route_claim_adc`, the Recorder claims the same way), and an IN channel
on the DAC loop is silent, so no send can feed back. **Three inputs, two ADCs:**
Mic = codec 0 LINE2, Line 1/2 = codec 1 LINE2, Line 3/4 = LINE1 of both codecs,
the jack codec 1's LOUT1/ROUT1 also drive (the DAC loop); the Mixer's IN pair
(`IInputPairs`, `InputPairPort.cpp`, `IN_PAIR`) picks two, and a pair with Line
3/4 powers that loop pair down **and puts it in hi-Z** (VROI) -- a driver left
on is a ~3 kHz low-pass for the source and a feedback path -- and refuses the
bus presets. **Each physical output has its own
hardware level** (`IOutputLevels`, `OutputLevelsPort.cpp`): codec 0 LOUT1/ROUT1
= the speakers, LOUT2/ROUT2 = the Out 1/2 jack, codec 1 LOUT2/ROUT2 = Out 3/4; a bus with
such outputs plays at unity in the mixer and OUT shows a row per output. The
DAC's digital floor at 0 % waits until both pairs of a codec are at 0 -- the
loop pair (codec 1 LOUT1/ROUT1) does not count, so Out 3/4 at 0 % or muted is
off.
API and rationale: [docs/subsystems/audio-routing.md](docs/subsystems/audio-routing.md).

## Audio Engine Rate

Kits are 44.1 kHz WAVs, so the engine runs at 44.1 kHz by default. The native
sample engine steps a voice by the file's rate over the engine's (core
`SamplerEngine`, the audition voice and `STARTUP.wav` too) and the pitched
instrument converts at load, so a file at another rate keeps its pitch; only
the optional prebuilt streamer (`CONFIG_CROSSPAD_SAMPLER_MARCEL`) plays files
1:1 and needs the clock at the content rate. `AudioControl_Setup()` pushes
`audioEngine.sampleRate` (44100 default, 48000 allowed) into
`bsp_i2s_set_sample_rate()` before the codec init, and the USB audio bridge and
UAC report the same rate. Boards that saved settings before core 1.0.3 hold
`sr_idx=1` (48 kHz) in NVS: `AUDIO_RATE 44100` over CDC, then reboot.
**I2S0 RX is initialised before TX** — as the full-duplex slave the RX lost
~165 frames/s at 44.1 kHz (clean at 48 kHz); as the master it is clean at both.
**MCLK is bound back to the RX divider after the TX init**: setting the TX
clock binds it to the TX divider, and on about half the boots the codecs'
MCLK and LRCK then drift against each other (a 200 Hz timing smear, THD+N
−28 dB at 3 kHz on every output and on USB).
Measure with `tools/make_tone_kit.py` + `crosspad-hil run sampler_record`;
judge offline. Details and numbers: [docs/subsystems/audio-engine-rate.md](docs/subsystems/audio-engine-rate.md).

## BLE MIDI

Browser companion control shares NimBLE: custom GATT service `7e400001-b5a3-f393-e0a9-e50e24dcca9e`, bounded request/reply mailbox, main-task HIL dispatch. The same verb set rides USB MIDI SysEx `0x1F` (`main/companion_sysex.cpp`, `crosspad-hil companion "<VERB>"`) — that is the control plane of the MIDI+UAC2 profile, which has no CDC. A host's request for the CDC profile (SysEx `0x1B 01`) is confirmed **on the device** unless `USB_GUARD 0` (persisted; a development board sets it once). Protocol and limits: `docs/subsystems/ble-midi.md`. No files or OTA over BLE or SysEx.

NimBLE, both roles, routed through the same `MidiInputHandler` as every other
transport (`components/crosspad-platform-idf/EspBleMidi.{hpp,cpp}`, wiring in
`main/midiCallbacks.cpp`). Lazy-started after boot per `wireless.enableBLE`.
**Cost: ~30–60 kB of internal RAM while running** — with BLE up the launcher sits
at ~8 kB free, which is why `v1.0.0-rc1` ships it off and why anything that needs
an internal stack (audio tasks, `fx_worker`) must be checked against `MEM` /
`MEM_BLOCKS`. Off-device: `BLE_ENABLE 0|1` (persisted — `crosspad-hil ble enable off`
puts a development board in the rc1 shape), `BLE_START|STOP|STATUS|SCAN|CONNECT|SEND|TXOFF`.
Design, memory measurements, status-bar fixes: [docs/subsystems/ble-midi.md](docs/subsystems/ble-midi.md).

## WiFi (on demand; network mode as the fallback)

Nothing turns WiFi on: a caller holds a lease (`wifi_link_acquire/release`,
`main/wifi_link.cpp`). **A board paired to a crosspad.app account with Remote
management on (the default) holds one all the time** (`"remote"`,
`main/cp_remote.cpp`, while WiFi is allowed), so its WiFi stays up and BLE off;
turning BLE on in Settings turns Remote management off. The first lease starts the radio and joins the saved
networks in order; the last one arms a 30 s linger, then the radio stops and
its RAM comes back. One control task (`wifi_ctl`, internal stack) makes every
`esp_wifi_*` call; BLE gives way while WiFi runs and comes back after.
**Four checks on three heaps run before anything is built** (internal
`DEFAULT` 10240 B, all internal 48128 B, PSRAM 116736 B, a free PSRAM block of
20480 B; `radio_fits()`), set from a connected link's measured peak. They hold
in a normal boot because LVGL's blend code left IRAM (Build Gotchas). **A
Sampler with a kit is refused by design** (`failed(no_mem)`; measured with the
earlier fixed engine arena, not re-measured with the dynamic one): the user-facing request then offers **network mode**
(`main/net_mode.cpp`, `wifi_fallback_offer()`), a boot with no engine, kits or
BLE and its own screen; background callers and verbs answer `ERR no_mem`.
Verbs: `WIFI_STATUS|ENABLE|SET|LIST|FORGET|UP|DOWN|SCAN|PING`, `NET_MODE
1|0` (CDC-only); `WIFI_SET` saves first and answers `ERR short_pass` under 8
characters. Saved networks live in NVS `wifi/nets`, shared by both slots.
Testing: `WIFI_ENABLE 1` first; the test network comes from `CROSSPAD_WIFI_SSID`
/ `CROSSPAD_WIFI_PASS` in the environment (keep credentials out of every repo),
then `python3 tools/bench.py test wifi`. The TLS client costs a 64 kB PSRAM page: check `ext_ram.dummy`
(2949088) after touching the network code.
Details, numbers, limitations, traps: [docs/subsystems/wifi.md](docs/subsystems/wifi.md).

## Pitched Instrument (one sample, sixteen notes)

A kit with `"engine": "pitched"` holds 1–24 recorded zones whole in PSRAM (`kInstrumentMaxZones`); every
pad plays the nearest zone transposed to its `instrument.pad_notes` note —
varispeed Hermite, 32.32 phase, `max_voices` clamped to 28 (`kMaxSounding`)
and to the board's cap of 4 (`PitchedSampleNode::kVoiceCap`). **Core owns the engine**
(`crosspad/instrument/*`, `crosspad/dsp/*`): voices, render gate, `installBank()`,
`createFft()`, `pitchedBuildBank()`. FFT runs at load time only (root detection,
rate conversion) — one 1024-point FFT is 0.56 ms of a 1.45 ms block where 16
voices are 0.72 ms. Platform is glue: `PitchedKitPort.cpp` fills `PitchedLoadEnv`,
`PitchedSampleNode.hpp` adds rt-max/peak, dispatch in `components/crosspad-platform-idf/SamplerPlatformPort.cpp`. **The
engines take turns owning one arena** (`engine_arena.hpp`; no reserved
block: the general PSRAM heap with every block on one list, never
below a 512 kB floor kept for the UI), handed
over by `kit_engines_stop()` (`engine_handoff.h`: the native sampler's heads,
the prebuilt streamer when built, the song rings) — a bank can take most of it, not 256 kB.
**One owner at a time:** a native take/pad rebuild, a pitched build/unload and
the handoff all hold `EngineArenaOwner` (`engine_arena_owner_lock`, taken
*before* the native `LoaderLock`; `engine_arena_reset()` takes it too), and
`kit_engines_stop()` unloads the installed pitched bank *before* it resets.
Every reset bumps `engine_arena_generation()`; the bank is stamped with it
(core `SampleBank` `MemoryGenerationFn`) and a stale one forgets its blocks
instead of freeing them (freeing them after a reset corrupts the heap).
Size the arena's requests in whole 128 kB TLSF classes:
the heap rounds a 4–8 MiB request up to one and keeps the rest. Off-device
`PITCHED_STATUS`, `PITCHED_BENCH`, `ARENA`, `tools/make_pitched_kit.py`,
`crosspad-hil run pitched_kit`; numbers: [docs/subsystems/pitched-instrument.md](docs/subsystems/pitched-instrument.md).

## Kit Types (Performance, Drums, Slice, Pitch)

The kit browser lists PERF, DRUMS, SLICE, PITCH (then SONGS, USER), filed by
the folder a kit is in (core `KitFolders`, all three scanned at boot):
`/crosspad/kits` PERF, `/crosspad/drums` DRUMS, `/crosspad/slicer` SLICE (where
the Slicer's KIT writes); a kit elsewhere by its kit.json. PERF starts with
**+ NEW KIT** and DRUMS with **+ NEW DRUMS**: an empty kit of one's own in that
folder, sixteen coloured pads, filled in the Sampler's pad editor; `"own": true`
(core 2.32 `KitInfo::own`) makes it the user's to rename and delete while a drum
kit's category stays `drums`. **Drums** kits carry
`category` drums (`KitInfo::isDrums()`): the native engine
plays them at most 12 voices (`voiceCap()`, `TakeSpec::voiceCap`, applied on a
take and on a seamless swap's first pad). **Slice** kits (`"engine": "slice"`,
what the Slicer's KIT writes) are one WAV in regions, a pad each
(`{sample, start, end}`), one voice (choke group 1 forced in
`buildTakeSpec()`), the kit's `"pitch"` and each pad's `"attack"` in real
time; a trim that ends inside its file fades over `kTrimFadeMs`.
**Pitch** plays at most 4 voices (`PitchedSampleNode::kVoiceCap`).
**One song holds Drums, Slice, Pitch and audio at once** (core 2.6): route 0
the kits' engine, 2 the Pitch instrument (`AudioSong::seqRoute()`); up to eight
tracks (`kTracks`), every seq track but the armed one frozen, kit tracks a kit
each taking turns on the one kits' engine (`AudioSong::liveKit`, whose kit is
the song's and the Sampler's; `liveRoute()`; no second kit engine since core
2.42) -- and the Pitch track's instrument is built **beside** the kits' engines
(`PitchedLoadOptions::besideKits`: general PSRAM heap, not the arena, so a
kit take's arena reset leaves it; `instrument_host_beside()`, INST_STATUS
`beside=1`); only a bank in the arena (`pitchedBankInArena()`) still makes the
kits give way. Arrange's ADD TRACK is AUDIO / KITS, and KITS is Perf, Drums,
Slice or Pitch, a kit picked from the browser's list of its kind
(`IKitPicker::open(..., KitList)`). With Pitch armed the fingers play the
instrument and the song's route-0 notes still reach the kit (`PitchFingers`).
Measured (`tools/hil_perf_song.py --only C`): ~760 us of work a 1451 us block,
no underruns, a few single passes over the block a minute. Slice filter and
sample position are next.
Details: [docs/subsystems/sample-tools.md](docs/subsystems/sample-tools.md).

## Sampler Engines (native by default)

The native engine (core `crosspad/sampler/*`, `components/crosspad-platform-idf/SamplerEnginePort.cpp`)
plays every sampler kit: a kit load ends in `native_sampler_take_kit()`, which
reads each sample's head into the arena on its own PSRAM-stack task. The asker
copies the kit into a plain-data `TakeSpec` (core `KitTake.hpp`; a pad edit a
`TakePadSpec`) -- the worker never reads the `KitInfo` the UI frees -- and a
take asked for during another runs right after it (`WorkerGate`); `SMPL_ENGINE`
`kit=` is the kit the engine holds. Every teardown (release, arena reclaim)
goes through `io_quiesce()`: one streaming pass of its own, then
`abandonVoices()` for voices a parked render never ends, then the audition. The
prebuilt streaming engine and the glue only it uses are compiled out unless
`CONFIG_CROSSPAD_SAMPLER_MARCEL=y` (`components/crosspad-platform-idf/Kconfig`,
off in `sdkconfig.v*`/`defaults`): then it is back with its own "Sampler"
mixer channel, the native one on "Sampler2", and `SMPL_ENGINE native|marcel`
switches; its memory is the caller's (`SampleStreamPlayer_Init(32 kB, 16 kB)` in
`engine_handoff.cpp`: a first block per sample and 16 kB stream blocks, from the
arena as a kit is set up -- 1343 kB for HIL SINE A's 16 samples -- and ~6 kB of
state from internal RAM while 48 kB stays free). Without it the native engine's channel is "Sampler" in the old slot,
`SMPL_ENGINE` reports `engines=native`, `SMPL_POOL`/`SMPL_BKG` answer
`ERR only_native`. A file outside the kit (the browser, `SMPL_PREVIEW`, the
start-up sound `crosspad/STARTUP.wav`) plays on the engine's **audition voice**
(core `SamplerEngine::previewStart`, its own ring of 4 pool blocks reserved
with every kit, streamed on audio_bg, heard on the "Preview" channel; the next
one, `SMPL_PREVIEW_STOP`, closing the browser or a kit load ends it). The
live scope is the native node's own `ScopeTap` (32 kB PSRAM, taken the first
time it is shown). While the native engine holds a kit the song and preview
rings borrow the arena's slack (`engine_arena_lend()`: never what its pool
may still grow by, plus one head), and give it back before every reset. Mixer desks are matched by channel name (crosspad-mixer desk
v4), so a desk from either layout loads onto the right channels. Design and
measurements: [docs/subsystems/sampler-engine.md](docs/subsystems/sampler-engine.md).

## DAW Control (FL Studio, Ableton)

One app (`components/crosspad-dawcontrol`), one device-shaped SysEx protocol
`F0 7D 1E sub … F7` (`CROSSPAD_CMD_DAW`; catalog of record: crosspad-hil
`knowledge/sysex.yaml` `daw`): pads send `base_note+pad` notes + aftertouch,
the host paints LEDs (`led`/`led_all`), three LCD rows (`text`), `beat`,
`transport`, `mode`, `config`. The firmware knows no DAW; each DAW gets a
self-contained script in the app repo (`host/fl_studio/device_CrossPad.py`,
FL's embedded Python has no pip). Host frames reach the app through
`PlatformServices::stm32MessageHandler->setDawCallback()`; replies go out via
`IMidiOutput::sendSysEx`. The crosspad-hil MIDI hub (systemd --user, from
login) is the host endpoint: every 2 s it sends `HOST_INFO` / `FW_LATEST` as
companion notify frames (`0x1F 04`, run and never answered) — status-bar USB
glyph, DAW Control host row, persisted "update available" in Settings → Info
(`main/host_info.cpp`, `docs/subsystems/ble-midi.md`); the same report's
`daws=` says which DAWs the hub can start, and DAW Control then offers
**Launch FL Studio / Ableton / REAPER** while no script answers (`0C launch`
back to the hub, `crosspad_hil/daw_launcher.py`). REAPER's script is
`host/reaper/CrossPad.lua` (`crosspad-hil reaper install`); REAPER opens raw
MIDI only, so on Linux it talks to a `snd-virmidi` card the hub bridges. Encoder is not the app's: firmware emits CC 1
(detent 0..15) / CC 64 globally. Kit sync is host-side only
(`crosspad-hil kit push <dir>`), never from inside the DAW. Test:
`crosspad-hil run daw_control`. Design: `docs/design/2026-09-06-daw-control-design.md`.
**FL under Wine:** one open MIDI input at a time, a board reboot or script
reload leaves it allocated but unlinked (relink with `aconnect`, then restart
the app so it hellos again), and Wine routes MIDI to the device by ALSA source
address — a host tool cannot inject into FL from its own client. FL colour
ints are `0xRRGGBB`; `mixer.getCurrentTempo()` is milli-BPM. `SYSEX_ECHO 1`
(CDC, off at boot) makes the board bounce foreign-manufacturer SysEx back to
the host; that is how the `flstudio` MCP (fl-studio-mcp bridge, 0x7C, piggybacked
on the CrossPad script) reaches FL. FL script Python has no sockets, threads
or file I/O. **A mixer track's audio input is not reachable from the script
API** (no `mixer` call, no REC event — probing that id range crashes FL): the
mixer only ever shows the driver's "In 1 - In 2", which is `[Default input]`
= the CrossPad Microphone node. `crosspad-hil fl template install` writes an
FL project template (Insert 1 "CrossPad", input pair 0) into the Wine
prefix's `Projects/Templates`; File → New from template → CrossPad.

## Browser Twin and Live Stage

The board mirrors its LVGL inputs (`main/twin_mirror.cpp`, SysEx `0x1F 09`) and
its IMU (`main/motion_stream.cpp`, `0x1F 0A`) to a host page while one keeps
asking (`TWIN 1` / `MOTION 1` notifies): the web twin (this firmware in
WebAssembly, crosspad-web-twin) and mucha's live stage. **A twin is only right
when built from the sources the board was flashed from**: `TWIN_HELLO` answers
the protocol (`twin_state.h kProtocol`) and the source id
(`tools/twin_source_id.cmake`, run by both builds), and the twin compares.
Changing `twin_state.h` means a reflash and a twin rebuild; a new `TWIN_STATE`
field goes at the end, within the 470-byte line. Frames, verbs, rules:
[docs/subsystems/twin-protocol.md](docs/subsystems/twin-protocol.md).

## MIDI Clock

`main/midi_clock.cpp` over core's `MidiClock`: follows a USB host's 24-PPQN clock
(tempo from a 4-beat fit, click transport from FA/FB/FC), hands back to the
metronome's tempo after 1 s without host traffic and then generates the clock
itself (`esp_timer`, drift-free). Every tick and transport byte goes to BLE MIDI
(with a peer) and the STM, never back to the host. The Sequencer app's BPM field turns
blue and refuses edits while the host drives. `CLOCK_STATUS` over CDC;
`crosspad-hil run midi_clock`. The clock reaches TRS MIDI OUT (STM 1.14), not yet
the STM's USB port. Details:
[docs/subsystems/midi-clock.md](docs/subsystems/midi-clock.md).

## TRS MIDI (IN and OUT jacks)

The STM owns both: IN on USART2 (PD6), OUT on USART6 (STM 1.14), Type A
(data on PA4) or B (`CR2.SWAP`, data on PA5), Settings → MIDI → MIDI Out Type.
`REG_CTL_ROUTING_0/1` are core's `STMRoutingSettings` bytes; MIDI IN → USB/ESP/
thru, USB → ESP/OUT, ESP → OUT. Loop test (straight cable OUT → IN):
`crosspad-hil run midi_trs_loop` -- both types, throughput, TX baud skew and a
bit-flip injector per segment (`MIDI_TRS`, `MIDI_LOOP` over CDC). Measured:
1027 of the line's 1042 msg/s, clean within ±5 % skew, a bad byte costs its
own message. Details: [docs/subsystems/trs-midi.md](docs/subsystems/trs-midi.md).

## Audio Song (bounce, arrange, play back)

Sequencer (`components/crosspad-sequencer`, repo `crosspad-pattern-seq`)
bounces a scene to a WAV in a project on the card; Arrange lays clips on eight
tracks; Song arranges scenes as blocks with repeats; Pad Mixer sets per-pad
levels through `PlatformServices::padMix`. Core owns the model and interface
(`AudioSong`, `ISongEngine`, `WavWriter`) and, since 1.24.0, the engine itself
(`crosspad/song/SongEngine`); `crosspad-platform-idf/SongEnginePort.cpp` only
implements `ISongEnginePlatform` for the board, crosspad-pc has its own port.
**Every card operation is a
request to the `song_io` worker — nothing on the LVGL thread or the CDC loop
waits for the card**; a bounce writes 4 kB pieces that end on 4 kB of the file
through the board's DMA stage (`writeIo`, one CMD25 each; `ASONG` reports
`chunk_avg_us`/`drain_kbps`/`io_fb`); the scenes are the song's, `<project>/sequences.json` (a song from before 11.10.2026 copies the old shared `/sdcard/crosspad/sequences.json`), not the kit's -- a kit change keeps the groove (`seq_kit_change`); only `song_io` frees the rings, after RT has left
(`rtGen_`); rings never take the last 128 kB of PSRAM. The sequencer's
playback presses pads as `EventSource::SequencerCallback` (read with
`PadManager::pressSource()`); count-in is on by default (`SEQ COUNTIN 0` in a
scenario that records at once). Home screen: pad map, grid or list, Settings → Display
(`CrosspadSettings::launcherStyle`). Arrange edits by bars: a cut keeps an
audio block's `offset`/`frames` into its file, and a seq track that has been
cut or region-copied plays its scene only under its own blocks
(`seqBlocks`, `PatternSequencer::setSongMap()`). **REC/DUB in Arrange record the
pads onto the seq track from the playhead** (`ISongEngine::recordSeq`): a REC is
blocks of its own notes (`SongClip::notes`, shared copy-on-write, in song.json as
one string per block, `version` 2), to the bar of its last note -- shorter
than the loop it was played in, one block going round to the loop's end --, at most 8
bars / 192 notes each -- what one scene holds, so CLIP EDIT opens every one
(`AudioSong::putSeqTakeNotes`); a DUB adds to
the blocks under it; LOOP over a region goes round it, and with LOOP on and no region a take inside the song goes round the whole song (core 2.26). **Kit tracks,
a kit each, on one kits' engine** (core 2.42; "Kit 2"/"Kit 3", more engines, are
gone), and a Pitch track beside them (route 2, core 2.6): every kit track keeps
its kit (`SongTrack::kit`), the live one (`AudioSong::liveKit`) is the one the
engine plays and its kit is the song's -- the pads', the Sampler's; the others
play their freeze. Arrange's `live_kit()` gives the engine to the armed kit
track only after freezing the one it leaves (a new kit track stops the song:
stop, freeze, then the new kit), CLIP EDIT arms the
track and hands it over at once, so EDIT in its seq view edits that track's
kit; a kit loaded while the song is open is the live track's
(`AudioSong::kitHeld()`). REC/DUB take onto the armed track (ARM,
`AudioSong::armed`); the sequencer routes each note
(`PatternSequencer::RoutedSink`). Arrange picks a track's kit in the kit
browser (core `IKitPicker`, crosspad-sampler `kit_gate`). The seq view edits one block at a time in the
track's scene (`openSeqEdit`/`syncSeqEdit`); a seq track's bounce takes only
the bars its blocks play. **Freeze** (core 2.9, `ISongEngine::freezeTrack()`): a seq track's engine
alone bounced to `clips/.freeze/FRZ%08X.wav` and played in its place while it is not
armed and its notes, sound and tempo are unchanged (`SongTrack::freeze`,
`freezeKey`) -- a looping block's two passes only, the second played as often
as the block goes round (core 2.11), each render found on the card by what it
plays so only changed blocks render (`clips/.freeze/`, core 2.12), rendered
off the clock ~5x faster than real time (`audio_rt` offline: the clock and the
captured channel only, the sequencer polled by the render; core 2.13); Arrange freezes every seq track ARM is not on that is not frozen as it is once the song stops (core 2.26 `autoFreezeNext()`), PLAY
cancels a freeze, and the transport bar shows the render's load as `CPU nn%`.
A mute, solo or fader on a ring track is heard in ~0.3 s (core 2.36, the ring
re-mixed from 300 ms ahead, `remixRing()`); a kit track the kits' engine does
not play plays its last freeze, stale or not, instead of silence; Export mix
to WAV writes `<song>/<name>.wav`; a song keeps its Pitch pad map
(`pitch_pads`, in the freeze key) -- [docs/subsystems/audio-song.md](docs/subsystems/audio-song.md#arrange-as-a-performer-uses-it-28102026-core-233-236-arrange-029-031).
`ASONG REC|DUB|PUNCH|ARM|SEQTRACK [kit]|TRACKKIT|EXPORTWAV`,
`ASONG_SEQ [track]`. **A song names its kit and its Pitch instrument**
(`AudioSong::kit`/`instrument`, `main/song_sounds.cpp`): opening it loads them --
no kit in the song frees the kit's memory -- and a kit or instrument picked
while it is open is written into it (an unsaved instrument is saved to
`<song>/pitch/` first). One tempo:
`transportSetTempo()`/`transportCommitTempo()` in core, turned on the top bar's
BPM (its position locates Arrange while stopped). **Elements** (core 2.25, from a region or a song, baked from the card, copy or replace, a bounded cache): a block can be another arrangement, baked to `.bake/` -- [docs/subsystems/audio-song.md](docs/subsystems/audio-song.md#elements-arrangements-inside-arrangements). `ASONG`/`SONG` over CDC;
`crosspad-hil run song_bounce`. Details:
[docs/subsystems/audio-song.md](docs/subsystems/audio-song.md).

## Changing Kits While Playing

Four load-bearing facts: the loader is one long-lived worker (a task created
`WithCaps` must be deleted `WithCaps`, or every swap leaks its stack); a kit that
is not current drops its parsed pads (`releaseParsedPads()`); a request made
mid-load is queued, newest wins; three threads share one prebuilt player through
`SamplePlayerAccess.hpp` (file mutex per library call + render gate + the streamer
held out per `SetupSample()`/`WipeOut()`; the library's files are the port's
handles in `sample_player_port.cpp` — see both); the port gives it a voice cap of
32 channels after every `Init()` (`SetMaxPolyphony()`, `SMPL_POLY` overrides it) and
`audio_bg` refills through the library's own `SampleStreamPlayer_BkgProc()`. The swap is
seamless: a sounding pad finishes its sample before it is reloaded (last one forced after 1 s), the
render gate is taken only for a sounding pad, a hit on the pad being rebuilt is
held and played after it, and every bulk reader of the card yields to the
streamer (`sample_player_port_yield_to_streamer()`). The streamer's refills,
the loader's preloads and a bounce's pieces all go around stdio straight into
or out of their PSRAM buffers: the S3's SDMMC cannot DMA to or from PSRAM, and
the card's sector layer (`sd_sector_io.cpp` -- it also caches single sectors, the directory walks every open repeats, `SD_CACHE`; `--wrap=sdmmc_read_sectors`/
`_write_sectors`) bounces such a transfer through one internal buffer taken
at boot, eight sectors a command -- IDF's own path is a CMD17 a sector
through a fresh malloc (4x slower, "not enough mem" under WiFi). The older 4 kB
stage is not allocated while the sector bounce is
(`sample_player_port_direct_io()`). `SD_TRACE` counts every transaction by
task. Pad LED frames go out from the `pad_anim`
worker only (coalesced refresh): `audio_rt` records a voice's state and the
worker paints it -- the render takes no strip lock -- and the engine's voice
reports are not posted on the event queue. `KIT_SWAP_LOG` over CDC is
the per-swap evidence. Test with `crosspad-hil run kit_churn [--rapid 0.4]` and
`kit_swap_audio` (sine kits, recorded through UAC2).
Why each of those holds: [docs/subsystems/kit-swap.md](docs/subsystems/kit-swap.md).
**The native engine never stops for a kit change**: the loader swaps it pad by
pad (`native_sampler_swap_pad()`), a sounding voice plays its old sample out,
and an old head goes back once `SamplerEngine::slotInUse()` is false; no whole
take after the load while it already holds the kit. See
[docs/subsystems/sampler-engine.md](docs/subsystems/sampler-engine.md#changing-kits-while-playing).

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
traps: [docs/subsystems/stm32-update.md](docs/subsystems/stm32-update.md).

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
[docs/subsystems/firmware-library.md](docs/subsystems/firmware-library.md).

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
Details, numbers, limitations: [docs/subsystems/firmware-updates.md](docs/subsystems/firmware-updates.md).

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
gives it back); measured 97-122 kB on the Store tab. At most 32 packages; v2 is
41 PSRAM pages with 1220 B of text headroom (`ext_ram.dummy` 2686944, `.flash.text`
2030364, the page ends at 2031584 from 0x42000020; measured 2026-10-05 with
LVGL's themes, observer and text entry at `-Os`) -- check it after adding code. `SCREENSHOT` cannot
capture the overlay layer (cards, dialogs). `FW_UI` over CDC;
`crosspad-hil run firmware_app`. Details:
[docs/subsystems/firmware-app.md](docs/subsystems/firmware-app.md).

## USB Transfer Mode (the SD card as a USB drive)

A third USB profile, **MSC + CDC** (`bcdDevice` profile nibble `0x2`), and a
**boot mode**: Settings → USB → *SD card as USB drive*, or `USB_MSC 1` over CDC, arms a one-shot
`RTC_NOINIT_ATTR` flag and restarts. That boot brings up display, LVGL, USB and
the card raw (`bsp_sdcard_raw_init()`, no FATFS) and starts **no engine, no kit
scan, no kit loader, no BLE, no SD hotplug** — two writers on one FAT volume
corrupt it, so the second one is never created. Leaving is another restart; the
flag never survives a power cycle, and only `ESP_RST_SW` honours it. One
full-screen LVGL screen with the live state and an Exit reachable by touch,
encoder and power click. **The `tud_msc_*` callbacks are ours**
(`main/usb_transfer.cpp`): esp_tinyusb's `tusb_msc_storage.c` `f_mkfs`-es a card
it cannot mount and panics on unplug in every other profile, and `-Wl,--wrap`
cannot reach those callbacks (weak definitions in the TU that calls them), so
`main/CMakeLists.txt` drops that file from the build. PID stays `0x3456` or the
host tools lose the board. No SysEx/BLE entry point, on purpose. Cost: 2112 B of
internal RAM in every profile (`CFG_TUD_MSC_EP_BUFSIZE=2048`). Measured 668 kB/s
read, 495 kB/s write on a card on the 1-bit bus. Mechanism, state machine, traps:
[docs/subsystems/usb-transfer-mode.md](docs/subsystems/usb-transfer-mode.md).

## Assets Flashing and File Transfer over CDC

Device `main/file_transfer.cpp`, host `tools/fs_transfer.py`
(`assets | roots | ls | push | pull | rm | mkdir`), mounts `/assets`, `/spiflash`,
`/sdcard` only. Framing is a mode switch for an announced byte count, decided in
`cdc_dispatch_lines()` beside OTA. The assets image is staged whole in PSRAM and
CRC-checked before the erase. All filesystem work runs on `fx_worker` (5 kB
**internal** stack — under BLE it cannot be created and verbs answer
`FS_ERR no_worker …`; `BLE_STOP` first). Chunk size 4096 on both ends, fixed.
Test: `crosspad-hil run fs_transfer [--assets build_v2/assets.bin]`.
Protocol table and design: [docs/subsystems/file-transfer.md](docs/subsystems/file-transfer.md).

## Driving Your Board (start here before any device work)

`python3 tools/bench.py` is the entry point: it checks and prepares the board
connected over USB, so the traps below are handled for you.

```bash
python3 tools/bench.py check              # what is wrong + the command that fixes it
python3 tools/bench.py ready              # CDC profile, MICS route, a kit loaded
python3 tools/bench.py flash [--assets]   # flash the NEWEST build, prove it landed
python3 tools/bench.py test smoke ...     # scenarios, with the flags each one needs
python3 tools/bench.py recover            # UART-reflash via the STM bridge (no CDC left)
python3 tools/bench.py hil <args>         # raw crosspad-hil, venv resolved for you
```

Exit codes match crosspad-hil: **0** fine, **1** the firmware is at fault, **2**
the test setup is. `test` re-prepares the board before *each* scenario, because a
scenario that resets the board (smoke does) leaves it with no kit loaded and the
next one then fails as a setup problem instead of telling you anything.

| Trap | What you see | Why |
|---|---|---|
| `python` is not installed | `python: no such file or directory` | many Linux systems ship only `python3` — every doc here says `python3` |
| `crosspad-hil` is not on PATH | `command not found` | it lives in its repo's venv; use `bench.py hil` |
| Board in the MIDI+UAC2 profile | every verb "no reply", board looks dead | that profile is the **default** and has **no CDC endpoint**; `bench.py ready` switches back over SysEx, and the switch persists |
| Flashed, nothing changed | smoke passes, the change is absent | the image came from another build dir than `build_<rev>/`, or it rolled back because the board was reset inside its ~10 s trial window (an OTA over CDC is a trial boot — `bench.py flash` waits for the confirm and compares `APP_SHA` against `sha256sum build_<rev>/CrossPad.elf`, which `LIBVER` alone cannot do). Always confirm `LIBVER:` after OTA and `FS_LIST /assets` after assets |
| Icon/asset unchanged after OTA | old file still listed | OTA never touches the assets partition — `bench.py flash --assets` |
| Flashed a v1 image on a v2 board | reboots every ~19.5 s, CDC enumerates and never answers | hardware and image revision differ; `bench.py check` says MISMATCH, `bench.py recover --build build_v2` fixes it |
| OTA hangs: `expected OTA_READY, got: ''` and every verb unanswered after | the board logs `OTA started`, the host never sees the reply | the crosspad-hil MIDI hub keeps the CDC port open 5 s after each verb it carries and its reader takes `OTA_READY`; `ota_flash.py` takes the hub's exclusive op (run it with crosspad-hil's python, as `bench.py flash` does). The board is then in a half-open OTA session: `crosspad-hil console --reset` (no other console reader on the STM VCP, or the reset silently fails) |
| Scenario exits 2 with "seed kit" | setup problem, not firmware | no kit loaded; `bench.py ready` |
| `KIT_LIST` times out right after boot | "device is talking but never answered" | the boot scan holds the kit vector; retry, it is not a hang |
| Every audio test fails on good hardware | e.g. `audio_loopback` 227/227 | a DAC→ADC preset (`SPEAKER_BUS`/`HEADPHONE_BUS`) rails on codec 1. It lasts only until the next boot — every boot applies `SAMPLER` — so a reboot clears it; without one, `bench.py ready` puts MICS back |
| `TASKS` answers only `OK` | no table on CDC | it prints CPU-time stats to the **STM console**, and never stack sizes |
| `ble_midi` reports ENV | "bleak is not installed" | host-side package missing — that is not "BLE off" |
| Capture says "recorded through PipeWire" | audio scores drift late in a long take | another client (e.g. a DAW) holds the sound card, so the take runs on PipeWire's clock (~0.1 s per 16 s); it is healthy, but measure silence at the start of the take, not the end |
| Board has no CDC and no window to OTA into | every verb "no reply", `usb_mode=unknown` | firmware panicking before USB is up; `bench.py recover` UART-reflashes through the STM bridge (DTR/RTS there reach EN and GPIO0) and rewrites `ota_data_initial` so boot stops picking the bad slot |
| A line comes back as an echo | the board repeats what it was sent | on purpose: the dispatcher waits for LF (a host may split one write into two USB packets; Web Serial does) and echoes every line it does not handle, ending the echo with CRLF so it never glues onto the next reply, so a mistyped or truncated command shows itself |
| `usb-mode set default` (or `bench.py ready`, `ota_flash.py`) times out at 45 s and the board shows "USB serial mode?" | the USB serial guard is on (the default): a host's SysEx request for the CDC profile waits for Allow on the encoder | `USB_GUARD 0` over CDC once (persisted) on a development board; a user's board keeps it on. The prompt cannot be pressed from the host — `ENC_*` over BLE/SysEx answers `ERR guard_prompt_active` while it is up |
| `udisksctl power-off` on the transfer-mode drive takes the whole board off USB | every verb "no reply", `usb_mode=unknown`, the STM bridge VCP still there | that call removes the USB *device*, and the CDC port goes with it. `crosspad-hil console --reset` reboots the ESP and it re-enumerates; eject with the UDisks2 `Drive.Eject` method instead (a SCSI START STOP UNIT — the board stays on USB). See [docs/subsystems/usb-transfer-mode.md](docs/subsystems/usb-transfer-mode.md) |
| Browser companion (Web Serial) says "The device has been lost" on connect | Chrome/Chromium opens the port, then the first read fails; hil talks to the same port fine | a tty keeps its termios across reopen; pyserial-style `VMIN=0` left behind makes `poll()` always readable and `read()` return 0 bytes, which Chromium maps to device-lost. `stty -F /dev/ttyACMx min 1` or replug; hil's `RawTty.close()` restores `VMIN=1`, `bench.py check` reports it. Same family: Chromium leaves `PARMRK` set, which doubles every 0xFF byte for a reader that does not clear it (an `fs_transfer` pull then comes back too long, with a garbage trailer); hil's `RawTty` clears it |

Two test-setup facts that are **not** firmware bugs: kit loads under play
are slow — `kit_churn` needs `--load-timeout 30`, which `bench.py test` adds
for you — and `back_churn` takes 13 min for six rounds, 27 for twelve, against
the release gate's 17 min budget. A card with a bad DAT3 contact runs on the
1-bit bus; the boot's mount line says which width it got:
`SD Card mounted successfully at /sdcard (4-bit, 40000 kHz)` (`USB_MSC` has
`width= freq=` too). A failed 4-bit mount is retried 4-bit at 20 MHz at boot,
then 1-bit -- a second 4-bit attempt more than a cure: the usual failure is the
SSR read at 400 kHz, before the clock is raised, so 20 MHz helps only a card
marginal at 40 (that attempt logs `sdmmc_sd` at W, so the failing step shows).
The hotplug probe makes the two quick attempts only, and once on 1-bit the
card stays there until a restart; `USB_MSC` says which.

## Hardware-in-the-loop Testing

There are no unit tests here. The test suite is
[**crosspad-hil**](https://github.com/CrossPad/crosspad-hil) (pip package: library
+ CLI + daemon) run against a real board; exit **0** passed, **1** firmware
failed, **2** setup problem (host, cable, board state). Install: `pip install -r tools/requirements-hil.txt`
(the `git+https` form; not on PyPI yet) or `pip install -e "../crosspad-hil[all]"` from
a checkout. User walkthrough: [Test on hardware](https://crosspad.app/docs/how-to/hil-testing/).

```bash
python3 tools/bench.py test smoke         # preferred: prepares the board first
crosspad-hil run smoke                    # after every flash
crosspad-hil run app_churn --rounds 10    # app lifecycle leaks/crashes
crosspad-hil run back_churn               # multi-level back gesture, pads played in the Sampler
crosspad-hil run kit_churn --rapid 0.4    # kit swaps while pads fire
crosspad-hil run fs_transfer              # CDC file transfer + framing
crosspad-hil run release_gate             # the whole chain, one report
python3 tools/hil_perf_song.py            # A Drums+Slice+audio, B Pitch+audio, C all four at once: built, played, render judged
python3 tools/hil_stress_song.py --rounds 10   # PERF_ALL played under kit swaps and song switches: heap, resets, leaks (--listen: speakers on)
python3 tools/hil_torture_song.py --actions 250 # every Arrange action at random (--seed): tracks, takes, chords, storms, blocks, FX, freeze/bounce, kit swaps, reopen
python3 tools/hil_track_capacity.py            # a song built track by track to 8, played with chords at each count: what is safe
python3 tools/hil_track_transitions.py --tracks 5   # kit loads, ARM, TRACKKIT, edits, PUNCH under a playing song: underruns, card time by task
```

Device side of the command set: `main/hil_control.cpp` (add verbs there); the
state of the verbs the simulator answers too comes from crosspad-core
`crosspad/control/`, their CDC text from `main/hil_text.cpp` (host goldens in
`main/tests/`); host side: `crosspad_hil/verbs.py` + `knowledge/cdc.yaml`. No finger needed:
`TOUCH_DRAG x1 y1 x2 y2 ms [rest_ms] [hold_ms]` / `TOUCH_TAP x y [hold_ms]` play a synthetic
contact through the panel's read path (`main/touch_trace.cpp`; the indev is in
LVGL's event mode, so a timer reads it while one plays; `hold_ms` is a long press
before the drag), `TOAST text` shows a toast (wrap, time on screen), and `TOPBAR` says what
the top row, the drawer, the encoder and the last swipe did. `UI_TREE` lists every
visible widget with its text, rectangle and `hit` (a finger on its middle reaches
it, not something drawn over it): crosspad-hil `ui.py` finds a control by what it
says and taps it, so a scenario presses the buttons a player presses
(`studio_session`). Multi-line replies carry a
`NAME:` prefix per row — the reader drops unprefixed rows.

Traps: control is the CrossPad CDC (`0x303A`), console
is the STM bridge VCP (`0x0483:5740`) and **opening it with DTR/RTS asserted
reboots the ESP**; `PAD_PRESS` answers `OK` like `KIT_LOAD` does, so ask a state
verb instead; no CDC at all in the USB-audio profile; `ENC_GROUP` before driving
the UI; nothing that blocks belongs on the prio-1 drain loop (`CDC_STATS
maxcmd_us/maxgap_us` tells which wait made a reply late — and when both are
small yet nothing answers, the loop is starved of CPU by something at a higher
priority on core 0: `LVGL_STATS render_us` first); `app_queue` drops are
counted, not logged per drop. Pad storms come from the STM (`STM_VPAD`), not
`PAD_PRESS`; a reboot leaves a trace in `RESET_LOG` / `COREDUMP_INFO`. The
RT mixer runs in every USB profile (host playback and UAC capture go through
it); only `AUDIO_TASKS 0` / SysEx `1D 06 00` parks it -- silence both ways --
and the next USB mode switch resumes it.
All of it, with the measurements: [docs/subsystems/hil-testing.md](docs/subsystems/hil-testing.md).

## Fonts (Polish, and every other European name)

LVGL ships Montserrat as ASCII plus the FontAwesome symbols, so a kit called
"ŚWIĘTE SŁOWA" drew as boxes. crosspad-gui's
`src/fonts/lv_font_montserrat_{10,12,14,16,18,24}.c` replace LVGL's copies
**under the same symbol names** — same TTF, same bpp, same `LV_SYMBOL_*` list,
plus Latin-1 Supplement and Latin Extended-A — so none of the ~240 call sites
change and no app picks a special font; the firmware and crosspad-pc take the
same files from the submodule (12 is off in `sdkconfig.v*` and compiles to
nothing here; the simulator uses it). crosspad-gui's `CMakeLists.txt` drops
LVGL's own sources from the `lvgl__lvgl` target
(`cmake/crosspad_gui_fonts.cmake`, duplicate symbols otherwise) and forces each
enabled size undefined with `-u`: its archive is scanned before LVGL's, and
LVGL's default theme and later archives reference the fonts too.

Regenerate with `python3 components/crosspad-gui/tools/fonts/generate.py`
(needs node; fetches lv_font_conv via npx). The range has deliberate holes: Å å
Ů ů and the Latvian cedilla letters raise `line_height` by 2 px at every size on
their own, so they are left out and everything else in both blocks is in. What
remains costs one pixel per line (14 px: 16 → 17) and ~105 kB of flash.
crosspad-gui's `tools/fonts/` carries the TTF, the FontAwesome WOFF and both
licences; `tools/fw_text.py` (profile text in CI) reads its range from there,
so the CI matrix job checks the submodules out.

**FATFS speaks UTF-8** (`CONFIG_FATFS_API_ENCODING_UTF_8`, in
`sdkconfig.defaults` and both `sdkconfig.v*`). The default ANSI/OEM encoding
transcodes every filename to code page 437, which has no Polish letters: a
folder named `Żółć` came back with '?' where the diacritics were and could not
be opened. Kit *display* names were never affected — those come from the kit's
JSON as UTF-8 — only paths.

Before/after evidence for anything that touches fonts, styles or layout: the
`before-after` skill in crosspad-mcp (per-app simulator screenshots plus an
`LVGL_STATS` benchmark on the board).

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
  Check `xtensa-esp32s3-elf-size -A build_v2/CrossPad.elf | grep ext_ram.dummy`
  after anything big.
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
  hardware is the test suite (see [docs/subsystems/hil-testing.md](docs/subsystems/hil-testing.md)). No linting
  configured.

## API Documentation (Doxygen)

`doxygen` in the repo root writes browsable HTML to `docs/doxygen/html/` (open
`index.html`) for `main/` and `components/crosspad-platform-idf/` — dark theme,
sidebar, hyperlinked source. `.github/workflows/docs.yml` publishes it to
GitHub Pages (https://crosspad.github.io/platform-idf/) on every push to `main`
that touches those paths. crosspad-core and crosspad-gui have their own
Doxyfiles: run `doxygen` in each submodule root. The BSP is not in this
Doxyfile; its reference is generated on the docs site.

## CrossPad Manifesto

Write once, run everywhere; platform repos thin, shared repos thick; hardware is
software you can touch; small team, big surface area; open by default;
documentation ships with the code. Full text: [docs/MANIFESTO.md](docs/MANIFESTO.md).

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
| Audio routing / USB capture | `main/audio_route_control.{h,cpp}` |
| Audio engine rate (BSP clock, UAC rate, HIL tone kit) | `components/bsp/crosspad/i2s_driver.c`, `main/audio/audio_control.cpp`, `tools/make_tone_kit.py` |
| Kit-swap audio fixture (two interleaved sine kits, `crosspad-hil run kit_swap_audio`) | `tools/make_sine_kits.py` |
| Bench entry point (check / ready / flash / test) | `tools/bench.py` |
| Board revision resolver (`idf.py board`) | `tools/crosspad_board.py` |
| ULP RISC-V program / BSP API | `components/bsp/crosspad/ulp/ulp_background.c`, `components/bsp/crosspad/include/bsp_ulp.h` |
| HIL command set (device side) | `main/hil_control.cpp` |
| Assets flashing / file transfer (device) | `main/file_transfer.cpp`, `main/include/file_transfer.h` |
| Assets flashing / file transfer (host) | `tools/fs_transfer.py` |
| USB transfer mode (boot flag + MSC callbacks, screen, descriptors) | `main/usb_transfer.cpp`, `main/gui/usb_transfer_screen.cpp`, `main/usb_descriptors_transfer.c` |
| Firmware library / A/B switch (image descriptor, package scan, slots + rollback, assets resync, Firmware app) | `main/fw_image_desc.{h,c}`, `main/fw_library.{h,cpp}`, `main/fw_slots.{h,cpp}`, `main/fw_assets_sync.{h,cpp}`, `main/app/update/update_app.cpp`, `main/gui/fw_flow_overlay.cpp`, `tools/make_fw_package.py` |
| Firmware updates (catalog, download, cleanup, triggers, verbs, Updates view, Settings page) | `main/fw_updater.cpp`, `main/fw_updater_verbs.cpp`, `main/fw_download.cpp`, `main/fw_catalog.cpp`, `main/fw_http.cpp`, `main/fw_gc.cpp`, crosspad-gui `src/fw/fw_updates_view.cpp`, `main/app/settings/settings_updates.cpp`, [docs/subsystems/firmware-updates.md](docs/subsystems/firmware-updates.md) |
| Firmware signing and CI kits | `tools/fw_sign.py`, `tools/make_fw_package.py`, `tools/ci_matrix.py`, `tools/ci_release_assets.py`, `config/fw_signing/`, `main/fw_sign_pubkey.h`, `main/fw_verify.cpp` |
| STM32 in-field update | `components/stm32_hal/stm32_fw_update.c`, UI `main/gui/stm_fw_overlay.cpp`, image `main/stm32_fw/stm32_fw.bin` |
| Firmware app views (tiles, card, actions, Store/Updates/Library, icons, the app shell; portable, host tests) | `components/crosspad-gui/{include/crosspad-gui,src}/fw/` (`IFwViewService.h`, `fw_*_view.cpp`, `fw_kit_card.cpp`, `fw_actions.cpp`, `fw_views_common.cpp`, `fw_icons.cpp`, `fw_app.cpp`), `components/crosspad-gui/tests/test_fw_views.cpp` |
| Firmware app on the board (service, worker, FW_UI) | `main/fw_view_service.cpp`, `main/fw_view_data.cpp`, `main/fw_ui_state.cpp`, `main/fw_store_files.cpp`, [docs/subsystems/firmware-app.md](docs/subsystems/firmware-app.md) |
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
| Recorder / Slicer (sample tools: takes, audition, loops, kits from slices) | `components/crosspad-platform-idf/SampleToolsPort.cpp`, `crosspad/sampletools/`, [docs/subsystems/sample-tools.md](docs/subsystems/sample-tools.md) |
| WiFi (link, saved networks, verbs, page, fallback) / network mode (boot flag, screen) | `main/wifi_link.cpp`, `main/wifi_creds.cpp`, `main/wifi_verbs.cpp`, `main/wifi_ping.cpp`, `main/gui/wifi_page.cpp`, `main/gui/wifi_fallback.cpp`, `main/net_mode.cpp`, `main/gui/net_mode_screen.cpp`, [docs/subsystems/wifi.md](docs/subsystems/wifi.md) |
| Partition table | `partitions_16mb.csv` |
| Subsystem deep-dives (source of truth for the summaries above) | `docs/subsystems/*.md`, `docs/MANIFESTO.md` |
| SDK config defaults | `sdkconfig.defaults` |
| Open work (short list) | `TODO.md` |
