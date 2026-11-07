#pragma once

/**
 * @file wifi_page.h
 * @brief The WiFi page: WiFi allowed, status, find networks (scan list +
 *        password keyboard), test connection, saved networks (Connect/Forget).
 *        Settings -> WiFi in a normal boot and the WiFi tab in network mode.
 */

#ifdef USE_LVGL
#include <stdbool.h>
#include "lvgl.h"

typedef struct {
    bool holdLease;          /* hold a lease while the page exists (Settings); network mode's screen holds its own */
    bool offerNetworkMode;   /* no_mem on this page's request offers a restart into network mode (never in network mode) */
} wifi_page_opts_t;

/** Build into @p parent (rows added to @p group). LVGL thread; one page at a
 *  time. Everything the page owns goes when @p parent's children are deleted. */
void wifi_page_build(lv_obj_t *parent, lv_group_t *group, const wifi_page_opts_t *opts);

#endif // USE_LVGL
