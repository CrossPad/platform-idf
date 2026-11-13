#pragma once
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Blocking: waits up to 10 s for the link to be up, then
 * GET https://crosspad.app/api/device/ping — DNS, TLS and the Worker in one
 * call. No overall bound: DNS, connect and handshake together 20 s, then
 * every write and read 20 s each, so a server that trickles its status line
 * can hold it for tens of seconds. ~2-3 s on a healthy link, up to ~12 s right
 * after a cold radio start. Never on the LVGL thread. The caller holds a
 * lease. > 0: HTTP status; < 0: -(esp_err_t), every ESP error code being
 * positive. WIFI_PING_NO_LINK: the link was not up within the 10 s, so
 * nothing was sent and the server was never tried; wifi_link_fail() says why
 * (still "none" while the connect sequence has not given up). *ms gets the
 * request's round trip; *tlsCode (may be NULL) the mbedTLS error recorded
 * beside a failure, 0 if none. */
#define WIFI_PING_NO_LINK (-(ESP_ERR_WIFI_BASE + 15))   /* -ESP_ERR_WIFI_NOT_CONNECT */
int wifi_ping(uint32_t *ms, int *tlsCode);
#ifdef __cplusplus
}
#endif
