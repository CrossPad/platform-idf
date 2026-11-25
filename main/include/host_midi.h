/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/**
 * @file host_midi.h
 * @brief The host control surface: a DAW's Control Changes ride the board's
 *        levels, and host-driven pad LEDs (with an honest way back).
 *
 * Companion to main/midi_kit_pc.cpp (Program Change picks the kit). The
 * decisions -- which (channel, CC) pair means what, how a 7-bit value becomes a
 * level, what a frame of pad LEDs is -- live in main/include/host_midi_map.h
 * and are pinned on the host by main/tests/test_host_midi.cpp. This file is the
 * part that touches a board.
 *
 * Threading, the whole contract in one paragraph: a Control Change arrives on
 * the MIDI parsing task (`uartLoop`, main/midiCallbacks.cpp:684-747) and a SysEx
 * pad-LED frame on the same task; neither may block, take the LVGL lock, write
 * the strip or call into the mixer (main.cpp:1246-1252 turns coalesced LED
 * refresh on precisely because a strip refresh from the wrong place disturbs
 * the audio, and the render thread reads the mixer's levels while rendering).
 * So every caller here only parks a few bytes, and one `bsp_worker` job -- the
 * same single task that already runs `pad_anim` (main.cpp:739, :1252), so the
 * LED model is written where it is already painted -- takes the newest park and
 * applies it: mixer faders, output levels, pad mix, pad colours, animation
 * steps. A host therefore gets one frame per verb call at up to the worker's
 * rate, never a stall, and never a partial write of the model from two tasks.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load the saved Control Change map and register the apply job. Call once after
 * the platform is up (the mixer's channels are registered by the engines). */
void host_midi_init(void);

/* An incoming Control Change. @p ch is the MIDI library's channel, 1..16; the
 * map is 0-based inside, and the conversion happens here once. Returns 1 when
 * the CC was taken for the board (and so should not also be treated as plain
 * forwarding), 0 when nothing is mapped to it. The caller still forwards: this
 * board's routing rules are the mixer's and the STM's, not the CC map's. */
int host_midi_control_change(uint8_t ch, uint8_t cc, uint8_t val);

/* A host pad-LED frame. Pads are the board's own order, 0..15. */
void host_midi_led_pad(uint8_t pad, uint8_t r, uint8_t g, uint8_t b);
void host_midi_led_all(uint8_t r, uint8_t g, uint8_t b);
/* One whole grid, 48 raw bytes (r,g,b per pad, pad 0 first) -- the form a
 * SysEx frame carries, one frame per animation step. */
void host_midi_led_bytes(const uint8_t *rgb48);
/* The same grid as 96 hex characters (48 bytes), the form a verb line carries.
 * Returns 1 when the text was exactly that, 0 when nothing is staged. */
int host_midi_led_hex(const char *hex96);
/* Give the pads back: the colours the board itself was showing when the host
 * took them, re-applied, and the lease released. */
void host_midi_led_reset(void);
/* An animation the board runs itself: -1 = none, else chase, ring, strobe. */
void host_midi_led_anim(int which, int period_ms);

/* --- the verbs -------------------------------------------------------------
 * MIDI_CC [MAP [first] [count] | SET ch cc target | RESET | STATUS]
 * SURFACE [STATUS | PAD <0..15> | LVL <out|hw|chv|pan|padmix> <addr> | CLR]
 * LED_HOST [SET pad r g b | ALL r g b | GRID hex | ANIM name [ms] | OFF |
 *           LEASE ms | STATUS]
 * Both answer through these helpers so the reply shape lives in one place. */

/* The map, one row per position of the table; 0 when the row does not exist. */
int host_midi_cc_row(int position, char *out, size_t len);
int host_midi_cc_total(void);
int host_midi_cc_status_line(char *out, size_t len);
/* Parse the argument of MIDI_CC SET/RESET; returns 0 on success, else a short
 * reason word for the ERR line. */
const char *host_midi_cc_set(const char *arg);

int host_midi_led_status_line(char *out, size_t len);
/* Parse the argument of LED_HOST ...; 0 on success, else a reason word. */
const char *host_midi_led_cmd(const char *arg);

/* --- the SysEx control surface (address writes, main/include/surface_map.h) -----
 * A frame arrives on the MIDI task with its grammar already decoded and checked by
 * main/audio_route_control.cpp; these only park what was decided, and the worker
 * applies it -- the same contract as every other entry point above, because the LED
 * model and the mixer each have one task allowed to write them.
 *
 * `action` points at a surface-side SurfAction (main/host_midi.cpp): kind, section,
 * start, count, values. It is passed as bytes so the SysEx handler does not have to
 * know the struct's layout; the sizes are pinned by the tests. */
void host_midi_park(const uint8_t *action);
void host_midi_park_palette(uint8_t index, uint8_t n, const uint8_t *rgb7);
void host_midi_surface_refused(void);   /* a frame that did not decode, counted */
void host_midi_surface_counters(unsigned *c);   /* 8 values: see host_midi.cpp */
int host_midi_surface_status_line(char *out, size_t len);
int host_midi_surface_pad_line(char *out, size_t len, int pad);
/* One level address's record and the board's live value, in both units: the read-out
 * a bench needs to judge a level write against the thing the write actually lands on. */
int host_midi_surface_level_line(char *out, size_t len, const char *sec, int addr);
void host_midi_surface_clear(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
