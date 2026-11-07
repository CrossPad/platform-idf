#include "EspImu.hpp"
#include <cmath>

#ifdef BSP_BOARD_CROSSPAD_S3
#include "imu.h"
#endif

namespace crosspad {

#ifdef BSP_BOARD_CROSSPAD_S3

static constexpr float SHAKE_GYRO_THRESHOLD = 250.0f;   // dps
static constexpr float FREEFALL_ACCEL_THRESHOLD = 0.3f;  // g

bool EspImu::isAvailable() const {
    return bsp_imu_is_task_running();
}

bool EspImu::getData(ImuData& out) const {
    bsp_imu_data_t raw;
    if (bsp_imu_get_data(&raw) != ESP_OK) {
        return false;
    }
    out.accel_x = raw.accel_x;
    out.accel_y = raw.accel_y;
    out.accel_z = raw.accel_z;
    out.gyro_x = raw.gyro_x;
    out.gyro_y = raw.gyro_y;
    out.gyro_z = raw.gyro_z;
    out.temperature = raw.temp;
    out.pitch = raw.fused_pitch;
    out.roll = raw.fused_roll;
    return true;
}

Orientation EspImu::getOrientation() const {
    ImuData d;
    if (!getData(d)) return Orientation::Unknown;

    float abs_ax = fabsf(d.accel_x);
    float abs_ay = fabsf(d.accel_y);
    float abs_az = fabsf(d.accel_z);

    if (abs_az > abs_ax && abs_az > abs_ay) {
        return d.accel_z > 0 ? Orientation::FaceUp : Orientation::FaceDown;
    }
    if (abs_ax > abs_ay) {
        return d.accel_x > 0 ? Orientation::LandscapeRight : Orientation::LandscapeLeft;
    }
    return d.accel_y > 0 ? Orientation::PortraitUp : Orientation::PortraitDown;
}

bool EspImu::isShaking() const {
    ImuData d;
    if (!getData(d)) return false;
    float gyroMag = sqrtf(d.gyro_x * d.gyro_x + d.gyro_y * d.gyro_y + d.gyro_z * d.gyro_z);
    return gyroMag > SHAKE_GYRO_THRESHOLD;
}

bool EspImu::isFreeFall() const {
    ImuData d;
    if (!getData(d)) return false;
    float accelMag = sqrtf(d.accel_x * d.accel_x + d.accel_y * d.accel_y + d.accel_z * d.accel_z);
    return accelMag < FREEFALL_ACCEL_THRESHOLD;
}

#else // no IMU hardware on this board

bool EspImu::isAvailable() const { return false; }
bool EspImu::getData(ImuData&) const { return false; }
Orientation EspImu::getOrientation() const { return Orientation::Unknown; }
bool EspImu::isShaking() const { return false; }
bool EspImu::isFreeFall() const { return false; }

#endif // BSP_BOARD_CROSSPAD_S3

} // namespace crosspad
