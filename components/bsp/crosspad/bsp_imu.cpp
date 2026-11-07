/*
 * SPDX-FileCopyrightText: 2025 Mateusz Czarnecki
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Driver for the ST LSM6DS3TR-C (WHO_AM_I 0x6A) on the board I2C bus. Register
 * addresses and bit positions come from ST's lsm6ds3tr-c_reg.h
 * (github.com/STMicroelectronics/lsm6ds3tr-c-pid). The part filters; it has no
 * sensor fusion, so pitch and roll are a Kalman filter here. */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "imu.h"
#include "crosspad_s3.h"

static const char *TAG = "BSP_IMU";

namespace {

constexpr uint8_t kI2cAddress = 0x6B;
constexpr uint8_t kWhoAmI = 0x6A;

namespace reg {
constexpr uint8_t DRDY_PULSE_CFG_G = 0x0B;
constexpr uint8_t INT2_CTRL = 0x0E;
constexpr uint8_t WHO_AM_I = 0x0F;
constexpr uint8_t CTRL1_XL = 0x10;
constexpr uint8_t CTRL2_G = 0x11;
constexpr uint8_t CTRL3_C = 0x12;
constexpr uint8_t CTRL4_C = 0x13;
constexpr uint8_t CTRL6_C = 0x15;
constexpr uint8_t CTRL7_G = 0x16;
constexpr uint8_t CTRL8_XL = 0x17;
constexpr uint8_t WAKE_UP_SRC = 0x1B;
constexpr uint8_t OUT_TEMP_L = 0x20;
constexpr uint8_t TAP_CFG = 0x58;
constexpr uint8_t TAP_THS_6D = 0x59;
constexpr uint8_t INT_DUR2 = 0x5A;
constexpr uint8_t WAKE_UP_THS = 0x5B;
constexpr uint8_t WAKE_UP_DUR = 0x5C;
constexpr uint8_t FREE_FALL = 0x5D;
constexpr uint8_t MD2_CFG = 0x5F;
}  // namespace reg

constexpr uint8_t kCtrl3SwReset = 0x01;
constexpr uint8_t kStatusGda = 0x02;

/* Gyro at 52 Hz: its digital LPF (16.6 Hz) sits below the 25 Hz a 50 Hz consumer
 * resolves. Accel at 416 Hz, which tap recognition needs, with LPF2 low-latency
 * at ODR/50 (8.3 Hz) on the data path. No gyro HPF: the Kalman filter estimates
 * the bias. BDU, so a burst never mixes two samples.
 *
 * The part recognises taps, double taps, wake-up (motion over a threshold),
 * stillness, free fall and 6D orientation changes itself, latched until the source
 * registers are read. All of them and the gyro data-ready pulse share INT2;
 * every read of 0x1B.. clears the latches, so the line drops again. */
struct RegValue { uint8_t reg, val; };
/* The tap recogniser and the wake-up detector share the slope comparator, and a
 * wake-up threshold at or below the tap threshold silences taps altogether:
 * measured 17.09 with taps of the same strength, tap threshold 375 mg —
 * wake-up 62.5 mg and 250 mg: not one tap seen; 500 mg and 970 mg: every tap.
 * So the awake threshold sits above the tap's. While the part is still its
 * accel runs at 12.5 Hz, where tap recognition does not work anyway, so the
 * threshold drops to what should wake a board someone picks up. That state is
 * entered only when the screen goes dark (the platform enables it on
 * PWR_STATE_LIGHT); a board in use keeps its accel at 416 Hz so every tap
 * counts. */
constexpr uint8_t kTapThsDefault = 0x02;    // 2 x FS/32 = 125 mg
constexpr uint8_t kWakeThsAwake = 0x08;     // 8 x FS/64 = 250 mg
constexpr uint8_t kWakeThsStill = 0x02;     // 2 x FS/64 = 62.5 mg
/* SINGLE_DOUBLE_TAP left clear on purpose: the part's own double-tap machine
 * answered a hand tapping this case unpredictably -- doubles arriving in pairs,
 * singles where a double was meant, whatever DUR and QUIET were set to. Every
 * knock is reported as a single tap instead, and the two are paired below,
 * where the window is ours to name. */
constexpr uint8_t kTapSingleOnly = 0x00;
/* INTERRUPTS_ENABLE and the three tap axes, and deliberately no LIR: with the
 * sources latched, this task's read every ~19 ms reset the tap machine between
 * the two halves of a double tap -- 17.09 on hardware, double taps counted 0
 * against dozens of singles, and 4 of 4 the moment LIR went off. Nothing is
 * missed by not latching: the INT2 edge wakes the task, which reads the sources
 * about a millisecond later. */
constexpr uint8_t kTapCfg = 0x8E;
/* INACT_EN 10: after SLEEP_DUR without motion over the wake-up threshold the
 * gyro sleeps and the accel drops to 12.5 Hz; the next such motion restores
 * both and raises WU_IA. */
constexpr uint8_t kInactGyroSleep = 0x40;
constexpr RegValue kConfig[] = {
    {reg::CTRL3_C, 0x44},           // BDU, IF_INC
    {reg::CTRL1_XL, 0x61},          // ODR 416 Hz, 2 g, BW0_XL (400 Hz analog)
    {reg::CTRL2_G, 0x3C},           // ODR 52 Hz, 2000 dps
    {reg::CTRL4_C, 0x00},           // gyro LPF1 off the path
    {reg::CTRL6_C, 0x00},           // accel high-performance, FTYPE 00
    {reg::CTRL7_G, 0x00},           // gyro high-performance, HPF off
    {reg::CTRL8_XL, 0x81},          // LPF2_XL_EN, HPCF_XL 00 = low-latency ODR/50, LOW_PASS_ON_6D
    {reg::TAP_CFG, kTapCfg},        // INTERRUPTS_ENABLE, TAP_X/Y/Z_EN, LIR (inactivity: LIGHT sleep only)
    {reg::TAP_THS_6D, kTapThsDefault},  // 6D threshold 80 deg, tap threshold
    /* QUIET is the silence the tap machine wants before it will see another
     * knock; the shortest it has keeps a deliberate second knock from being
     * swallowed. DUR belongs to the part's own double-tap machine, which is
     * off. */
    {reg::INT_DUR2, 0xF7},          // DUR unused, QUIET 1 (10 ms), SHOCK 3 (58 ms)
    {reg::WAKE_UP_THS, kTapSingleOnly | kWakeThsAwake},
    {reg::WAKE_UP_DUR, 0x02},       // wake duration 1 sample, SLEEP_DUR 2 x 512/ODR = 2.5 s
    {reg::FREE_FALL, 0x33},         // FF_DUR 6 samples, FF_THS 312 mg
    {reg::DRDY_PULSE_CFG_G, 0x80},  // DRDY_PULSED
    {reg::INT2_CTRL, 0x02},         // INT2_DRDY_G
    {reg::MD2_CFG, 0xFC},           // INT2: inactivity state, single tap, wake-up, free fall, double tap, 6D
};

constexpr float kAccelGPerLsb = 0.061f / 1000.0f;   // 2 g
constexpr float kGyroDpsPerLsb = 70.0f / 1000.0f;   // 2000 dps
constexpr float kTempLsbPerDegC = 256.0f;
constexpr float RAD_TO_DEG = 57.29577951308232f;

/* WAKE_UP_SRC (0x1B) through OUTZ_H_A (0x2D) in one read. */
constexpr size_t kBurstBytes = 19;
constexpr size_t kWakeUpSrcAt = 0;
constexpr size_t kTapSrcAt = 1;
constexpr size_t kD6dSrcAt = 2;
constexpr uint8_t kWakeUpSrcFfIa = 0x20;
constexpr uint8_t kWakeUpSrcWuIa = 0x08;
constexpr uint8_t kWakeUpSrcSleepState = 0x10;
constexpr uint8_t kTapSrcSingle = 0x20;
constexpr uint8_t kD6dSrcIa = 0x40;
constexpr uint8_t kD6dSrcAxes = 0x3F;   // ZH ZL YH YL XH XL
constexpr size_t kStatusAt = 3;
constexpr size_t kTempAt = 5;
constexpr size_t kGyroAt = 7;
constexpr size_t kAccelAt = 13;

/* Without a data-ready line the task polls at this period; with one it only
 * wakes this late when a pulse was missed. */
constexpr uint32_t kDefaultPollMs = 20;
constexpr uint32_t kDrdyWatchdogMs = 100;
constexpr uint32_t kStillWatchdogMs = 1000;   // gyro asleep: no pulses, events still interrupt
constexpr uint32_t kBackoffMs = 1000;
constexpr uint32_t kFailuresBeforeBackoff = 10;
constexpr uint32_t kTaskStackBytes = 4096;
constexpr UBaseType_t kTaskPriority = 5;
constexpr BaseType_t kTaskCore = 0;

// Simple Kalman filter implementation for angle estimation (angle + bias)
class KalmanFilter {
public:
    void setAngle(float a) { angle = a; }

    static float wrap180(float a)
    {
        while (a > 180.0f) a -= 360.0f;
        while (a <= -180.0f) a += 360.0f;
        return a;
    }

    // angle from the accelerometer (deg), rate from the gyro (deg/s), dt (s).
    float getAngle(float newAngle, float newRate, float dt)
    {
        float rate = newRate - bias;
        angle += dt * rate;

        // Project the error covariance ahead.
        P[0][0] += dt * (dt * P[1][1] - P[0][1] - P[1][0] + Q_angle);
        P[0][1] -= dt * P[1][1];
        P[1][0] -= dt * P[1][1];
        P[1][1] += Q_bias * dt;

        float S = P[0][0] + R_measure;
        float K0 = P[0][0] / S;
        float K1 = P[1][0] / S;

        // Fold in the measurement. Angles are circular: roll sits at ±180° on a
        // board lying face up (the part is mounted upside down), where the
        // accelerometer's atan2 flips from +179 to -179 -- an innovation of
        // 358° that threw the estimate (and the gyro bias with it) across the
        // circle whenever the board rocked around flat. The shortest way round.
        float y = wrap180(newAngle - angle);
        angle = wrap180(angle + K0 * y);
        bias += K1 * y;

        float P00 = P[0][0];
        float P01 = P[0][1];
        float P10 = P[1][0];
        float P11 = P[1][1];

        P[0][0] = P00 - K0 * P00;
        P[0][1] = P01 - K0 * P01;
        P[1][0] = P10 - K1 * P00;
        P[1][1] = P11 - K1 * P01;

        return angle;
    }

    void setQangle(float v) { Q_angle = v; }
    void setQbias(float v) { Q_bias = v; }
    void setRmeasure(float v) { R_measure = v; }

private:
    // Responsive defaults: less lag, slightly more noise.
    float Q_angle = 0.02f;   // process noise variance for the angle
    float Q_bias = 0.003f;   // process noise variance for the gyro bias
    float R_measure = 0.02f; // measurement noise variance for accel angle
    float angle = 0.0f;
    float bias = 0.0f;
    float P[2][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}};
};

i2c_master_dev_handle_t s_dev = nullptr;
bool s_initialized = false;

KalmanFilter s_kalman_pitch;
KalmanFilter s_kalman_roll;
bool s_kalman_initialized = false;

SemaphoreHandle_t s_data_mutex = nullptr;
bsp_imu_data_t s_data = {};

TaskHandle_t s_task = nullptr;
SemaphoreHandle_t s_task_exited = nullptr;
volatile bool s_task_stop = false;
volatile bool s_task_running = false;
uint32_t s_poll_ms = kDefaultPollMs;

bool s_has_drdy_line = false;
volatile int64_t s_drdy_us = 0;   // when the last data-ready pulse arrived
volatile uint32_t s_drdy_pulses = 0;
uint32_t s_samples = 0;
uint32_t s_read_failures = 0;

bsp_imu_event_counts_t s_events = {};
uint8_t s_wake_ths_awake = kWakeThsAwake;
bsp_imu_event_cb_t s_event_cb = nullptr;
void *s_event_ctx = nullptr;

bool reg_write(uint8_t r, uint8_t v)
{
    const uint8_t buf[2] = {r, v};
    return s_dev && i2c_master_transmit(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(100)) == ESP_OK;
}

bool reg_read(uint8_t r, uint8_t *out, size_t len)
{
    return s_dev && i2c_master_transmit_receive(s_dev, &r, 1, out, len, pdMS_TO_TICKS(100)) == ESP_OK;
}

int16_t word_at(const uint8_t *b, size_t i)
{
    return (int16_t)(b[i] | (b[i + 1] << 8));
}

/* Reset to defaults, then the whole configuration: the boot path and the
 * recovery path must leave the part in the same state. */
bool configure(void)
{
    uint8_t id = 0;
    if (!reg_read(reg::WHO_AM_I, &id, 1)) return false;
    if (id != kWhoAmI) {
        ESP_LOGE(TAG, "WHO_AM_I 0x%02X, expected 0x%02X (LSM6DS3TR-C)", id, kWhoAmI);
        return false;
    }
    if (!reg_write(reg::CTRL3_C, kCtrl3SwReset)) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
    for (const auto &w : kConfig) {
        if (!reg_write(w.reg, w.val)) return false;
    }
    return true;
}

void IRAM_ATTR drdy_isr(void *)
{
    s_drdy_us = esp_timer_get_time();
    s_drdy_pulses = s_drdy_pulses + 1;
    BaseType_t woken = pdFALSE;
    if (s_task) xTaskNotifyFromISR(s_task, 0, eNoAction, &woken);
    if (woken) portYIELD_FROM_ISR();
}

/* The GPIO ISR service belongs to whoever installs it first (esp_lvgl_port does,
 * for the touch interrupt, in bsp_display_start()), and IDF logs an error both
 * for a second install and for adding a handler before any install. Called
 * after the display is up: the handler goes onto the existing service, and only
 * a board without touch gets a service installed here -- the one refused add on
 * that path is not logged. */
bool attach_drdy_line(void)
{
    const esp_log_level_t gpio_level = esp_log_level_get("gpio");
    esp_log_level_set("gpio", ESP_LOG_NONE);
    esp_err_t err = gpio_isr_handler_add(BSP_IMU_INT2_PIN, drdy_isr, nullptr);
    esp_log_level_set("gpio", gpio_level);
    if (err == ESP_ERR_INVALID_STATE) {
        err = gpio_install_isr_service(0);
        if (err == ESP_OK) err = gpio_isr_handler_add(BSP_IMU_INT2_PIN, drdy_isr, nullptr);
    }
    return err == ESP_OK;
}

/* Unlatched, a tap's source bit stands for longer than the ~19 ms between two
 * reads, so one knock on the case arrived as several events -- and a subscriber
 * that toggles something on a tap flipped it back and forth. One knock per
 * settle window; a second knock inside the pair window is the second half of a
 * double tap and replaces the single, so a gesture is one event either way. */
constexpr int64_t kTapSettleUs = 150000;
constexpr int64_t kTapPairUs = 500000;

/* Set by whoever knows the board is being played (the pad input). A knock that
 * falls in the window is dropped here, before the pairing, and the pairing is
 * reset with it -- otherwise the first real knock after a flurry of playing
 * pairs with one the player made, and what should have opened something
 * happens a tap too early. */
std::atomic<int64_t> s_tap_mute_until_us{0};
std::atomic<uint32_t> s_tap_mutes{0};
std::atomic<uint32_t> s_taps_muted{0};

/* The sources of the embedded functions, as the burst read them. */
void dispatch_events(const uint8_t *b)
{
    static int64_t s_last_tap_us = 0;      // last knock, settled
    static int64_t s_pending_tap_us = 0;   // knock that may still become a pair
    const int64_t now = esp_timer_get_time();

    uint32_t ev = 0;
    if (now < s_tap_mute_until_us.load()) {
        if ((b[kTapSrcAt] & kTapSrcSingle) && now - s_last_tap_us > kTapSettleUs) {
            ++s_taps_muted;
            s_last_tap_us = now;
        }
        s_pending_tap_us = 0;
    } else if ((b[kTapSrcAt] & kTapSrcSingle) && now - s_last_tap_us > kTapSettleUs) {
        if (s_pending_tap_us && now - s_pending_tap_us <= kTapPairUs) {
            ev |= BSP_IMU_EVENT_DOUBLE_TAP;
            ++s_events.double_tap;
            s_pending_tap_us = 0;
        } else {
            ev |= BSP_IMU_EVENT_SINGLE_TAP;
            ++s_events.single_tap;
            s_pending_tap_us = now;
        }
        s_last_tap_us = now;
    }
    if (b[kWakeUpSrcAt] & kWakeUpSrcFfIa) { ev |= BSP_IMU_EVENT_FREE_FALL; ++s_events.free_fall; }
    if (b[kWakeUpSrcAt] & kWakeUpSrcWuIa) { ev |= BSP_IMU_EVENT_WAKE_UP; ++s_events.wake_up; }
    const bool still = (b[kWakeUpSrcAt] & kWakeUpSrcSleepState) != 0;
    if (still != s_events.still) {
        s_events.still = still;
        reg_write(reg::WAKE_UP_THS, kTapSingleOnly | (still ? kWakeThsStill : s_wake_ths_awake));
        if (still) { ev |= BSP_IMU_EVENT_STILL; ++s_events.still_periods; }
    }
    if (b[kD6dSrcAt] & kD6dSrcIa) {
        ev |= BSP_IMU_EVENT_ORIENTATION;
        ++s_events.orientation;
        s_events.orientation_6d = b[kD6dSrcAt] & kD6dSrcAxes;
    }
    if (ev && s_event_cb) s_event_cb(ev, s_events.orientation_6d, s_event_ctx);
}

/* One burst, then Kalman on the gyro sample if it is new. dt is the gap between
 * the samples' own arrival times, so a missed pulse or a backoff is integrated
 * over the time it really took. */
bool read_and_fuse(int64_t &last_sample_us)
{
    uint8_t b[kBurstBytes];
    if (!reg_read(reg::WAKE_UP_SRC, b, sizeof(b))) return false;
    dispatch_events(b);
    if (!(b[kStatusAt] & kStatusGda)) return true;

    const int64_t sample_us = s_has_drdy_line ? s_drdy_us : esp_timer_get_time();
    const float dt = last_sample_us
        ? std::min(std::max((sample_us - last_sample_us) / 1e6f, 0.001f), 0.1f)
        : 1.0f / 52.0f;
    last_sample_us = sample_us;
    ++s_samples;

    const float temp = 25.0f + word_at(b, kTempAt) / kTempLsbPerDegC;
    const float gyro_x = word_at(b, kGyroAt) * kGyroDpsPerLsb;
    const float gyro_y = word_at(b, kGyroAt + 2) * kGyroDpsPerLsb;
    const float gyro_z = word_at(b, kGyroAt + 4) * kGyroDpsPerLsb;
    const float accel_x = word_at(b, kAccelAt) * kAccelGPerLsb;
    const float accel_y = word_at(b, kAccelAt + 2) * kAccelGPerLsb;
    const float accel_z = word_at(b, kAccelAt + 4) * kAccelGPerLsb;

    // Accelerometer-derived angles, in degrees.
    const float accel_roll = atan2f(accel_y, accel_z) * RAD_TO_DEG;
    const float accel_pitch = atan2f(-accel_x, sqrtf(accel_y * accel_y + accel_z * accel_z)) * RAD_TO_DEG;

    // Initialize Kalman filter with current accel-derived angles on first run to avoid startup settling
    if (!s_kalman_initialized) {
        s_kalman_roll.setAngle(accel_roll);
        s_kalman_pitch.setAngle(accel_pitch);
        s_kalman_initialized = true;
    }
    // The gyro measures body rates; roll and pitch are Euler angles, and their
    // rates are the body rates turned by the attitude (ZYX kinematics):
    //   roll'  = gx + tan(pitch) (gy sin(roll) + gz cos(roll))
    //   pitch' = gy cos(roll) - gz sin(roll)
    // Feeding gx and gy straight in is right only near roll 0. This part is
    // mounted upside down -- roll sits at 180 on a board lying face up -- so
    // gy drove the pitch backwards: rocked about that axis, the gyro pushed
    // the angle one way and the accelerometer pulled it back, and the attitude
    // jumped. tan is held off +-90 (pitch's own limit) to stay finite.
    static float s_roll = accel_roll, s_pitch = accel_pitch;   // the last fused attitude
    const float sr = sinf(s_roll / RAD_TO_DEG), cr = cosf(s_roll / RAD_TO_DEG);
    const float tp = tanf(std::min(std::max(s_pitch, -85.0f), 85.0f) / RAD_TO_DEG);
    const float roll_rate = gyro_x + tp * (gyro_y * sr + gyro_z * cr);
    const float pitch_rate = gyro_y * cr - gyro_z * sr;
    const float fused_roll = s_kalman_roll.getAngle(accel_roll, roll_rate, dt);
    const float fused_pitch = s_kalman_pitch.getAngle(accel_pitch, pitch_rate, dt);
    s_roll = fused_roll; s_pitch = fused_pitch;

    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_data = {accel_x, accel_y, accel_z, gyro_x, gyro_y, gyro_z, temp, fused_pitch, fused_roll};
        xSemaphoreGive(s_data_mutex);
    }
    return true;
}

void imu_task(void *)
{
    int64_t last_sample_us = 0;
    uint32_t failures = 0;
    TickType_t last_failure_log = 0;
    bool backed_off = false;

    while (!s_task_stop) {
        const uint32_t wait_ms = backed_off ? kBackoffMs
                               : !s_has_drdy_line ? s_poll_ms
                               : s_events.still ? kStillWatchdogMs : kDrdyWatchdogMs;
        xTaskNotifyWait(0, 0, nullptr, pdMS_TO_TICKS(wait_ms));
        if (s_task_stop) break;

        if (read_and_fuse(last_sample_us)) {
            if (failures) {
                ESP_LOGI(TAG, "IMU recovered after %lu failed reads", (unsigned long)failures);
                failures = 0;
                backed_off = false;
            }
            continue;
        }

        /* A dead or absent IMU fails every wake. First failure prints, then one
         * line a second with the count; after a second of them the task
         * reconfigures the part once a second until a read succeeds, which is
         * how a transient bus glitch stays transient. */
        ++failures;
        ++s_read_failures;
        const TickType_t now = xTaskGetTickCount();
        if (failures == 1 || (now - last_failure_log) >= pdMS_TO_TICKS(1000)) {
            ESP_LOGW(TAG, "Failed to read IMU (%lu in a row)", (unsigned long)failures);
            last_failure_log = now;
        }
        if (failures >= kFailuresBeforeBackoff) {
            if (!backed_off) {
                ESP_LOGW(TAG, "IMU unresponsive — reconfiguring once a second");
                backed_off = true;
            }
            configure();
        }
    }
    s_task_running = false;
    xSemaphoreGive(s_task_exited);
    vTaskSuspend(nullptr);
}

}  // namespace

extern "C" {

esp_err_t bsp_imu_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "IMU already initialized");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "Failed to initialize I2C");

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGE(TAG, "Failed to get I2C bus handle");
        return ESP_FAIL;
    }

    const i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = kI2cAddress,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags = {},
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_config, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add IMU device to I2C bus: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    if (!configure()) {
        ESP_LOGE(TAG, "LSM6DS3TR-C not answering or not configured");
        i2c_master_bus_rm_device(s_dev);
        s_dev = nullptr;
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "LSM6DS3TR-C initialized (gyro 52 Hz, accel 416 Hz LPF2 ODR/50, tap/6D/free-fall/wake-up on INT2)");
    return ESP_OK;
}

esp_err_t bsp_imu_start_task(uint32_t update_rate_ms, uint32_t task_priority, uint32_t task_stack_size)
{
    (void)task_priority;
    (void)task_stack_size;
    if (!s_initialized) {
        ESP_LOGE(TAG, "IMU not initialized, call bsp_imu_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task_running) {
        ESP_LOGW(TAG, "IMU task already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_data_mutex = xSemaphoreCreateMutex();
    s_task_exited = xSemaphoreCreateBinary();
    if (!s_data_mutex || !s_task_exited) {
        ESP_LOGE(TAG, "Failed to create IMU semaphores");
        return ESP_FAIL;
    }

    if (BSP_HAS_IMU_INT2) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << BSP_IMU_INT2_PIN,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_POSEDGE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "INT2 GPIO config failed");
    }
    s_poll_ms = update_rate_ms ? update_rate_ms : kDefaultPollMs;
    s_task_stop = false;
    s_task_running = true;
    /* PSRAM stack: the task does I2C and float arithmetic, never flash. */
    if (xTaskCreatePinnedToCoreWithCaps(imu_task, "imu", kTaskStackBytes, nullptr, kTaskPriority,
                                        &s_task, kTaskCore, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create IMU task");
        s_task_running = false;
        return ESP_FAIL;
    }

    s_has_drdy_line = BSP_HAS_IMU_INT2 && attach_drdy_line();
    xTaskNotify(s_task, 0, eNoAction);   // re-evaluate the wait with the line known
    ESP_LOGI(TAG, "IMU task started (%s)",
             s_has_drdy_line ? "gyro data-ready on INT2" : "polling");
    return ESP_OK;
}

esp_err_t bsp_imu_stop_task(void)
{
    if (!s_task_running || s_task == nullptr) {
        ESP_LOGW(TAG, "IMU task is not running");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_has_drdy_line) {
        gpio_isr_handler_remove(BSP_IMU_INT2_PIN);
        s_has_drdy_line = false;
    }
    s_task_stop = true;
    xTaskNotify(s_task, 0, eNoAction);
    xSemaphoreTake(s_task_exited, portMAX_DELAY);
    vTaskDeleteWithCaps(s_task);
    s_task = nullptr;

    vSemaphoreDelete(s_task_exited);
    s_task_exited = nullptr;
    vSemaphoreDelete(s_data_mutex);
    s_data_mutex = nullptr;

    ESP_LOGI(TAG, "IMU task stopped");
    return ESP_OK;
}

esp_err_t bsp_imu_get_data(bsp_imu_data_t *data)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_task_running) {
        ESP_LOGE(TAG, "IMU task is not running");
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return ESP_FAIL;
    }
    memcpy(data, &s_data, sizeof(bsp_imu_data_t));
    xSemaphoreGive(s_data_mutex);
    return ESP_OK;
}

void bsp_imu_get_stats(bsp_imu_stats_t *stats)
{
    stats->drdy_line = s_has_drdy_line;
    stats->drdy_pulses = s_drdy_pulses;
    stats->samples = s_samples;
    stats->read_failures = s_read_failures;
    stats->tap_mutes = s_tap_mutes.load();
    stats->taps_muted = s_taps_muted.load();
    const int64_t left = s_tap_mute_until_us.load() - esp_timer_get_time();
    stats->tap_mute_ms_left = left > 0 ? (int32_t)(left / 1000) : 0;
}

void bsp_imu_mute_taps_for(uint32_t ms)
{
    s_tap_mute_until_us.store(esp_timer_get_time() + (int64_t)ms * 1000);
    ++s_tap_mutes;
}

void bsp_imu_get_events(bsp_imu_event_counts_t *counts)
{
    *counts = s_events;
}

void bsp_imu_set_event_callback(bsp_imu_event_cb_t cb, void *ctx)
{
    s_event_ctx = ctx;
    s_event_cb = cb;
}

esp_err_t bsp_imu_read_registers(uint8_t first, uint8_t *out, size_t len)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return reg_read(first, out, len) ? ESP_OK : ESP_FAIL;
}

esp_err_t bsp_imu_write_register(uint8_t reg_addr, uint8_t value)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return reg_write(reg_addr, value) ? ESP_OK : ESP_FAIL;
}

esp_err_t bsp_imu_set_inactivity(bool enable)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!reg_write(reg::TAP_CFG, kTapCfg | (enable ? kInactGyroSleep : 0))) return ESP_FAIL;
    ESP_LOGI(TAG, "gyro sleep when still: %s", enable ? "on" : "off");
    return ESP_OK;
}

esp_err_t bsp_imu_set_thresholds(uint8_t tap_ths, uint8_t wake_ths)
{
    if (tap_ths > 31 || wake_ths > 63) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!reg_write(reg::TAP_THS_6D, tap_ths) ||
        !reg_write(reg::WAKE_UP_THS, kTapSingleOnly | wake_ths)) {
        return ESP_FAIL;
    }
    s_wake_ths_awake = wake_ths;
    ESP_LOGI(TAG, "thresholds: tap %u x FS/32, wake-up %u x FS/64%s", tap_ths, wake_ths,
             wake_ths <= tap_ths * 2 ? " (wake-up at or below the tap threshold silences taps)" : "");
    return ESP_OK;
}

bool bsp_imu_is_task_running(void)
{
    return s_task_running;
}

esp_err_t bsp_imu_read(bsp_imu_data_t *data)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "IMU not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t b[14];   // OUT_TEMP_L .. OUTZ_H_A
    if (!reg_read(reg::OUT_TEMP_L, b, sizeof(b))) {
        ESP_LOGE(TAG, "Failed to read IMU data");
        return ESP_FAIL;
    }
    data->temp = 25.0f + word_at(b, 0) / kTempLsbPerDegC;
    data->gyro_x = word_at(b, 2) * kGyroDpsPerLsb;
    data->gyro_y = word_at(b, 4) * kGyroDpsPerLsb;
    data->gyro_z = word_at(b, 6) * kGyroDpsPerLsb;
    data->accel_x = word_at(b, 8) * kAccelGPerLsb;
    data->accel_y = word_at(b, 10) * kAccelGPerLsb;
    data->accel_z = word_at(b, 12) * kAccelGPerLsb;
    return ESP_OK;
}

esp_err_t bsp_imu_get_temperature(float *temp)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "IMU not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (temp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t b[2];
    if (!reg_read(reg::OUT_TEMP_L, b, sizeof(b))) {
        ESP_LOGE(TAG, "Failed to read temperature");
        return ESP_FAIL;
    }
    *temp = 25.0f + word_at(b, 0) / kTempLsbPerDegC;
    return ESP_OK;
}

esp_err_t bsp_imu_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    if (s_task_running) {
        bsp_imu_stop_task();
    }
    ESP_LOGI(TAG, "Deinitializing IMU");
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = nullptr;
    }
    s_initialized = false;
    return ESP_OK;
}

esp_err_t bsp_imu_set_kalman_params(float q_angle, float q_bias, float r_measure)
{
    if (q_angle <= 0.0f || q_bias <= 0.0f || r_measure <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    s_kalman_pitch.setQangle(q_angle);
    s_kalman_pitch.setQbias(q_bias);
    s_kalman_pitch.setRmeasure(r_measure);
    s_kalman_roll.setQangle(q_angle);
    s_kalman_roll.setQbias(q_bias);
    s_kalman_roll.setRmeasure(r_measure);
    ESP_LOGI(TAG, "Kalman params updated: Q_angle=%.5f Q_bias=%.5f R_measure=%.5f", q_angle, q_bias, r_measure);
    return ESP_OK;
}

esp_err_t bsp_imu_set_update_rate(uint32_t update_rate_ms)
{
    if (update_rate_ms == 0) return ESP_ERR_INVALID_ARG;
    s_poll_ms = update_rate_ms;
    ESP_LOGI(TAG, "IMU poll period set to %lu ms", update_rate_ms);
    return ESP_OK;
}

}  // extern "C"
