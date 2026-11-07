#pragma once

#include "crosspad/platform/II2cDevice.hpp"
#include "driver/i2c_master.h"

namespace crosspad {

/**
 * @brief II2cDevice bound to one slave on the shared BSP i2c_master bus.
 */
class EspI2cDevice : public II2cDevice {
public:
    bool init(i2c_master_bus_handle_t bus, uint8_t addr7, uint32_t sclHz = 400000) {
        i2c_device_config_t cfg = {};
        cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        cfg.device_address = addr7;
        cfg.scl_speed_hz = sclHz;
        return i2c_master_bus_add_device(bus, &cfg, &dev_) == ESP_OK;
    }

    bool write(const uint8_t* data, size_t len) override {
        return dev_ && i2c_master_transmit(dev_, data, len, 100) == ESP_OK;
    }

    bool readRegister(uint8_t reg, uint8_t* data, size_t len) override {
        return dev_ && i2c_master_transmit_receive(dev_, &reg, 1, data, len, 100) == ESP_OK;
    }

private:
    i2c_master_dev_handle_t dev_ = nullptr;
};

} // namespace crosspad
