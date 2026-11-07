/**
 * @file fw_download.cpp
 * @brief crosspad::FwDownloader on this board -- see fw_download.h. The
 *        decisions are core's; here are the card, the HTTP client, the SHA-256
 *        accelerator, the trusted keys, the cleanup and the blocklist's lock.
 */
#include "fw_download.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include "fw_gc.h"
#include "fw_http.h"
#include "fw_image_desc.h"
#include "fw_library.h"
#include "fw_updater.h"
#include "fw_verify.h"
#include "psram_json_allocator.h"

extern void sample_player_port_yield_to_streamer(uint32_t weight);

static const char* TAG = "fw_download";
static constexpr int kSha256NotSha224 = 0;
static constexpr char kCardDir[] = "/sdcard/crosspad";

static_assert(FW_PKG_NAME_LEN == crosspad::kFwPackageNameMax, "package names are core's");
static_assert(FWU_URL_LEN <= crosspad::kFwBaseUrlMax, "the base URL must fit core's file URL");
static_assert(sizeof(FW_PKG_ROOT) <= crosspad::kFwLibraryRootMax, "the library root must fit core's paths");
static_assert(sizeof(FW_PKG_PART_SUFFIX) <= crosspad::kFwPartSuffixMax, "the part suffix must fit core's paths");
static_assert(sizeof(FW_PKG_COMPLETE) <= sizeof(crosspad::FwManifestFile::name) &&
                  sizeof(FW_PKG_DOWNLOADING) <= sizeof(crosspad::FwManifestFile::name) &&
                  sizeof(FW_PKG_PINNED) <= sizeof(crosspad::FwManifestFile::name),
              "markers must fit core's paths");

static const crosspad::FwPackageLayout kLayout{
    kCardDir, FW_PKG_ROOT, FW_PKG_COMPLETE, FW_PKG_DOWNLOADING, FW_PKG_PINNED, FW_PKG_PART_SUFFIX,
};

static crosspad::FwBlocklist s_blocked;
static portMUX_TYPE s_blockedMux = portMUX_INITIALIZER_UNLOCKED;

bool fw_download_blocked(const char* kit, const char* version) {
    taskENTER_CRITICAL(&s_blockedMux);
    const bool hit = s_blocked.has(kit, version);
    taskEXIT_CRITICAL(&s_blockedMux);
    return hit;
}

static void block(const char* kit, const char* version) {
    crosspad::FwBlockedVersion forgotten{};
    bool dropped = false;
    taskENTER_CRITICAL(&s_blockedMux);
    const bool added = s_blocked.add(kit, version, &forgotten, &dropped);
    taskEXIT_CRITICAL(&s_blockedMux);
    if (!added) return;
    if (dropped) ESP_LOGW(TAG, "blocklist full: %s %s is no longer refused", forgotten.kit, forgotten.version);
    ESP_LOGE(TAG, "%s %s refused until reboot", kit, version);
}

namespace {

class EspSha256 final : public crosspad::ISha256 {
public:
    EspSha256() { mbedtls_sha256_init(&ctx_); }
    ~EspSha256() override { mbedtls_sha256_free(&ctx_); }
    void start() override {
        mbedtls_sha256_free(&ctx_);
        mbedtls_sha256_init(&ctx_);
        mbedtls_sha256_starts(&ctx_, kSha256NotSha224);
    }
    void update(const uint8_t* data, size_t n) override { mbedtls_sha256_update(&ctx_, data, n); }
    void finish(uint8_t digest[kDigestLen]) override {
        mbedtls_sha256_finish(&ctx_, digest);
        mbedtls_sha256_free(&ctx_);
    }

private:
    mbedtls_sha256_context ctx_;
};

class EspFwHttp final : public crosspad::IHttpStream {
public:
    bool getBuffer(const char* url, char* buf, size_t cap, size_t* len, crosspad::HttpStreamReply* out) override {
        FwHttpReply reply;
        const bool ok = fw_http_get_buffer(url, buf, cap, len, &reply);
        out->status = reply.status;
        out->rangeMismatch = reply.rangeMismatch;
        return ok;
    }
    bool getStream(const char* url, uint64_t offset, uint64_t expectedTotal, Sink sink, void* ctx, uint8_t* chunk,
                   size_t chunkLen, crosspad::HttpStreamReply* out) override {
        FwHttpReply reply;
        const bool ok = fw_http_get_stream(url, offset, expectedTotal, sink, ctx, chunk, chunkLen, &reply);
        out->status = reply.status;
        out->rangeMismatch = reply.rangeMismatch;
        return ok;
    }
};

class EspFwFiles final : public crosspad::IFwFiles {
public:
    void* open(const char* path, crosspad::FwFileMode mode) override {
        switch (mode) {
            case crosspad::FwFileMode::Read:   return fopen(path, "rb");
            case crosspad::FwFileMode::Write:  return fopen(path, "wb");
            case crosspad::FwFileMode::Append: return fopen(path, "ab");
        }
        return nullptr;
    }
    size_t read(void* f, void* buf, size_t n) override { return fread(buf, 1, n, static_cast<FILE*>(f)); }
    size_t write(void* f, const void* data, size_t n) override { return fwrite(data, 1, n, static_cast<FILE*>(f)); }
    bool close(void* f) override { return fclose(static_cast<FILE*>(f)) == 0; }
    uint64_t size(const char* path) override {
        struct stat st;
        return stat(path, &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
    }
    bool exists(const char* path) override { return access(path, F_OK) == 0; }
    bool makeDir(const char* path) override { return mkdir(path, 0755) == 0 || errno == EEXIST; }
    bool remove(const char* path) override { return unlink(path) == 0; }
    bool rename(const char* from, const char* to) override { return ::rename(from, to) == 0; }
};

crosspad::FwDlResult from_gc(FwGcResult r) {
    switch (r) {
        case FwGcResult::Ok:       return FwDlResult::Ok;
        case FwGcResult::CardFull: return FwDlResult::CardFull;
        case FwGcResult::NoMem:    return FwDlResult::NoMem;
        case FwGcResult::NoCard:   return FwDlResult::NoCard;
        case FwGcResult::Io:       return FwDlResult::Io;
    }
    return FwDlResult::Io;
}

/* The job task's stack is PSRAM: nothing here touches flash. */
class EspFwDownloadPlatform final : public crosspad::IFwDownloadPlatform {
public:
    void* allocBuffers(size_t bytes) override { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM); }
    void freeBuffers(void* p) override { heap_caps_free(p); }
    bool verifyManifest(const uint8_t* manifest, size_t manifestLen, const uint8_t* sig, size_t sigLen) override {
        return fw_verify_manifest(manifest, manifestLen, sig, sigLen);
    }
    uint8_t boardRev() override { return fw_image_desc_running()->board_rev; }
    ArduinoJson::Allocator* jsonAllocator() override { return PsramJsonAllocator::instance(); }
    uint64_t packageBytes(const char* name) override { return fw_library_package_bytes(name); }
    void removePackage(const char* name) override { fw_library_remove(name); }
    crosspad::FwDlResult cleanup(uint64_t incoming, const char* keep, const char* slotA, const char* slotB) override {
        return from_gc(fw_gc_run(incoming, keep, slotA, slotB));
    }
    bool isBlocked(const char* kit, const char* version) override { return fw_download_blocked(kit, version); }
    void block(const char* kit, const char* version) override { ::block(kit, version); }
    void yieldIo() override { sample_player_port_yield_to_streamer(1); }
    void sleepMs(uint32_t ms) override { vTaskDelay(pdMS_TO_TICKS(ms)); }
    void onEvent(const crosspad::FwDlEvent& e) override {
        using K = crosspad::FwDlEventKind;
        switch (e.kind) {
            case K::UnsafeName:
                ESP_LOGE(TAG, "%s: a catalog name is not safe for a path or URL", e.subject);
                break;
            case K::PartUnreadable:
                ESP_LOGW(TAG, "%s: unreadable, starting the file over", e.subject);
                break;
            case K::ResumeRefused:
                ESP_LOGW(TAG, "%s: resume at %llu B answered %d%s, starting the file over", e.subject,
                         static_cast<unsigned long long>(e.at), e.status, e.otherRange ? " with another range" : "");
                break;
            case K::RangeNotSatisfiable:
                ESP_LOGW(TAG, "%s: 416 at %llu B, starting the file over", e.subject,
                         static_cast<unsigned long long>(e.at));
                break;
            case K::Resuming:
                ESP_LOGW(TAG, "%s: ended at %llu of %u B, resuming", e.subject, static_cast<unsigned long long>(e.at),
                         static_cast<unsigned>(e.of));
                break;
            case K::ShaMismatch:
                ESP_LOGW(TAG, "%s: SHA-256 is not the signed one (try %d of %d)", e.subject, e.attempt, e.attempts);
                break;
            case K::NoLongerVerifies:
                ESP_LOGW(TAG, "%s: complete but no longer verifies, downloading it again", e.subject);
                break;
            case K::BadSignature:
                ESP_LOGE(TAG, "%s: manifest signature does not verify with the %d trusted key(s)", e.subject,
                         fw_verify_key_count());
                break;
            case K::NotThisBoard:
                ESP_LOGE(TAG, "%s: the signed manifest is not this board's %s %s", e.subject, e.kit, e.version);
                break;
            case K::Complete:
                ESP_LOGI(TAG, "%s complete and verified", e.subject);
                break;
        }
    }
};

} // namespace

FwDlResult fw_download_run(const char* baseUrl, const char* kit, const crosspad::FwCatalogVersion& v,
                           FwDlProgress& progress, const char* slotA, const char* slotB) {
    EspFwDownloadPlatform platform;
    EspFwFiles files;
    EspFwHttp http;
    EspSha256 sha;
    crosspad::FwDownloader dl(platform, files, http, sha, kLayout);
    return dl.run(baseUrl, kit, v, progress, slotA, slotB);
}

bool fw_download_verify_on_card(const char* kit, const char* version) {
    EspFwDownloadPlatform platform;
    EspFwFiles files;
    EspFwHttp http;
    EspSha256 sha;
    crosspad::FwDownloader dl(platform, files, http, sha, kLayout);
    return dl.verifyOnCard(kit, version);
}
