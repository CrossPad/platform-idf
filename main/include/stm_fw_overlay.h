/**
 * @file stm_fw_overlay.h
 * @brief STM32 in-field update UI: the boot-time prompt, and the same flow on
 *        demand from Settings → Programming. Implementation: gui/stm_fw_overlay.cpp.
 */
#pragma once

#include "stm32_fw_update.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Boot verdict from stm32_fw_update_check(); the prompt waits for the launcher. */
void stm_fw_overlay_show(const stm32_fw_update_status_t *st);

/** LVGL thread, from LoadMainScreen once the launcher and its encoder group exist. */
void stm_fw_overlay_on_main_screen(void);

/**
 * Re-check the bundled image against the running STM32 and prompt right now
 * (LVGL thread). Prompts even when nothing is due — that is a reflash.
 */
void stm_fw_overlay_request(void);

#ifdef __cplusplus
}
#endif
