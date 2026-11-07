#ifdef USE_LVGL

#include "net_mode_screen.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "acct_pair_overlay.h"
#include "cp_account.h"
#include "crosspad-gui/fw/fw_actions.h"
#include "crosspad-gui/fw/fw_kit_card.h"
#include "crosspad-gui/fw/fw_store_view.h"
#include "fw_ui_state.h"
#include "crosspad-gui/fw/fw_updates_view.h"
#include "crosspad-gui/fw/fw_views_common.h"
#include "net_mode.h"
#include "wifi_link.h"
#include "wifi_page.h"
#include "crosspad/net/WifiTypes.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad-gui/components/power_gesture.h"
#include "crosspad-gui/components/ui_wifi_bars.h"
#include "crosspad-gui/widgets/gui_helpers.h"

extern lv_group_t * encoderTop_group;
extern lv_indev_t * indev_encoderTop;

static const char * TAG = "net_screen";

/* The link is read through atomics (wifi_link.h); this timer is its reader on
 * the LVGL thread, for the header and for every tab. One poll is one step of
 * the bars' connecting sweep, so the period is the sweep's. */
static constexpr uint32_t kPollMs = crosspad::kWifiConnectStepMs;
static constexpr int32_t  kScreenPad = 4;
static constexpr int32_t  kHeaderHeight = 28;
static constexpr int32_t  kTabBarHeight = 28;
static constexpr int32_t  kExitBtnWidth = 72;
static constexpr size_t   kSsidLen = 32;   // the 802.11 limit
/* The longest headline is the connecting one with a full-length SSID. */
static constexpr size_t   kHeadlineMax = sizeof("Connecting to ...") + kSsidLen;

struct NetTab {
    net_tab_t id;
    const char * title;
    void (*build)(lv_obj_t * page, lv_group_t * group);
    void (*refresh)(void);   // every poll; may be null
    void (*shown)(void);     // the tab became the visible one; nullptr = nothing to do
};

/* The network-mode screen holds the lease itself and is where the fallback
 * would send the user, so the page does neither. */
static void build_wifi_tab(lv_obj_t * page, lv_group_t * group)
{
    static const wifi_page_opts_t kOpts = {false, false};
    wifi_page_build(page, group, &kOpts);
}

/* One row per tab. Each tab's controls have an encoder group of their own, so
 * the encoder never visits a page that is not shown; the tab bar and Exit move
 * to the shown tab's group, behind its controls. */
static const NetTab kTabs[] = {
    {NET_TAB_WIFI, "WiFi", build_wifi_tab, nullptr, nullptr},   // the page refreshes itself
    {NET_TAB_STORE, "Store", fw_store_view_build, fw_store_view_refresh, fw_store_view_shown},
    {NET_TAB_UPDATES, "Updates", fw_updates_view_build, fw_updates_view_refresh, fw_updates_view_shown},
};
static constexpr size_t kTabCount = sizeof(kTabs) / sizeof(kTabs[0]);

/* The tab bar's buttons and Exit, the encoder group's last members. */
static constexpr size_t kTailCount = kTabCount + 1;

static lv_obj_t * s_header;
static lv_obj_t * s_headline;
static lv_obj_t * s_bars;
static lv_timer_t * s_poll;
static lv_group_t * s_groups[kTabCount];
static lv_obj_t * s_tail[kTailCount];
static lv_obj_t * s_tabs;
static size_t s_shownTab;
static uint32_t s_backToken;
static bool s_lease;
static bool s_allowedSeen;

static bool wifi_allowed(void)
{
    auto * cfg = crosspad::CrosspadSettings::getInstance();
    return cfg && cfg->wireless.enableWiFi;
}

/* Holds the screen's lease exactly while WiFi is allowed. On a change only: a
 * refused lease (no_mem) is not retried every tick; switching WiFi off and on
 * again is the retry. The page's "WiFi allowed" switch reaches it through the
 * next poll. */
static void lease_sync(void)
{
    const bool allowed = wifi_allowed();
    if (allowed == s_allowedSeen) return;
    s_allowedSeen = allowed;
    if (allowed) {
        s_lease = wifi_link_acquire("netmode");
    } else if (s_lease) {
        wifi_link_release();
        s_lease = false;
    }
}

static void headline(char * out, size_t n)
{
    if (!wifi_allowed()) { snprintf(out, n, "WiFi off"); return; }
    char ssid[kSsidLen + 1];
    wifi_link_info(ssid, sizeof(ssid), nullptr, nullptr);
    switch ((crosspad::WifiLinkState)wifi_link_state()) {
        case crosspad::WifiLinkState::Up:
            snprintf(out, n, "%s", ssid);
            break;
        case crosspad::WifiLinkState::Starting:
        case crosspad::WifiLinkState::Connecting:
            snprintf(out, n, "Connecting to %s...", ssid[0] ? ssid : "WiFi");
            break;
        default:
            snprintf(out, n, "Not connected");
            break;
    }
}

/* A view that adds controls later (the Store's tiles when the catalog comes,
 * the WiFi page's rows) appends them to its group behind the tail; the tail
 * goes back to the end, the focus stays where it was. */
static void keep_tail_last(void)
{
    lv_group_t * g = s_groups[s_shownTab];
    if (!g) return;
    const uint32_t n = lv_group_get_obj_count(g);
    bool inOrder = n >= kTailCount;
    for (uint32_t k = 0; inOrder && k < kTailCount; ++k)
        inOrder = lv_group_get_obj_by_index(g, n - kTailCount + k) == s_tail[k];
    if (inOrder) return;
    lv_obj_t * focused = lv_group_get_focused(g);
    for (lv_obj_t * obj : s_tail) lv_group_add_obj(g, obj);   // out of wherever it is, onto the end
    if (focused) lv_group_focus_obj(focused);
}

static void poll_cb(lv_timer_t * t)
{
    (void)t;
    lease_sync();
    int8_t rssi = 0;
    wifi_link_info(nullptr, 0, &rssi, nullptr);
    crosspad_gui::ui_wifi_bars_update(s_bars, wifi_allowed(),
                                      (crosspad::WifiLinkState)wifi_link_state(), rssi, lv_tick_get());
    char text[kHeadlineMax];
    headline(text, sizeof(text));
    /* Compared first: setting the same text still invalidates the label. */
    if (strcmp(lv_label_get_text(s_headline), text) != 0) lv_label_set_text(s_headline, text);
    for (const NetTab & tab : kTabs) if (tab.refresh) tab.refresh();
    keep_tail_last();
}

/* The order is the Firmware app's teardown: an open confirm is answered "no"
 * first and hands the encoder back to the card it came from, the card then
 * gives it back to the screen's group, and only then do the pages go. */
void net_mode_screen_close(void)
{
    acct_pair_overlay_close();
    cp_account_pair_cancel();
    fw_actions_forget();
    fw_kit_card_close();
    fw_store_view_close();
    fw_updates_view_close();
}

/* Shared by the touch button, the encoder and the power-button click. */
static void exit_clicked_cb(lv_event_t * e)
{
    (void)e;
    net_mode_screen_close();
    net_mode_leave();
}

static bool exit_back_cb(void * user_data)
{
    (void)user_data;
    net_mode_screen_close();
    net_mode_leave();
    return true;
}

/* Nothing deletes the screen today (leaving is a restart); if something does,
 * nothing it held outlives it: the views, the poll that reads its labels, the
 * back handler, the lease, what FW_UI reports. */
static void root_deleted_cb(lv_event_t * e)
{
    (void)e;
    net_mode_screen_close();
    if (s_poll) lv_timer_delete(s_poll);
    s_poll = nullptr;
    crosspad_gui::ui_back_remove(s_backToken);
    s_backToken = 0;
    if (s_lease) wifi_link_release();
    s_lease = false;
    s_header = s_headline = s_bars = nullptr;
    s_tabs = nullptr;
    lv_indev_restore_group(indev_encoderTop, encoderTop_group);
    for (lv_group_t *& g : s_groups) {
        if (g) lv_group_delete(g);
        g = nullptr;
    }
    fw_ui_state_reset();
}

/* Tab `i` becomes the one the encoder walks: its controls, then the tab bar
 * and Exit. The focus goes to the page's first visible control, or stays on
 * the tab button when the page has none yet (the Store before its catalog). */
static void select_tab(size_t i)
{
    s_shownTab = i;
    lv_group_t * g = s_groups[i];
    for (lv_obj_t * obj : s_tail) lv_group_add_obj(g, obj);
    lv_indev_set_group(indev_encoderTop, g);
    lv_obj_t * page = lv_obj_get_child(lv_tabview_get_content(s_tabs), static_cast<int32_t>(i));
    if (!fw_views_focus_first(page, g)) lv_group_focus_obj(s_tail[i]);
    fw_ui_state_set_tab(net_tab_name(kTabs[i].id));   // the WiFi tab has no `shown`, and FW_UI still says "wifi"
    if (kTabs[i].shown) {
        kTabs[i].shown();
    } else {   // nothing of the tab left behind is on screen
        fw_ui_state_set_items(0);
        fw_ui_state_set_sel(-1, nullptr, nullptr);
        fw_ui_state_set_store(nullptr);
    }
}

/* The only way to another tab: the pages do not scroll (see below). LVGL 9.3
 * makes the tab active before its scroll and sends no VALUE_CHANGED for a
 * press; the tabview's own handler was added with the button and has run by
 * now. */
static void tab_button_clicked_cb(lv_event_t * e)
{
    (void)e;
    if (!s_tabs) return;
    const uint32_t i = lv_tabview_get_tab_active(s_tabs);
    if (i < kTabCount && i != s_shownTab) select_tab(i);
}

void net_mode_screen_create(lv_obj_t * parent)
{
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, kScreenPad, 0);
    lv_obj_set_style_pad_row(root, kScreenPad, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, root_deleted_cb, LV_EVENT_DELETE, nullptr);

    s_header = lv_obj_create(root);
    lv_obj_remove_style_all(s_header);
    lv_obj_set_size(s_header, LV_PCT(100), kHeaderHeight);
    lv_obj_set_flex_flow(s_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_header, kScreenPad, 0);
    lv_obj_remove_flag(s_header, LV_OBJ_FLAG_SCROLLABLE);

    s_bars = crosspad_gui::ui_wifi_bars_create(s_header);

    s_headline = lv_label_create(s_header);
    lv_label_set_text(s_headline, "");
    lv_label_set_long_mode(s_headline, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(s_headline, 1);
    lv_obj_set_style_text_font(s_headline, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_headline, lv_color_white(), 0);

    lv_obj_t * exit_btn = lv_button_create(s_header);
    lv_obj_set_size(exit_btn, kExitBtnWidth, kHeaderHeight);
    lv_obj_add_event_cb(exit_btn, exit_clicked_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t * exit_label = lv_label_create(exit_btn);
    lv_label_set_text(exit_label, "Exit");
    lv_obj_center(exit_label);

    s_tabs = lv_tabview_create(root);
    lv_tabview_set_tab_bar_size(s_tabs, kTabBarHeight);
    lv_obj_set_width(s_tabs, LV_PCT(100));
    lv_obj_set_flex_grow(s_tabs, 1);
    lv_obj_set_style_bg_color(s_tabs, lv_color_black(), 0);
    /* Tabs change by the tab bar only, as in the Firmware app. A swipe would
     * move to a page without moving the encoder there, and a control focused
     * on a page out of view would slide the pages over to it. */
    lv_obj_remove_flag(lv_tabview_get_content(s_tabs), LV_OBJ_FLAG_SCROLLABLE);

    fw_ui_state_set_host("net");
    size_t start = 0;
    for (size_t i = 0; i < kTabCount; ++i) {
        lv_obj_t * page = lv_tabview_add_tab(s_tabs, kTabs[i].title);
        lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(page, kScreenPad, 0);
        s_groups[i] = lv_group_create();
        kTabs[i].build(page, s_groups[i]);
        if (kTabs[i].id == net_mode_start_tab()) start = i;
    }
    if (kTabs[start].id != net_mode_start_tab()) {
        ESP_LOGW(TAG, "no %s tab yet, opening %s", net_tab_name(net_mode_start_tab()), kTabs[start].title);
    }
    lv_tabview_set_active(s_tabs, (uint32_t)start, LV_ANIM_OFF);

    lv_obj_t * bar = lv_tabview_get_tab_bar(s_tabs);
    for (uint32_t i = 0; i < kTabCount; ++i) {
        s_tail[i] = lv_obj_get_child(bar, static_cast<int32_t>(i));
        lv_obj_add_event_cb(s_tail[i], tab_button_clicked_cb, LV_EVENT_CLICKED, nullptr);
    }
    s_tail[kTabCount] = exit_btn;
    /* Update rows rebuilt while the Updates tab is shown go back in front of
     * the tail at once; the view moves only the tail members in its group. */
    fw_updates_view_set_tail(s_tail, kTailCount);
    select_tab(start);   // NET_MODE 1 TAB=updates opens with a check

    /* The bottom of the back stack: an open overlay or prompt -- the kit card
     * too, pushed later -- pops first. */
    s_backToken = crosspad_gui::ui_back_push(exit_back_cb, nullptr);

    s_poll = lv_timer_create(poll_cb, kPollMs, nullptr);
    poll_cb(nullptr);
}

#endif // USE_LVGL
