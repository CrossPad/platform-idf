#pragma once

/* Raw touch sampling, tapped at the source.
 *
 * The samples are taken by wrapping the touch indev's read_cb, not by
 * listening to LVGL events: LVGL's own gesture detector gives up the moment
 * the finger lands on a scrollable object (indev_gesture() returns early on
 * pointer.scroll_obj), which on this UI is the launcher grid, every list and
 * every settings page. A swipe that must work everywhere has to be recognised
 * below that, from the coordinate stream itself.
 *
 * Recognition lives in crosspad-gui's touch_gesture, which wraps this.
 *
 * It also injects: touch_trace_inject() replaces what the panel reports with
 * a synthetic contact for its duration, so the bench can drag and tap without
 * a finger -- through the same read path a finger takes, under the trace, the
 * swipe recogniser and LVGL alike.
 */

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Wraps the indev's read_cb. Safe to call with NULL (no touch panel). */
void touch_trace_init(lv_indev_t *indev);

/* Recording allocates the ring in PSRAM on the first ON and keeps it. */
bool touch_trace_set(bool on);      /* false = allocation failed */
bool touch_trace_is_on(void);
void touch_trace_clear(void);

/* Live mirror for a host (the "fly twin" in brag-output, a remote UI): always on,
 * independent of recording. Called from the MIDI task; sends one USB MIDI SysEx
 * when the touch changed since the last call:
 *   F0 7D 1F 05 <st> <x_hi> <x_lo> <y_hi> <y_lo> F7
 *   st 1 = press, 2 = drag (<= 40 Hz), 0 = release (at the last point);
 *   x 0..319, y 0..239 in LCD pixels, as LVGL sees them.
 * A contact that starts while a text prompt is open is not mirrored. */
void touch_mirror_pump(void (*send)(const uint8_t *frame, size_t len));

/* True while a text prompt (crosspad-gui ui_text_prompt: title, field,
 * keyboard) is up: the one place a tap or the encoder types something, a WiFi
 * password included, so no mirror sends what they do there. LVGL task only. */
bool touch_trace_text_entry_open(void);

/* Number of samples held, and one sample by index (0 = oldest). */
unsigned touch_trace_count(void);
bool touch_trace_get(unsigned idx, unsigned *t_ms, int *x, int *y, unsigned *pressed);

/* A synthetic contact: pressed at (x1,y1), held still there hold_ms (a long
 * press before a drag), moved in a straight line to (x2,y2) over move_ms,
 * held still there rest_ms, then released. Panel coordinates. Replaces the
 * panel's own samples until it is over; false while another is still
 * running. Safe from any task. */
bool touch_trace_inject(int x1, int y1, int x2, int y2, unsigned move_ms, unsigned rest_ms,
                        unsigned hold_ms = 0);
bool touch_trace_injecting(void);

#ifdef __cplusplus
}
#endif
