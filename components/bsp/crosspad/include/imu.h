/*
 * SPDX-FileCopyrightText: 2025 Mateusz Czarnecki
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file imu.h
 * @brief BSP IMU (ST LSM6DS3TR-C) interface
 */

#pragma once

#include "esp_err.h"
#include "crosspad_s3.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief IMU data structure
 */
typedef struct {
    float accel_x;  ///< Acceleration X-axis (g)
    float accel_y;  ///< Acceleration Y-axis (g)
    float accel_z;  ///< Acceleration Z-axis (g)
    float gyro_x;   ///< Gyroscope X-axis (dps)
    float gyro_y;   ///< Gyroscope Y-axis (dps)
    float gyro_z;   ///< Gyroscope Z-axis (dps)
    float temp;     ///< Temperature (°C)
    float fused_pitch; ///< Fused pitch angle (degrees) - Kalman output
    float fused_roll;  ///< Fused roll angle (degrees) - Kalman output
} bsp_imu_data_t;

/**
 * @brief How the IMU task is being driven, for bench diagnostics
 */
typedef struct {
    bool drdy_line;          ///< reads are triggered by the gyro data-ready pulse on INT2
    uint32_t drdy_pulses;    ///< data-ready pulses seen by the ISR
    uint32_t samples;        ///< new gyro samples fused
    uint32_t read_failures;  ///< I2C reads that failed
    uint32_t tap_mutes;      ///< times something asked for taps to be ignored
    uint32_t taps_muted;     ///< knocks dropped because they fell in a muted window
    int32_t tap_mute_ms_left;///< what is left of the current mute, 0 when listening
} bsp_imu_stats_t;

/**
 * @brief Events the LSM6DS3TR-C recognises itself (bitmask)
 */
typedef enum {
    BSP_IMU_EVENT_SINGLE_TAP  = 1u << 0,
    BSP_IMU_EVENT_DOUBLE_TAP  = 1u << 1,
    BSP_IMU_EVENT_FREE_FALL   = 1u << 2,
    BSP_IMU_EVENT_WAKE_UP     = 1u << 3,   ///< motion over the wake-up threshold
    BSP_IMU_EVENT_ORIENTATION = 1u << 4,   ///< 6D orientation changed
    BSP_IMU_EVENT_STILL       = 1u << 5,   ///< no motion for ~10 s: gyro asleep until a wake-up
} bsp_imu_event_t;

/**
 * @brief Counters of recognised events since boot
 */
typedef struct {
    uint32_t single_tap;
    uint32_t double_tap;
    uint32_t free_fall;
    uint32_t wake_up;
    uint32_t orientation;
    uint32_t still_periods;   ///< times the part went still and slept its gyro
    bool still;               ///< the part is in its still (gyro asleep) state now
    uint8_t orientation_6d;   ///< D6D_SRC axes: bit5 ZH, 4 ZL, 3 YH, 2 YL, 1 XH, 0 XL
} bsp_imu_event_counts_t;

/**
 * @brief Called from the IMU task when the part reports events
 *
 * @param events bsp_imu_event_t bits
 * @param orientation_6d Latest D6D_SRC axes (see bsp_imu_event_counts_t)
 */
typedef void (*bsp_imu_event_cb_t)(uint32_t events, uint8_t orientation_6d, void *ctx);

/**
 * @brief Initialize IMU
 *
 * This function initializes the LSM6DS3TR-C IMU sensor on the I2C bus.
 * The I2C bus must be initialized first by calling bsp_i2c_init().
 * This function does NOT start the IMU task automatically.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if I2C is not initialized
 *      - ESP_FAIL if IMU initialization failed
 */
esp_err_t bsp_imu_init(void);

/**
 * @brief Start IMU task
 *
 * Starts a FreeRTOS task that reads the IMU in the background: on every gyro
 * data-ready pulse (52 Hz) where BSP_HAS_IMU_INT2, otherwise every
 * update_rate_ms. The data buffer is read with bsp_imu_get_data().
 *
 * @param update_rate_ms Poll period without a data-ready line (default: 20 ms)
 * @param task_priority Task priority (default: 5)
 * @param task_stack_size Task stack size in bytes (default: 4096)
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if IMU not initialized or task already running
 *      - ESP_FAIL on task creation failure
 */
esp_err_t bsp_imu_start_task(uint32_t update_rate_ms, uint32_t task_priority, uint32_t task_stack_size);

/**
 * @brief Stop IMU task
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if task is not running
 */
esp_err_t bsp_imu_stop_task(void);

/**
 * @brief Get latest IMU data (thread-safe)
 *
 * Returns the latest IMU data read by the background task.
 * This function is thread-safe and can be called from any context.
 *
 * @param[out] data Pointer to structure to store IMU data
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if data is NULL
 *      - ESP_ERR_INVALID_STATE if IMU task is not running
 */
esp_err_t bsp_imu_get_data(bsp_imu_data_t *data);

/**
 * @brief Read IMU data directly (blocking)
 *
 * This function directly reads data from the IMU sensor (blocking call).
 * Use bsp_imu_get_data() instead if you are using the IMU task.
 *
 * @param[out] data Pointer to structure to store IMU data
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if data is NULL
 *      - ESP_FAIL if read failed
 */
esp_err_t bsp_imu_read(bsp_imu_data_t *data);

/**
 * @brief Get IMU temperature (thread-safe)
 *
 * @param[out] temp Pointer to store temperature value
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if temp is NULL
 *      - ESP_FAIL if read failed
 */
esp_err_t bsp_imu_get_temperature(float *temp);

/**
 * @brief Check if IMU task is running
 *
 * @return true if task is running, false otherwise
 */
bool bsp_imu_is_task_running(void);

/**
 * @brief Counters of the IMU task (lock-free snapshot)
 *
 * @param[out] stats Filled with the current counters
 */
void bsp_imu_get_stats(bsp_imu_stats_t *stats);

/**
 * @brief Ignore knocks on the case for the next @p ms
 *
 * A pad struck with a finger shakes the case as hard as a knock on it, so the
 * board's own playing reads as taps. The pad input says when it happens; this
 * drops the knocks *before* they are paired, so a real tap arriving afterwards
 * never pairs with one the player made.
 */
void bsp_imu_mute_taps_for(uint32_t ms);

/**
 * @brief Event counters since boot (lock-free snapshot)
 */
void bsp_imu_get_events(bsp_imu_event_counts_t *counts);

/**
 * @brief Register the event callback (one; NULL removes it)
 *
 * Runs in the IMU task: keep it short and never block on the I2C bus.
 */
void bsp_imu_set_event_callback(bsp_imu_event_cb_t cb, void *ctx);

/**
 * @brief Raw register read from the LSM6DS3TR-C, for bench diagnostics
 *
 * Reading a latched source register (0x1B..0x1D, 0x53) clears it.
 */
esp_err_t bsp_imu_read_registers(uint8_t first, uint8_t *out, size_t len);

/**
 * @brief Raw register write to the LSM6DS3TR-C, for bench experiments
 *
 * Not persisted: the next reconfiguration (boot, recovery) writes the driver's
 * values back.
 */
esp_err_t bsp_imu_write_register(uint8_t reg_addr, uint8_t value);

/**
 * @brief Let the part sleep its gyro after ~2.5 s without motion (default off)
 *
 * While still the accel drops to 12.5 Hz, where taps are not recognised, and
 * pitch/roll/gyro stop updating until motion over the wake-up threshold. The
 * platform turns it on when the screen goes dark (LIGHT sleep) and off again on
 * wake, so a board in use keeps every tap.
 */
esp_err_t bsp_imu_set_inactivity(bool enable);

/**
 * @brief Set the tap and wake-up thresholds (at 2 g full scale)
 *
 * The wake-up threshold has to stay above the tap threshold or the part stops
 * reporting taps (they share the slope comparator). While the part is still it
 * uses a fixed low wake-up threshold instead, so motion wakes it.
 *
 * @param tap_ths 0..31, 1 LSB = 62.5 mg (default 3 = 187 mg)
 * @param wake_ths 0..63, 1 LSB = 31.25 mg (default 8 = 250 mg)
 */
esp_err_t bsp_imu_set_thresholds(uint8_t tap_ths, uint8_t wake_ths);

/**
 * @brief Deinitialize IMU
 *
 * Stops the IMU task if running and deinitializes the sensor.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_FAIL on error
 */
esp_err_t bsp_imu_deinit(void);

/**
 * @brief Tune Kalman filter parameters (angle + bias) for responsiveness
 *
 * @param q_angle Process noise variance for the angle state
 * @param q_bias Process noise variance for the gyro bias state
 * @param r_measure Measurement noise variance for accelerometer angle
 * @return ESP_OK on success
 */
esp_err_t bsp_imu_set_kalman_params(float q_angle, float q_bias, float r_measure);

/**
 * @brief Change the IMU task's poll period (used only without a data-ready line)
 *
 * @param update_rate_ms New poll period in milliseconds (min 1ms)
 * @return ESP_OK on success
 */
esp_err_t bsp_imu_set_update_rate(uint32_t update_rate_ms);

#ifdef __cplusplus
}
#endif
