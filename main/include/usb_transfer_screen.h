#pragma once

/**
 * @file usb_transfer_screen.h
 * @brief The only screen of USB transfer mode: what the host is doing with the
 *        card, and the one way out.
 */

#ifdef USE_LVGL
#include "lvgl.h"

/** Build the screen into @p parent. LVGL thread only; called once per boot. */
void usb_transfer_screen_create(lv_obj_t * parent);

#endif // USE_LVGL
