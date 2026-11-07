#include "fw_catalog.h"

#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "fw_http.h"
#include "fw_image_desc.h"
#include "psram_json_allocator.h"

static const char* TAG = "fw_catalog";
/* 16 kits x 8 versions is ~45 kB of JSON at most; real catalogs are a few kB. */
static constexpr size_t kCatalogMaxBytes = 64 * 1024;
static constexpr size_t kUrlMax = 192;
static constexpr int kStatusOk = 200;
static constexpr int kStatusUnauthorized = 401;
static constexpr int kCacheVersion = 1;
/* The card is shared with the sample streamer: write in pieces and give way between them. */
static constexpr size_t kCacheChunk = 4096;
extern void sample_player_port_yield_to_streamer(uint32_t weight);
static constexpr char kCardDir[] = "/sdcard/crosspad";
static constexpr char kCacheTmp[] = FW_CATALOG_CACHE ".tmp";

const char* fw_catalog_fetch_name(FwCatalogFetch r) {
    switch (r) {
        case FwCatalogFetch::Ok:      return "ok";
        case FwCatalogFetch::Network: return "network";
        case FwCatalogFetch::Http:    return "http";
        case FwCatalogFetch::Parse:   return "parse";
        case FwCatalogFetch::NoMem:   return "no_mem";
        case FwCatalogFetch::Cancelled: return "cancelled";
    }
    return "?";
}

static bool ensure_dir(const char* path) { return mkdir(path, 0755) == 0 || errno == EEXIST; }

static bool write_chunked(FILE* f, const char* body, size_t len) {
    for (size_t off = 0; off < len; off += kCacheChunk) {
        const size_t n = len - off < kCacheChunk ? len - off : kCacheChunk;
        if (fwrite(body + off, 1, n, f) != n) return false;
        sample_player_port_yield_to_streamer(1);
    }
    return true;
}

/* Written under a temporary name and renamed: a card pulled mid-write never
 * leaves half a cache behind. Best effort -- the check itself succeeded. */
static void write_cache(const char* body, size_t len, int64_t fetched, unsigned rev) {
    if (!ensure_dir(kCardDir) || !ensure_dir(FW_PKG_ROOT)) return;
    FILE* f = fopen(kCacheTmp, "wb");
    if (!f) return;
    const bool wrote = fprintf(f, "{\"v\":%d,\"fetched\":%lld,\"rev\":\"v%u\",\"catalog\":", kCacheVersion,
                               static_cast<long long>(fetched), rev) > 0 &&
                       write_chunked(f, body, len) && fputs("}\n", f) >= 0;
    if (fclose(f) != 0 || !wrote) {
        unlink(kCacheTmp);
        ESP_LOGW(TAG, "catalog cache not written");
        return;
    }
    unlink(FW_CATALOG_CACHE);   // FATFS rename refuses an existing destination
    if (rename(kCacheTmp, FW_CATALOG_CACHE) != 0) ESP_LOGW(TAG, "catalog cache not renamed");
}

FwCatalogFetch fw_catalog_fetch(const char* baseUrl, crosspad::FwCatalog& out, int64_t* checkedAt,
                                bool (*cancel)()) {
    out.kitCount = 0;
    if (checkedAt) *checkedAt = 0;
    const unsigned rev = fw_image_desc_running()->board_rev;
    char url[kUrlMax];
    snprintf(url, sizeof url, "%s/api/device/catalog?rev=v%u", baseUrl, rev);
    char* body = static_cast<char*>(heap_caps_malloc(kCatalogMaxBytes, MALLOC_CAP_SPIRAM));
    if (!body) return FwCatalogFetch::NoMem;
    size_t len = 0;
    FwHttpReply reply;
    bool got = fw_http_get_buffer(url, body, kCatalogMaxBytes, &len, &reply, cancel);
    // Unlinked on the website: the 401 cleared the token, so the official catalog comes without it.
    if (!got && reply.status == kStatusUnauthorized && reply.authSent)
        got = fw_http_get_buffer(url, body, kCatalogMaxBytes, &len, &reply, cancel);
    FwCatalogFetch result = FwCatalogFetch::Ok;
    if (!got && cancel && reply.err == ESP_ERR_INVALID_STATE) {
        result = FwCatalogFetch::Cancelled;
    } else if (!got) {
        const bool overCap = reply.err == ESP_ERR_INVALID_SIZE;
        if (overCap) ESP_LOGW(TAG, "catalog body is over %u bytes, refused", static_cast<unsigned>(kCatalogMaxBytes));
        if (reply.status != 0 && reply.status != kStatusOk) result = FwCatalogFetch::Http;
        else if (overCap) result = FwCatalogFetch::Parse;   // a body that does not parse as a catalog
        else result = FwCatalogFetch::Network;              // no response, or a 200 cut off mid-body
    } else if (!crosspad::parseFwCatalog(body, len, out, PsramJsonAllocator::instance())) {
        result = FwCatalogFetch::Parse;
    } else if (cancel && cancel()) {
        result = FwCatalogFetch::Cancelled;   // abandoned during the parse: the cache stays as it was
    } else {
        write_cache(body, len, reply.dateUnix, rev);
    }
    heap_caps_free(body);
    if (checkedAt) *checkedAt = reply.dateUnix;
    ESP_LOGI(TAG, "%s: %s, %u kits, %u B", url, fw_catalog_fetch_name(result), static_cast<unsigned>(out.kitCount),
             static_cast<unsigned>(len));
    return result;
}

bool fw_catalog_file_url(const char* baseUrl, const char* tag, const char* name, char* out, size_t n) {
    return crosspad::fwCatalogFileUrl(baseUrl, tag, name, out, n);
}
