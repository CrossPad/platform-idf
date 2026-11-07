#include "EspLedStrip.hpp"
#include "esp_log.h"

static const char* TAG = "EspLedStrip";

namespace crosspad {

EspLedStrip::EspLedStrip(const Config& config) : config_(config) {
    mutex_ = xSemaphoreCreateMutex();
}

EspLedStrip::~EspLedStrip() {
    if (strip_) {
        led_strip_del(strip_);
        strip_ = nullptr;
    }
    if (mutex_) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
}

void EspLedStrip::begin() {
    if (initialized_) return;

    if (config_.gpioPin < 0 || config_.ledCount == 0) {
        ESP_LOGI(TAG, "LED strip not available (GPIO=%d, count=%d), skipping",
                 config_.gpioPin, config_.ledCount);
        initialized_ = true;
        return;
    }

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = config_.gpioPin;
    strip_config.max_leds = config_.ledCount;
    strip_config.led_pixel_format = LED_PIXEL_FORMAT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;
    strip_config.flags.invert_out = false;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_config.resolution_hz = config_.rmtResolutionHz;
    rmt_config.mem_block_symbols = config_.useDma ? (config_.ledCount * 4) : 0;
    rmt_config.flags.with_dma = config_.useDma;

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED strip: %s", esp_err_to_name(err));
        return;
    }

    led_strip_clear(strip_);
    initialized_ = true;
    ESP_LOGI(TAG, "LED strip initialized: %d LEDs on GPIO %d (remap: %s)",
             config_.ledCount, config_.gpioPin,
             config_.padToLedMap ? "yes" : "no");
}

void EspLedStrip::setPixel(uint16_t idx, RgbColor color) {
    if (!strip_) return;

    // Apply pad->LED remap if configured
    uint16_t physIdx = idx;
    if (config_.padToLedMap && idx < config_.padCount) {
        physIdx = config_.padToLedMap[idx];
    }

    if (physIdx >= config_.ledCount) return;
    led_strip_set_pixel(strip_, physIdx, color.R, color.G, color.B);
}

void EspLedStrip::refresh() {
    if (!strip_) return;
    ++refreshes_;
    // The RMT transmission can fail (channel busy, queue full). Dropping that
    // on the floor means the pads silently keep whatever frame they had, and
    // the next repaint only happens when something else changes — which is
    // indistinguishable, from the outside, from "the LEDs are broken". So it
    // is retried once and counted (LED_STATE txfail=). Not logged: this runs
    // under the strip lock, and a line on UART0 (no driver) is a busy wait of
    // several ms that everyone else waiting for the strip would sit through.
    if (led_strip_refresh(strip_) == ESP_OK) return;
    if (led_strip_refresh(strip_) != ESP_OK) ++refreshFailures_;
}

uint16_t EspLedStrip::getPixelCount() const {
    return config_.ledCount;
}

void EspLedStrip::lock() {
    if (mutex_) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
    }
}

void EspLedStrip::unlock() {
    if (mutex_) {
        xSemaphoreGive(mutex_);
    }
}

} // namespace crosspad
