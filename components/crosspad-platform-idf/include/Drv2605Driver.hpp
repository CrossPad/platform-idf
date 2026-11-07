// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file Drv2605Driver.hpp
 * @brief DRV2605/DRV2605L haptic motor driver implementing IVibration.
 *
 * Communicates with the DRV2605 over I2C via the II2cDevice abstraction and
 * uses IClock for timing. Runs a FreeRTOS background task for event processing.
 * The PC simulator uses NullVibration instead.
 */

#include <cstdint>
#include <mutex>
#include "crosspad/platform/IVibration.hpp"
#include "crosspad/platform/VibrationPreset.hpp"
#include "crosspad/settings/VibrationSettings.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

namespace crosspad {

class II2cDevice;
class IClock;

class Drv2605Driver : public IVibration {
public:
    /**
     * @param i2c   I2C device bound to DRV2605 slave address (typically 0x5A)
     * @param clock Platform clock for timing
     */
    Drv2605Driver(II2cDevice& i2c, IClock& clock);
    ~Drv2605Driver();

    /**
     * @brief Probe the device, configure LRA motor, run auto-calibration.
     *
     * Blocks during calibration (up to ~5 s). Call once after I2C bus is ready.
     * @return true on success
     */
    bool initHardware();

    /**
     * @brief Start the background event-processing task (FreeRTOS).
     *
     * Must be called after initHardware() succeeds.
     * @param stackSize  Task stack in bytes (default 4096)
     * @param priority   FreeRTOS priority (default 5)
     * @param coreId     CPU core to pin the task to (default 1 = APP_CPU)
     * @return true if the task was created
     */
    bool startTask(uint32_t stackSize = 4096, uint8_t priority = 5, int coreId = 1);

    // ── IVibration ──────────────────────────────────────────────────────
    bool isAvailable() const override;
    void vibrate(VibrationPreset preset) override;
    void stop() override;
    void startAudioToVibe() override;
    void stopAudioToVibe() override;
    void applySettings(const VibrationSettings& settings) override;

private:
    // I2C register helpers
    void writeRegister(uint8_t reg, uint8_t value);
    uint8_t readRegister(uint8_t reg);

    // DRV2605 operations
    bool probeDevice();
    void setMode(uint8_t mode);
    void setWaveform(uint8_t slot, uint8_t effect);
    void go();
    void stopMotor();
    bool isGoBitSet();
    void useLRA();
    void selectLibrary(uint8_t lib);
    void enterStandby();

    // Background task
    static void taskEntry(void* param);
    void taskLoop();
    void handleVibrationEvent(VibrationPreset preset, const VibrationSettings& settings);
    void startAudioToVibeHw(uint8_t inputMin, uint8_t inputMax,
                            uint8_t outputMin, uint8_t outputMax);

    // Dependencies (injected)
    II2cDevice& i2c_;
    IClock& clock_;

    // State
    VibrationSettings settings_;
    std::mutex settingsMutex_;  // guards settings_ — written by applySettings() (GUI/settings
                                 // thread, e.g. on every slider drag event) and read by
                                 // taskLoop() (background task, ~every 10ms)
    TaskHandle_t taskHandle_;
    QueueHandle_t vibrationQueue_;
    bool deviceFound_;
    bool audioToVibeActive_;
    bool standbyMode_;
    int64_t lastVibrationTimeMs_;
};

} // namespace crosspad
