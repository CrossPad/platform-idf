#pragma once
/**
 * @file motion_stream.h
 * @brief The board's motion, live, for a page on the host (mucha's live stage).
 *
 * While a host keeps asking (companion verb "MOTION 1", repeated as a keepalive,
 * as TWIN does), the IMU goes out over USB MIDI as SysEx:
 *
 *   F0 7D 1F 0A 00 <t3 t2 t1 t0> <8 x (hi lo)> F7      a sample, ~50 Hz
 *       t   esp_timer milliseconds, 4 x 7 bit
 *       8 values, 14-bit two's-complement-free: v + 8192, hi then lo 7 bits:
 *         accel x y z   mg (the sensor's axes; ±2 g full scale, low-passed)
 *         gyro  x y z   dps x 4
 *         pitch, roll   degrees x 40 (the fused attitude)
 *   F0 7D 1F 0A 01 <t3 t2 t1 t0> <ev lo> <ev hi> <d6d> F7   what the IMU recognised
 *       ev  bsp_imu_event_t bits (taps, free fall, wake-up, orientation)
 *   F0 7D 1F 0A 02 <t3 t2 t1 t0> <phase> <percent> F7      an OTA over CDC (ota_cdc.h),
 *       on every change, at most 10 a second: 1 receiving, 2 written (restarting), 3 failed
 *
 * Off unless asked in the last few seconds: no host, no cost.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Companion keepalive ("MOTION 1" / "MOTION 0"). */
void motion_stream_enable(bool on);
bool motion_stream_active(void);

/** IMU task: events the part recognised (imu.h bsp_imu_event_t bits). */
void motion_stream_event(uint32_t events, uint8_t orientation_6d);

typedef enum { MOTION_OTA_RECEIVING = 1, MOTION_OTA_RESTARTING = 2, MOTION_OTA_FAILED = 3 } motion_ota_phase_t;

/** The OTA receiver (any task): where the transfer is. */
void motion_stream_ota(motion_ota_phase_t phase, unsigned percent);

/** MIDI task, every loop: a sample when one is due, and any events. */
void motion_stream_pump(void (*send)(const uint8_t *frame, size_t len));

#ifdef __cplusplus
}
#endif
