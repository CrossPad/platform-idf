#pragma once
#include <stdint.h>
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
 * positive. *ms gets the request's round trip; *tlsCode (may be NULL) the
 * mbedTLS error recorded beside a failure, 0 if none. */
int wifi_ping(uint32_t *ms, int *tlsCode);
#ifdef __cplusplus
}
#endif
