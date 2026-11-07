#pragma once

#include "crosspad/stm32/IStm32RegisterBus.hpp"
#include "crosspad/stm32/reg_map.h"
#include "stm32_i2c.h"
#include "esp_err.h"

namespace crosspad {

/**
 * @brief ESP-IDF I2C register bus to the STM32 co-processor (PCB rev2).
 *
 * Thin wrapper around stm32_i2c_read / stm32_i2c_write — those handle the
 * 16-bit big-endian register address framing and reuse the shared BSP I2C
 * bus (codecs / touch / IMU keep working alongside).
 *
 * Requires stm32_i2c_init() to have run; on rev1 boards (no BSP_HAS_STM32_I2C)
 * stm32_i2c_* return ESP_ERR_NOT_SUPPORTED and every call here returns false.
 */
class EspStm32I2cBus : public IStm32RegisterBus {
public:
    bool writeReg(uint16_t reg, const uint8_t* data, size_t len) override {
        return stm32_i2c_write(reg, data, len) == ESP_OK;
    }

    bool readReg(uint16_t reg, uint8_t* dst, size_t len) override {
        return stm32_i2c_read(reg, dst, len) == ESP_OK;
    }
};

} // namespace crosspad
