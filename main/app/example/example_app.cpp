/**
 * @file example_app.cpp
 * @brief Minimal CrossPad application example
 *
 * This is a template for creating new CrossPad applications.
 * It demonstrates:
 *   - Auto-registration with REGISTER_APP
 *   - LVGL UI creation (factory function)
 *   - App lifecycle (create, show, destroy)
 *   - Encoder input handling
 *   - Pad LED logic via IPadLogicHandler
 *   - Closing from a button with destroyAppAsync()
 *
 * To use as a starting point, copy this directory and rename "Example" to your app name.
 */

#include "example_app.hpp"
#include "crosspad/app/AppRegistrar.hpp"
#include "CrosspadPlatform.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad-gui/platform/IGuiPlatform.h"
using crosspad::PadManager;
using crosspad::RgbColor;
using crosspad::IPadLogicHandler;

static lv_group_t * s_group = nullptr;
static lv_obj_t * s_label = nullptr;
static int s_counter = 0;

// ── Pad Logic Handler ───────────────────────────────────────────────────

class ExamplePadLogic : public IPadLogicHandler {
public:
    void onActivate(PadManager &) override {
        // No pad state to set up for this template — real apps typically
        // paint an idle color for all pads here.
    }

    void onDeactivate(PadManager &) override {
        // No pad state to tear down for this template.
    }

    void onPadPress(PadManager & pm, uint8_t padIdx, uint8_t velocity) override {
        pm.setPadColor(padIdx, RgbColor(0, 200, 255));
    }

    void onPadRelease(PadManager & pm, uint8_t padIdx) override {
        pm.setPadColor(padIdx, RgbColor(0, 0, 0));
    }

    void onPadPressure(PadManager & pm, uint8_t padIdx, uint8_t pressure) override {
        pm.setPadColor(padIdx, RgbColor(0, pressure, 0));
    }
};

static std::shared_ptr<ExamplePadLogic> s_padLogic;

// --- UI Creation (factory) ---

lv_obj_t * lv_CreateExampleApp(lv_obj_t * parent, App * a) {
    // Create main container
    lv_obj_t * container = lv_obj_create(parent);
    lv_obj_set_size(container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(container, lv_color_black(), 0);
    lv_obj_set_style_pad_all(container, 10, 0);

    // Title
    lv_obj_t * title = lv_label_create(container);
    lv_label_set_text(title, "Example App");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    // Counter label (updated by encoder)
    s_label = lv_label_create(container);
    s_counter = 0;
    lv_label_set_text_fmt(s_label, "Counter: %d", s_counter);
    lv_obj_set_style_text_color(s_label, lv_color_make(0, 200, 255), 0);

    // Exit button
    lv_obj_t * btn = lv_button_create(container);
    lv_obj_t * btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Exit");
    lv_obj_add_event_cb(btn, [](lv_event_t * e) {
        App * app = static_cast<App *>(lv_event_get_user_data(e));
        if (app) app->destroyAppAsync();   // never destroyApp() from an event callback
    }, LV_EVENT_CLICKED, a);

    // Setup encoder group for this app
    // (the framework restores the previous encoder group after destroy)
    lv_indev_t * enc = crosspad_gui::getGuiPlatform().getNavigationEncoder();
    if (enc) {
        s_group = lv_group_create();
        lv_group_add_obj(s_group, btn);
        lv_indev_set_group(enc, s_group);
    }

    // Register pad logic handler
    if (!s_padLogic) {
        s_padLogic = std::make_shared<ExamplePadLogic>();
        crosspad::getPadManager().registerPadLogic("Example", s_padLogic);
    }

    return container;
}

// --- Cleanup ---

void lv_ExampleApp_destroy(lv_obj_t * app) {
    if (s_group) {
        lv_group_delete_deferred(s_group);
        s_group = nullptr;
    }
    s_label = nullptr;
    // root is null when the app's screen was already wiped: free what is ours, delete nothing.
    if (app) lv_obj_delete_async(app);
}

// --- Auto-registration ---
// No wiring needed: on the board the app is in MODE → More (pad 8).
// Parameters: name, appLogic, icon, createFn, destroyFn,
//             initSimpleGUI, refreshGUI, cleanup, priority, padLogicName
// Pad logic activation/restoration is framework-owned (AppBase push/pop) —
// apps only registerPadLogic() their handler, they don't activate it.

REGISTER_APP_PL(Example, nullptr, "play.png",
                 lv_CreateExampleApp, lv_ExampleApp_destroy,
                 nullptr, nullptr, nullptr, 5, "Example");
