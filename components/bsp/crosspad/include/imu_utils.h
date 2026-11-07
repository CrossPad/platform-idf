/*
 * SPDX-FileCopyrightText: 2025 Mateusz Czarnecki
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file imu_utils.h
 * @brief Utility functions for IMU data processing
 */

#pragma once

#include "imu.h"
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Calculate total acceleration magnitude
 *
 * @param data Pointer to IMU data structure
 * @return Total acceleration magnitude in g
 */
static inline float bsp_imu_get_accel_magnitude(const bsp_imu_data_t *data)
{
    return sqrtf(data->accel_x * data->accel_x + 
                 data->accel_y * data->accel_y + 
                 data->accel_z * data->accel_z);
}

/**
 * @brief Calculate total gyroscope magnitude
 *
 * @param data Pointer to IMU data structure
 * @return Total gyroscope magnitude in dps
 */
static inline float bsp_imu_get_gyro_magnitude(const bsp_imu_data_t *data)
{
    return sqrtf(data->gyro_x * data->gyro_x + 
                 data->gyro_y * data->gyro_y + 
                 data->gyro_z * data->gyro_z);
}

/**
 * @brief Calculate pitch angle from accelerometer data
 *
 * @param data Pointer to IMU data structure
 * @return Pitch angle in degrees
 */
static inline float bsp_imu_get_pitch(const bsp_imu_data_t *data)
{
    return atan2f(-data->accel_x, sqrtf(data->accel_y * data->accel_y + 
                                        data->accel_z * data->accel_z)) * 180.0f / M_PI;
}

/**
 * @brief Calculate roll angle from accelerometer data
 *
 * @param data Pointer to IMU data structure
 * @return Roll angle in degrees
 */
static inline float bsp_imu_get_roll(const bsp_imu_data_t *data)
{
    return atan2f(data->accel_y, data->accel_z) * 180.0f / M_PI;
}

/**
 * @brief Detect if device is in free fall
 *
 * @param data Pointer to IMU data structure
 * @param threshold Free fall threshold in g (typically 0.3 - 0.5)
 * @return true if in free fall, false otherwise
 */
static inline bool bsp_imu_is_free_fall(const bsp_imu_data_t *data, float threshold)
{
    float magnitude = bsp_imu_get_accel_magnitude(data);
    return (magnitude < threshold);
}

/**
 * @brief Detect if device is being shaken
 *
 * @param data Pointer to IMU data structure
 * @param threshold Shake threshold in dps (typically 100 - 200)
 * @return true if being shaken, false otherwise
 */
static inline bool bsp_imu_is_shaking(const bsp_imu_data_t *data, float threshold)
{
    float magnitude = bsp_imu_get_gyro_magnitude(data);
    return (magnitude > threshold);
}

/**
 * @brief Detect device orientation
 */
typedef enum {
    BSP_IMU_ORIENTATION_UNKNOWN = 0,
    BSP_IMU_ORIENTATION_FACE_UP,      ///< Display facing up
    BSP_IMU_ORIENTATION_FACE_DOWN,    ///< Display facing down
    BSP_IMU_ORIENTATION_UPRIGHT,      ///< Standing upright
    BSP_IMU_ORIENTATION_UPSIDE_DOWN,  ///< Upside down
    BSP_IMU_ORIENTATION_LEFT_SIDE,    ///< On left side
    BSP_IMU_ORIENTATION_RIGHT_SIDE,   ///< On right side
} bsp_imu_orientation_t;

/**
 * @brief Get device orientation
 *
 * @param data Pointer to IMU data structure
 * @return Device orientation
 */
static inline bsp_imu_orientation_t bsp_imu_get_orientation(const bsp_imu_data_t *data)
{
    // Find axis with highest absolute value
    float abs_x = fabsf(data->accel_x);
    float abs_y = fabsf(data->accel_y);
    float abs_z = fabsf(data->accel_z);
    
    if (abs_z > abs_x && abs_z > abs_y) {
        // Z axis is dominant
        return (data->accel_z > 0) ? BSP_IMU_ORIENTATION_FACE_UP : BSP_IMU_ORIENTATION_FACE_DOWN;
    } else if (abs_y > abs_x && abs_y > abs_z) {
        // Y axis is dominant
        return (data->accel_y > 0) ? BSP_IMU_ORIENTATION_UPRIGHT : BSP_IMU_ORIENTATION_UPSIDE_DOWN;
    } else if (abs_x > abs_y && abs_x > abs_z) {
        // X axis is dominant
        return (data->accel_x > 0) ? BSP_IMU_ORIENTATION_RIGHT_SIDE : BSP_IMU_ORIENTATION_LEFT_SIDE;
    }
    
    return BSP_IMU_ORIENTATION_UNKNOWN;
}

#ifdef __cplusplus
}
#endif
