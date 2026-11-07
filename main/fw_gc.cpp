#include "fw_gc.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "crosspad/fw/FwLibraryGc.hpp"
#include "fw_image_desc.h"
#include "fw_library.h"

extern void sample_player_port_yield_to_streamer(uint32_t weight);

static const char* TAG = "fw_gc";
static constexpr const char* kCardRoot = "/sdcard";
static constexpr uint64_t kBytesPerMb = 1024ull * 1024ull;
/* "<FW_PKG_ROOT>/<name>" for a name that fit FwGcPackage::name, and a loose
 * file or directory entry as readdir() names it (d_name is char[256]). */
static constexpr size_t kPkgDirMax = sizeof(FW_PKG_ROOT) + 1 + sizeof(crosspad::FwGcPackage::name);
static constexpr size_t kPkgFileMax = kPkgDirMax + sizeof("/CrossPad.bin");
static constexpr size_t kDirentNameMax = 256;
static constexpr size_t kEntryPathMax = sizeof(FW_PKG_ROOT) + 1 + kDirentNameMax;

static std::atomic<uint32_t> s_limitMb{0};

void fw_gc_set_library_limit_mb(uint32_t mb) { s_limitMb.store(mb); }

uint32_t fw_gc_library_limit_mb(void) {
    const uint32_t mb = s_limitMb.load();
    return mb ? mb : static_cast<uint32_t>(crosspad::kFwLibraryMaxDefault / kBytesPerMb);
}

/* Kit and version from the image itself (CPFI), as fw_library judges it. */
static void describe(crosspad::FwGcPackage& p) {
    char image[kPkgFileMax];
    snprintf(image, sizeof image, "%s/%s/CrossPad.bin", FW_PKG_ROOT, p.name);
    esp_app_desc_t app;
    fw_image_desc_t desc;
    bool hasDesc = false;
    if (fw_image_desc_read_file(image, &app, &desc, &hasDesc)) strlcpy(p.version, app.version, sizeof p.version);
    if (hasDesc) strlcpy(p.kit, desc.kit_id, sizeof p.kit);
}

/* A directory is its own name; a loose image "<name>.bin" is the package <name>
 * (fw_library folds it into that directory). FATFS names compare without case. */
static bool same_name(const char* entry, bool isDir, const char* pkg) {
    if (!pkg) return false;
    const char* dot = strrchr(entry, '.');
    const size_t len = isDir || !dot ? strlen(entry) : static_cast<size_t>(dot - entry);
    return strlen(pkg) == len && strncasecmp(entry, pkg, len) == 0;
}

/* Bytes of the regular files in the directory at `path` (a flat package), 0
 * for anything else. Counts dot files too: the library limit is about the card. */
static uint64_t dir_bytes(const char* path) {
    DIR* d = opendir(path);
    if (!d) return 0;
    uint64_t sum = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        char file[kEntryPathMax + 1 + kDirentNameMax];
        snprintf(file, sizeof file, "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(file, &st) == 0 && S_ISREG(st.st_mode)) sum += static_cast<uint64_t>(st.st_size);
    }
    closedir(d);
    return sum;
}

FwGcResult fw_gc_run(uint64_t incoming, const char* keep, const char* slotA, const char* slotB) {
    uint64_t total = 0, freeBytes = 0;
    if (esp_vfs_fat_info(kCardRoot, &total, &freeBytes) != ESP_OK) {
        ESP_LOGW(TAG, "card size unknown");
        return FwGcResult::NoCard;
    }
    auto* pkgs = static_cast<crosspad::FwGcPackage*>(
        heap_caps_calloc(crosspad::kFwGcMaxPackages, sizeof(crosspad::FwGcPackage), MALLOC_CAP_SPIRAM));
    if (!pkgs) return FwGcResult::NoMem;
    bool isDir[crosspad::kFwGcMaxPackages] = {};
    size_t n = 0;
    /* Entries that cannot be candidates (past the cap, a name too long to keep
     * in a FwGcPackage, dot files, other files) still occupy the library. */
    uint64_t uncounted = 0;
    if (DIR* d = opendir(FW_PKG_ROOT)) {
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            char path[kEntryPathMax];
            snprintf(path, sizeof path, "%s/%s", FW_PKG_ROOT, ent->d_name);
            struct stat st;
            if (stat(path, &st) != 0) continue;
            const bool dir = S_ISDIR(st.st_mode);
            const uint64_t bytes = dir ? dir_bytes(path) : static_cast<uint64_t>(st.st_size);
            const char* dot = strrchr(ent->d_name, '.');
            const bool candidate = ent->d_name[0] != '.' && n < crosspad::kFwGcMaxPackages &&
                                   (dir || (dot && strcasecmp(dot, ".bin") == 0)) &&
                                   strlen(ent->d_name) < sizeof pkgs[n].name;
            if (!candidate) {
                uncounted += bytes;
                continue;
            }
            crosspad::FwGcPackage& p = pkgs[n];
            memset(&p, 0, sizeof p);
            strlcpy(p.name, ent->d_name, sizeof p.name);
            p.bytes = bytes;
            if (dir) {
                describe(p);
                char pin[kPkgFileMax + sizeof(FW_PKG_PINNED)];
                snprintf(pin, sizeof pin, "%s/%s/%s", FW_PKG_ROOT, p.name, FW_PKG_PINNED);
                p.isProtected = access(pin, F_OK) == 0;
                isDir[n] = true;
            }
            p.mtime = st.st_mtime;
            p.isProtected = p.isProtected || same_name(p.name, dir, keep) ||
                            same_name(p.name, dir, slotA) || same_name(p.name, dir, slotB);
            ++n;
            sample_player_port_yield_to_streamer(1);
        }
        closedir(d);
    }
    /* What cannot be evicted counts against the limit the candidates are held to. */
    const uint64_t limit = static_cast<uint64_t>(fw_gc_library_limit_mb()) * kBytesPerMb;
    const crosspad::FwGcLimits lim{limit > uncounted ? limit - uncounted : 0, total, freeBytes, incoming};
    const crosspad::FwGcPlan plan = crosspad::FwLibraryGc::select(pkgs, n, lim);
    if (!plan.satisfiable) {
        ESP_LOGW(TAG, "%llu B do not fit (library limit %u MB, card %llu/%llu B free) without a protected package",
                 static_cast<unsigned long long>(incoming), static_cast<unsigned>(fw_gc_library_limit_mb()),
                 static_cast<unsigned long long>(freeBytes), static_cast<unsigned long long>(total));
        heap_caps_free(pkgs);
        return FwGcResult::CardFull;
    }
    FwGcResult result = FwGcResult::Ok;
    for (size_t i = 0; i < plan.count; ++i) {
        const crosspad::FwGcPackage& p = pkgs[plan.evict[i]];
        bool ok;
        if (isDir[plan.evict[i]]) {
            ok = fw_library_remove(p.name);
        } else {
            char path[kPkgDirMax];
            snprintf(path, sizeof path, "%s/%s", FW_PKG_ROOT, p.name);
            ok = unlink(path) == 0;
        }
        ESP_LOGI(TAG, "evicted %s (%llu B)%s", p.name, static_cast<unsigned long long>(p.bytes),
                 ok ? "" : " -- delete failed");
        if (!ok) result = FwGcResult::Io;
    }
    heap_caps_free(pkgs);
    return result;
}
