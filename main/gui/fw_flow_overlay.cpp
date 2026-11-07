/**
 * @file fw_flow_overlay.cpp
 * @brief Full-screen progress for "install this package and restart into it"
 *        and "restart into the other slot" — see fw_flow_overlay.h.
 *
 * One LVGL timer drives a small state machine; the fw_slots calls that touch
 * the card or the flash run on their own tasks (fw_install_start() needs a
 * dedicated stack for the package lookup alone, fw_switch_request() writes
 * flash and therefore needs an internal-RAM stack — the same findings that
 * shaped the CDC FW_INSTALL/FW_SWITCH handlers in hil_control.cpp). The
 * pads mirror the bar the way the OTA and STM32 overlays do.
 */

#ifdef USE_LVGL

#include "fw_flow_overlay.h"
#include "fw_slots.h"
#include "gui.h"
#include "lvgl.h"
#include "crosspad-gui/components/power_gesture.h"
#include "crosspad-gui/widgets/gui_helpers.h"
#include "CrosspadPlatform.hpp"
#include "crosspad/pad/PadAnimator.hpp"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <cstdio>
#include <cstring>

extern lv_indev_t * indev_encoderTop;

static const char * TAG = "fw_flow";

static constexpr uint32_t kPollMs         = 150;
static constexpr uint32_t kRestartDelayMs = 200;   // matches the CDC FW_SWITCH path
static constexpr uint32_t kResultHoldMs   = 3500;  // how long a verdict stays on screen
static constexpr size_t   kTaskStackBytes = 8192;

enum class Step : uint8_t {
    Idle,
    InstallStarting,   // fw_install_start() running on its task
    Installing,        // polling fw_install_status()
    Switching,         // fw_switch_request() running on its task
    Restarting,        // switch committed, esp_restart() imminent
    Result,            // a message is being shown, then the overlay closes
};

static Step           s_step = Step::Idle;
static lv_timer_t *   s_timer = nullptr;
static lv_obj_t *     s_overlay = nullptr;
static lv_obj_t *     s_title = nullptr;
static lv_obj_t *     s_bar = nullptr;
static lv_obj_t *     s_phase = nullptr;
static lv_group_t *   s_group = nullptr;        // empty: the encoder does nothing meanwhile
static lv_group_t *   s_priorGroup = nullptr;
static uint32_t       s_backToken = 0;
static uint32_t       s_resultSince = 0;

static char           s_package[FW_PKG_NAME_LEN] = { 0 };
static bool           s_allowUnknown = false;
static bool           s_clearRollback = false;
static bool           s_restartAfterInstall = false;
static char           s_targetLabel = '?';

// Written by the tasks, read by the timer.
static volatile bool  s_taskDone = false;
static volatile bool  s_taskOk = false;
static char           s_taskError[FW_INSTALL_ERROR_LEN] = { 0 };

// ── tasks ────────────────────────────────────────────────────────────────

static void install_task(void *) {
    const char * error = nullptr;
    s_taskOk = fw_install_start(s_package, s_allowUnknown, &error);
    if (!s_taskOk) strlcpy(s_taskError, (error && error[0]) ? error : "install refused", sizeof(s_taskError));
    s_taskDone = true;
    vTaskDeleteWithCaps(nullptr);
}

static void switch_task(void *) {
    if (s_clearRollback) {
        fw_slot_clear_rollback(s_targetLabel);
        s_clearRollback = false;
    }
    const char * error = nullptr;
    s_taskOk = fw_switch_request(&error);
    if (!s_taskOk) strlcpy(s_taskError, (error && error[0]) ? error : "refused", sizeof(s_taskError));
    s_taskDone = true;
    if (s_taskOk) {
        vTaskDelay(pdMS_TO_TICKS(kRestartDelayMs));
        esp_restart();
    }
    vTaskDeleteWithCaps(nullptr);
}

static bool start_task(TaskFunction_t fn, const char * name, uint32_t caps) {
    s_taskDone = false;
    s_taskOk = false;
    s_taskError[0] = '\0';
    return xTaskCreatePinnedToCoreWithCaps(fn, name, kTaskStackBytes, nullptr, 1, nullptr, 0, caps) == pdPASS;
}

// ── overlay ──────────────────────────────────────────────────────────────

static bool back_cb(void *) {
    return true;  // swallowed: nothing to go back to while the flash is being written
}

static void set_phase(const char * text, lv_color_t color) {
    if (!s_phase) return;
    lv_label_set_text(s_phase, text);
    lv_obj_set_style_text_color(s_phase, color, 0);
}

static void open_overlay(const char * title) {
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_center(s_overlay);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(s_overlay);
    lv_label_set_text(s_title, title);
    lv_obj_set_width(s_title, LV_PCT(90));
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_title, CROSSPAD_THEME_PRIMARY, 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_16, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t * warn = lv_label_create(s_overlay);
    lv_label_set_text(warn, "Do not power off.");
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

    s_phase = lv_label_create(s_overlay);
    lv_obj_set_width(s_phase, LV_PCT(90));
    lv_label_set_long_mode(s_phase, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_phase, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_phase, &lv_font_montserrat_14, 0);
    lv_obj_align(s_phase, LV_ALIGN_CENTER, 0, 40);
    set_phase("", lv_color_white());

    // An empty group on the encoder: the app's own buttons stay underneath
    // and must not answer an encoder press through the overlay.
    s_priorGroup = lv_indev_get_group(indev_encoderTop);
    s_group = lv_group_create();
    lv_indev_set_group(indev_encoderTop, s_group);
    s_backToken = crosspad_gui::ui_back_push(back_cb, nullptr);
}

static void close_overlay() {
    crosspad_gui::ui_back_remove(s_backToken);
    s_backToken = 0;
    lv_indev_restore_group(indev_encoderTop, s_priorGroup);
    s_priorGroup = nullptr;
    if (s_group) {
        lv_group_delete(s_group);
        s_group = nullptr;
    }
    if (s_overlay) lv_obj_delete(s_overlay);
    s_overlay = nullptr;
    s_title = nullptr;
    s_bar = nullptr;
    s_phase = nullptr;
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    s_step = Step::Idle;
}

static void show_result(const char * text, bool failed) {
    if (s_bar) lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    if (s_phase) lv_obj_align(s_phase, LV_ALIGN_CENTER, 0, 10);
    set_phase(text, failed ? lv_color_make(255, 80, 80) : lv_color_make(0, 220, 100));
    if (failed) crosspad::getPadAnimator().playError();
    else        crosspad::getPadAnimator().playSuccess();
    s_resultSince = lv_tick_get();
    s_step = Step::Result;
}

static void begin_switch() {
    char buf[40];
    snprintf(buf, sizeof(buf), "Restarting into %c...", s_targetLabel);
    set_phase(buf, lv_color_white());
    if (s_bar) lv_bar_set_value(s_bar, 100, LV_ANIM_OFF);
    if (!start_task(switch_task, "fw_switch_gui", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) {
        show_result("Could not start the switch", true);
        return;
    }
    s_step = Step::Switching;
}

static void poll_cb(lv_timer_t *) {
    char buf[FW_INSTALL_ERROR_LEN + 48];
    switch (s_step) {
    case Step::InstallStarting:
        if (!s_taskDone) return;
        if (!s_taskOk) {
            show_result(s_taskError, true);
            return;
        }
        s_step = Step::Installing;
        [[fallthrough]];
    case Step::Installing: {
        const FwInstallStatus st = fw_install_status();
        if (s_bar) lv_bar_set_value(s_bar, st.percent, LV_ANIM_OFF);
        crosspad::getPadAnimator().setProgress(st.percent / 100.0f);
        if (st.phase == FwInstallPhase::Verifying) {
            set_phase("Verifying...", lv_color_white());
        } else if (st.phase == FwInstallPhase::Writing) {
            snprintf(buf, sizeof(buf), "Writing to slot %c...  %u%%", s_targetLabel, st.percent);
            set_phase(buf, lv_color_white());
        } else if (st.phase == FwInstallPhase::Done) {
            if (s_restartAfterInstall) begin_switch();
            else show_result("Installed", false);
        } else {
            // Idle (refused synchronously) or Failed — either way, done trying.
            show_result(st.error[0] ? st.error : "Install failed", true);
        }
        return;
    }
    case Step::Switching:
        if (!s_taskDone) return;
        if (s_taskOk) {
            s_step = Step::Restarting;  // the task restarts the board itself
            return;
        }
        if (s_restartAfterInstall) {
            // The package is in the slot; only the restart was refused (the
            // running image has not confirmed itself yet, say). Switch later.
            snprintf(buf, sizeof(buf), "Installed to %c.\n%s\nUse Switch to %c later.",
                     s_targetLabel, s_taskError, s_targetLabel);
            show_result(buf, false);
        } else {
            show_result(s_taskError, true);
        }
        return;
    case Step::Result:
        if (lv_tick_elaps(s_resultSince) >= kResultHoldMs) close_overlay();
        return;
    case Step::Restarting:
    case Step::Idle:
        return;
    }
}

static bool begin(const char * title) {
    if (s_step != Step::Idle) return false;
    FwSlotInfo slots[2];
    fw_slots_get(slots);
    s_targetLabel = slots[slots[0].running ? 1 : 0].label;
    open_overlay(title);
    crosspad::getPadAnimator().startProgress();
    s_timer = lv_timer_create(poll_cb, kPollMs, nullptr);
    return true;
}

// ── public ───────────────────────────────────────────────────────────────

bool fw_flow_install_and_restart(const char * package, bool allowUnknownBuild) {
    if (s_step != Step::Idle) return false;
    strlcpy(s_package, package, sizeof(s_package));
    s_allowUnknown = allowUnknownBuild;
    s_restartAfterInstall = true;
    s_clearRollback = false;
    char title[FW_PKG_NAME_LEN + 16];
    snprintf(title, sizeof(title), "Installing %s", s_package);
    if (!begin(title)) return false;
    set_phase("Starting...", lv_color_white());
    if (!start_task(install_task, "fw_install_gui", MALLOC_CAP_SPIRAM)) {
        ESP_LOGW(TAG, "install: no task");
        show_result("Could not start the install", true);
        return true;  // the overlay is up and will close itself
    }
    s_step = Step::InstallStarting;
    return true;
}

bool fw_flow_switch_and_restart(bool clearRollback) {
    if (s_step != Step::Idle) return false;
    s_restartAfterInstall = false;
    s_clearRollback = clearRollback;
    if (!begin("Switching firmware")) return false;
    begin_switch();
    return true;
}

bool fw_flow_active(void) {
    return s_step != Step::Idle;
}

#endif // USE_LVGL
