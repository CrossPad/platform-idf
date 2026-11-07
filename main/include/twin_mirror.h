#pragma once
/**
 * @file twin_mirror.h
 * @brief Input lockstep for a browser twin (crosspad-web-twin; docs/subsystems/twin-protocol.md).
 *
 * The twin runs this same firmware (crosspad-core, crosspad-gui, the apps)
 * compiled to WebAssembly. Nothing of the screen is sent: the twin reaches the
 * same screen by getting the same inputs at the same LVGL times. What the
 * board's LVGL actually read is recorded where it read it — the touch panel
 * and the navigation encoder at their read callbacks, the pads where they
 * enter the PadManager, the power button's resolved gesture where it runs —
 * each stamped with lv_tick_get(), and sent over USB MIDI. A touch or encoder
 * frame is one LVGL read (every read while held or while a scroll is thrown),
 * so the twin reads its indevs at exactly those ticks:
 *
 *   F0 7D 1F 09 <kind> <t3 t2 t1 t0> <payload> F7      t = lv tick, 4 x 7 bit
 *     kind 0 touch    state, x hi, x lo, y hi, y lo
 *     kind 1 encoder  state, diff + 64
 *     kind 2 pad      pad, velocity (0 = release)
 *     kind 3 power    0 click (back), 1 hold (quick settings)
 *     kind 4 host     a MIDI message from the USB host as its handler got it
 *                     (status byte first; SysEx whole), 8 bytes in 7-bit groups:
 *                     a byte of high bits, then up to 7 low 7-bit bytes
 *     kind 5 clock    — nothing before this tick is still to come
 *     kind 6 lost     — the ring overflowed: resync
 *     kind 8 status   what the status bar and the host rows show that no input
 *                     decides, sent in order with the inputs whenever it moves:
 *                     battery (127 none), flags (charging, card, host alive,
 *                     update, BLE on, BLE peer, Wi-Fi), host CPU %, host RAM
 *                     used and total MB (3 x 7 bit each), DAWs installed,
 *                     DAWs running, the wall clock (5 x 7 bit seconds)
 *
 * The clock frames bound the twin's LVGL time: it never runs past a tick the
 * board has vouched for, so a time-dependent decision (long press, the kit
 * list's acceleration) only happens there once every input up to that tick is
 * in, and comes out the same. Off unless a host asked for it in the last few
 * seconds (companion verb "TWIN 1", repeated as a keepalive): no host, no cost.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Wrap an indev's read_cb (the touch panel before any gesture recogniser, the encoder). */
void twin_mirror_attach(lv_indev_t *indev);

/** A pad as it enters the PadManager: velocity 0 is a release. Any task. */
void twin_mirror_pad(uint8_t pad, uint8_t velocity);

/** A message from the USB host, as the firmware dispatches it (MIDI task). */
void twin_mirror_host(const uint8_t *msg, size_t len);

/** The power button's gesture, as it runs on the LVGL task. */
void twin_mirror_power(bool hold);

/** Companion keepalive ("TWIN 1" / "TWIN 0"). */
void twin_mirror_enable(bool on);
bool twin_mirror_active(void);

/** MIDI task: send what is queued, then a clock frame. */
void twin_mirror_pump(void (*send)(const uint8_t *frame, size_t len));

#ifdef __cplusplus
}
#endif
