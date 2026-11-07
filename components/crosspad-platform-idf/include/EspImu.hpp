#pragma once
#include "crosspad/platform/IImu.hpp"

namespace crosspad {

/**
 * @brief ESP-IDF implementation of IImu wrapping bsp_imu_* functions
 */
class EspImu : public IImu {
public:
    bool isAvailable() const override;
    bool getData(ImuData& out) const override;
    Orientation getOrientation() const override;
    bool isShaking() const override;
    bool isFreeFall() const override;
};

} // namespace crosspad
