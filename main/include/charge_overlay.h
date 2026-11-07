/**
 * @file charge_overlay.h
 * @brief Charge-while-off UI — fullscreen "Charging" + battery%% LVGL overlay
 *        plus the 4x4 pad battery-gauge animation.
 *
 * Triggered when the STM reports PWR_STATE_CHARGE (charger plugged while the
 * device was soft-off). The STM has minimally repowered the ESP just to draw
 * this feedback. See docs spec 2026-06-10-charge-while-off-animation-design.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Enter charge mode: start the pad gauge animation, raise pad brightness, and
 *  show the "Charging" + battery%% screen (auto-fades after ~5 s). Idempotent —
 *  a second call while active just refreshes the reading. Safe from any task. */
void charge_overlay_start(void);

/** Charger plugged in while the device is on: the same screen for ~5 s over
 *  whatever runs, live readings, then it fades away (a tap closes it). No pad
 *  gauge, no brightness change. No-op in charge mode or without a display.
 *  Call from a non-LVGL task (reads the STM over I2C). */
void charge_overlay_peek(void);

/** Periodic refresh (call ~every 10 ms from the animation task). Re-reads the
 *  battery every ~1 s and updates the gauge SoC + the %% label. No-op when
 *  neither charge mode nor a peek is active. */
void charge_overlay_tick(void);

/** Leave charge mode: stop the gauge animation and dismiss the screen. Safe to
 *  call when not active. */
void charge_overlay_stop(void);

/** True while charge mode is active. */
bool charge_overlay_active(void);

#ifdef __cplusplus
}
#endif
