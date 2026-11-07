/**
 * @file update_app.cpp
 * @brief The Firmware app on this board: crosspad-gui's app
 *        (crosspad-gui/fw/fw_app.h, docs/subsystems/firmware-app.md) with the
 *        Over USB block (CDC switch, auto-accept) at the end of its Updates
 *        tab. Installs and switches run in gui/fw_flow_overlay.cpp; the OTA
 *        progress overlay is global (gui/ota_overlay.cpp).
 */

#include "update_app.hpp"
#include "crosspad/app/AppRegistrar.hpp"

#include "usb_config_manager.h"
#include "gui.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "crosspad/CrosspadCoreVersion.hpp"
#include "NvsSettingsStore.hpp"
#include "crosspad-gui/fw/fw_app.h"
#include "crosspad-gui/theme/crosspad_theme.h"

#include "fw_slots.h"

#include <cstring>
#include <cstdio>

extern lv_group_t * encoderTop_group;

static const char * TAG = "update_app";

static const lv_color_t kColorMuted = lv_color_make(140, 140, 140);
static const lv_color_t kColorWarn  = lv_color_make(255, 160, 0);

static lv_group_t * s_group = nullptr;   // the app's encoder group, where the Restore Audio buttons go

// ── Over USB (unchanged from the one-page app) ─────────────────────────────

static lv_obj_t * s_usbBox = nullptr;
static lv_obj_t * s_usb_status_label = nullptr;
static lv_obj_t * s_switch_btn = nullptr;
static lv_obj_t * s_auto_accept_cb = nullptr;
static lv_obj_t * s_close_btn = nullptr;
static bool s_was_audio_mode = false;

static bool nvs_get_auto_accept() {
    return NvsSettingsStore::getInstance().readU8("ota_prefs", "auto_accept", 0) != 0;
}

static void nvs_set_auto_accept(bool accept) {
    NvsSettingsStore::getInstance().saveU8("ota_prefs", "auto_accept", accept ? 1 : 0);
}

static bool is_cdc_mode() {
    return usb_config_manager_get_mode() == USB_MODE_DEFAULT;
}

static void update_usb_status_ui() {
    if (!s_usb_status_label) return;
    bool cdc = is_cdc_mode();
    if (cdc) {
        lv_label_set_text(s_usb_status_label, "USB: CDC + MIDI \xC2\xB7 ready for ota_flash.py");
        lv_obj_set_style_text_color(s_usb_status_label, lv_color_make(0, 200, 80), 0);
        if (s_switch_btn) lv_obj_add_flag(s_switch_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_usb_status_label, "USB: MIDI + Audio \xC2\xB7 no CDC, no OTA over USB");
        lv_obj_set_style_text_color(s_usb_status_label, kColorWarn, 0);
        if (s_switch_btn) lv_obj_remove_flag(s_switch_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_switch_usb_cb(lv_event_t * e) {
    if (is_cdc_mode()) return;
    ESP_LOGI(TAG, "Switching to CDC mode for OTA");
    s_was_audio_mode = true;
    usb_config_manager_set_mode(USB_MODE_DEFAULT);
    update_usb_status_ui();
}

static void on_auto_accept_changed(lv_event_t * e) {
    bool checked = lv_obj_has_state(s_auto_accept_cb, LV_STATE_CHECKED);
    nvs_set_auto_accept(checked);
    ESP_LOGI(TAG, "Auto-accept OTA: %s", checked ? "ON" : "OFF");
}

static void restore_confirm_yes_cb(lv_event_t * e) {
    lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t * mbox = lv_obj_get_parent(lv_obj_get_parent(btn));
    lv_msgbox_close(mbox);
    ESP_LOGI(TAG, "Restoring Audio USB mode");
    usb_config_manager_set_mode(USB_MODE_AUDIO);
    s_was_audio_mode = false;
    LoadMainScreen(lv_scr_act());
}

static void restore_confirm_no_cb(lv_event_t * e) {
    lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t * mbox = lv_obj_get_parent(lv_obj_get_parent(btn));
    lv_msgbox_close(mbox);
    LoadMainScreen(lv_scr_act());
}

static void on_close_cb(lv_event_t * e) {
    if (s_was_audio_mode) {
        lv_obj_t * mbox = lv_msgbox_create(lv_layer_top());
        lv_msgbox_add_title(mbox, "Restore Audio?");
        lv_msgbox_add_text(mbox, "USB was in Audio mode.\nRestore it before closing?");

        lv_obj_t * yesBtn = lv_msgbox_add_footer_button(mbox, "Yes, restore");
        lv_obj_t * noBtn = lv_msgbox_add_footer_button(mbox, "No, keep CDC");
        lv_obj_add_style(yesBtn, &styleButton, 0);
        lv_obj_add_style(noBtn, &styleButton, 0);
        lv_obj_add_event_cb(yesBtn, restore_confirm_yes_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(noBtn, restore_confirm_no_cb, LV_EVENT_CLICKED, NULL);

        lv_group_t * grp = s_group ? s_group : encoderTop_group;
        lv_group_add_obj(grp, yesBtn);
        lv_group_add_obj(grp, noBtn);
    } else {
        LoadMainScreen(lv_scr_act());
    }
}

static lv_obj_t * make_section_header(lv_obj_t * container, const char * text) {
    lv_obj_t * lbl = lv_label_create(container);
    lv_label_set_text(lbl, text);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_color(lbl, CROSSPAD_THEME_PRIMARY, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_top(lbl, 4, 0);
    return lbl;
}

/* The Over USB block, in screen order: status, Switch USB to CDC, auto-accept,
 * the versions, Close. Its focusables go to `group` in that order. */
static void build_usb_block(lv_obj_t * parent, lv_group_t * group) {
    s_usbBox = lv_obj_create(parent);
    lv_obj_remove_style_all(s_usbBox);
    lv_obj_set_size(s_usbBox, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_usbBox, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_usbBox, 6, 0);

    make_section_header(s_usbBox, "Over USB");

    s_usb_status_label = lv_label_create(s_usbBox);
    lv_obj_set_style_text_font(s_usb_status_label, &lv_font_montserrat_10, 0);
    lv_obj_set_width(s_usb_status_label, LV_PCT(100));
    lv_label_set_long_mode(s_usb_status_label, LV_LABEL_LONG_WRAP);

    s_switch_btn = lv_button_create(s_usbBox);
    lv_obj_add_style(s_switch_btn, &styleButton, 0);
    lv_obj_set_width(s_switch_btn, LV_PCT(100));
    lv_obj_t * sw_label = lv_label_create(s_switch_btn);
    lv_obj_center(sw_label);
    lv_label_set_text(sw_label, LV_SYMBOL_USB "  Switch USB to CDC");
    lv_obj_set_style_text_font(sw_label, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(s_switch_btn, on_switch_usb_cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(group, s_switch_btn);

    s_auto_accept_cb = lv_checkbox_create(s_usbBox);
    lv_checkbox_set_text(s_auto_accept_cb, "Auto-accept updates over USB");
    lv_obj_set_style_text_font(s_auto_accept_cb, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_auto_accept_cb, lv_color_white(), 0);
    if (nvs_get_auto_accept()) lv_obj_add_state(s_auto_accept_cb, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_auto_accept_cb, on_auto_accept_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_group_add_obj(group, s_auto_accept_cb);

    const esp_app_desc_t * app_desc = esp_app_get_description();
    lv_obj_t * version_label = lv_label_create(s_usbBox);
    lv_label_set_text_fmt(version_label, "core %s \xC2\xB7 git %s", CROSSPAD_CORE_VERSION, app_desc->version);
    lv_obj_set_width(version_label, LV_PCT(100));
    lv_label_set_long_mode(version_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(version_label, kColorMuted, 0);
    lv_obj_set_style_text_font(version_label, &lv_font_montserrat_10, 0);

    s_close_btn = lv_button_create(s_usbBox);
    lv_obj_add_style(s_close_btn, &styleButton, 0);
    lv_obj_set_width(s_close_btn, LV_PCT(100));
    lv_obj_t * close_label = lv_label_create(s_close_btn);
    lv_obj_center(close_label);
    lv_label_set_text(close_label, LV_SYMBOL_CLOSE "  Close");
    lv_obj_set_style_text_font(close_label, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(s_close_btn, on_close_cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(group, s_close_btn);

    update_usb_status_ui();
}

static void forget_usb_block() {
    s_usbBox = nullptr;
    s_usb_status_label = nullptr;
    s_switch_btn = nullptr;
    s_auto_accept_cb = nullptr;
    s_close_btn = nullptr;
}

// ── The app ─────────────────────────────────────────────────────────────

static size_t build_updates_tail(lv_obj_t * page, lv_group_t * group, lv_obj_t ** tail, size_t cap) {
    s_group = group;
    build_usb_block(page, group);
    lv_obj_t * const block[] = {s_switch_btn, s_auto_accept_cb, s_close_btn};
    size_t n = 0;
    for (lv_obj_t * obj : block)
        if (n < cap) tail[n++] = obj;
    return n;
}

static void close_updates_tail(bool deleteBlock) {
    if (deleteBlock && s_usbBox) {
        lv_obj_add_flag(s_usbBox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(s_usbBox);
    }
    forget_usb_block();
}

static lv_group_t * home_group(void) { return encoderTop_group; }

static const crosspad_gui::FwAppHooks kHooks = {build_updates_tail, close_updates_tail, update_usb_status_ui,
                                                home_group};

lv_obj_t * lv_CreateUpdateApp(lv_obj_t * parent, App * a) {
    s_was_audio_mode = false;
    return crosspad_gui::fw_app_create(parent, a, kHooks);
}

void lv_UpdateApp_destroy(lv_obj_t * app) {
    ESP_LOGI(TAG, "Destroying Firmware app");
    crosspad_gui::fw_app_destroy(app);
    s_group = nullptr;
}

// ── Auto-registration ───────────────────────────────────────────────────
// Activation is framework-owned (AppBase push/pop); this app only registers.

static const char * firmware_app_icon() {
    FwSlotInfo slots[2];
    fw_slots_get(slots);
    // Raw colours in the launcher: the outline must contrast with the tile.
    const bool darkTile = lv_color_brightness(CROSSPAD_THEME_CARD_BG) < 128;
    if (slots[0].running) return darkTile ? "fw_a_white.png" : "fw_a.png";
    return darkTile ? "fw_b_white.png" : "fw_b.png";
}

REGISTER_APP_PL(Firmware, nullptr, firmware_app_icon(),
                 lv_CreateUpdateApp, lv_UpdateApp_destroy,
                 nullptr, nullptr, nullptr, 8, "Firmware");
