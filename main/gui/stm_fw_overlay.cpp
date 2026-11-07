/**
 * @file stm_fw_overlay.cpp
 * @brief STM32 firmware update prompt — the user decides, watches the pages
 *        land, and is told what to do when it fails.
 *
 * Dialogs are crosspad-gui's ui_modal_confirm; the progress box is a panel on
 * lv_layer_top(). The update runs in its own task (stm32_fw_update blocks
 * ~15 s and must never touch the LVGL thread) and every UI call goes through
 * lv_async_call_locked. The pads mirror it with the ESP OTA's progress bar.
 */

#ifdef USE_LVGL

#include "stm_fw_overlay.h"
#include "gui.h"
#include "lvgl.h"
#include "lv_async_safe.h"
#include "crosspad-gui/components/ui_modal.h"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "CrosspadPlatform.hpp"
#include "crosspad/pad/PadAnimator.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>

static const char * TAG = "stm_fw_overlay";

/* app_main has the verdict long before the launcher exists, and a modal
 * raised before main_screen swaps the encoder group loses focus — so the
 * prompt waits for stm_fw_overlay_on_main_screen(). */
static bool s_pending = false;
static bool s_main_screen_up = false;

static stm32_fw_update_status_t s_status;
static lv_obj_t * s_panel = nullptr;
static lv_obj_t * s_bar = nullptr;
static lv_obj_t * s_label = nullptr;

static void show_prompt(void *);

static lv_group_t * current_group() {
    lv_indev_t * nav = crosspad_gui::getGuiPlatform().getNavigationEncoder();
    return nav ? lv_indev_get_group(nav) : nullptr;
}

// ── progress panel ───────────────────────────────────────────────────────

static void close_panel() {
    if (s_panel) lv_obj_delete(s_panel);
    s_panel = nullptr;
    s_bar = nullptr;
    s_label = nullptr;
}

static void show_progress() {
    close_panel();
    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_panel, 320, 240);
    lv_obj_set_style_bg_color(s_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_center(s_panel);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(s_panel);
    char buf[48];
    snprintf(buf, sizeof(buf), "Flashing STM32 v%u.%u", s_status.bundled.major, s_status.bundled.minor);
    lv_label_set_text(title, buf);
    lv_obj_set_style_text_color(title, CROSSPAD_THEME_PRIMARY, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t * warn = lv_label_create(s_panel);
    lv_label_set_text(warn, "Pads are off. Do not power off.");
    lv_obj_set_style_text_color(warn, lv_color_white(), 0);
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_10, 0);
    lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, 65);

    s_bar = lv_bar_create(s_panel);
    lv_obj_set_size(s_bar, 240, 20);
    lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, 10);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, lv_color_make(40, 40, 40), 0);
    lv_obj_set_style_bg_color(s_bar, CROSSPAD_THEME_PRIMARY, LV_PART_INDICATOR);

    s_label = lv_label_create(s_panel);
    lv_label_set_text(s_label, "entering the flasher...");
    lv_obj_set_style_text_color(s_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_label, LV_ALIGN_CENTER, 0, 40);
}

struct ProgressData { uint16_t done, total; };

static void update_progress(void * param) {
    ProgressData * d = static_cast<ProgressData *>(param);
    if (s_bar && s_label) {
        lv_bar_set_value(s_bar, d->done * 100 / d->total, LV_ANIM_OFF);
        char buf[40];
        snprintf(buf, sizeof(buf), "page %u / %u", d->done, d->total);
        lv_label_set_text(s_label, buf);
    }
    delete d;
}

static void progress_cb(uint16_t done, uint16_t total, void *) {
    crosspad::getPadAnimator().setProgress((float)done / (float)total);
    lv_async_call_locked(update_progress, new ProgressData{done, total});
}

// ── results ──────────────────────────────────────────────────────────────

static void done_timer_cb(lv_timer_t *) { close_panel(); }

static void show_done(void *) {
    if (!s_label) return;
    stm32_link_info_t li = {};
    stm32_hal_get_link_info(&li);
    char buf[64];
    snprintf(buf, sizeof(buf), "Done: STM32 v%u.%u (proto 0x%04X)", li.fw_major, li.fw_minor, li.proto_version);
    if (s_bar) lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_label, buf);
    lv_obj_set_style_text_color(s_label, lv_color_make(0, 255, 0), 0);
    lv_obj_align(s_label, LV_ALIGN_CENTER, 0, 0);
    lv_timer_t * t = lv_timer_create(done_timer_cb, 3000, nullptr);
    lv_timer_set_repeat_count(t, 1);
}

static void failed_cb(bool retry, void *) {
    /* ui_modal_confirm restores the encoder group and deletes its overlay
     * after this returns, so a modal raised here loses focus. Queue it. */
    if (retry) lv_async_call(show_prompt, nullptr);
}

static void show_failed(void * param) {
    close_panel();
    esp_err_t err = (esp_err_t)(intptr_t)param;
    char body[260];
    snprintf(body, sizeof(body),
             "%s\n"
             "Pads still work: old firmware kept, retry any time.\n"
             "Pads dead after a power cycle: the STM32 waits in USB DFU — "
             "on a PC run CrossPad_STM32_r2/tools/dfu_flash.py.",
             esp_err_to_name(err));
    ui_modal_confirm("STM32 update failed", body, "Retry", "Close", failed_cb, nullptr, current_group());
}

// ── the update task ──────────────────────────────────────────────────────

static void update_task(void *) {
    esp_err_t err = stm32_fw_update(STM32_FW_BUNDLED_PATH, progress_cb, nullptr);
    if (err == ESP_OK) {
        crosspad::getPadAnimator().playSuccess();
        lv_async_call_locked(show_done, nullptr);
    } else {
        ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
        crosspad::getPadAnimator().playError();
        lv_async_call_locked(show_failed, (void *)(intptr_t)err);
    }
    vTaskDelete(nullptr);
}

// ── prompt ───────────────────────────────────────────────────────────────

static void prompt_cb(bool update, void *) {
    if (!update || s_status.battery_low || s_status.pcb_mismatch) return;
    show_progress();
    crosspad::getPadAnimator().startProgress();
    if (xTaskCreate(update_task, "stm_fw_upd", 6144, nullptr, 5, nullptr) != pdPASS) {
        crosspad::getPadAnimator().playError();
        show_failed((void *)(intptr_t)ESP_ERR_NO_MEM);
    }
}

static void show_prompt(void *) {
    const stm32_link_info_t &r = s_status.running;
    const stm32_fw_image_info_t &b = s_status.bundled;
    char body[240];
    if (s_status.pcb_mismatch) {
        snprintf(body, sizeof(body),
                 "Running: v%u.%u on PCB r%u\nBundled: v%u.%u built for PCB r%u\n"
                 "This firmware ships an STM32 image for another board. Not flashed. "
                 "Use a build for r%u, or FORCE over CDC if you know better.",
                 r.fw_major, r.fw_minor, r.pcb_rev, b.major, b.minor, b.pcb, r.pcb_rev);
        ui_modal_confirm("STM32 image mismatch", body, "OK", "Later", prompt_cb, nullptr, current_group());
        return;
    }
    snprintf(body, sizeof(body),
             "Running: v%u.%u (proto 0x%04X)\nBundled: v%u.%u (proto 0x%04X)\n%s",
             r.fw_major, r.fw_minor, r.proto_version, b.major, b.minor, b.proto,
             s_status.battery_low
                 ? "Battery under 20%, no charger: plug in and reboot to be asked again."
                 : "About 15 s, pads off meanwhile; the STM32 reboots at the end.");
    const char * title = b.proto != r.proto_version ? "STM32 update required"
                       : s_status.due               ? "STM32 update available"
                                                    : "Reflash STM32";
    if (s_status.battery_low) {
        ui_modal_confirm(title, body, "OK", "Later", prompt_cb, nullptr, current_group());
    } else {
        ui_modal_confirm(title, body, "Update", "Later", prompt_cb, nullptr, current_group());
    }
}

// ── public ───────────────────────────────────────────────────────────────

extern "C" void stm_fw_overlay_show(const stm32_fw_update_status_t * st) {
    s_status = *st;
    if (s_main_screen_up) {
        lv_async_call_locked(show_prompt, nullptr);
        ESP_LOGI(TAG, "STM32 update prompt queued");
    } else {
        s_pending = true;
        ESP_LOGI(TAG, "STM32 update prompt pending until the launcher is up");
    }
}

extern "C" void stm_fw_overlay_request(void) {
    stm32_fw_update_status_t st = {};
    if (!stm32_fw_update_check(STM32_FW_BUNDLED_PATH, &st) || !st.have_image) {
        ESP_LOGW(TAG, "update requested but the link is down or the bundle is missing");
        return;
    }
    s_status = st;
    lv_async_call_locked(show_prompt, nullptr);
}

/* LVGL thread, from LoadMainScreen once the launcher and its encoder group
 * exist. */
extern "C" void stm_fw_overlay_on_main_screen(void) {
    s_main_screen_up = true;
    if (!s_pending) return;
    s_pending = false;
    lv_async_call_locked(show_prompt, nullptr);   /* port lock is recursive: safe from the LVGL task too */
}

#endif // USE_LVGL
