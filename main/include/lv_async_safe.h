#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Thread-safe lv_async_call for non-LVGL tasks (CDC, MIDI, ...).
 *
 * lv_async_call() itself mutates the LVGL timer list and is NOT thread-safe
 * without LV_USE_OS — calling it concurrently with the LVGL task corrupts the
 * list and the LVGL task later jumps through a garbage callback pointer. So
 * the call has to happen under the esp_lvgl_port display lock.
 *
 * That lock is held for a whole frame, and a kit load ends with pad reloads
 * (SD reads) on the LVGL thread, so waiting for it is not bounded by anything
 * the caller controls — measured at 691 ms from the CDC command loop, which is
 * how a KIT_STATUS asked right after a KIT_LOAD timed out. The wait therefore
 * happens on a forwarder task instead: the caller queues, `lv_async_fwd` takes
 * the lock and hands the callback to LVGL. Order is preserved per caller.
 *
 * The caller never waits for the lock or for room in the queue: a call that
 * finds the queue full joins an overflow list behind it (counted, warned about
 * at most once a second). Pad-logic callbacks and event-bus handlers post GUI
 * work through here, and the LVGL thread can be waiting for exactly those
 * callbacks to return (PadManager deactivation, IEventBus::unsubscribe), so a
 * wait here would be a deadlock.
 *
 * Never needed (and never used) from the LVGL thread itself — plain
 * lv_async_call is fine there.
 */
void lv_async_call_locked(lv_async_cb_t cb, void *user_data);

/** Start the forwarder. Calls made before this wait in order and run once it
 *  starts. If it cannot start, callers take the lock inline with a bounded
 *  wait and the call is dropped (counted) past that. */
void lv_async_safe_start(void);

#ifdef __cplusplus
}
#endif
