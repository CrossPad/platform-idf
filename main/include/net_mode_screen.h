#pragma once

/**
 * @file net_mode_screen.h
 * @brief The only screen of network mode: a header with the network name and
 *        Exit, and the tabs WiFi, Store and Updates.
 */

#ifdef USE_LVGL
#include "lvgl.h"

/** Build the screen into @p parent. LVGL thread only; once per boot. */
void net_mode_screen_create(lv_obj_t *parent);

/** Network mode is being left (Exit, NET_MODE 0): an open confirm is answered
 *  "no", the kit card closes, the Store and Updates views let go. LVGL thread;
 *  nothing happens before the screen exists or a second time. */
void net_mode_screen_close(void);

#endif // USE_LVGL
