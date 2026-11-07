#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* The firmware updater's HTTP: one GET into a buffer (catalog, manifest,
 * signature), one ranged GET into a sink (package files) and one JSON POST
 * (account pairing). https:// uses the certificate bundle; http:// is allowed
 * for a local test server -- integrity comes from the package signature, not the
 * transport. The crosspad.app device token is attached here and nowhere else,
 * only to URLs under CROSSPAD_ACCOUNT_URL (cp_account.h); a 401 to a request
 * that carried it unlinks the board. Blocking; the caller holds a WiFi lease
 * and runs on a PSRAM-stack job task. */

struct FwHttpReply {
    int status = 0;              // HTTP status, 0 = no response (DNS, TLS, socket)
    esp_err_t err = ESP_OK;
    int64_t contentLength = -1;
    int64_t dateUnix = 0;        // the response's Date header, 0 when absent or unreadable
    int64_t rangeStart = -1;     // Content-Range "bytes <start>-<end>/<total>": start, -1 when absent
    int64_t rangeTotal = -1;     // ... and total, -1 when absent or "*"
    bool rangeMismatch = false;  // a 206 whose range is not the one asked for (fw_http_get_stream)
    bool authSent = false;       // the request carried the device token (cp_account.h)
    uint32_t authGen = 0;        // ... of this link generation: a 401 for an older one unlinks nothing
};

/* Asked between reads: true abandons the transfer (out->err ESP_ERR_INVALID_STATE). */
using FwHttpCancel = bool (*)();

/* The whole 200 body into buf (NUL-terminated, at most cap-1 bytes). False on
 * any other status, a transport error, a body that does not fit, or `cancel`. */
bool fw_http_get_buffer(const char* url, char* buf, size_t cap, size_t* len, FwHttpReply* out,
                        FwHttpCancel cancel = nullptr);

/* Returns false to stop the transfer. */
using FwHttpSink = bool (*)(const uint8_t* data, size_t n, void* ctx);

/* The body from `offset` on ("Range: bytes=<offset>-") through `sink`, in
 * pieces of at most chunkLen. True only when the body arrived complete. A
 * server that answers a resume (offset > 0) with 200 sent the whole file:
 * then nothing reaches the sink, out->status is 200 and the result is false,
 * so a partial file is never followed by a whole one. Likewise a 206 whose
 * Content-Range does not start at `offset`, or whose total is not
 * `expectedTotal` (0 = not checked): nothing reaches the sink and
 * out->rangeMismatch is set. 416 and every other status are left in
 * out->status. */
bool fw_http_get_stream(const char* url, uint64_t offset, uint64_t expectedTotal, FwHttpSink sink, void* ctx,
                        uint8_t* chunk, size_t chunkLen, FwHttpReply* out);

/* POST `body` as application/json; the reply body into buf (NUL-terminated,
 * at most cap-1 bytes). True when a status line and a body that fit arrived,
 * whatever the status: the caller reads out->status. */
bool fw_http_post_json(const char* url, const char* body, char* buf, size_t cap, size_t* len, FwHttpReply* out);
