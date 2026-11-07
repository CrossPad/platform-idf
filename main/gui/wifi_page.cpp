#ifdef USE_LVGL

#include "wifi_page.h"

#include <atomic>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "acct_pair_overlay.h"
#include "cp_account.h"
#include "cp_remote.h"
#include "crosspad-gui/fw/fw_updates_view.h"
#include "net_mode.h"
#include "wifi_creds.h"
#include "wifi_fallback.h"
#include "wifi_link.h"
#include "wifi_ping.h"
#include "crosspad/fw/FwAccount.hpp"
#include "crosspad/net/WifiTypes.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad-gui/components/power_gesture.h"
#include "crosspad-gui/components/settings_ui.h"
#include "crosspad-gui/components/ui_modal.h"
#include "crosspad-gui/components/ui_text_prompt.h"
#include "crosspad-gui/widgets/gui_helpers.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

extern lv_indev_t * indev_encoderTop;

/* One page at a time (Settings and network mode never share a boot), so the
 * state is static. In Settings the page is deleted when the category closes:
 * s_pageGen moves on then, and a worker result for an older page is dropped.
 * Blocking work runs on one worker at a time; the page's timer picks the
 * result up. */
static constexpr size_t   kStatusMax = 96;
static constexpr int32_t  kStatusValueWidthPct = 70;
static constexpr uint32_t kRefreshMs = crosspad::kWifiConnectStepMs;
static constexpr uint32_t kNoteMs = 5000;
static constexpr uint32_t kScanStack = 4096;      // waits on the control task: PSRAM is fine
static constexpr uint32_t kTestStack = 8192;      // a TLS handshake: PSRAM is fine
static constexpr uint32_t kForgetStack = 4096;    // an NVS write: internal stack (wifi_creds.h)
static constexpr UBaseType_t kJobPrio = 2;
static constexpr BaseType_t kJobCore = 1;
static constexpr int32_t  kListPad = 4;
static constexpr int32_t  kListRowH = 32;
static constexpr int kHttpReachableMin = 200;     // as WIFI_PING: a 404 still proves the path
static constexpr int kHttpReachableEnd = 500;
static constexpr size_t   kMaxSaved = crosspad::WifiNetworkList::kMax;
static constexpr size_t   kSsidBuf = crosspad::WifiNetworkList::kMaxSsid + 1;
static constexpr int      kSsidPrec = (int)crosspad::WifiNetworkList::kMaxSsid;   // a scan record's SSID may fill its array
static constexpr char     kShortPassNote[] = "Min 8 characters: ";
static constexpr size_t   kTitleMax = sizeof("Password for ") + crosspad::WifiNetworkList::kMaxSsid;
static constexpr size_t   kRowTextMax = kSsidBuf + sizeof("   -128 dBm  wpa2wpa3");

enum class Job : uint8_t { None, Scan, Forget, Test };

/* The page's buffers live in PSRAM (EXT_RAM_BSS_ATTR): they exist in every
 * boot, and internal RAM is what the WiFi gates run short of. The two job
 * flags stay internal -- an atomic exchange needs internal RAM on the S3. */
static std::atomic<uint8_t> s_jobDone{(uint8_t)Job::None};
static std::atomic<bool> s_jobBusy{false};

/* Written by the worker, read on the LVGL thread once s_jobDone says so. */
EXT_RAM_BSS_ATTR static struct {
    uint32_t gen;            // the page the job was started for
    int count;
    wifi_ap_t aps[WIFI_LINK_SCAN_MAX];
    int pingResult;          // > 0 HTTP status, < 0 -(esp_err_t), 0 lease refused
    uint32_t pingMs;
    bool forgetOk;
    char ssid[kSsidBuf];     // the Forget argument
} s_box;

static wifi_page_opts_t s_opts;
static uint32_t s_pageGen;
static lv_timer_t * s_timer;
static lv_group_t * s_group;
static lv_obj_t * s_status_value;
static lv_obj_t * s_account_value;
static lv_obj_t * s_account_action;   // the action row's label and its button's: Link or Unlink
static lv_obj_t * s_account_button;
static lv_obj_t * s_remote_value;
/* Built once and updated in place, hidden past the list's end: a row deleted
 * and made again would join the encoder group at its end (after Exit in
 * network mode), and deleting the focused one hands the focus on -- to Exit,
 * on hardware. */
static lv_obj_t * s_saved_none;
static lv_obj_t * s_saved_rows[kMaxSaved];
EXT_RAM_BSS_ATTR static char s_saved[kMaxSaved][kSsidBuf];
/* The network a saved row's dialog was opened for: the rows are relabelled in
 * place while it is open (a connect reorders the list), so the dialog acts on
 * the name it showed, not on whatever its row says by then. */
EXT_RAM_BSS_ATTR static char s_choiceSsid[kSsidBuf];
/* A snapshot of the saved list, LVGL thread only; it holds the passwords, so
 * it is cleared after each use. */
EXT_RAM_BSS_ATTR static crosspad::WifiNetworkList s_creds;
EXT_RAM_BSS_ATTR static char s_note[kStatusMax];
static uint32_t s_noteUntil;
static uint8_t s_lastState = (uint8_t)crosspad::WifiLinkState::Off;
static bool s_lease;
static bool s_allowedSeen;
static bool s_offered;       // one network-mode offer per page

/* The scan list overlay, while it is open. */
static lv_obj_t * s_list;
static lv_group_t * s_list_group;
static lv_group_t * s_list_prev;
static uint32_t s_list_back;
EXT_RAM_BSS_ATTR static wifi_ap_t s_shown[WIFI_LINK_SCAN_MAX];
EXT_RAM_BSS_ATTR static wifi_ap_t s_picked;

static bool wifi_allowed(void)
{
    auto * cfg = crosspad::CrosspadSettings::getInstance();
    return cfg && cfg->wireless.enableWiFi;
}

static const char * fail_text(crosspad::WifiFail f)
{
    switch (f) {
        case crosspad::WifiFail::Auth:      return "wrong password";
        case crosspad::WifiFail::NoNetwork: return "network not found";
        case crosspad::WifiFail::Timeout:   return "no answer from the network";
        case crosspad::WifiFail::Lost:      return "connection lost";
        case crosspad::WifiFail::NoMem:     return "not enough memory";
        case crosspad::WifiFail::Disabled:  return "WiFi is off";
        default:                            return crosspad::wifiFailName(f);
    }
}

static void note(const char * fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_note, sizeof(s_note), fmt, ap);
    va_end(ap);
    s_noteUntil = lv_tick_get() + kNoteMs;
}

static void status_text(char * out, size_t n)
{
    if (s_note[0] && (int32_t)(s_noteUntil - lv_tick_get()) > 0) { snprintf(out, n, "%s", s_note); return; }
    if (!wifi_allowed()) { snprintf(out, n, "WiFi is off"); return; }
    char ssid[kSsidBuf];
    int8_t rssi = 0;
    uint32_t ip = 0;
    wifi_link_info(ssid, sizeof(ssid), &rssi, &ip);
    switch ((crosspad::WifiLinkState)wifi_link_state()) {
        case crosspad::WifiLinkState::Up: {
            esp_ip4_addr_t a = {ip};
            snprintf(out, n, "Connected: %s, %d dBm, " IPSTR, ssid, (int)rssi, IP2STR(&a));
            break;
        }
        case crosspad::WifiLinkState::Starting:   snprintf(out, n, "Starting..."); break;
        case crosspad::WifiLinkState::Connecting: snprintf(out, n, "Connecting to %s...", ssid); break;
        case crosspad::WifiLinkState::Failed:
            snprintf(out, n, "Error: %s", fail_text((crosspad::WifiFail)wifi_link_fail()));
            break;
        default: snprintf(out, n, "Off"); break;
    }
}

/* Settings holds a lease while open (spec §3) and only while WiFi is allowed;
 * acts on a change only, so a refused lease is not retried every tick. */
static void lease_sync(void)
{
    if (!s_opts.holdLease) return;
    const bool allowed = wifi_allowed();
    if (allowed == s_allowedSeen) return;
    s_allowedSeen = allowed;
    if (allowed) {
        s_lease = wifi_link_acquire("settings");
    } else if (s_lease) {
        wifi_link_release();
        s_lease = false;
    }
}

/* Every request this page makes ends in the link's state: a refused lease, a
 * radio gate refusal and a failed scan all leave failed(no_mem). With WiFi not
 * allowed the page makes none, and a no_mem left from before is not its own. */
static void offer_if_no_mem(void)
{
    if (!s_opts.offerNetworkMode || s_offered || !wifi_allowed()) return;
    if ((crosspad::WifiLinkState)wifi_link_state() != crosspad::WifiLinkState::Failed ||
        (crosspad::WifiFail)wifi_link_fail() != crosspad::WifiFail::NoMem) return;
    s_offered = true;
    wifi_fallback_offer(NET_TAB_WIFI, s_group);
}

// ---- workers ----

static bool start_job(TaskFunction_t fn, const char * name, uint32_t stack, bool internal)
{
    if (s_jobBusy.exchange(true)) return false;
    s_box.gen = s_pageGen;
    const BaseType_t ok = internal
        ? xTaskCreatePinnedToCore(fn, name, stack, nullptr, kJobPrio, nullptr, kJobCore)
        : xTaskCreatePinnedToCoreWithCaps(fn, name, stack, nullptr, kJobPrio, nullptr, kJobCore,
                                          MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) { s_jobBusy.store(false); return false; }
    return true;
}

static void scan_job(void *)
{
    s_box.count = wifi_link_scan(s_box.aps, WIFI_LINK_SCAN_MAX);
    s_jobDone.store((uint8_t)Job::Scan);
    vTaskDeleteWithCaps(nullptr);
}

static void test_job(void *)
{
    s_box.pingResult = 0;
    if (wifi_link_acquire("test")) {
        s_box.pingResult = wifi_ping(&s_box.pingMs, nullptr);
        wifi_link_release();
    }
    s_jobDone.store((uint8_t)Job::Test);
    vTaskDeleteWithCaps(nullptr);
}

static void forget_job(void *)
{
    s_box.forgetOk = wifi_creds_forget(s_box.ssid);
    s_jobDone.store((uint8_t)Job::Forget);
    vTaskDelete(nullptr);
}

// ---- saved networks ----

static void saved_choice_cb(int index, void * user)
{
    (void)user;
    if (index == 0) {
        /* As a candidate: a failed attempt leaves the saved order alone. */
        wifi_creds_snapshot(s_creds);
        const int at = s_creds.find(s_choiceSsid);
        if (at < 0) note("%s is no longer saved", s_choiceSsid);
        else wifi_link_connect_candidate(s_creds.at((size_t)at).ssid, s_creds.at((size_t)at).pass);
        s_creds = {};
    } else if (index == 1) {
        /* Checked before the argument is written: a forget still running reads
         * it. Only this thread starts jobs, so busy cannot turn on in between. */
        if (s_jobBusy.load()) { note("Busy, try again"); return; }
        snprintf(s_box.ssid, sizeof(s_box.ssid), "%s", s_choiceSsid);
        if (!start_job(forget_job, "wifi_forget", kForgetStack, true)) note("Busy, try again");
    }
}

static void saved_clicked_cb(lv_event_t * e)
{
    const size_t i = (size_t)(uintptr_t)lv_event_get_user_data(e);
    static const ui_modal_button_t kButtons[] = {
        {"Connect", UI_MODAL_BUTTON_PRIMARY},
        {"Forget", UI_MODAL_BUTTON_DANGER},
        {"Cancel", UI_MODAL_BUTTON_NORMAL},
    };
    snprintf(s_choiceSsid, sizeof(s_choiceSsid), "%s", s_saved[i]);
    ui_modal_choice(s_choiceSsid, nullptr, kButtons, (int)(sizeof(kButtons) / sizeof(kButtons[0])), 0,
                    saved_choice_cb, nullptr, s_group);
}

static void saved_sync(void)
{
    if (!s_saved_none) return;
    wifi_creds_snapshot(s_creds);
    const crosspad::WifiNetworkList & list = s_creds;
    for (size_t i = 0; i < kMaxSaved; ++i) {
        lv_obj_t * row = s_saved_rows[i];
        if (i >= list.size()) { lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN); continue; }
        if (strcmp(s_saved[i], list.at(i).ssid) != 0) {
            snprintf(s_saved[i], sizeof(s_saved[i]), "%s", list.at(i).ssid);
            lv_label_set_text(lv_obj_get_child(row, 0), s_saved[i]);   // settings_row_action(): label, then button
        }
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
    if (list.size() == 0) lv_obj_remove_flag(s_saved_none, LV_OBJ_FLAG_HIDDEN);
    else                  lv_obj_add_flag(s_saved_none, LV_OBJ_FLAG_HIDDEN);
    s_creds = {};
    /* The focused row went (a forget): the group skips hidden rows from here. */
    lv_obj_t * focused = lv_group_get_focused(s_group);
    if (focused && lv_obj_has_flag(lv_obj_get_parent(focused), LV_OBJ_FLAG_HIDDEN))
        lv_group_focus_prev(s_group);
}

// ---- scan list ----

static void password_prompt(const char * title, const char * initial);

static void password_done(bool ok, const char * text, void * user)
{
    (void)user;
    if (!ok) return;
    if (!wifi_pass_acceptable(text)) {
        /* A candidate of 1..7 characters can never join and would drop the
         * link that works: ask again, with what was typed and the reason. */
        char title[kTitleMax + sizeof(kShortPassNote)];
        snprintf(title, sizeof(title), "%s%s", kShortPassNote, s_picked.ssid);
        password_prompt(title, text);
        return;
    }
    wifi_link_connect_candidate(s_picked.ssid, text);
}

static void password_prompt(const char * title, const char * initial)
{
    /* Required: an empty password for a secured network would go to
     * connect_candidate(), which drops a working link and ends failed(auth). */
    crosspad_gui::ui_text_prompt(title, initial, crosspad_gui::UI_TEXT_PASSWORD | crosspad_gui::UI_TEXT_REQUIRED,
                                 crosspad::WifiNetworkList::kMaxPass, password_done, nullptr);
}

static void list_close(void)
{
    if (!s_list) return;
    crosspad_gui::ui_back_remove(s_list_back);
    lv_indev_restore_group(indev_encoderTop, s_list_prev);
    lv_group_delete_deferred(s_list_group);
    lv_obj_delete_async(s_list);
    s_list = nullptr;
    s_list_group = nullptr;
}

static bool list_back_cb(void * user)
{
    (void)user;
    list_close();
    return true;
}

static void list_close_clicked_cb(lv_event_t * e)
{
    (void)e;
    list_close();
}

static void ap_clicked_cb(lv_event_t * e)
{
    s_picked = s_shown[(size_t)(uintptr_t)lv_event_get_user_data(e)];
    list_close();
    if (s_picked.auth == WIFI_AUTH_OPEN) {   // an open network: no keyboard
        wifi_link_connect_candidate(s_picked.ssid, "");
        return;
    }
    char title[kTitleMax];
    snprintf(title, sizeof(title), "Password for %.*s", kSsidPrec, s_picked.ssid);
    password_prompt(title, "");
}

static lv_obj_t * list_button(const char * text)
{
    lv_obj_t * btn = lv_button_create(s_list);
    lv_obj_set_size(btn, LV_PCT(100), kListRowH);
    lv_obj_t * label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_center(label);
    lv_group_add_obj(s_list_group, btn);
    return btn;
}

static void list_open(int count)
{
    memcpy(s_shown, s_box.aps, (size_t)count * sizeof(wifi_ap_t));
    s_list = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_list, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_list, kListPad, 0);
    lv_obj_set_style_pad_row(s_list, kListPad, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * title = lv_label_create(s_list);
    lv_label_set_text(title, count ? "Networks" : "No networks found");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    s_list_prev = lv_indev_get_group(indev_encoderTop);
    s_list_group = lv_group_create();
    for (int i = 0; i < count; ++i) {
        char row[kRowTextMax];
        snprintf(row, sizeof(row), "%.*s   %d dBm  %s", kSsidPrec, s_shown[i].ssid, (int)s_shown[i].rssi,
                 wifi_link_auth_name(s_shown[i].auth));
        lv_obj_add_event_cb(list_button(row), ap_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    lv_obj_add_event_cb(list_button("Close"), list_close_clicked_cb, LV_EVENT_CLICKED, nullptr);
    lv_indev_set_group(indev_encoderTop, s_list_group);
    s_list_back = crosspad_gui::ui_back_push(list_back_cb, nullptr);
}

// ---- crosspad.app account ----

static void unlink_confirmed_cb(bool confirmed, void * user)
{
    (void)user;
    if (confirmed) cp_account_unlink();
}

static void account_cb(lv_event_t * e)
{
    (void)e;
    if (cp_account_linked()) {
        ui_modal_confirm("Unlink this CrossPad?", "Remote management from crosspad.app stops; nothing installed changes.",
                         "Unlink", "Cancel", unlink_confirmed_cb, nullptr, s_group);
        return;
    }
    char why[CPA_ERROR_LEN];
    /* busy: a pairing started elsewhere (ACCT_PAIR) is running -- show its code. */
    if (cp_account_pair_start(why, sizeof(why)) || strcmp(why, "busy") == 0) acct_pair_overlay_open();
    else if (strcmp(why, crosspad::wifiFailName(crosspad::WifiFail::NoMem)) == 0 && s_opts.offerNetworkMode)
        wifi_fallback_offer(NET_TAB_WIFI, s_group);
    else note("%s", fw_updates_error_text(why));
}

static void account_sync(void)
{
    const bool linked = cp_account_linked();
    char text[kStatusMax];
    if (linked) {
        char name[crosspad::kFwAccountNameMax];
        cp_account_name(name, sizeof(name));
        snprintf(text, sizeof(text), "Linked: %s", name);
    } else {
        snprintf(text, sizeof(text), "%s", cp_account_notice() ? "Unlinked from your account - link again" : "Not linked");
    }
    if (strcmp(lv_label_get_text(s_account_value), text) != 0) lv_label_set_text(s_account_value, text);
    const char * action = linked ? "Unlink this CrossPad" : "Link this CrossPad";
    if (strcmp(lv_label_get_text(s_account_action), action) != 0) lv_label_set_text(s_account_action, action);
    const char * button = linked ? "Unlink" : "Link";
    if (strcmp(lv_label_get_text(s_account_button), button) != 0) lv_label_set_text(s_account_button, button);
    const char * remote = cp_remote_state_name();
    if (strcmp(lv_label_get_text(s_remote_value), remote) != 0) lv_label_set_text(s_remote_value, remote);
}

// ---- rows and refresh ----

static void allowed_changed_cb(lv_event_t * e)
{
    (void)e;   // settings_row_switch() has stored and saved the value already
    wifi_link_enable_changed();
    lease_sync();   // network mode: the screen's own poll re-syncs its lease
}

static void remote_changed_cb(lv_event_t * e)
{
    (void)e;   // stored and saved by settings_row_switch(); cp_remote follows within a second
    auto * cfg = crosspad::CrosspadSettings::getInstance();
    if (cfg && cfg->wireless.enableBLE && cp_remote_wanted()) note("BLE MIDI is off while remote management runs");
}

static void scan_cb(lv_event_t * e)
{
    (void)e;
    if (start_job(scan_job, "wifi_scan", kScanStack, false)) note("Scanning...");
    else note("Busy, try again");
}

static void test_cb(lv_event_t * e)
{
    (void)e;
    if (start_job(test_job, "wifi_test", kTestStack, false)) note("Testing...");
    else note("Busy, try again");
}

static void take_result(void)
{
    const Job job = (Job)s_jobDone.exchange((uint8_t)Job::None);
    if (job == Job::None) return;
    const bool mine = s_box.gen == s_pageGen;   // a job for a page already gone: drop it
    s_jobBusy.store(false);
    if (!mine) return;
    switch (job) {
        case Job::Scan:
            /* Only a refused scan has its reason in wifi_link_fail(); the other
             * two leave it alone (wifi_link.h), so it would read "none". */
            if (s_box.count == WIFI_LINK_SCAN_TIMEOUT) note("Scan failed: no answer, try again");
            else if (s_box.count == WIFI_LINK_SCAN_FAILED) note("Scan failed: radio error, try again");
            else if (s_box.count < 0) note("Scan failed: %s", fail_text((crosspad::WifiFail)wifi_link_fail()));
            else { s_noteUntil = lv_tick_get(); list_open(s_box.count); }
            break;
        case Job::Forget:
            note(s_box.forgetOk ? "Forgot %s" : "%s was not saved", s_box.ssid);
            saved_sync();
            break;
        case Job::Test:
            if (s_box.pingResult >= kHttpReachableMin && s_box.pingResult < kHttpReachableEnd)
                note("Connection OK (%u ms)", (unsigned)s_box.pingMs);
            else if (s_box.pingResult > 0)
                note("crosspad.app answered %d", s_box.pingResult);
            else if (s_box.pingResult < 0)
                note("crosspad.app unreachable (%s)", esp_err_to_name((esp_err_t)-s_box.pingResult));
            else
                note("No connection: %s", fail_text((crosspad::WifiFail)wifi_link_fail()));
            break;
        case Job::None:
            break;
    }
}

static void refresh_cb(lv_timer_t * t)
{
    (void)t;
    take_result();
    offer_if_no_mem();
    /* A candidate that connected joined the saved list. */
    const uint8_t st = wifi_link_state();
    if (st == (uint8_t)crosspad::WifiLinkState::Up && s_lastState != st) saved_sync();
    s_lastState = st;
    char text[kStatusMax];
    status_text(text, sizeof(text));
    if (strcmp(lv_label_get_text(s_status_value), text) != 0) lv_label_set_text(s_status_value, text);
    account_sync();
}

/* The first row's delete is the page's: the category closed (Settings) -- a
 * network-mode tab is never deleted, it ends with a restart. */
static void page_deleted_cb(lv_event_t * e)
{
    (void)e;
    ++s_pageGen;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    list_close();
    /* Settings closed mid-pairing: the code screen and its pairing go too. */
    acct_pair_overlay_close();
    cp_account_pair_cancel();
    if (s_lease) { wifi_link_release(); s_lease = false; }
    s_status_value = nullptr;
    s_account_value = nullptr;
    s_account_action = nullptr;
    s_account_button = nullptr;
    s_remote_value = nullptr;
    s_saved_none = nullptr;
}

void wifi_page_build(lv_obj_t * parent, lv_group_t * group, const wifi_page_opts_t * opts)
{
    auto * cfg = crosspad::CrosspadSettings::getInstance();
    if (!cfg) return;
    s_opts = *opts;
    s_group = group;
    s_allowedSeen = false;
    s_lease = false;
    s_offered = false;
    s_lastState = wifi_link_state();

    lv_obj_t * allowed = crosspad_gui::settings_row_switch(parent, group, "WiFi allowed",
                                                           &cfg->wireless.enableWiFi, allowed_changed_cb);
    lv_obj_add_event_cb(allowed, page_deleted_cb, LV_EVENT_DELETE, nullptr);
    crosspad_gui::settings_row_switch(parent, group, "Remote management", &cfg->wireless.enableRemote,
                                      remote_changed_cb);
    lv_obj_t * row = crosspad_gui::settings_row_info(parent, "Status", "");
    s_status_value = lv_obj_get_child(row, 1);   // settings_row_info(): label, then value
    lv_obj_set_width(s_status_value, LV_PCT(kStatusValueWidthPct));
    lv_label_set_long_mode(s_status_value, LV_LABEL_LONG_DOT);
    crosspad_gui::settings_row_action(parent, group, "Find networks", "Scan", scan_cb);
    crosspad_gui::settings_row_action(parent, group, "Test connection", "Test", test_cb);
    crosspad_gui::settings_section_header(parent, "Saved networks");
    s_saved_none = crosspad_gui::settings_row_info(parent, "None yet", "");
    for (size_t i = 0; i < kMaxSaved; ++i) {
        s_saved[i][0] = '\0';
        s_saved_rows[i] = crosspad_gui::settings_row_action(parent, group, "", "...", saved_clicked_cb,
                                                            (void *)(uintptr_t)i);
    }
    saved_sync();
    crosspad_gui::settings_section_header(parent, "crosspad.app account");
    row = crosspad_gui::settings_row_info(parent, "Account", "");
    s_account_value = lv_obj_get_child(row, 1);   // settings_row_info(): label, then value
    lv_obj_set_width(s_account_value, LV_PCT(kStatusValueWidthPct));
    lv_label_set_long_mode(s_account_value, LV_LABEL_LONG_DOT);
    row = crosspad_gui::settings_row_action(parent, group, "Link this CrossPad", "Link", account_cb);
    s_account_action = lv_obj_get_child(row, 0);                         // settings_row_action(): label, then button
    s_account_button = lv_obj_get_child(lv_obj_get_child(row, 1), 0);
    row = crosspad_gui::settings_row_info(parent, "Remote", "");
    s_remote_value = lv_obj_get_child(row, 1);   // settings_row_info(): label, then value
    lv_obj_set_width(s_remote_value, LV_PCT(kStatusValueWidthPct));
    lv_label_set_long_mode(s_remote_value, LV_LABEL_LONG_DOT);

    lease_sync();
    cp_remote_retry_now();   // the user is looking at WiFi: a link failure is retried now, not in 30 min
    s_timer = lv_timer_create(refresh_cb, kRefreshMs, nullptr);
    refresh_cb(nullptr);
}

#endif // USE_LVGL
