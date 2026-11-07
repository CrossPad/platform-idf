#pragma once

/*
 * Power-button GUI gestures (ESP-IDF side).
 *
 * The STM32 owns the button's power ladder (0.5 s → LIGHT sleep on release,
 * REG_CTL_LONG_PRESS → soft-off, 7 s → ship mode) and does NOT push press or
 * release events to the ESP while awake: REG_IN_PWR_HOLD_* is maintained only
 * for the wake gesture. So the ESP samples REG_IN_BUTTONS bit0 and times the
 * hold itself, from the same task that watches REG_IN_PWR_STATE (the two
 * registers are adjacent, one 2-byte read covers both).
 *
 * Two gestures, both resolved in crosspad-gui so the PC simulator behaves
 * identically:
 *   - release under 500 ms  → crosspad_gui::powerBack()               (contextual back)
 *   - 500 ms while held     → crosspad_gui::powerToggleQuickSettings() (drawer)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One sample of the button and the STM power state. @p nowMs is monotonic. */
void power_button_feed(bool pressed, uint8_t pwrState, uint32_t nowMs);

/** Live mirror for a host (the "fly twin" in brag-output): called from the MIDI
 * task, sends one USB MIDI SysEx when the button changed since the last call —
 *   F0 7D 1F 06 <pressed 0|1> F7
 * — raw press/release as sampled every 40 ms, whatever the gesture means. */
void power_button_mirror_pump(void (*send)(const uint8_t *frame, size_t len));

/** Fire a gesture without the button (HIL: PWR_GESTURE CLICK|HOLD). */
void power_button_inject(bool hold);

#ifdef __cplusplus
}
#endif
