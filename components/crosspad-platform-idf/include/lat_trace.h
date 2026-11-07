// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file lat_trace.h
 * @brief One STM note's trip through the ESP, timed stage by stage (LAT_TRACE).
 *
 * Every stamp is esp_timer's low 32 bits. A hit is followed from the UART
 * event that carried it to the moment the DMA puts its first frame on the
 * DAC; one slot per pad, so it times hits that do not overlap on a pad --
 * which is what a latency measurement sends. Off until the first
 * lat_trace_reset(); then a stage costs one timer read, and the render's
 * share is two stores a pass.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The STM's note reached its handler (uartLoop); `uart_rx_us` is when the
/// UART event task took the bytes.
void lat_trace_note_in(uint8_t pad, uint32_t uart_rx_us);
/// The pad logic has run and posted the playback event.
void lat_trace_note_posted(uint8_t pad);
/// The event task is handling the playback event.
void lat_trace_note_handler(uint8_t pad);
/// The engine's noteOn() returned.
void lat_trace_note_engine(uint8_t pad);
/// A touch armed `pad`'s voice, before its note arrived.
void lat_trace_note_arm(uint8_t pad);
/// A finger's hit reached the engine straight from the pad logic, without the
/// event queue (the handler and engine stamps then stay empty).
void lat_trace_note_fire(uint8_t pad);

/// Render thread: a pass starts rendering (after any wait for its buffer).
void lat_trace_pass_begin(uint32_t now_us);
/// Render thread: a voice of `pad` is heard for the first time in this pass.
void lat_trace_voice_start(uint8_t pad);
/// Render thread: the pass handed its block to the DMA at `write_done_us`;
/// `dac_us` is when that block's first frame reaches the DAC, 0 if unknown.
void lat_trace_pass_end(uint32_t write_done_us, uint32_t dac_us);

/// Start over; the first call takes the trace's PSRAM (~15 kB) and starts
/// tracing. False when that memory is not there.
bool lat_trace_reset(void);
/// `LAT:` lines, one per segment (n, min, p50, p95, max, mean in us).
size_t lat_trace_report(char *out, size_t cap);
/// `LATREC:` lines: complete hits `first`.. (`count` of them, 0 = all still
/// kept) with their absolute stamps, numbered from the last reset.
size_t lat_trace_raw(char *out, size_t cap, uint32_t first, uint32_t count);

#ifdef __cplusplus
}
#endif
