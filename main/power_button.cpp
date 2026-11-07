#include "include/power_button.h"

#include <atomic>

#include "esp_log.h"
#include "include/lv_async_safe.h"
#include "include/twin_mirror.h"

#include "crosspad-gui/components/power_button_classifier.h"
#include "crosspad-gui/components/power_gesture.h"
#include "crosspad/status/CrosspadStatus.hpp"
#include "crosspad/stm32/reg_map.h"
#include "stm32_i2c.h"

extern crosspad::CrosspadStatus status;

static const char *TAG = "PWR_BTN";

/* WOKE is the STM's state from a wake out of soft-off (or its watchdog
 * resetting the ESP) until the next LIGHT round trip -- which can be the whole
 * session. The ESP booted fresh into it, so it is awake; the press that woke
 * it is the classifier's first sample, taken as the baseline. */
static bool awake(uint8_t pwrState)
{
    return pwrState == PWR_STATE_AWAKE || pwrState == PWR_STATE_WOKE;
}

/* Click / hold timing, the wake guard and the LIGHT-sleep cancel window are
 * crosspad-gui's (power_button_classifier.h); this file only samples the STM
 * and acts on what the classifier decides. */
static crosspad_gui::PowerButtonClassifier s_classifier;

/* Live state for a host mirror (power_button_mirror_pump): the raw button,
 * published by the 40 ms I2C job, read by the MIDI task. bit 7 pressed, bits
 * 6..0 a change counter. */
static std::atomic<uint8_t> s_mirror{0};

static void back_async(void *)
{
    twin_mirror_power(false);
    const crosspad_gui::BackResult r = crosspad_gui::powerBack();
    ESP_LOGI(TAG, "click -> back: %s", crosspad_gui::backResultName(r));
}

static void drawer_async(void *)
{
    twin_mirror_power(true);
    const bool acted = crosspad_gui::powerToggleQuickSettings();
    ESP_LOGI(TAG, "hold -> quick settings: %s", acted ? "toggled" : "unavailable");
}

static void dispatch(bool hold)
{
    lv_async_call_locked(hold ? drawer_async : back_async, nullptr);
}

void power_button_inject(bool hold)
{
    ESP_LOGI(TAG, "injected %s gesture", hold ? "hold" : "click");
    if (hold) s_classifier.clearLightSleepCancel();
    dispatch(hold);
}

void power_button_mirror_pump(void (*send)(const uint8_t *frame, size_t len))
{
    static uint8_t s_sent = 0;
    const uint8_t v = s_mirror.load(std::memory_order_acquire);
    if (v == s_sent || !send) return;
    s_sent = v;
    const uint8_t frame[] = { 0xF0, 0x7D, 0x1F, 0x06, (uint8_t)(v >> 7), 0xF7 };
    send(frame, sizeof frame);
}

void power_button_feed(bool pressed, uint8_t pwrState, uint32_t nowMs)
{
    {
        const uint8_t m = s_mirror.load(std::memory_order_relaxed);
        if ((bool)(m >> 7) != pressed)
            s_mirror.store((uint8_t)((pressed ? 0x80 : 0) | ((m + 1) & 0x7F)), std::memory_order_release);
    }

    crosspad_gui::PowerButtonClassifier::Sample sample;
    sample.pressed    = pressed;
    sample.awake      = awake(pwrState);
    sample.lightSleep = pwrState == PWR_STATE_LIGHT;
    sample.guiReady   = status.isGuiInitialised;
    sample.nowMs      = nowMs;
    const crosspad_gui::PowerButtonClassifier::Result r = s_classifier.feed(sample);

    if (r.cancelLightSleep) {
        ESP_LOGI(TAG, "LIGHT sleep followed our drawer hold; waking the STM back up");
        stm32_i2c_write_u8(REG_CMD_USER_ACTIVITY, 1);
    }
    if (r.gesture != crosspad_gui::PowerGesture::None) {
        dispatch(r.gesture == crosspad_gui::PowerGesture::Hold);
    }
}
