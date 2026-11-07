/**
 * @file ota_overlay.cpp
 * @brief Global OTA LVGL overlay — progress bar, confirm dialog, complete/error states
 *
 * Registered once at startup via ota_overlay_init(). Works on any screen
 * (launcher, running app, settings) by drawing on lv_layer_top().
 */

#ifdef USE_LVGL

#include "ota_cdc.h"
#include "gui.h"
#include "lvgl.h"
#include "lv_async_safe.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "crosspad-gui/components/app_orchestrator.h"

extern lv_group_t * encoderTop_group;
extern lv_indev_t * indev_encoderTop;
// styleButton comes from crosspad-gui (via gui.h -> crosspad_gui.h -> styles.h)

static const char * TAG = "ota_overlay";

// ── Overlay objects ─────────────────────────────────────────────────────

static lv_obj_t * s_overlay = nullptr;
static lv_obj_t * s_bar = nullptr;
static lv_obj_t * s_progress_label = nullptr;
static lv_obj_t * s_confirm_auto_cb = nullptr;
static int s_last_percent = -1;

// ── NVS helpers (same namespace as update_app) ──────────────────────────

static void nvs_set_auto_accept(bool accept) {
    nvs_handle_t h;
    if (nvs_open("ota_prefs", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "auto_accept", accept ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ── Progress overlay ────────────────────────────────────────────────────

static void show_progress(void * param) {
    // Tear down running app: free CPU/audio/RAM so OTA write doesn't stall or crash.
    auto& orch = crosspad_gui::AppOrchestrator::getInstance();
    if (auto* running = orch.getRunningApp()) {
        if (running->isStarted()) {
            running->destroyApp();
        }
        orch.setRunningApp(nullptr);
    }

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, 320, 240);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_center(s_overlay);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(s_overlay);
    lv_label_set_text(title, ota_is_delta() ? "Applying Delta Update..." : "Updating Firmware...");
    lv_obj_set_style_text_color(title, CROSSPAD_THEME_PRIMARY, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t * warn = lv_label_create(s_overlay);
    lv_label_set_text(warn, "Do not disconnect!");
    lv_obj_set_style_text_color(warn, lv_color_white(), 0);
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_10, 0);
    lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, 65);

    s_bar = lv_bar_create(s_overlay);
    lv_obj_set_size(s_bar, 240, 20);
    lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, 10);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, lv_color_make(40, 40, 40), 0);
    lv_obj_set_style_bg_color(s_bar, CROSSPAD_THEME_PRIMARY, LV_PART_INDICATOR);

    s_progress_label = lv_label_create(s_overlay);
    lv_label_set_text(s_progress_label, "0%");
    lv_obj_set_style_text_color(s_progress_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_progress_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_progress_label, LV_ALIGN_CENTER, 0, 40);
}

struct OtaProgressData {
    size_t received;
    size_t total;
};

static void update_progress(void * param) {
    OtaProgressData * data = (OtaProgressData *)param;
    if (!data) return;
    if (s_bar && s_progress_label) {
        int percent = (int)(data->received * 100 / data->total);
        lv_bar_set_value(s_bar, percent, LV_ANIM_ON);
        char buf[32];
        snprintf(buf, sizeof(buf), "%d%%  (%zu KB)", percent, data->received / 1024);
        lv_label_set_text(s_progress_label, buf);
    }
    delete data;
}

// ── Complete / Error ────────────────────────────────────────────────────

static void show_complete(void * param) {
    if (!s_overlay) return;
    if (s_bar) lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    if (s_progress_label) {
        lv_label_set_text(s_progress_label, "Update Complete!\nRestarting...");
        lv_obj_set_style_text_color(s_progress_label, lv_color_make(0, 255, 0), 0);
        lv_obj_set_style_text_font(s_progress_label, &lv_font_montserrat_16, 0);
        lv_obj_align(s_progress_label, LV_ALIGN_CENTER, 0, 0);
    }
}

static void error_timer_cb(lv_timer_t * timer) {
    lv_obj_t * overlay = (lv_obj_t *)lv_timer_get_user_data(timer);
    if (overlay) lv_obj_delete(overlay);
}

static void show_error(void * param) {
    if (!s_overlay) return;
    if (s_bar) lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    if (s_progress_label) {
        lv_label_set_text(s_progress_label, "Update Failed!");
        lv_obj_set_style_text_color(s_progress_label, lv_color_make(255, 0, 0), 0);
        lv_obj_set_style_text_font(s_progress_label, &lv_font_montserrat_16, 0);
        lv_obj_align(s_progress_label, LV_ALIGN_CENTER, 0, 0);
    }
    lv_timer_t * t = lv_timer_create(error_timer_cb, 3000, s_overlay);
    lv_timer_set_repeat_count(t, 1);
    s_overlay = nullptr;
    s_bar = nullptr;
    s_progress_label = nullptr;
}

// ── Confirm dialog ──────────────────────────────────────────────────────

static void confirm_update_cb(lv_event_t * e) {
    lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t * mbox = lv_obj_get_parent(lv_obj_get_parent(btn));
    if (s_confirm_auto_cb) {
        bool checked = lv_obj_has_state(s_confirm_auto_cb, LV_STATE_CHECKED);
        nvs_set_auto_accept(checked);
    }
    lv_msgbox_close(mbox);
    s_confirm_auto_cb = nullptr;
    ota_user_respond(true);
}

static void confirm_cancel_cb(lv_event_t * e) {
    lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t * mbox = lv_obj_get_parent(lv_obj_get_parent(btn));
    lv_msgbox_close(mbox);
    s_confirm_auto_cb = nullptr;
    ota_user_respond(false);
}

struct OtaConfirmData {
    size_t size;
    char version[33];
};

static void show_confirm(void * param) {
    OtaConfirmData * data = (OtaConfirmData *)param;
    if (!data) return;
    char body[128];
    float size_kb = (float)data->size / 1024.0f;
    if (ota_is_delta()) {
        snprintf(body, sizeof(body), "Delta update to %s\n(%.1f KB).\nRestart required.", data->version[0] ? data->version : "unknown version", size_kb);
    } else {
        float size_mb = (float)data->size / 1024.0f / 1024.0f;
        snprintf(body, sizeof(body), "Update to %s\n(%.2f MB).\nRestart required.", data->version[0] ? data->version : "unknown version", size_mb);
    }

    lv_obj_t * mbox = lv_msgbox_create(lv_layer_top());
    lv_msgbox_add_title(mbox, "OTA Update");
    lv_msgbox_add_text(mbox, body);

    lv_obj_t * content = lv_msgbox_get_content(mbox);
    s_confirm_auto_cb = lv_checkbox_create(content);
    lv_checkbox_set_text(s_confirm_auto_cb, "Auto-accept updates");
    lv_obj_set_style_text_font(s_confirm_auto_cb, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_confirm_auto_cb, lv_color_white(), 0);

    lv_obj_t * updateBtn = lv_msgbox_add_footer_button(mbox, "Update");
    lv_obj_t * cancelBtn = lv_msgbox_add_footer_button(mbox, "Cancel");
    lv_obj_add_style(updateBtn, &styleButton, 0);
    lv_obj_add_style(cancelBtn, &styleButton, 0);
    lv_obj_add_event_cb(updateBtn, confirm_update_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(cancelBtn, confirm_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_group_add_obj(encoderTop_group, updateBtn);
    lv_group_add_obj(encoderTop_group, cancelBtn);
    lv_group_add_obj(encoderTop_group, s_confirm_auto_cb);
    delete data;
}

// ── Callback bridge (called from CDC task → lv_async_call) ──────────────

static void on_begin(size_t total_size, const char *version) {
    s_last_percent = -1;
    lv_async_call_locked(show_progress, nullptr);
}

static void on_progress(size_t received, size_t total) {
    int percent = (int)(received * 100 / total);
    if (percent != s_last_percent) {
        s_last_percent = percent;
        OtaProgressData * pd = new OtaProgressData{received, total};
        lv_async_call_locked(update_progress, pd);
    }
}

static void on_complete() {
    lv_async_call_locked(show_complete, nullptr);
}

static void on_error() {
    lv_async_call_locked(show_error, nullptr);
}

static void on_confirm_request(size_t size, const char *version) {
    OtaConfirmData * data = new OtaConfirmData;
    data->size = size;
    if (version) strncpy(data->version, version, sizeof(data->version) - 1);
    data->version[sizeof(data->version) - 1] = '\0';
    lv_async_call_locked(show_confirm, data);
}

// ── Public init ─────────────────────────────────────────────────────────

extern "C" void ota_overlay_init(void) {
    ota_ui_callbacks_t cbs = {};
    cbs.on_begin = on_begin;
    cbs.on_progress = on_progress;
    cbs.on_complete = on_complete;
    cbs.on_error = on_error;
    cbs.on_confirm_request = on_confirm_request;
    ota_set_ui_callbacks(&cbs);
    ESP_LOGI(TAG, "Global OTA overlay registered");
}

#endif // USE_LVGL
