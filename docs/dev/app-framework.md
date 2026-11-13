# Writing an app

Contributor notes: apps: registration, lifecycle, events, pad logic, the code patterns every app uses. The short version of what a change in this area
must not break; the pages it links to have the detail.

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

// Register once in createFn. With REGISTER_APP_PL, that's
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

// A value repainted from a timer: set the text only when it changed
// (lv_label_set_text() frees, allocates and lays out again even for equal text)
lv_label_update_text_fmt(label, "%u/%u", active, limit);   // crosspad-gui/widgets/gui_helpers.h

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
