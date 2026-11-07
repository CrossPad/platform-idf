/**
 * @file fw_view_data.cpp
 * @brief The Firmware views' card work on one short-lived worker -- see fw_view_data.h.
 */
#include "fw_view_data.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "crosspad/fw/FwKit.hpp"
#include "fw_catalog.h"
#include "crosspad-gui/fw/fw_icons.h"
#include "fw_image_desc.h"
#include "fw_slots.h"
#include "fw_store_files.h"
#include "psram_json_allocator.h"

extern void sample_player_port_yield_to_streamer(uint32_t weight);

static const char* TAG = "fw_views";

static constexpr size_t kReqMax = 16;
static constexpr size_t kResMax = 16;
/* PSRAM: SD, FATFS and JSON only. Sized like the old app's fw_scan_task. */
static constexpr uint32_t kWorkerStackBytes = 8192;
static constexpr UBaseType_t kWorkerPrio = 1;
static constexpr BaseType_t kWorkerCore = 0;
static constexpr uint32_t kResultWaitMs = 20;
static constexpr size_t kManifestMax = 4096;
static constexpr size_t kDetailBufBytes = kFwDetailMax + 1;   // read_small() refuses a file that fills its buffer
static constexpr size_t kPathMax = 160;
static constexpr size_t kReadChunk = 512;
static constexpr uint32_t kYieldWeight = 1;
static constexpr char kCommunityOkFile[] = FW_PKG_ROOT "/.community_ok";
static constexpr char kCommunityOkTmp[] = FW_PKG_ROOT "/.community_ok.tmp";
static constexpr size_t kCommunityFileMax = kFwCommunityOkMax + 1;   // the list and its newline
static constexpr char kListSeparators[] = " \n";   // one line now; files from before have a kit a line
static constexpr char kPinnedBody[] = "1\n";

enum class Req : uint8_t { Scan, Store, Detail, Icon, Pin, Unpin, Remove, Ack };

struct Request {
    Req kind;
    uint32_t ticket;   // icon ticket / detail request number
    char kit[FW_KIT_ID_LEN];
    char version[32];
    char pkg[FW_PKG_NAME_LEN];
};

struct Result {
    Req kind;
    uint32_t ticket;
    void* payload;     // heap_caps allocation, or nullptr
    crosspad::FwStoreLoad load;
};

struct Ctx {
    Request req[kReqMax];
    size_t reqHead;
    size_t reqCount;
    Result res[kResMax];
    size_t resCount;
    char removeSlots[2][FW_PKG_NAME_LEN];
    bool rescanWanted;   // a card write found the queue full: the scan goes in once a request leaves
    bool running;
    bool closing;
    bool open;   // between the first open and the last close (or the worker's exit after it)
    int owners;
    /* Published to the LVGL thread: only it reads or writes these. */
    FwLibrarySnap* lib;
    uint32_t libGen;
    crosspad::FwStore* store;
    crosspad::FwStoreLoad storeState;
    bool storeLoaded;
    uint32_t storeGen;
    crosspad::FwKitDetail* detail;
    uint32_t detailWant;
    uint32_t detailGen;
};

static SemaphoreHandle_t s_lock;   // created once and never deleted
/* Static, reset on open and never freed: a getter that tests `open` just as
 * the worker ends the last session reads stale fields, never freed memory. */
EXT_RAM_BSS_ATTR static Ctx s_ctx;

namespace {

struct Lock {
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};

/* An ArduinoJson reader over a FILE*, giving the sample streamer its turn between chunks. */
class FileReader {
public:
    explicit FileReader(FILE* f) : f_(f) {}
    int read() {
        if (pos_ == len_ && !refill()) return -1;
        return static_cast<unsigned char>(buf_[pos_++]);
    }
    size_t readBytes(char* out, size_t n) {
        size_t got = 0;
        while (got < n) {
            if (pos_ == len_ && !refill()) break;
            const size_t k = std::min(n - got, len_ - pos_);
            memcpy(out + got, buf_ + pos_, k);
            pos_ += k;
            got += k;
        }
        return got;
    }

private:
    bool refill() {
        len_ = fread(buf_, 1, sizeof buf_, f_);
        pos_ = 0;
        sample_player_port_yield_to_streamer(kYieldWeight);
        return len_ > 0;
    }
    FILE* f_;
    char buf_[kReadChunk];
    size_t pos_ = 0;
    size_t len_ = 0;
};

}  // namespace

static bool produces_result(Req k) { return k == Req::Scan || k == Req::Store || k == Req::Detail || k == Req::Icon; }

/* A package name from our own scan, but it builds paths: never a path itself. */
static bool safe_pkg(const char* name) { return name && name[0] && name[0] != '.' && !strchr(name, '/'); }

static bool exists(const char* path) { return access(path, F_OK) == 0; }

static size_t read_small(const char* path, char* buf, size_t cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    const size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n < cap ? n : 0;
}

static bool queued_locked(Req kind) {
    for (size_t i = 0; i < s_ctx.reqCount; ++i)
        if (s_ctx.req[(s_ctx.reqHead + i) % kReqMax].kind == kind) return true;
    return false;
}

static bool push_locked(const Request& r) {
    if (s_ctx.reqCount >= kReqMax) return false;
    s_ctx.req[(s_ctx.reqHead + s_ctx.reqCount) % kReqMax] = r;
    ++s_ctx.reqCount;
    return true;
}

/* The acknowledged kits, deduplicated, as many as fit `out`. A file longer than
 * the buffer (the append-only format before) keeps the kits before its cut.
 * False when a list is there but could not be read (out of file handles while
 * the sampler streams, say): a write must not replace what it never saw. */
static bool read_community_ok(char* out, size_t n) {
    out[0] = '\0';
    FILE* f = fopen(kCommunityOkFile, "rb");
    if (!f && errno == ENOENT) f = fopen(kCommunityOkTmp, "rb");   // cut between the remove and the rename
    if (!f) return errno == ENOENT;
    char buf[kCommunityFileMax + 1];
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    const bool cut = len == sizeof buf - 1 && fgetc(f) != EOF;
    const bool failed = ferror(f) != 0;
    fclose(f);
    if (failed) return false;
    buf[len] = '\0';
    if (cut)
        while (len && !strchr(kListSeparators, buf[len - 1])) buf[--len] = '\0';   // no half a kit id
    char* save = nullptr;
    for (char* kit = strtok_r(buf, kListSeparators, &save); kit; kit = strtok_r(nullptr, kListSeparators, &save))
        if (crosspad::fwKitIdValid(kit)) crosspad::fwBetaListSet(out, n, kit, true);
    return true;
}

static FwLibrarySnap* do_scan() {
    auto* pkgs = static_cast<FwPackage*>(heap_caps_calloc(kFwLibMax, sizeof(FwPackage), MALLOC_CAP_SPIRAM));
    auto* snap = static_cast<FwLibrarySnap*>(heap_caps_calloc(1, sizeof(FwLibrarySnap), MALLOC_CAP_SPIRAM));
    auto* manifest = static_cast<char*>(heap_caps_malloc(kManifestMax, MALLOC_CAP_SPIRAM));
    auto* detail = static_cast<crosspad::FwKitDetail*>(heap_caps_malloc(sizeof(crosspad::FwKitDetail), MALLOC_CAP_SPIRAM));
    if (!pkgs || !snap || !manifest || !detail) {
        heap_caps_free(pkgs);
        heap_caps_free(snap);
        heap_caps_free(manifest);
        heap_caps_free(detail);
        ESP_LOGW(TAG, "library scan: no memory");
        return nullptr;
    }
    const size_t n = fw_library_scan(pkgs, kFwLibMax);
    for (size_t i = 0; i < n; ++i) {
        const FwPackage& p = pkgs[i];
        FwLibItem& it = snap->items[snap->count++];
        strlcpy(it.name, p.name, sizeof it.name);
        strlcpy(it.kit, p.kit, sizeof it.kit);
        strlcpy(it.version, p.version[0] == 'v' ? p.version + 1 : p.version, sizeof it.version);
        it.verdict = p.verdict;
        it.complete = p.verdict != FwPkgVerdict::Downloading;
        it.onCard = true;
        it.stmProtoOlder = fw_package_stm_proto_older(p);   // stm32_hal's cached link info: RAM only
        char path[kPathMax];
        snprintf(path, sizeof path, "%s/%s/%s", FW_PKG_ROOT, p.name, FW_PKG_PINNED);
        it.pinned = exists(path);
        snprintf(path, sizeof path, "%s/%s/%s", FW_PKG_ROOT, p.name, FW_PKG_ICON);
        if (exists(path)) {
            it.icon = FwIconSrc::Package;
        } else if (it.kit[0]) {
            fw_icon_cache_path(it.kit, it.version, path, sizeof path);
            it.icon = exists(path) ? FwIconSrc::Cache : FwIconSrc::None;
        }
        snprintf(path, sizeof path, "%s/%s/manifest.json", FW_PKG_ROOT, p.name);
        const size_t len = read_small(path, manifest, kManifestMax);
        if (len && crosspad::parseFwManifestDetail(manifest, len, *detail, PsramJsonAllocator::instance())) {
            crosspad::fwCopyText(it.displayName, sizeof it.displayName, detail->displayName);
            it.community = detail->tier == crosspad::FwTier::Community;
        }
        if (!it.displayName[0]) crosspad::fwCopyText(it.displayName, sizeof it.displayName, it.kit[0] ? it.kit : it.name);
        it.bytes = fw_library_package_bytes(p.name);
        snap->bytes += it.bytes;
        sample_player_port_yield_to_streamer(kYieldWeight);
    }
    read_community_ok(snap->communityOk, sizeof snap->communityOk);
    heap_caps_free(pkgs);
    heap_caps_free(manifest);
    heap_caps_free(detail);
    return snap;
}

static crosspad::FwStore* do_store(crosspad::FwStoreLoad* load) {
    FILE* f = fopen(FW_CATALOG_CACHE, "rb");
    if (!f) {
        *load = crosspad::FwStoreLoad::Missing;
        return nullptr;
    }
    auto* store = static_cast<crosspad::FwStore*>(heap_caps_calloc(1, sizeof(crosspad::FwStore), MALLOC_CAP_SPIRAM));
    if (!store) {
        fclose(f);
        *load = crosspad::FwStoreLoad::Format;
        return nullptr;
    }
    FileReader reader(f);
    *load = crosspad::parseFwStoreCacheFrom(reader, fw_image_desc_running()->board_rev, *store,
                                            PsramJsonAllocator::instance());
    fclose(f);
    if (*load != crosspad::FwStoreLoad::Ok) {
        heap_caps_free(store);
        return nullptr;
    }
    return store;
}

static crosspad::FwKitDetail* do_detail(const Request& r) {
    auto* d = static_cast<crosspad::FwKitDetail*>(heap_caps_calloc(1, sizeof(crosspad::FwKitDetail), MALLOC_CAP_SPIRAM));
    auto* cat = static_cast<crosspad::FwKitDetail*>(heap_caps_calloc(1, sizeof(crosspad::FwKitDetail), MALLOC_CAP_SPIRAM));
    // One buffer, the manifest and then the detail file. The detail file is read
    // whole (fw_store_files writes at most kFwDetailMax) so it goes through the
    // buffer parser that is linked already, not a second stream instantiation.
    auto* buf = static_cast<char*>(heap_caps_malloc(kDetailBufBytes, MALLOC_CAP_SPIRAM));
    if (!d || !cat || !buf) {
        heap_caps_free(cat);
        heap_caps_free(buf);
        return d;   // an empty text (or nullptr): the card stops waiting either way
    }
    char path[kPathMax];
    if (safe_pkg(r.pkg)) {
        snprintf(path, sizeof path, "%s/%s/manifest.json", FW_PKG_ROOT, r.pkg);
        const size_t len = read_small(path, buf, kManifestMax);
        if (len) crosspad::parseFwManifestDetail(buf, len, *d, PsramJsonAllocator::instance());
    }
    if (crosspad::fwKitIdValid(r.kit)) {
        fw_detail_path(r.kit, path, sizeof path);
        const size_t len = read_small(path, buf, kDetailBufBytes);
        if (len && crosspad::parseFwKitDetail(buf, len, r.version, *cat, PsramJsonAllocator::instance()))
            crosspad::fwKitDetailMerge(*d, *cat);
    }
    heap_caps_free(cat);
    heap_caps_free(buf);
    return d;
}

static uint8_t* read_icon(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    // One byte more than an icon: a longer file must not pass as one.
    auto* b = static_cast<uint8_t*>(heap_caps_malloc(kFwIconBytes + 1, MALLOC_CAP_SPIRAM));
    const size_t n = b ? fread(b, 1, kFwIconBytes + 1, f) : 0;
    fclose(f);
    if (!b || !fw_icon_valid(b, n)) {
        heap_caps_free(b);
        return nullptr;
    }
    return b;
}

static uint8_t* do_icon(const Request& r) {
    char path[kPathMax];
    if (safe_pkg(r.pkg)) {
        snprintf(path, sizeof path, "%s/%s/%s", FW_PKG_ROOT, r.pkg, FW_PKG_ICON);
        if (uint8_t* b = read_icon(path)) return b;
    }
    if (crosspad::fwKitIdValid(r.kit) && r.version[0] && !strchr(r.version, '/')) {
        fw_icon_cache_path(r.kit, r.version, path, sizeof path);
        return read_icon(path);
    }
    return nullptr;
}

static void set_pinned(const char* pkg, bool pinned) {
    if (!safe_pkg(pkg)) return;
    char path[kPathMax];
    snprintf(path, sizeof path, "%s/%s/%s", FW_PKG_ROOT, pkg, FW_PKG_PINNED);
    if (!pinned) {
        unlink(path);
        return;
    }
    FILE* f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "pin %s: cannot write", pkg);
        return;
    }
    fwrite(kPinnedBody, 1, sizeof kPinnedBody - 1, f);
    fclose(f);
}

static void remove_pkg(const char* pkg, const char (*slots)[FW_PKG_NAME_LEN]) {
    if (!safe_pkg(pkg)) return;
    // The UI disables Delete for these; this is the line nothing crosses.
    // FATFS names: case-insensitive, as the UI compares them.
    if (strcasecmp(pkg, slots[0]) == 0 || strcasecmp(pkg, slots[1]) == 0) {
        ESP_LOGW(TAG, "remove %s refused: it is in a slot", pkg);
        return;
    }
    if (!fw_library_remove(pkg)) ESP_LOGW(TAG, "remove %s failed", pkg);
}

/* Rewritten whole from the deduplicated list, under a temporary name first:
 * appending grew the file past what the reader takes, and lost every kit then. */
static void write_community_ok(const char* kit) {
    char list[kFwCommunityOkMax];
    if (!read_community_ok(list, sizeof list)) {
        ESP_LOGW(TAG, "community list unreadable (errno %d): %s not remembered", errno, kit);
        return;
    }
    if (!crosspad::fwBetaListSet(list, sizeof list, kit, true)) {
        ESP_LOGW(TAG, "community list full: %s not remembered", kit);
        return;
    }
    FILE* f = fopen(kCommunityOkTmp, "wb");
    if (!f) return;
    const bool wrote = fprintf(f, "%s\n", list) > 0;
    if (fclose(f) != 0 || !wrote) {
        unlink(kCommunityOkTmp);
        return;
    }
    unlink(kCommunityOkFile);   // FATFS rename refuses an existing destination
    if (rename(kCommunityOkTmp, kCommunityOkFile) != 0) ESP_LOGW(TAG, "community list not saved");
}

static void* run_request(const Request& r, const char (*slots)[FW_PKG_NAME_LEN], crosspad::FwStoreLoad* load) {
    switch (r.kind) {
        case Req::Scan:   return do_scan();
        case Req::Store:  return do_store(load);
        case Req::Detail: return do_detail(r);
        case Req::Icon:   return do_icon(r);
        case Req::Pin:    set_pinned(r.pkg, true); break;
        case Req::Unpin:  set_pinned(r.pkg, false); break;
        case Req::Remove: remove_pkg(r.pkg, slots); break;
        case Req::Ack:    write_community_ok(r.kit); return nullptr;   // the published list already has it
    }
    Lock l;   // what the card holds changed: the views see it after one more scan
    if (!s_ctx.closing && !queued_locked(Req::Scan) && !push_locked(Request{Req::Scan, 0, {}, {}, {}}))
        s_ctx.rescanWanted = true;
    return nullptr;
}

static void deliver(const Request& r, void* payload, crosspad::FwStoreLoad load) {
    for (;;) {
        {
            Lock l;
            if (s_ctx.closing) {
                heap_caps_free(payload);
                return;
            }
            if (s_ctx.resCount < kResMax) {
                s_ctx.res[s_ctx.resCount++] = Result{r.kind, r.ticket, payload, load};
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kResultWaitMs));   // the views' timer drains these every 250 ms
    }
}

static void worker_task(void*) {
    for (;;) {
        Request r;
        char slots[2][FW_PKG_NAME_LEN];
        {
            Lock l;
            if (s_ctx.closing || s_ctx.reqCount == 0) {
                s_ctx.running = false;
                if (s_ctx.closing && s_ctx.owners == 0) s_ctx.open = false;
                break;
            }
            r = s_ctx.req[s_ctx.reqHead];
            s_ctx.reqHead = (s_ctx.reqHead + 1) % kReqMax;
            --s_ctx.reqCount;
            if (s_ctx.rescanWanted && (queued_locked(Req::Scan) || push_locked(Request{Req::Scan, 0, {}, {}, {}})))
                s_ctx.rescanWanted = false;
            memcpy(slots, s_ctx.removeSlots, sizeof slots);
        }
        crosspad::FwStoreLoad load = crosspad::FwStoreLoad::Ok;
        void* payload = run_request(r, slots, &load);
        if (produces_result(r.kind)) deliver(r, payload, load);
    }
    vTaskDeleteWithCaps(nullptr);   // created WithCaps: deleted WithCaps, or the stack leaks
}

static bool enqueue(const Request& r) {
    if (!s_lock) return false;
    bool spawn = false;
    {
        Lock l;
        if (!s_ctx.open || s_ctx.closing) return false;
        if ((r.kind == Req::Scan || r.kind == Req::Store) && queued_locked(r.kind)) return true;
        if (!push_locked(r)) {
            ESP_LOGW(TAG, "queue full, request %u dropped", static_cast<unsigned>(r.kind));
            return false;
        }
        if (!s_ctx.running) spawn = s_ctx.running = true;
    }
    if (spawn && xTaskCreatePinnedToCoreWithCaps(worker_task, "fw_views", kWorkerStackBytes, nullptr, kWorkerPrio,
                                                 nullptr, kWorkerCore, MALLOC_CAP_SPIRAM) != pdPASS) {
        Lock l;
        s_ctx.running = false;
        s_ctx.reqCount = 0;   // the worker was not running, so this request was the only one
        ESP_LOGW(TAG, "no memory for the worker");
        return false;
    }
    return true;
}

void fw_view_data_open(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    Lock l;
    if (!s_ctx.open) {
        memset(&s_ctx, 0, sizeof s_ctx);
        s_ctx.open = true;
    }
    s_ctx.closing = false;   // a previous session's worker may still be finishing: it keeps going
    ++s_ctx.owners;
}

void fw_view_data_close(void) {
    if (!s_lock) return;
    Lock l;
    if (!s_ctx.open || --s_ctx.owners > 0) return;
    for (size_t i = 0; i < s_ctx.resCount; ++i) heap_caps_free(s_ctx.res[i].payload);
    s_ctx.resCount = 0;
    s_ctx.reqCount = 0;
    s_ctx.rescanWanted = false;
    heap_caps_free(s_ctx.lib);
    heap_caps_free(s_ctx.store);
    heap_caps_free(s_ctx.detail);
    s_ctx.lib = nullptr;
    s_ctx.store = nullptr;
    s_ctx.detail = nullptr;
    s_ctx.storeLoaded = false;
    if (s_ctx.running) {
        s_ctx.closing = true;   // the worker frees the rest when its current item is done
    } else {
        s_ctx.open = false;
    }
}

bool fw_view_data_busy(void) {
    if (!s_lock) return false;
    Lock l;
    return s_ctx.open && !s_ctx.closing && (s_ctx.running || s_ctx.reqCount > 0 || s_ctx.resCount > 0);
}

void fw_view_data_poll(void) {
    if (!s_ctx.open) return;
    Result got[kResMax];
    size_t n = 0;
    {
        Lock l;
        n = s_ctx.resCount;
        memcpy(got, s_ctx.res, n * sizeof(Result));
        s_ctx.resCount = 0;
    }
    for (size_t i = 0; i < n; ++i) {
        const Result& r = got[i];
        switch (r.kind) {
            case Req::Scan:
                if (!r.payload) break;   // out of memory: the last scan stays
                heap_caps_free(s_ctx.lib);
                s_ctx.lib = static_cast<FwLibrarySnap*>(r.payload);
                ++s_ctx.libGen;
                break;
            case Req::Store:
                heap_caps_free(s_ctx.store);
                s_ctx.store = static_cast<crosspad::FwStore*>(r.payload);
                s_ctx.storeState = r.load;
                s_ctx.storeLoaded = true;
                ++s_ctx.storeGen;
                break;
            case Req::Detail:
                if (r.ticket != s_ctx.detailWant) {
                    heap_caps_free(r.payload);   // a card that has been closed or replaced asked for it
                    break;
                }
                heap_caps_free(s_ctx.detail);
                s_ctx.detail = static_cast<crosspad::FwKitDetail*>(r.payload);
                ++s_ctx.detailGen;
                break;
            case Req::Icon:
                fw_icons_deliver(r.ticket, static_cast<uint8_t*>(r.payload));
                break;
            default:
                heap_caps_free(r.payload);
                break;
        }
    }
}

void fw_view_data_scan_library(void) { enqueue(Request{Req::Scan, 0, {}, {}, {}}); }
FwLibrarySnap* fw_view_data_library(void) { return s_ctx.open ? s_ctx.lib : nullptr; }
uint32_t fw_view_data_library_gen(void) { return s_ctx.open ? s_ctx.libGen : 0; }

void fw_view_data_load_store(void) { enqueue(Request{Req::Store, 0, {}, {}, {}}); }

void fw_view_data_drop_store(void) {
    if (!s_ctx.open) return;
    heap_caps_free(s_ctx.store);
    s_ctx.store = nullptr;
    s_ctx.storeLoaded = false;
    ++s_ctx.storeGen;
}

const crosspad::FwStore* fw_view_data_store(void) { return s_ctx.open ? s_ctx.store : nullptr; }
crosspad::FwStoreLoad fw_view_data_store_state(void) { return s_ctx.open ? s_ctx.storeState : crosspad::FwStoreLoad::Missing; }
bool fw_view_data_store_loaded(void) { return s_ctx.open && s_ctx.storeLoaded; }
uint32_t fw_view_data_store_gen(void) { return s_ctx.open ? s_ctx.storeGen : 0; }

void fw_view_data_load_detail(const char* kit, const char* version, const char* pkg) {
    if (!s_ctx.open) return;
    heap_caps_free(s_ctx.detail);   // the previous card's text is not this one's
    s_ctx.detail = nullptr;
    Request r{Req::Detail, ++s_ctx.detailWant, {}, {}, {}};
    strlcpy(r.kit, kit ? kit : "", sizeof r.kit);
    strlcpy(r.version, version ? version : "", sizeof r.version);
    strlcpy(r.pkg, pkg ? pkg : "", sizeof r.pkg);
    enqueue(r);
}

const crosspad::FwKitDetail* fw_view_data_detail(void) { return s_ctx.open ? s_ctx.detail : nullptr; }
uint32_t fw_view_data_detail_gen(void) { return s_ctx.open ? s_ctx.detailGen : 0; }

void fw_view_data_set_pinned(const char* pkg, bool pinned) {
    Request r{pinned ? Req::Pin : Req::Unpin, 0, {}, {}, {}};
    strlcpy(r.pkg, pkg ? pkg : "", sizeof r.pkg);
    enqueue(r);
}

void fw_view_data_remove(const char* pkg, const char* slotA, const char* slotB) {
    if (!s_ctx.open) return;
    {
        Lock l;
        strlcpy(s_ctx.removeSlots[0], slotA ? slotA : "", FW_PKG_NAME_LEN);
        strlcpy(s_ctx.removeSlots[1], slotB ? slotB : "", FW_PKG_NAME_LEN);
    }
    Request r{Req::Remove, 0, {}, {}, {}};
    strlcpy(r.pkg, pkg ? pkg : "", sizeof r.pkg);
    enqueue(r);
}

void fw_view_data_ack_community(const char* kit) {
    if (!s_ctx.open || !crosspad::fwKitIdValid(kit)) return;
    if (s_ctx.lib) crosspad::fwBetaListSet(s_ctx.lib->communityOk, sizeof s_ctx.lib->communityOk, kit, true);
    Request r{Req::Ack, 0, {}, {}, {}};
    strlcpy(r.kit, kit, sizeof r.kit);
    enqueue(r);
}

bool fw_view_data_community_ok(const char* kit) {
    return s_ctx.open && s_ctx.lib && crosspad::fwBetaListHas(s_ctx.lib->communityOk, kit);
}

bool fw_view_data_request_icon(uint32_t ticket, const char* pkg, const char* kit, const char* version) {
    Request r{Req::Icon, ticket, {}, {}, {}};
    strlcpy(r.pkg, pkg ? pkg : "", sizeof r.pkg);
    strlcpy(r.kit, kit ? kit : "", sizeof r.kit);
    strlcpy(r.version, version ? version : "", sizeof r.version);
    return enqueue(r);
}

void fw_view_data_cancel_icon(uint32_t ticket) {
    if (!s_ctx.open || !ticket) return;
    Lock l;
    size_t kept = 0;
    for (size_t i = 0; i < s_ctx.reqCount; ++i) {
        const Request r = s_ctx.req[(s_ctx.reqHead + i) % kReqMax];
        if (r.kind == Req::Icon && r.ticket == ticket) continue;
        s_ctx.req[(s_ctx.reqHead + kept++) % kReqMax] = r;   // kept <= i: never a slot still to read
    }
    s_ctx.reqCount = kept;
}
