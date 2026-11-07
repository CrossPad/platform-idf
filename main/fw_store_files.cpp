/**
 * @file fw_store_files.cpp
 * @brief The Store's icons and kit text on the card -- see fw_store_files.h.
 */
#include "fw_store_files.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "crosspad/fw/FwStore.hpp"
#include "fw_catalog.h"
#include "fw_http.h"
#include "fw_image_desc.h"
#include "psram_json_allocator.h"

extern void sample_player_port_yield_to_streamer(uint32_t weight);

static const char* TAG = "fw_store_files";
static constexpr uint8_t kLvImageMagic = 0x19;    // LV_IMAGE_HEADER_MAGIC
static constexpr uint8_t kLvCfRgb565A8 = 0x14;    // LV_COLOR_FORMAT_RGB565A8
static constexpr uint16_t kIconStride = kFwIconSide * 2;
static constexpr size_t kOffsetW = 4;
static constexpr size_t kOffsetH = 6;
static constexpr size_t kOffsetFlags = 2;
static constexpr size_t kOffsetStride = 8;
static constexpr size_t kOffsetReserved = 10;
static constexpr size_t kPathMax = 128;
static constexpr size_t kUrlMax = 320;
static constexpr size_t kNameMax = 64;            // "<kit>-<version>.bin" and its ".tmp"
static constexpr size_t kChunk = 4096;            // what the card sees at a time, as everywhere else
static constexpr size_t kPruneBatch = 16;
static constexpr int kPrunePasses = 8;            // list, then delete: FATFS readdir() is positional
static constexpr uint32_t kYieldWeight = 1;
static constexpr char kTmpSuffix[] = ".tmp";

static uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

bool fw_icon_valid(const uint8_t* d, size_t len) {
    // tools/fw_icon.py writes no flags: a compressed, premultiplied or "allocated"
    // file came from somewhere else, and LVGL would read the buffer as that.
    return d && len == kFwIconBytes && d[0] == kLvImageMagic && d[1] == kLvCfRgb565A8 &&
           le16(d + kOffsetFlags) == 0 && le16(d + kOffsetW) == kFwIconSide && le16(d + kOffsetH) == kFwIconSide &&
           le16(d + kOffsetStride) == kIconStride && le16(d + kOffsetReserved) == 0;
}

void fw_icon_cache_path(const char* kit, const char* version, char* out, size_t n) {
    snprintf(out, n, "%s/%s-%s.bin", FW_ICON_DIR, kit, version);
}

void fw_detail_path(const char* kit, char* out, size_t n) { snprintf(out, n, "%s/%s.json", FW_DETAIL_DIR, kit); }

static bool ensure_dir(const char* path) { return mkdir(path, 0755) == 0 || errno == EEXIST; }

/* Under a temporary name, then renamed: a card pulled mid-write leaves the old file or none. */
static bool write_atomic(const char* path, const char* data, size_t len) {
    char tmp[kPathMax];
    snprintf(tmp, sizeof tmp, "%s%s", path, kTmpSuffix);
    FILE* f = fopen(tmp, "wb");
    if (!f) return false;
    bool wrote = true;
    for (size_t off = 0; wrote && off < len; off += kChunk) {
        const size_t n = len - off < kChunk ? len - off : kChunk;
        wrote = fwrite(data + off, 1, n, f) == n;
        sample_player_port_yield_to_streamer(kYieldWeight);
    }
    if (fclose(f) != 0 || !wrote) {
        unlink(tmp);
        return false;
    }
    unlink(path);   // FATFS rename refuses an existing destination
    return rename(tmp, path) == 0;
}

/* The whole file, or 0 when it is missing or does not fit under `cap`. */
static size_t read_file(const char* path, char* buf, size_t cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    size_t len = 0;
    for (;;) {
        const size_t room = cap - len;
        const size_t n = fread(buf + len, 1, room < kChunk ? room : kChunk, f);
        len += n;
        sample_player_port_yield_to_streamer(kYieldWeight);
        if (n == 0 || len == cap) break;
    }
    fclose(f);
    return len < cap ? len : 0;   // one that fills the buffer is not one this code wrote
}

static crosspad::FwChannel channel_of(const char* kit, FwIsBeta isBeta) {
    return isBeta(kit) ? crosspad::FwChannel::Beta : crosspad::FwChannel::Stable;
}

/* Deletes the files of `dir` for which keep() is false, `.tmp` leftovers included. */
static void prune(const char* dir, bool (*keep)(const char* name, const void* ctx), const void* ctx) {
    for (int pass = 0; pass < kPrunePasses; ++pass) {
        char victims[kPruneBatch][kNameMax];
        size_t n = 0;
        DIR* d = opendir(dir);
        if (!d) return;
        struct dirent* e;
        while (n < kPruneBatch && (e = readdir(d)) != nullptr) {
            if (e->d_name[0] == '.' || strlen(e->d_name) >= kNameMax) continue;
            if (!keep(e->d_name, ctx)) strlcpy(victims[n++], e->d_name, kNameMax);
        }
        closedir(d);
        if (n == 0) return;
        for (size_t i = 0; i < n; ++i) {
            char path[kPathMax];
            strlcpy(path, dir, sizeof path);
            strlcat(path, "/", sizeof path);
            strlcat(path, victims[i], sizeof path);
            unlink(path);
        }
    }
}

struct KeepCtx {
    const crosspad::FwCatalog* cat;
    FwIsBeta isBeta;
};

static bool keep_icon(const char* name, const void* ctx) {
    const auto* k = static_cast<const KeepCtx*>(ctx);
    for (size_t i = 0; i < k->cat->kitCount; ++i) {
        const crosspad::FwCatalogKit& kit = k->cat->kits[i];
        const int v = crosspad::fwShownVersion(kit, channel_of(kit.kit, k->isBeta));
        if (v < 0) continue;
        char want[kNameMax];
        snprintf(want, sizeof want, "%s-%s.bin", kit.kit, kit.versions[v].version);
        if (strcmp(want, name) == 0) return true;
    }
    return false;
}

static bool keep_detail(const char* name, const void* ctx) {
    const auto* k = static_cast<const KeepCtx*>(ctx);
    for (size_t i = 0; i < k->cat->kitCount; ++i) {
        char want[kNameMax];
        snprintf(want, sizeof want, "%s.json", k->cat->kits[i].kit);
        if (strcmp(want, name) == 0) return true;
    }
    return false;
}

/* Names that become part of a path or a URL, checked here as fw_download does: the catalog is unsigned. */
static bool names_safe(const crosspad::FwCatalogKit& kit, const crosspad::FwCatalogVersion& v) {
    return crosspad::fwKitIdValid(kit.kit) && crosspad::fwSafeName(v.version, sizeof v.version) &&
           crosspad::fwSafeName(v.tag, sizeof v.tag) && crosspad::fwSafeName(v.icon.name, sizeof v.icon.name);
}

static unsigned fetch_icons(const char* baseUrl, const crosspad::FwCatalog& cat, FwIsBeta isBeta,
                            FwAbandoned abandoned, char* buf) {
    unsigned fetched = 0;
    for (size_t i = 0; i < cat.kitCount && !abandoned(); ++i) {
        const crosspad::FwCatalogKit& kit = cat.kits[i];
        const int vi = crosspad::fwShownVersion(kit, channel_of(kit.kit, isBeta));
        if (vi < 0) continue;
        const crosspad::FwCatalogVersion& v = kit.versions[vi];
        if (!v.icon.name[0] || v.icon.size != kFwIconBytes || !names_safe(kit, v)) continue;
        char path[kPathMax];
        fw_icon_cache_path(kit.kit, v.version, path, sizeof path);
        if (access(path, F_OK) == 0) continue;
        char url[kUrlMax];
        if (!fw_catalog_file_url(baseUrl, v.tag, v.icon.name, url, sizeof url)) continue;
        size_t len = 0;
        FwHttpReply reply;
        // +1: fw_http_get_buffer() NUL-terminates, and a longer body must not fit.
        const bool got = fw_http_get_buffer(url, buf, kFwIconBytes + 1, &len, &reply, abandoned);
        if (abandoned()) break;
        if (got && fw_icon_valid(reinterpret_cast<const uint8_t*>(buf), len) && write_atomic(path, buf, len)) {
            ++fetched;
        } else {
            ESP_LOGW(TAG, "icon of %s %s not cached (status %d)", kit.kit, v.version, reply.status);
        }
        sample_player_port_yield_to_streamer(kYieldWeight);
    }
    return fetched;
}

/* The detail file is current when it already describes the kit's newest version. */
static bool detail_current(const char* path, const char* version, char* buf, crosspad::FwKitDetail* scratch) {
    // kFwDetailMax + 1, as the fetch and the card read: a file of exactly kFwDetailMax is whole.
    const size_t len = read_file(path, buf, kFwDetailMax + 1);
    return len > 0 && crosspad::parseFwKitDetail(buf, len, version, *scratch, PsramJsonAllocator::instance());
}

static unsigned fetch_details(const char* baseUrl, const crosspad::FwCatalog& cat, FwAbandoned abandoned,
                              char* buf, crosspad::FwKitDetail* scratch) {
    const unsigned rev = fw_image_desc_running()->board_rev;
    unsigned fetched = 0;
    for (size_t i = 0; i < cat.kitCount && !abandoned(); ++i) {
        const crosspad::FwCatalogKit& kit = cat.kits[i];
        const int newest = crosspad::fwShownVersion(kit, crosspad::FwChannel::Beta);
        if (newest < 0 || !crosspad::fwKitIdValid(kit.kit)) continue;
        char path[kPathMax];
        fw_detail_path(kit.kit, path, sizeof path);
        if (detail_current(path, kit.versions[newest].version, buf, scratch)) continue;
        char url[kUrlMax];
        snprintf(url, sizeof url, "%s/api/device/kit/%s?rev=v%u", baseUrl, kit.kit, rev);
        size_t len = 0;
        FwHttpReply reply;
        const bool got = fw_http_get_buffer(url, buf, kFwDetailMax + 1, &len, &reply, abandoned);
        if (abandoned()) break;
        if (!got) {
            ESP_LOGW(TAG, "kit text of %s not fetched (status %d)", kit.kit, reply.status);
            continue;
        }
        // The same parser the card read uses: it keeps the text only when the kit's newest version is in it.
        const bool ok = crosspad::parseFwKitDetail(buf, len, kit.versions[newest].version, *scratch,
                                                   PsramJsonAllocator::instance());
        if (ok && write_atomic(path, buf, len)) ++fetched;
        else ESP_LOGW(TAG, "kit text of %s refused", kit.kit);
        sample_player_port_yield_to_streamer(kYieldWeight);
    }
    return fetched;
}

void fw_store_files_fetch(const char* baseUrl, const crosspad::FwCatalog& cat, FwIsBeta isBeta,
                          FwAbandoned abandoned) {
    if (abandoned() || !ensure_dir(FW_ICON_DIR) || !ensure_dir(FW_DETAIL_DIR)) return;
    // One buffer for both: an icon (6.9 kB) and a detail file (<= 16 kB) are fetched one at a time.
    auto* buf = static_cast<char*>(heap_caps_malloc(kFwDetailMax + 1, MALLOC_CAP_SPIRAM));
    auto* scratch = static_cast<crosspad::FwKitDetail*>(heap_caps_malloc(sizeof(crosspad::FwKitDetail), MALLOC_CAP_SPIRAM));
    if (!buf || !scratch) {
        heap_caps_free(buf);
        heap_caps_free(scratch);
        ESP_LOGW(TAG, "no memory for the Store files");
        return;
    }
    const unsigned icons = fetch_icons(baseUrl, cat, isBeta, abandoned, buf);
    const unsigned details = fetch_details(baseUrl, cat, abandoned, buf, scratch);
    heap_caps_free(buf);
    heap_caps_free(scratch);
    if (abandoned()) return;   // a half-done pass prunes nothing
    const KeepCtx keep{&cat, isBeta};
    prune(FW_ICON_DIR, keep_icon, &keep);
    prune(FW_DETAIL_DIR, keep_detail, &keep);
    ESP_LOGI(TAG, "Store files: %u icons, %u kit texts fetched", icons, details);
}
