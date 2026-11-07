#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* WiFi on demand. Nothing turns WiFi "on": callers hold a lease. The first
 * lease starts the radio and connects to the saved networks in order; the last
 * release arms a 30 s linger, after which the radio stops and its memory goes
 * back. One control task with an internal stack owns every esp_wifi_* call
 * (start/stop touch flash); it exists only from the first lease to the last
 * stop, so nothing of WiFi is created at boot. BLE is stopped while WiFi runs
 * and restored after. docs/subsystems/wifi.md */

#ifdef __cplusplus
extern "C" {
#endif

bool     wifi_link_acquire(const char *reason);   /* false = refused now (disabled, transfer mode, a memory gate); outcome of a start via wifi_link_state() */
void     wifi_link_release(void);
uint8_t  wifi_link_state(void);                   /* crosspad::WifiLinkState */
uint8_t  wifi_link_fail(void);                    /* crosspad::WifiFail */
bool     wifi_link_wait_up(uint32_t timeout_ms);  /* never on the LVGL thread */
bool     wifi_link_info(char *ssid, size_t n, int8_t *rssi, uint32_t *ip);
uint8_t  wifi_link_leases(void);
bool     wifi_link_radio_idle(void);              /* no lease, radio off and not starting or stopping: its internal RAM is free */
uint32_t wifi_link_stack_free(void);              /* control task high-water mark, bytes */
uint8_t  wifi_link_last_reason(void);             /* last 802.11 disconnect reason not caused by us; 0 = none since boot */
uint32_t wifi_link_heap_init(void);               /* internal RAM the driver can use (MALLOC_CAP_DEFAULT) when the last radio start began */
uint32_t wifi_link_heap_low(void);                /* its lowest from that start until the radio stopped, transient dips included; read at the start, connect, IP, disconnect, every 5 s RSSI poll and the stop; == heap_init when the gate refused */
uint32_t wifi_link_int_init(void);                /* all free internal RAM, the DMA/stack pool included, when the last radio start began */
uint32_t wifi_link_int_low(void);                 /* its lowest, as heap_low */
uint32_t wifi_link_psram_init(void);              /* free PSRAM when the last radio start began */
uint32_t wifi_link_psram_low(void);               /* its lowest, as heap_low */
void     wifi_link_connect_now(void);             /* (re)start the connect sequence from the first saved network */
/* Connect to `ssid` first and alone, and save it (first in the list) only once
 * it has an IP; a failed attempt saves nothing. Starts the radio like
 * wifi_link_connect_now(). Any task. */
void     wifi_link_connect_candidate(const char *ssid, const char *pass);
void     wifi_link_enable_changed(void);          /* after enableWiFi changed */
/* The settings factory reset's share of WiFi: every saved network forgotten
 * (NVS and RAM), then wifi_link_enable_changed(), which stops a running radio
 * once WiFi is no longer allowed. Runs on a one-shot task with an internal
 * stack, so any thread may call it, the LVGL one included. False = that task
 * could not be created. */
bool     wifi_factory_reset(void);

#define WIFI_LINK_SCAN_MAX 20
typedef struct { char ssid[33]; int8_t rssi; uint8_t auth; } wifi_ap_t;   /* auth: wifi_auth_mode_t */
/* Blocking (bounded by 15 s): lease, active scan, release. Strongest first, one
 * row per SSID, hidden networks left out. Count, or one of the codes below.
 * Never on the LVGL thread. A scan's own failure leaves wifi_link_fail() alone:
 * it says how the link is, and a link can be up while a scan fails. */
#define WIFI_LINK_SCAN_REFUSED (-1)   /* the lease or the radio start was refused: wifi_link_fail() says why */
#define WIFI_LINK_SCAN_TIMEOUT (-2)   /* no answer from the control task within the bound */
#define WIFI_LINK_SCAN_FAILED  (-3)   /* the driver refused the scan or its records */
int wifi_link_scan(wifi_ap_t *out, int max);
/* "open" "wep" "wpa" "wpa2" "wpa2e" "wpa3" "wpa2wpa3" "owe" "other" */
const char *wifi_link_auth_name(uint8_t auth);

#ifdef __cplusplus
}
#endif
