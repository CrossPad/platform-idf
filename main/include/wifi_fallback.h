#pragma once

/**
 * @file wifi_fallback.h
 * @brief When WiFi does not fit beside what is running, offer network mode.
 *
 * A user-facing WiFi request (Settings now; the store and the update button in
 * sub-projects B/C) that comes back refused with no_mem calls this once: a
 * confirm offers a restart into network mode on the tab that asked. Background
 * callers never call it -- they log no_mem and try again later -- and verbs
 * answer ERR no_mem. Inside network mode it does nothing: there is nowhere
 * further to go, and the caller shows the error. (Spec Revision 2.)
 */

#ifdef USE_LVGL
#include "lvgl.h"
#include "net_mode.h"

/** LVGL thread only. @p encoder_group: the group to give the encoder back. */
void wifi_fallback_offer(net_tab_t tab, lv_group_t *encoder_group);

#endif // USE_LVGL
