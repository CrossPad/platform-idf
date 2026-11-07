// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file Drv2605Driver.cpp
 * @brief DRV2605 haptic motor driver (FreeRTOS background task + queue).
 */

#include "Drv2605Driver.hpp"
#include "crosspad/platform/II2cDevice.hpp"
#include "crosspad/platform/IClock.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char* TAG = "Drv2605";

// ── DRV2605 register addresses ──────────────────────────────────────────

#define DRV2605_REG_STATUS      0x00
#define DRV2605_REG_MODE        0x01
#define DRV2605_REG_RTPIN       0x02
#define DRV2605_REG_LIBRARY     0x03
#define DRV2605_REG_WAVESEQ1    0x04
#define DRV2605_REG_GO          0x0C
#define DRV2605_REG_AUDIOIN_MIN 0x12
#define DRV2605_REG_AUDIOIN_MAX 0x13
#define DRV2605_REG_AUDIOOUT_MIN 0x14
#define DRV2605_REG_AUDIOOUT_MAX 0x15
#define DRV2605_REG_RATEDV      0x16
#define DRV2605_REG_CLAMPV      0x17
#define DRV2605_REG_CONTROL1    0x1B
#define DRV2605_REG_CONTROL2    0x1C
#define DRV2605_REG_CONTROL3    0x1D
#define DRV2605_REG_CONTROL4    0x1E

#define DRV2605_MODE_INTTRIG    0x00
#define DRV2605_MODE_AUDIOVIBE  0x04
#define DRV2605_MODE_AUTOCAL    0x07

namespace crosspad {

// ── Construction / Destruction ──────────────────────────────────────────

Drv2605Driver::Drv2605Driver(II2cDevice& i2c, IClock& clock)
    : i2c_(i2c)
    , clock_(clock)
    , taskHandle_(nullptr)
    , vibrationQueue_(nullptr)
    , deviceFound_(false)
    , audioToVibeActive_(false)
    , standbyMode_(true)
    , lastVibrationTimeMs_(0)
{
    vibrationQueue_ = xQueueCreate(8, sizeof(VibrationPreset));
    if (!vibrationQueue_) {
        ESP_LOGE(TAG, "Failed to create vibration queue");
    }
}

Drv2605Driver::~Drv2605Driver() {
    if (taskHandle_) {
        vTaskDelete(taskHandle_);
        taskHandle_ = nullptr;
    }
    if (vibrationQueue_) {
        vQueueDelete(vibrationQueue_);
        vibrationQueue_ = nullptr;
    }
}

// ── Hardware Initialization ─────────────────────────────────────────────

bool Drv2605Driver::initHardware() {
    if (!probeDevice()) {
        ESP_LOGE(TAG, "DRV2605 not found on I2C bus");
        return false;
    }

    deviceFound_ = true;
    ESP_LOGI(TAG, "DRV2605 found — configuring LRA");

    useLRA();
    selectLibrary(6);  // LRA library

    // Voltage rating & overdrive clamp
    writeRegister(DRV2605_REG_RATEDV, 0x3F);
    writeRegister(DRV2605_REG_CLAMPV, 0x89);

    // Control registers
    writeRegister(DRV2605_REG_CONTROL1, 0x20 | 0x93);  // AC coupling on
    writeRegister(DRV2605_REG_CONTROL2, 0xF5);
    writeRegister(DRV2605_REG_CONTROL3, 0xA3);          // LRA closed-loop + analog input
    writeRegister(DRV2605_REG_CONTROL4, 0x30);           // Auto-calibration time

    // Run auto-calibration
    setMode(DRV2605_MODE_AUTOCAL);
    go();

    int timeout = 100;  // ~5 s at 50 ms intervals
    while (isGoBitSet() && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
        timeout--;
    }

    if (timeout <= 0) {
        ESP_LOGW(TAG, "Auto-calibration timeout");
        enterStandby();
        return false;
    }

    // GO clearing only says the run ended. DIAG_RESULT says whether it worked,
    // and OC/OT say the actuator drew too much doing it; an "init success" on
    // a failed run left the periodic status check to find the same bits a
    // second later and report a fault mid-boot.
    const uint8_t status = readRegister(DRV2605_REG_STATUS);
    const bool diagFailed = (status >> 3) & 1;
    const bool fault = (status & 0x03) != 0;
    enterStandby();
    if (diagFailed || fault) {
        ESP_LOGW(TAG, "Auto-calibration failed (DIAG=%d OT=%d OC=%d) — haptics disabled",
                 (status >> 3) & 1, (status >> 1) & 1, status & 1);
        return false;
    }
    ESP_LOGI(TAG, "DRV2605 init success");
    return true;
}

bool Drv2605Driver::startTask(uint32_t stackSize, uint8_t priority, int coreId) {
    BaseType_t ret = xTaskCreatePinnedToCore(
        taskEntry, "Drv2605Task", stackSize, this, priority, &taskHandle_, coreId);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create background task");
        return false;
    }
    return true;
}

// ── IVibration Implementation ───────────────────────────────────────────

bool Drv2605Driver::isAvailable() const {
    return deviceFound_;
}

void Drv2605Driver::vibrate(VibrationPreset preset) {
    if (vibrationQueue_) {
        xQueueSend(vibrationQueue_, &preset, 0);
    }
}

void Drv2605Driver::stop() {
    enterStandby();
}

void Drv2605Driver::startAudioToVibe() {
    uint8_t inputMin, inputMax, outputMin, outputMax;
    {
        std::lock_guard<std::mutex> lock(settingsMutex_);
        inputMin = settings_.inputMin;
        inputMax = settings_.inputMax;
        outputMin = settings_.outputMin;
        outputMax = settings_.outputMax;
    }
    startAudioToVibeHw(inputMin, inputMax, outputMin, outputMax);
}

void Drv2605Driver::stopAudioToVibe() {
    enterStandby();
}

void Drv2605Driver::applySettings(const VibrationSettings& settings) {
    std::lock_guard<std::mutex> lock(settingsMutex_);
    settings_ = settings;
}

// ── I2C Register Helpers ────────────────────────────────────────────────

void Drv2605Driver::writeRegister(uint8_t reg, uint8_t value) {
    uint8_t data[2] = {reg, value};
    if (!i2c_.write(data, 2)) {
        ESP_LOGW(TAG, "I2C write error (reg 0x%02X)", reg);
    }
}

uint8_t Drv2605Driver::readRegister(uint8_t reg) {
    uint8_t value = 0;
    if (!i2c_.readRegister(reg, &value, 1)) {
        ESP_LOGW(TAG, "I2C read error (reg 0x%02X)", reg);
    }
    return value;
}

// ── DRV2605 Operations ──────────────────────────────────────────────────

bool Drv2605Driver::probeDevice() {
    uint8_t id = readRegister(DRV2605_REG_STATUS);
    uint8_t devId = id >> 5;
    return (devId == 3 || devId == 7);  // DRV2605/L or DRV2604/L
}

void Drv2605Driver::setMode(uint8_t mode) {
    writeRegister(DRV2605_REG_MODE, mode);
    standbyMode_ = false;
}

void Drv2605Driver::setWaveform(uint8_t slot, uint8_t effect) {
    writeRegister(DRV2605_REG_WAVESEQ1 + slot, effect);
}

void Drv2605Driver::go() {
    writeRegister(DRV2605_REG_GO, 1);
}

void Drv2605Driver::stopMotor() {
    setMode(DRV2605_MODE_INTTRIG);
    writeRegister(DRV2605_REG_GO, 0);
}

bool Drv2605Driver::isGoBitSet() {
    return (readRegister(DRV2605_REG_GO) & 0x01) != 0;
}

void Drv2605Driver::useLRA() {
    uint8_t val = readRegister(DRV2605_REG_CONTROL3);
    writeRegister(DRV2605_REG_CONTROL3, val | 0x80);
}

void Drv2605Driver::selectLibrary(uint8_t lib) {
    writeRegister(DRV2605_REG_LIBRARY, lib);
}

void Drv2605Driver::enterStandby() {
    uint8_t mode = readRegister(DRV2605_REG_MODE);
    writeRegister(DRV2605_REG_MODE, mode | 0x40);  // Set STANDBY bit
    standbyMode_ = true;

    bool debugging;
    {
        std::lock_guard<std::mutex> lock(settingsMutex_);
        debugging = settings_.debugging;
    }
    if (debugging) {
        ESP_LOGI(TAG, "Entering standby");
    }
}

void Drv2605Driver::startAudioToVibeHw(uint8_t inputMin, uint8_t inputMax,
                                        uint8_t outputMin, uint8_t outputMax) {
    writeRegister(DRV2605_REG_AUDIOIN_MIN, inputMin);
    writeRegister(DRV2605_REG_AUDIOIN_MAX, inputMax);
    writeRegister(DRV2605_REG_AUDIOOUT_MIN, outputMin);
    writeRegister(DRV2605_REG_AUDIOOUT_MAX, outputMax);
    setMode(DRV2605_MODE_AUDIOVIBE);
    audioToVibeActive_ = true;

    bool debugging;
    {
        std::lock_guard<std::mutex> lock(settingsMutex_);
        debugging = settings_.debugging;
    }
    if (debugging) {
        ESP_LOGI(TAG, "Audio-to-vibe: in[%d..%d] out[%d..%d]",
                 inputMin, inputMax, outputMin, outputMax);
    }
}

// ── Background Task ─────────────────────────────────────────────────────

void Drv2605Driver::taskEntry(void* param) {
    static_cast<Drv2605Driver*>(param)->taskLoop();
    vTaskDelete(nullptr);
}

void Drv2605Driver::taskLoop() {
    int64_t checkTimerMs = clock_.getTimeUs() / 1000;
    VibrationPreset event;

    for (;;) {
        int64_t nowMs = clock_.getTimeUs() / 1000;

        // Snapshot settings once per iteration — applySettings() can be called
        // concurrently (e.g. on every settings-UI slider drag event) from a
        // different thread, so reading settings_.* field-by-field here would
        // race against that assignment.
        VibrationSettings s;
        {
            std::lock_guard<std::mutex> lock(settingsMutex_);
            s = settings_;
        }

        // Disabled — sleep and wait
        if (!s.enable) {
            if (!standbyMode_) {
                enterStandby();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Wake from standby if enabled
        if (standbyMode_) {
            setMode(DRV2605_MODE_INTTRIG);
        }

        // Process queued haptic events
        if (xQueueReceive(vibrationQueue_, &event, 0) == pdTRUE) {
            handleVibrationEvent(event, s);
            nowMs = clock_.getTimeUs() / 1000;
        }

        // Auto-enter audio-to-vibe if configured and idle for > 1 s
        if (s.enableAudioToVibe && !audioToVibeActive_) {
            if (nowMs - lastVibrationTimeMs_ > 1000) {
                startAudioToVibeHw(s.inputMin, s.inputMax,
                                   s.outputMin, s.outputMax);
            }
        }

        // Periodic status check (every ~1 s)
        if (nowMs - checkTimerMs > 1000) {
            uint8_t status = readRegister(DRV2605_REG_STATUS);

            if (s.debugging) {
                ESP_LOGI(TAG, "Status: DEV_ID=%d DIAG=%d FB=%d OT=%d OC=%d",
                         status >> 5, (status >> 3) & 1, (status >> 2) & 1,
                         (status >> 1) & 1, status & 1);
            }

            if (status & 0x03) {
                // Over-temperature or over-current — shut down
                enterStandby();
                {
                    std::lock_guard<std::mutex> lock(settingsMutex_);
                    settings_.enable = false;
                }
                ESP_LOGE(TAG, "Fault detected — vibration disabled");
            }
            checkTimerMs = nowMs;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void Drv2605Driver::handleVibrationEvent(VibrationPreset preset, const VibrationSettings& settings) {
    audioToVibeActive_ = false;
    setMode(DRV2605_MODE_INTTRIG);

    if (!settings.enableVibrationOnTouch) {
        return;
    }

    if (settings.debugging) {
        ESP_LOGW(TAG, "Haptic event: %d", static_cast<int>(preset));
    }

    switch (preset) {
    case VibrationPreset::TouchPressed:
        stopMotor();
        setWaveform(1, 6);   // Sharp tick 100%
        setWaveform(2, 0);   // End
        go();
        vTaskDelay(pdMS_TO_TICKS(10));
        break;

    case VibrationPreset::TouchReleased:
        stopMotor();
        setWaveform(1, 7);   // Soft bump 100%
        setWaveform(2, 0);   // End
        go();
        vTaskDelay(pdMS_TO_TICKS(10));
        break;

    default:
        stopMotor();
        setWaveform(1, 47);  // Buzz 2 60%
        setWaveform(2, 50);
        setWaveform(3, 42);
        setWaveform(4, 0);   // End
        go();
        vTaskDelay(pdMS_TO_TICKS(10));
        break;
    }

    lastVibrationTimeMs_ = clock_.getTimeUs() / 1000;
}

} // namespace crosspad
