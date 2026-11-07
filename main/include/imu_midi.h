#pragma once

/**
 * @file imu_midi.h
 * @brief Platform glue between the IMU and crosspad-core's ImuMidiMapper.
 *
 * The mapping itself is portable and lives in core; this only feeds it. The
 * feeder task exists only while the feature is on — internal RAM is the scarce
 * resource on this board (BLE cannot start below 60 kB free), so a feature
 * nobody enabled must not hold a stack.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Apply the persisted settings and start feeding if they say enabled.
void imu_midi_init(void);

/// Turn the feed on or off at runtime. Does not persist; the caller decides.
void imu_midi_set_enabled(bool on);

bool imu_midi_enabled(void);

/// Messages the mapper has put on the wire since boot.
uint32_t imu_midi_sent(void);

/// Re-read the settings into the mapper (after a verb or the UI changed them).
void imu_midi_apply_settings(void);

#ifdef __cplusplus
}
#endif
