#include "wifi_ping.h"
#include "device_serial.h"
#include "fw_image_desc.h"
#include "wifi_link.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

/* esp-tls directly, not esp_http_client: one GET needs only the status line,
 * and the HTTP client with its parser and transport layer cost ~21 kB of image,
 * which on this board is PSRAM the UI loses (docs/subsystems/wifi.md, "TLS"). */
static constexpr const char* kPingHost = "crosspad.app";
static constexpr int kHttpsPort = 443;
/* The ping says which board it is: its serial, firmware version and board
 * revision, recorded by the Worker as the board's last sighting. */
static constexpr const char* kPingRequestFmt =
    "GET /api/device/ping?sn=%s&rev=v%u&fw=%s HTTP/1.1\r\n"
    "Host: crosspad.app\r\n"
    "User-Agent: CrossPad/%s\r\n"
    "Connection: close\r\n\r\n";
static constexpr size_t kRequestMax = 384;
static constexpr size_t kVersionMax = 64;
static constexpr int kLinkTimeoutMs = 10000;
/* esp-tls timeout_ms: DNS + TCP connect + TLS handshake together, and each
 * socket read and write after it. A handshake
 * costs 1.27 s of CPU on its own (software ECC, code in PSRAM), and right after
 * a cold radio start a test AP stalled it for up to 12.5 s with the CPU idle
 * (2026-10-03), past a 10 s bound. Real networks are slower still. */
static constexpr int kTlsTimeoutMs = 20000;
static constexpr int64_t kUsPerMs = 1000;
static constexpr size_t kStatusLineMax = 64;   // "HTTP/1.1 404 Not Found\r\n" and then some

/* The esp-tls error behind a failed call, as a positive esp_err_t. With none
 * recorded the call ran out of time. Only a positive record is taken:
 * wifi_ping() negates the result, and a record of ESP_FAIL (-1) would come back
 * as +1, which reads as an HTTP status. *tlsCode gets the mbedTLS code beside
 * it (0 if none). */
static int last_error(esp_tls_t* tls, int* tlsCode) {
    esp_tls_error_handle_t h = nullptr;
    int code = 0, flags = 0;
    esp_err_t err = ESP_ERR_TIMEOUT;
    if (esp_tls_get_error_handle(tls, &h) == ESP_OK && h) {
        const esp_err_t last = esp_tls_get_and_clear_last_error(h, &code, &flags);
        if (last > 0 && last != ESP_ERR_INVALID_STATE) err = last;
    }
    *tlsCode = code;
    return (int)err;
}

/* Reads until the status line is complete; the HTTP status, or 0 if the reply
 * was not HTTP. */
static int read_status(esp_tls_t* tls, int* err, int* tlsCode) {
    char line[kStatusLineMax];
    size_t used = 0;
    while (used < sizeof(line) - 1) {
        const ssize_t n = esp_tls_conn_read(tls, line + used, sizeof(line) - 1 - used);
        if (n <= 0) { *err = n == 0 ? (int)ESP_ERR_INVALID_RESPONSE : last_error(tls, tlsCode); return 0; }
        used += (size_t)n;
        line[used] = '\0';
        if (strchr(line, '\n')) break;
    }
    line[used] = '\0';
    int status = 0;
    if (sscanf(line, "HTTP/%*d.%*d %d", &status) != 1) { *err = (int)ESP_ERR_INVALID_RESPONSE; return 0; }
    return status;
}

/* The firmware version for a query string: '+' (semver build metadata) would read
 * as a space, so it is sent as %2B; anything outside [0-9A-Za-z.-] is dropped. */
static void version_for_query(const char* in, char* out, size_t size) {
    size_t n = 0;
    for (const char* p = in; *p && n + 4 < size; ++p) {
        const char ch = *p;
        if (ch == '+') { memcpy(out + n, "%2B", 3); n += 3; }
        else if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                 ch == '.' || ch == '-') { out[n++] = ch; }
    }
    out[n] = '\0';
}

static_assert(WIFI_PING_NO_LINK == -(int)ESP_ERR_WIFI_NOT_CONNECT, "wifi_ping.h: WIFI_PING_NO_LINK");

int wifi_ping(uint32_t* ms, int* tlsCode) {
    if (tlsCode) *tlsCode = 0;
    if (ms) *ms = 0;
    /* A code of its own: -ESP_ERR_TIMEOUT is also a TLS step that ran out of
     * time, and that one does mean the server. */
    if (!wifi_link_wait_up(kLinkTimeoutMs)) return WIFI_PING_NO_LINK;
    esp_tls_t* tls = esp_tls_init();
    if (!tls) return -(int)ESP_ERR_NO_MEM;
    esp_tls_cfg_t c = {};
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.timeout_ms = kTlsTimeoutMs;
    const int64_t t0 = esp_timer_get_time();
    int err = 0, status = 0, code = 0;
    if (esp_tls_conn_new_sync(kPingHost, strlen(kPingHost), kHttpsPort, &c, tls) != 1) {
        err = last_error(tls, &code);
    } else {
        const char* version = esp_app_get_description()->version;
        char fw[kVersionMax];
        version_for_query(version, fw, sizeof fw);
        char request[kRequestMax];
        const int built = snprintf(request, sizeof request, kPingRequestFmt, device_serial(),
                                   (unsigned)fw_image_desc_running()->board_rev, fw, fw);
        const size_t len = built > 0 ? std::min((size_t)built, sizeof request - 1) : 0;
        size_t sent = 0;
        while (sent < len && err == 0) {
            const ssize_t n = esp_tls_conn_write(tls, request + sent, len - sent);
            if (n <= 0) err = last_error(tls, &code);
            else        sent += (size_t)n;
        }
        if (err == 0) status = read_status(tls, &err, &code);
    }
    if (ms) *ms = (uint32_t)((esp_timer_get_time() - t0) / kUsPerMs);
    if (tlsCode) *tlsCode = code;
    esp_tls_conn_destroy(tls);
    return err == 0 ? status : -err;
}
