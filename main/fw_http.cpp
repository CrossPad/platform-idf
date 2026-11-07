#include "fw_http.h"

#include <cstdio>
#include <cstring>
#include <strings.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "cp_account.h"
#include "crosspad/fw/FwAccount.hpp"
#include "crosspad/fw/HttpDate.hpp"

static const char* TAG = "fw_http";
static constexpr int kTimeoutMs = 15000;
static constexpr int kRxBuffer = 2048;
static constexpr int kTxBuffer = 1024;
static constexpr int kStatusOk = 200;
static constexpr int kStatusPartial = 206;
static constexpr int kStatusUnauthorized = 401;
static constexpr char kHttps[] = "https://";
static constexpr size_t kUserAgentMax = 48;
static constexpr size_t kRangeHeaderMax = 40;   // "bytes=" + 20 digits + "-"
static constexpr size_t kAuthHeaderMax = sizeof("Bearer ") + crosspad::kFwDeviceTokenLen;

static esp_err_t on_event(esp_http_client_event_t* e) {
    if (e->event_id != HTTP_EVENT_ON_HEADER || !e->user_data) return ESP_OK;
    auto* r = static_cast<FwHttpReply*>(e->user_data);
    if (strcasecmp(e->header_key, "Date") == 0) {
        int64_t t = 0;
        if (crosspad::parseHttpDate(e->header_value, t)) r->dateUnix = t;
    } else if (strcasecmp(e->header_key, "Content-Range") == 0) {
        unsigned long long start = 0, end = 0, total = 0;
        if (sscanf(e->header_value, "bytes %llu-%llu/%llu", &start, &end, &total) == 3) {
            r->rangeStart = static_cast<int64_t>(start);
            r->rangeTotal = static_cast<int64_t>(total);
        } else if (sscanf(e->header_value, "bytes %llu-%llu/*", &start, &end) == 2) {
            r->rangeStart = static_cast<int64_t>(start);
        }
    }
    return ESP_OK;
}

/* The one place the device token is attached: cp_account_auth_header()
 * answers only for a linked board and a URL under CROSSPAD_ACCOUNT_URL. */
static esp_http_client_handle_t open_client(const char* url, FwHttpReply* r, esp_http_client_method_t method) {
    esp_http_client_config_t c = {};
    c.url = url;
    c.method = method;
    c.timeout_ms = kTimeoutMs;
    c.buffer_size = kRxBuffer;
    c.buffer_size_tx = kTxBuffer;
    c.event_handler = on_event;
    c.user_data = r;
    char auth[kAuthHeaderMax];
    r->authSent = cp_account_auth_header(url, auth, sizeof auth, &r->authGen);
    // A redirect would carry the header to wherever it points.
    c.disable_auto_redirect = r->authSent;
    if (strncmp(url, kHttps, sizeof kHttps - 1) == 0) c.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (h && r->authSent) esp_http_client_set_header(h, "Authorization", auth);   // copied by the client
    memset(auth, 0, sizeof auth);
    return h;
}

/* Sends the request (`body`, when given, as application/json) and reads the
 * status line and headers. */
static bool begin(esp_http_client_handle_t h, uint64_t offset, const char* body, FwHttpReply* r) {
    char ua[kUserAgentMax];
    snprintf(ua, sizeof ua, "CrossPad/%s", esp_app_get_description()->version);
    esp_http_client_set_header(h, "User-Agent", ua);
    if (offset > 0) {
        char range[kRangeHeaderMax];
        snprintf(range, sizeof range, "bytes=%llu-", static_cast<unsigned long long>(offset));
        esp_http_client_set_header(h, "Range", range);
    }
    const int bodyLen = body ? static_cast<int>(strlen(body)) : 0;
    if (body) esp_http_client_set_header(h, "Content-Type", "application/json");
    r->err = esp_http_client_open(h, bodyLen);
    if (r->err != ESP_OK) return false;
    if (bodyLen > 0 && esp_http_client_write(h, body, bodyLen) != bodyLen) {
        r->err = ESP_FAIL;
        return false;
    }
    r->contentLength = esp_http_client_fetch_headers(h);
    r->status = esp_http_client_get_status_code(h);
    // The website no longer knows this token: the board is unlinked, unless it linked again meanwhile.
    if (r->status == kStatusUnauthorized && r->authSent) cp_account_revoked(r->authGen);
    if (r->contentLength < 0 && r->status == 0) {
        r->err = ESP_FAIL;   // no status line arrived: the transport failed
        return false;
    }
    return true;
}

static void finish(esp_http_client_handle_t h) {
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
}

/* The rest of the body into buf after begin(); false on a transport error, a
 * body that does not fit, or `cancel`. */
static bool read_body(esp_http_client_handle_t h, char* buf, size_t cap, size_t* len, FwHttpReply* out,
                      FwHttpCancel cancel) {
    bool ok = true;
    while (ok && *len + 1 < cap) {
        if (cancel && cancel()) {
            out->err = ESP_ERR_INVALID_STATE;
            ok = false;
            break;
        }
        const int n = esp_http_client_read(h, buf + *len, static_cast<int>(cap - 1 - *len));
        if (n < 0) {
            out->err = ESP_FAIL;
            ok = false;
        } else if (n == 0) {
            break;
        } else {
            *len += static_cast<size_t>(n);
        }
    }
    if (ok && !esp_http_client_is_complete_data_received(h)) {
        out->err = (*len + 1 >= cap) ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_RESPONSE;
        ok = false;
    }
    buf[*len] = '\0';
    return ok;
}

bool fw_http_get_buffer(const char* url, char* buf, size_t cap, size_t* len, FwHttpReply* out, FwHttpCancel cancel) {
    *out = FwHttpReply{};
    *len = 0;
    if (cap == 0) return false;
    buf[0] = '\0';
    esp_http_client_handle_t h = open_client(url, out, HTTP_METHOD_GET);
    if (!h) {
        out->err = ESP_ERR_NO_MEM;
        return false;
    }
    const bool ok = begin(h, 0, nullptr, out) && out->status == kStatusOk && read_body(h, buf, cap, len, out, cancel);
    finish(h);
    if (!ok && !(cancel && out->err == ESP_ERR_INVALID_STATE)) ESP_LOGW(TAG, "GET %s: status %d, %s", url, out->status, esp_err_to_name(out->err));
    return ok;
}

bool fw_http_get_stream(const char* url, uint64_t offset, uint64_t expectedTotal, FwHttpSink sink, void* ctx,
                        uint8_t* chunk, size_t chunkLen, FwHttpReply* out) {
    *out = FwHttpReply{};
    esp_http_client_handle_t h = open_client(url, out, HTTP_METHOD_GET);
    if (!h) {
        out->err = ESP_ERR_NO_MEM;
        return false;
    }
    bool ok = begin(h, offset, nullptr, out);
    if (ok) {
        const bool whole = out->status == kStatusOk;
        ok = (offset == 0 && whole) || (offset > 0 && out->status == kStatusPartial);
        if (ok && offset > 0) {
            // A 206 for some other range would splice foreign bytes into the file.
            out->rangeMismatch = out->rangeStart != static_cast<int64_t>(offset) ||
                                 (expectedTotal > 0 && out->rangeTotal != static_cast<int64_t>(expectedTotal));
            ok = !out->rangeMismatch;
        }
    }
    while (ok) {
        const int n = esp_http_client_read(h, reinterpret_cast<char*>(chunk), static_cast<int>(chunkLen));
        if (n < 0) {
            out->err = ESP_FAIL;
            ok = false;
        } else if (n == 0) {
            break;
        } else if (!sink(chunk, static_cast<size_t>(n), ctx)) {
            out->err = ESP_ERR_INVALID_STATE;
            ok = false;
        }
    }
    if (ok && !esp_http_client_is_complete_data_received(h)) {
        out->err = ESP_ERR_INVALID_RESPONSE;   // the connection ended early: resumable
        ok = false;
    }
    finish(h);
    return ok;
}

bool fw_http_post_json(const char* url, const char* body, char* buf, size_t cap, size_t* len, FwHttpReply* out) {
    *out = FwHttpReply{};
    *len = 0;
    if (cap == 0) return false;
    buf[0] = '\0';
    esp_http_client_handle_t h = open_client(url, out, HTTP_METHOD_POST);
    if (!h) {
        out->err = ESP_ERR_NO_MEM;
        return false;
    }
    const bool ok = begin(h, 0, body, out) && read_body(h, buf, cap, len, out, nullptr);
    finish(h);
    if (!ok) ESP_LOGW(TAG, "POST %s: status %d, %s", url, out->status, esp_err_to_name(out->err));
    return ok;
}
