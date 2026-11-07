/**
 * @file net_mode.h
 * @brief Network mode — the boot in which WiFi runs.
 *
 * WiFi runs on demand in a normal boot (wifi_link.h). When a user-facing
 * request is refused for memory (a Sampler with a kit loaded leaves too little
 * PSRAM, docs/subsystems/wifi.md) the user can restart into this boot, shaped
 * like USB Transfer mode (usb_transfer.h): entering arms a one-shot flag in RTC memory
 * and restarts; that boot starts no engine, kits, BLE or SD hotplug and shows
 * the network-mode screen instead of the launcher. Leaving is another restart.
 * The flag never survives a power cycle, a panic or a watchdog, so those boots
 * are always normal.
 *
 * Design: docs/design/2026-10-02-wifi-on-demand-design.md (Revision 2)
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The screen's tabs; B and C build Updates and Store, the flag carries all three already. */
typedef enum {
    NET_TAB_WIFI = 0,
    NET_TAB_STORE,
    NET_TAB_UPDATES,
    NET_TAB_COUNT,
} net_tab_t;

/** Read and clear the one-shot flag. Call exactly once, first thing in app_main(). */
bool net_mode_boot_claim(void);

/** True for the whole of a boot claimed above. */
bool net_mode_active(void);

/** The tab the request asked for; NET_TAB_WIFI outside network mode. */
net_tab_t net_mode_start_tab(void);

/** Arm the flag and restart into network mode. Does not return. */
void net_mode_request(net_tab_t tab);

/** Restart without the flag, back into a normal boot. Does not return. */
void net_mode_leave(void);

/** "wifi", "store", "updates". */
const char *net_tab_name(net_tab_t tab);

/** Inverse of net_tab_name(); false for anything else. */
bool net_tab_parse(const char *name, net_tab_t *out);

#ifdef __cplusplus
}
#endif
