/**
 * @file usb_serial_prompt.cpp
 * @brief USB serial guard prompt — see usb_serial_prompt.h.
 *
 * The CDC profile is the console, OTA and file plane, so a host's request
 * for it is confirmed here rather than honoured on arrival. The dialog is
 * crosspad-gui's ui_modal_confirm; the timeout closes it through
 * ui_modal_confirm_dismiss so every exit runs the same teardown.
 */
#include "usb_serial_prompt.h"
#include "esp_log.h"

static const char *TAG = "usb_serial_prompt";

#ifdef USE_LVGL

#include "lvgl.h"
#include "lv_async_safe.h"
#include "usb_config_manager.h"
#include "crosspad-gui/components/ui_modal.h"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "CrosspadPlatform.hpp"
#include "crosspad/status/CrosspadStatus.hpp"
#include <atomic>

extern crosspad::CrosspadStatus status;

static constexpr uint32_t kPromptTimeoutMs = 30000;

static std::atomic<bool> s_up{false};
static lv_obj_t *s_overlay = nullptr;
static lv_timer_t *s_timer = nullptr;

static lv_group_t *current_group()
{
    lv_indev_t *nav = crosspad_gui::getGuiPlatform().getNavigationEncoder();
    return nav ? lv_indev_get_group(nav) : nullptr;
}

static void switch_to_serial(void *)
{
    usb_config_manager_set_mode(USB_MODE_DEFAULT);
}

static void prompt_cb(bool allow, void *)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    s_overlay = nullptr;
    s_up = false;
    if (!allow) { ESP_LOGI(TAG, "USB serial request denied"); return; }
    ESP_LOGI(TAG, "USB serial request allowed");
    /* ui_modal_confirm restores the encoder group and deletes its overlay
     * after this returns; the re-enumeration waits for that. */
    lv_async_call(switch_to_serial, nullptr);
}

static void timeout_cb(lv_timer_t *)
{
    s_timer = nullptr;  // one-shot: LVGL deletes it after this returns
    ESP_LOGW(TAG, "USB serial request timed out");
    ui_modal_confirm_dismiss(s_overlay, false);
}

static void show(void *)
{
    if (!status.isGuiInitialised) {
        ESP_LOGW(TAG, "USB serial requested before the launcher is up; dropped");
        s_up = false;
        return;
    }
    s_overlay = ui_modal_confirm("USB serial mode?",
                                 "A computer asks for USB serial mode "
                                 "(files, updates, console). Allow only "
                                 "computers you trust.",
                                 "Allow", "Deny", prompt_cb, nullptr, current_group());
    if (!s_overlay) { s_up = false; return; }
    s_timer = lv_timer_create(timeout_cb, kPromptTimeoutMs, nullptr);
    lv_timer_set_repeat_count(s_timer, 1);
}

void usb_serial_prompt_request(void)
{
    if (s_up.exchange(true)) return;
    lv_async_call_locked(show, nullptr);
}

bool usb_serial_prompt_active(void) { return s_up.load(); }

#else  /* !USE_LVGL: nothing to confirm on, so nothing is granted */

void usb_serial_prompt_request(void)
{
    ESP_LOGW(TAG, "USB serial requested, no display to confirm on; dropped");
}

bool usb_serial_prompt_active(void) { return false; }

#endif
