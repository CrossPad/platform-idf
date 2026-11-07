#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MIDI Clock: follows the USB host's 24-PPQN clock, generates one from the
 * metronome's tempo when the host is silent, and sends every tick and
 * Start/Continue/Stop to BLE MIDI and to the STM. Never back to the host.
 * docs/subsystems/midi-clock.md */
void midi_clock_init(void);

/* USB MIDI realtime handlers (uartLoop). */
void midi_clock_host_tick(void);
void midi_clock_host_start(void);
void midi_clock_host_continue(void);
void midi_clock_host_stop(void);

/* "CLOCK: src=host|int bpm=… running=… ticks_in=… ticks_out=… last_gap_us=…
 *  max_jitter_us=… max_late_us=… restarts=…"; reset_stats clears gap, jitter and late after
 *  reading them. Returns snprintf's count. */
int midi_clock_status(char *buf, size_t len, bool reset_stats);

#ifdef __cplusplus
}
#endif
