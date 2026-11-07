/**
 * @file companion_sysex.h
 * @brief Companion control over USB MIDI SysEx (CROSSPAD_CMD_COMPANION 0x1F).
 *
 * The audio USB profile has no CDC, so a browser or DAW-side tool reaches
 * the companion verb set (hil_control_companion_handle) through SysEx:
 *   host -> device  F0 7D 1F 01 <ASCII verb line> F7      (<= 190 bytes)
 *   device -> host  F0 7D 1F 02 <text> F7 ... 0x03 <text> F7 (0x03 = last chunk)
 * The reply text is the CDC reply verbatim. One request in flight; a second
 * one answers "ERR busy" at once.
 *   host -> device  F0 7D 1F 04 <ASCII verb line> F7      notify: run, never answer
 * (dropped when the mailbox is busy) — for a periodic sender such as the MIDI
 * hub's HOST_INFO, whose replies would otherwise reach every listener of the
 * port. Catalog: crosspad-hil knowledge/sysex.yaml.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*companion_sysex_sender_t)(const uint8_t *frame, size_t len);

/** Sender for the reply frames — called only from the task that pumps. */
void companion_sysex_init(companion_sysex_sender_t send);

/** Called after a request is queued so the main loop answers it at once. */
void companion_sysex_set_wake(void (*wake)(void));

/** SysEx callback payload after the command byte: [sub, ascii...]. Any task. */
void companion_sysex_receive(const uint8_t *args, unsigned len);

/** Main loop: execute a queued request. */
void companion_sysex_poll(void);

/** MIDI task, before it drains RX: send a finished reply. */
void companion_sysex_pump(void);

#ifdef __cplusplus
}
#endif
