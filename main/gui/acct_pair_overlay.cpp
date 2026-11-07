#ifdef USE_LVGL

#include "acct_pair_overlay.h"

#include <cstdio>
#include <cstring>

#include "esp_attr.h"

#include "cp_account.h"
#include "crosspad-gui/components/power_gesture.h"
#include "crosspad-gui/components/ui_toast.h"
#include "crosspad-gui/theme/crosspad_theme.h"
#include "crosspad-gui/widgets/gui_helpers.h"
#include "crosspad/fw/FwAccount.hpp"
#include "crosspad-gui/fw/fw_updates_view.h"

extern lv_indev_t * indev_encoderTop;

static constexpr uint32_t kRefreshMs = 250;
static constexpr uint32_t kSecondsPerMinute = 60;
static constexpr uint32_t kLinkedToastMs = 2500;
static constexpr size_t kCodeShown = sizeof("XXX XXX");
static constexpr size_t kCodeHalf = crosspad::kFwPairCodeLen / 2;
static constexpr size_t kLineMax = 96;
static constexpr int32_t kPad = 8;
static constexpr int32_t kButtonH = 32;
static constexpr int32_t kButtonPadHor = 12;
static constexpr int32_t kRadius = 6;
static constexpr int32_t kFocusRing = 2;

namespace {
struct Overlay {
    lv_obj_t * root;
    lv_obj_t * code;
    lv_obj_t * status;
    lv_obj_t * retry;
    lv_obj_t * cancel;
    lv_group_t * group;
    lv_group_t * prevGroup;
    lv_timer_t * timer;
    uint32_t backToken;
};
}  // namespace

/* PSRAM: nothing on this screen needs fast RAM. */
EXT_RAM_BSS_ATTR static Overlay s;
static bool s_openPending;   // an open waits for its LVGL cycle; a close cancels it
static bool s_stylesReady;
static lv_style_t s_title;
static lv_style_t s_code;
static lv_style_t s_text;
static lv_style_t s_button;
static lv_style_t s_buttonFocused;

/* Rebuilt at every open: the theme's colours can change in Settings. */
static void styles_init(void) {
    if (s_stylesReady) {
        lv_style_reset(&s_title);
        lv_style_reset(&s_code);
        lv_style_reset(&s_text);
        lv_style_reset(&s_button);
        lv_style_reset(&s_buttonFocused);
    }
    lv_style_init(&s_title);
    lv_style_set_text_font(&s_title, &lv_font_montserrat_16);
    lv_style_set_text_color(&s_title, CROSSPAD_THEME_PRIMARY);
    lv_style_init(&s_code);
    lv_style_set_text_font(&s_code, &lv_font_montserrat_24);
    lv_style_set_text_color(&s_code, CROSSPAD_THEME_TEXT);
    lv_style_init(&s_text);
    lv_style_set_text_font(&s_text, &lv_font_montserrat_14);
    lv_style_set_text_color(&s_text, CROSSPAD_THEME_TEXT);
    lv_style_set_text_align(&s_text, LV_TEXT_ALIGN_CENTER);
    lv_style_init(&s_button);
    lv_style_set_bg_color(&s_button, CROSSPAD_THEME_CARD_BG);
    lv_style_set_bg_opa(&s_button, LV_OPA_COVER);
    lv_style_set_radius(&s_button, kRadius);
    lv_style_set_pad_hor(&s_button, kButtonPadHor);
    lv_style_set_text_font(&s_button, &lv_font_montserrat_14);
    lv_style_set_text_color(&s_button, CROSSPAD_THEME_TEXT);
    lv_style_init(&s_buttonFocused);
    lv_style_set_outline_width(&s_buttonFocused, kFocusRing);
    lv_style_set_outline_pad(&s_buttonFocused, -kFocusRing);
    lv_style_set_outline_color(&s_buttonFocused, CROSSPAD_THEME_PRIMARY);
    s_stylesReady = true;
}

static void set_text(lv_obj_t * label, const char * text) {
    if (label && strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void show_retry(bool shown) {
    if (lv_obj_has_flag(s.retry, LV_OBJ_FLAG_HIDDEN) != shown) return;
    if (shown) {
        lv_obj_remove_flag(s.retry, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    // The press that started a new code left the encoder on it.
    if (lv_group_get_focused(s.group) == s.retry) lv_group_focus_obj(s.cancel);
    lv_obj_add_flag(s.retry, LV_OBJ_FLAG_HIDDEN);
}

/* The website's host as a person types it: the base without its scheme. */
static void refresh_cb(lv_timer_t *) {
    char line[kLineMax];
    switch (cp_account_pair_state()) {
        case CPA_STARTING:
            set_text(s.code, "...");
            set_text(s.status, "Asking for a code...");
            show_retry(false);
            break;
        case CPA_CODE: {
            char code[crosspad::kFwPairCodeLen + 1];
            cp_account_pair_code(code, sizeof code);
            char shown[kCodeShown];
            snprintf(shown, sizeof shown, "%.*s %.*s", static_cast<int>(kCodeHalf), code, static_cast<int>(kCodeHalf),
                     code + strnlen(code, kCodeHalf));   // read in two halves
            set_text(s.code, shown);
            const uint32_t left = cp_account_pair_left_s();
            snprintf(line, sizeof line, "Enter it at %s - valid %u:%02u", CROSSPAD_LINK_PAGE,
                     static_cast<unsigned>(left / kSecondsPerMinute), static_cast<unsigned>(left % kSecondsPerMinute));
            set_text(s.status, line);
            show_retry(false);
            break;
        }
        case CPA_LINKED: {
            char name[crosspad::kFwAccountNameMax];
            cp_account_name(name, sizeof name);
            snprintf(line, sizeof line, "Linked to %s", name);
            ui_toast_show(line, kLinkedToastMs, UI_TOAST_SUCCESS);
            acct_pair_overlay_close();
            return;
        }
        case CPA_EXPIRED:
            set_text(s.code, "------");
            set_text(s.status, "The code expired. New code tries again.");
            show_retry(true);
            break;
        case CPA_FAILED: {
            char err[CPA_ERROR_LEN];
            cp_account_pair_error(err, sizeof err);
            snprintf(line, sizeof line, "Could not link - %s", fw_updates_error_text(err));
            set_text(s.code, "------");
            set_text(s.status, line);
            show_retry(true);
            break;
        }
        case CPA_IDLE:
            acct_pair_overlay_close();   // cancelled elsewhere (ACCT_PAIR_CANCEL, Unlink)
            return;
    }
}

static void cancel_and_close(void) {
    cp_account_pair_cancel();
    acct_pair_overlay_close();
}

static void cancel_clicked_cb(lv_event_t *) {
    lv_async_call([](void *) { cancel_and_close(); }, nullptr);
}

static void retry_clicked_cb(lv_event_t *) {
    char why[CPA_ERROR_LEN];
    if (cp_account_pair_start(why, sizeof why)) {
        refresh_cb(nullptr);
        return;
    }
    char line[kLineMax];
    snprintf(line, sizeof line, "Could not link - %s", fw_updates_error_text(why));
    set_text(s.status, line);
}

static bool back_cb(void *) {
    cancel_and_close();
    return true;
}

static lv_obj_t * button(lv_obj_t * parent, const char * text, lv_event_cb_t cb) {
    lv_obj_t * b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &s_button, 0);
    lv_obj_add_style(b, &s_buttonFocused, LV_STATE_FOCUSED);
    lv_obj_set_size(b, LV_SIZE_CONTENT, kButtonH);
    lv_obj_t * l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_group_add_obj(s.group, b);
    return b;
}

static lv_obj_t * label(lv_obj_t * parent, const lv_style_t * style, const char * text) {
    lv_obj_t * l = lv_label_create(parent);
    lv_obj_add_style(l, style, 0);
    lv_label_set_text(l, text);
    return l;
}

static void build(void) {
    styles_init();
    s.root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s.root);
    lv_obj_set_size(s.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s.root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s.root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s.root, kPad, 0);
    lv_obj_set_style_pad_row(s.root, kPad, 0);
    lv_obj_set_flex_flow(s.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s.root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s.root, LV_OBJ_FLAG_CLICKABLE);   // touches stop here, not on the page below

    label(s.root, &s_title, "Link to crosspad.app");
    s.code = label(s.root, &s_code, "...");
    s.status = label(s.root, &s_text, "");
    lv_obj_set_width(s.status, LV_PCT(100));
    lv_label_set_long_mode(s.status, LV_LABEL_LONG_WRAP);

    lv_obj_t * row = lv_obj_create(s.root);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, kPad, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    s.retry = button(row, "New code", retry_clicked_cb);
    lv_obj_add_flag(s.retry, LV_OBJ_FLAG_HIDDEN);
    s.cancel = button(row, "Cancel", cancel_clicked_cb);
}

static void open_async(void *) {
    if (!s_openPending) return;   // the host closed before this cycle came
    s_openPending = false;
    if (s.root) acct_pair_overlay_close();
    s = Overlay{};
    s.prevGroup = lv_indev_get_group(indev_encoderTop);
    s.group = lv_group_create();
    build();
    lv_indev_set_group(indev_encoderTop, s.group);
    lv_group_focus_obj(s.cancel);
    s.backToken = crosspad_gui::ui_back_push(back_cb, nullptr);
    s.timer = lv_timer_create(refresh_cb, kRefreshMs, nullptr);
    refresh_cb(nullptr);
}

void acct_pair_overlay_open(void) {
    s_openPending = true;
    lv_async_call(open_async, nullptr);
}

bool acct_pair_overlay_is_open(void) { return s.root != nullptr; }

void acct_pair_overlay_close(void) {
    s_openPending = false;
    if (!s.root) return;
    if (s.timer) lv_timer_delete(s.timer);
    crosspad_gui::ui_back_remove(s.backToken);
    lv_indev_restore_group(indev_encoderTop, s.prevGroup);
    lv_group_delete_deferred(s.group);   // closed from its own button: the encoder read may still hold it
    lv_obj_add_flag(s.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(s.root);
    s = Overlay{};
}

#endif // USE_LVGL
