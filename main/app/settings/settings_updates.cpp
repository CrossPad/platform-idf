#ifdef USE_LVGL

#include "settings_updates.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_app_desc.h"
#include "fw_image_desc.h"
#include "fw_updater.h"
#include "crosspad-gui/fw/fw_updates_view.h"
#include "wifi_fallback.h"
#include "crosspad-gui/components/settings_ui.h"
#include "crosspad-gui/components/ui_toast.h"
#include "crosspad/status/CrosspadStatus.hpp"

extern crosspad::CrosspadStatus status;

/* The switches and the channel are saved by the updater's own writer
 * (fw_updater.cpp persist()), so the LVGL thread never waits for flash. A
 * timer keeps the status row current while a check runs; it is deleted with
 * the page. */

static constexpr uint32_t kRefreshMs = 500;
static constexpr uint32_t kToastMs = 2000;
static constexpr size_t kRunningMax = FW_KIT_ID_LEN + sizeof(esp_app_desc_t::version) + 1;
static constexpr size_t kStatusMax = 96;
static constexpr int32_t kStatusPad = 4;   // as the settings rows around it
static constexpr char kBusy[] = "busy";
static const lv_color_t kCaptionColor = lv_color_make(160, 160, 160);   // as the settings rows' labels

static bool s_boot;
static uint8_t s_channel;           // dropdown index: 0 stable, 1 beta
static char s_running[kRunningMax];
static char s_status[kStatusMax];
static lv_obj_t * s_status_value;
static lv_timer_t * s_timer;
static lv_group_t * s_group;        // the encoder's group, for the network-mode offer
static bool s_checking;             // a check this page started is running

static void status_text(char * out, size_t n) {
    char err[FWU_ERROR_LEN];
    char ver[FWU_VERSION_LEN];
    switch (fw_updater_state()) {
        case FWU_DOWNLOADING:
            fw_updater_target(ver, sizeof ver);
            snprintf(out, n, "Downloading %s... %u %%", ver, (unsigned)fw_updater_progress());
            return;
        case FWU_INSTALLING:
            fw_updater_target(ver, sizeof ver);
            snprintf(out, n, "Installing %s... %u %%", ver, (unsigned)fw_updater_progress());
            return;
        case FWU_READY_TO_RESTART:
            fw_updater_ready_version(ver, sizeof ver);
            snprintf(out, n, "%s installed - see Firmware", ver);
            return;
        case FWU_CHECKING:
            snprintf(out, n, "Checking...");
            return;
        case FWU_FAILED:
            fw_updater_error(err, sizeof err);
            snprintf(out, n, "Last check failed: %s", fw_updates_error_text(err));
            return;
        default:
            break;
    }
    if (status.fwUpdateAvailable) {   // the last check's result, or the hub's
        snprintf(out, n, "Update available: %s", status.fwLatest);
        return;
    }
    if (!fw_updater_checked()) {
        snprintf(out, n, "Not checked yet");
        return;
    }
    const int64_t checked = fw_updater_last_check();
    if (checked <= 0) {   // the catalog's response carried no usable Date
        snprintf(out, n, "Up to date");
        return;
    }
    const time_t t = static_cast<time_t>(checked);
    struct tm tm;
    gmtime_r(&t, &tm);
    char day[16];
    strftime(day, sizeof day, "%Y-%m-%d", &tm);
    snprintf(out, n, "Up to date (checked %s)", day);
}

static void boot_cb(lv_event_t * e) {
    (void)e;
    fw_updater_set_boot_check(s_boot);
}

static void channel_cb(lv_event_t * e) {
    const char * kit = fw_image_desc_running()->kit_id;
    if (fw_updater_set_channel(kit, s_channel == 1)) return;
    // not saved (the beta list is full): the dropdown must not claim a choice nothing holds
    s_channel = fw_updater_channel_beta(kit) ? 1 : 0;
    lv_dropdown_set_selected(static_cast<lv_obj_t *>(lv_event_get_target(e)), s_channel);
    ui_toast_show("Could not save the channel", kToastMs, UI_TOAST_WARNING);
}

static void offer_network_mode(void) {
    wifi_fallback_offer(NET_TAB_UPDATES, s_group);   // a no-op in network mode
}

/* The press was refused at once: busy, or no_mem (no job task). A refused
 * lease (WiFi off, no_mem) ends the job failed instead: see timer_cb(). */
static void check_cb(lv_event_t * e) {
    (void)e;
    char why[FWU_ERROR_LEN];
    if (fw_updater_check_start(why, sizeof why)) {
        s_checking = true;
    } else if (strcmp(why, kBusy) == 0) {
        ui_toast_show("Busy - try again in a moment", kToastMs, UI_TOAST_INFO);
    } else if (fw_updates_is_no_mem(why)) {
        offer_network_mode();
    } else {
        ui_toast_show(fw_updates_error_text(why), kToastMs, UI_TOAST_WARNING);
    }
}

static void timer_cb(lv_timer_t * t) {
    (void)t;
    status_text(s_status, sizeof s_status);
    if (s_status_value && strcmp(lv_label_get_text(s_status_value), s_status) != 0) {
        lv_label_set_text(s_status_value, s_status);
        fw_updates_keep_focus_in_view(s_group);
    }
    if (!s_checking || fw_updater_busy()) return;
    s_checking = false;
    char err[FWU_ERROR_LEN];
    fw_updater_error(err, sizeof err);
    // no_mem from the job's lease or the radio's own memory gate after it; any
    // other reason (WiFi off) is in the status row as "Last check failed: ..."
    if (fw_updater_state() == FWU_FAILED && fw_updates_is_no_mem(err)) offer_network_mode();
}

static void page_deleted_cb(lv_event_t * e) {
    (void)e;
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    s_status_value = nullptr;
    s_group = nullptr;
    s_checking = false;
}

static void cat_build_updates(lv_obj_t * parent, lv_group_t * group) {
    s_group = group;
    s_boot = fw_updater_boot_check();
    const char * kit = fw_image_desc_running()->kit_id;
    s_channel = fw_updater_channel_beta(kit) ? 1 : 0;
    snprintf(s_running, sizeof s_running, "%s %s", kit, esp_app_get_description()->version);
    status_text(s_status, sizeof s_status);

    crosspad_gui::settings_section_header(parent, "Firmware updates");
    crosspad_gui::settings_row_switch(parent, group, "Check at startup", &s_boot, boot_cb);
    crosspad_gui::settings_row_dropdown(parent, group, "Channel", "Stable\nBeta", &s_channel, channel_cb);
    crosspad_gui::settings_row_info(parent, "Running", s_running);
    /* A caption over a full-width wrapped value: the failure texts are too
     * long for half of a 320 px row. */
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(row, kStatusPad, 0);
    lv_obj_t * caption = lv_label_create(row);
    lv_label_set_text(caption, "Status");
    lv_obj_set_style_text_color(caption, kCaptionColor, 0);
    lv_obj_set_style_text_font(caption, &lv_font_montserrat_14, 0);
    s_status_value = lv_label_create(row);
    lv_label_set_text(s_status_value, s_status);
    lv_obj_set_width(s_status_value, LV_PCT(100));
    lv_label_set_long_mode(s_status_value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_status_value, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_status_value, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(row, page_deleted_cb, LV_EVENT_DELETE, nullptr);
    crosspad_gui::settings_row_action(parent, group, "Check now", "Check", check_cb);

    if (s_timer) lv_timer_delete(s_timer);
    s_timer = lv_timer_create(timer_cb, kRefreshMs, nullptr);
}

void settings_updates_register(void) {
    static bool registered = false;
    if (registered) return;
    registered = true;
    crosspad_gui::SettingsCategory cat = { "Updates", LV_SYMBOL_DOWNLOAD, cat_build_updates };
    crosspad_gui::settings_ui_add_category(cat);
}

#endif // USE_LVGL
