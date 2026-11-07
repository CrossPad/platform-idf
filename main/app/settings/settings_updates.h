#pragma once

#ifdef USE_LVGL
/** Settings -> Updates: Check at startup, the running kit's channel, the
 *  state of the last check, Check now (a WiFi lease; on no_mem the offer to
 *  restart in network mode). Call before settings_ui_create(); registers once. */
void settings_updates_register(void);
#endif
