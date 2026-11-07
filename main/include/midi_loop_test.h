#pragma once
/* MIDI loop test, the ESP's end: a numbered note stream sent to the STM (the
 * ESP -> STM -> TRS OUT path) and a counter of the same stream coming back
 * from the STM (TRS IN -> STM -> ESP), checked in order. The host sends and
 * reads the identical stream on the STM's USB MIDI port, so every leg of the
 * loop is measured with one encoding.
 *
 * A message is a note-on on channel 1, note = seq % 32, velocity =
 * 1 + (seq / 32) % 127: below the pads' notes (36.. at the default offset),
 * never a note-off, and the sequence repeats every 4064 messages. While
 * capture is on, such notes from the STM are counted here and go no further
 * (no pad, no sampler). */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UART MIDI task: true when the note was a test message and is consumed. */
bool midi_loop_take(uint8_t channel_1_16, uint8_t note, uint8_t velocity);

/* Capture on clears its counters. */
void midi_loop_capture(bool on);

/* Send `count` messages at `rate` per second to the STM; false while a run
 * is going or when the task cannot start. */
bool midi_loop_send(uint32_t count, uint32_t rate);
void midi_loop_stop(void);

/* "MIDI_LOOP: capture=.. rx=.. ..." one line, CR LF. */
void midi_loop_format(char *out, size_t n);

#ifdef __cplusplus
}
#endif
