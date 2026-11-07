#include "include/fw_library.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#include "crosspad/fw/FwDownloader.hpp"
#include "crosspad/fw/FwManifest.hpp"
#include "psram_json_allocator.h"

static const char* TAG = "fw_library";

/* "<FW_PKG_ROOT>/<name>" — a package's own directory. */
#define FW_PKG_DIR_MAX (sizeof(FW_PKG_ROOT) + FW_PKG_NAME_LEN)

/* A package directory joined with the longest filename this module reads
 * from it ("settings.json"), plus the separating '/' and a NUL. Sized off
 * FW_PKG_DIR_MAX rather than shared with it, so appending a filename onto a
 * full-length directory path can never be reported as possible truncation. */
#define FW_PKG_PATH_MAX (FW_PKG_DIR_MAX + 1 + sizeof("settings.json"))

/* "<FW_PKG_ROOT>/<readdir() entry>" — dirent::d_name is declared char[256]
 * (NAME_MAX + 1), so this join has to fit that, not just FW_PKG_NAME_LEN. */
#define FW_PKG_ENTRY_MAX (sizeof(FW_PKG_ROOT) + 1 + 256)

/* Not declared in a header (sample_player_port.cpp and SamplerPlatformPort.cpp both forward-
 * declare it locally); every bulk reader of the card yields to the streamer
 * between pieces of work. */
extern void sample_player_port_yield_to_streamer(uint32_t weight);

static FwPkgVerdict judge(const char* imagePath, const esp_app_desc_t& app, bool isImage,
                          bool hasDesc, const fw_image_desc_t& desc, uint32_t size, uint32_t slotSize) {
    if (access(imagePath, F_OK) != 0)                                   return FwPkgVerdict::NoImage;
    if (!isImage)                                                       return FwPkgVerdict::NotAnImage;
    if (strcmp(app.project_name, esp_app_get_description()->project_name) != 0)
                                                                        return FwPkgVerdict::WrongProject;
    if (size > slotSize)                                                return FwPkgVerdict::TooLarge;
    if (!hasDesc)                                                       return FwPkgVerdict::UnknownBuild;
    if (desc.board_rev != fw_image_desc_running()->board_rev)           return FwPkgVerdict::WrongBoard;
    return FwPkgVerdict::Ok;
}

void fw_library_path(const FwPackage& pkg, const char* file, char* buf, size_t len)
{
    snprintf(buf, len, "%s/%s/%s", FW_PKG_ROOT, pkg.name, file);
}

void fw_library_package_name(const char* kit, const char* version, char* out, size_t n)
{
    crosspad::fwPackageName(kit, version, out, n);
}

bool fw_library_is_complete(const char* name)
{
    char path[FW_PKG_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s/%s", FW_PKG_ROOT, name, FW_PKG_COMPLETE);
    return access(path, F_OK) == 0;
}

/* A download in flight: .downloading and no .complete yet. Such a directory
 * is a download to resume, not a package to judge or install. */
static bool downloading(const char* dirPath)
{
    char marker[FW_PKG_PATH_MAX];
    snprintf(marker, sizeof(marker), "%s/%s", dirPath, FW_PKG_COMPLETE);
    if (access(marker, F_OK) == 0) return false;
    snprintf(marker, sizeof(marker), "%s/%s", dirPath, FW_PKG_DOWNLOADING);
    return access(marker, F_OK) == 0;
}

/* A manifest is a few hundred bytes; one that fills this is not one. */
static constexpr size_t kManifestBytesMax = 4096;

/* No image to read them from (a download that has not reached it, a package
 * made by hand): the manifest names the kit and version, under core's own
 * rules for it (parseFwManifest, the download's parser). The buffer and the
 * parse result live in PSRAM: some callers run on small internal stacks. */
static void kit_version_from_manifest(const char* dirPath, FwPackage* pkg)
{
    char path[FW_PKG_PATH_MAX];
    snprintf(path, sizeof(path), "%s/manifest.json", dirPath);
    FILE* f = fopen(path, "rb");
    if (!f) return;
    auto* buf = static_cast<char*>(heap_caps_malloc(kManifestBytesMax, MALLOC_CAP_SPIRAM));
    auto* m = static_cast<crosspad::FwManifest*>(heap_caps_malloc(sizeof(crosspad::FwManifest), MALLOC_CAP_SPIRAM));
    const size_t n = buf && m ? fread(buf, 1, kManifestBytesMax, f) : 0;
    fclose(f);
    if (n > 0 && n < kManifestBytesMax &&
        crosspad::parseFwManifest(buf, n, *m, PsramJsonAllocator::instance())) {
        strlcpy(pkg->kit, m->kit, sizeof(pkg->kit));
        strlcpy(pkg->version, m->version, sizeof(pkg->version));
    }
    heap_caps_free(buf);
    heap_caps_free(m);
}

static void fill_package(const char* dirPath, const char* dirName, FwPackage* pkg)
{
    memset(pkg, 0, sizeof(*pkg));
    strlcpy(pkg->name, dirName, sizeof(pkg->name));

    char imagePath[FW_PKG_PATH_MAX];
    snprintf(imagePath, sizeof(imagePath), "%s/CrossPad.bin", dirPath);

    esp_app_desc_t app = {};
    fw_image_desc_t desc = {};
    bool hasDesc = false;
    bool isImage = fw_image_desc_read_file(imagePath, &app, &desc, &hasDesc);
    if (isImage) strlcpy(pkg->version, app.version, sizeof(pkg->version));
    if (hasDesc) {
        strlcpy(pkg->kit, desc.kit_id, sizeof(pkg->kit));
        pkg->boardRev    = desc.board_rev;
        pkg->stmProtoMin = desc.stm_proto_min;
        memcpy(pkg->assetsId, desc.assets_id, sizeof(pkg->assetsId));
    }
    if (!isImage) kit_version_from_manifest(dirPath, pkg);

    struct stat st;
    uint32_t size = stat(imagePath, &st) == 0 ? (uint32_t)st.st_size : 0;
    pkg->imageSize = size;

    char sibling[FW_PKG_PATH_MAX];
    fw_library_path(*pkg, "assets.bin", sibling, sizeof(sibling));
    pkg->hasAssets = access(sibling, F_OK) == 0;
    fw_library_path(*pkg, "settings.json", sibling, sizeof(sibling));
    pkg->hasSettings = access(sibling, F_OK) == 0;

    const esp_partition_t* slot = esp_ota_get_next_update_partition(nullptr);
    const uint32_t slotSize = slot ? slot->size : 0;
    pkg->verdict = judge(imagePath, app, isImage, hasDesc, desc, size, slotSize);
    if (downloading(dirPath)) pkg->verdict = FwPkgVerdict::Downloading;
}

/* Loose *.bin files folded into their own package directory before the real
 * scan. Bounded independently of `capacity`: the fold has to run to
 * completion, with its own opendir()/closedir(), before the directory-only
 * pass below opens FW_PKG_ROOT again — see fw_library_scan(). */
static constexpr size_t kMaxLooseFiles = 16;

/* dirent::d_name is declared char[256] in this toolchain (NAME_MAX+1), same
 * reasoning as FW_PKG_ENTRY_MAX: the loose-file name is kept in full (not
 * just its FW_PKG_NAME_LEN-truncated stem) so the fold below renames the
 * exact file readdir() reported, whatever its extension's case. */
static constexpr size_t kLooseNameLen = 256;

static void fold_loose_files()
{
    /* PSRAM, not the stack: the 4 kB of names sat under every FATFS call
     * below and overflowed the Firmware views' 8 kB worker (fw_view_data.cpp). */
    auto* looseName = static_cast<char (*)[kLooseNameLen]>(
        heap_caps_malloc(kMaxLooseFiles * kLooseNameLen, MALLOC_CAP_SPIRAM));
    if (!looseName) {
        ESP_LOGW(TAG, "loose images not folded: no memory");
        return;
    }
    size_t looseCount = 0;
    {
        DIR* d = opendir(FW_PKG_ROOT);
        if (!d) {
            heap_caps_free(looseName);
            return;
        }
        struct dirent* ent;
        while (looseCount < kMaxLooseFiles && (ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            const char* dot = strrchr(ent->d_name, '.');
            if (!dot || strcasecmp(dot, ".bin") != 0) continue;

            char entryPath[FW_PKG_ENTRY_MAX];
            snprintf(entryPath, sizeof(entryPath), "%s/%s", FW_PKG_ROOT, ent->d_name);
            struct stat st;
            if (stat(entryPath, &st) != 0 || !S_ISREG(st.st_mode)) continue;

            strlcpy(looseName[looseCount], ent->d_name, kLooseNameLen);
            looseCount++;
        }
        closedir(d);
    }

    for (size_t i = 0; i < looseCount; i++) {
        /* A local copy, not looseName[i] directly: GCC cannot bound a %s
         * read through a runtime-indexed array element and assumes the
         * worst case across the whole backing array — same reasoning as
         * fill_package()'s callers taking a plain local instead of pkgs[i]. */
        char name[kLooseNameLen];
        strlcpy(name, looseName[i], sizeof(name));

        const char* dot = strrchr(name, '.');
        size_t stemLen = (size_t)(dot - name);
        char dirName[FW_PKG_NAME_LEN];
        if (stemLen >= sizeof(dirName)) stemLen = sizeof(dirName) - 1;
        memcpy(dirName, name, stemLen);
        dirName[stemLen] = '\0';

        char dirPath[FW_PKG_DIR_MAX];
        snprintf(dirPath, sizeof(dirPath), "%s/%s", FW_PKG_ROOT, dirName);
        if (mkdir(dirPath, 0755) != 0 && errno != EEXIST) continue;

        char entryPath[FW_PKG_ENTRY_MAX];
        snprintf(entryPath, sizeof(entryPath), "%s/%s", FW_PKG_ROOT, name);
        char dest[FW_PKG_PATH_MAX];
        snprintf(dest, sizeof(dest), "%s/CrossPad.bin", dirPath);
        rename(entryPath, dest);
    }
    heap_caps_free(looseName);
}

size_t fw_library_scan(FwPackage* out, size_t capacity)
{
    /* A loose *.bin's mkdir()+rename() must not run inside the same
     * opendir()/readdir() pass that lists FW_PKG_ROOT: ESP-IDF's FAT
     * readdir() is position-based, so a directory created mid-scan can be
     * handed back again later in the same pass — a duplicate row and an
     * inflated count. Folding is therefore its own pass, fully closed out,
     * before this one opens FW_PKG_ROOT fresh and reads directories only. */
    fold_loose_files();

    size_t n = 0;
    DIR* d = opendir(FW_PKG_ROOT);
    if (!d) return 0;

    struct dirent* ent;
    while (n < capacity && (ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;

        char entryPath[FW_PKG_ENTRY_MAX];
        snprintf(entryPath, sizeof(entryPath), "%s/%s", FW_PKG_ROOT, ent->d_name);

        struct stat st;
        if (stat(entryPath, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        char dirName[FW_PKG_NAME_LEN];
        if (strlcpy(dirName, ent->d_name, sizeof(dirName)) >= sizeof(dirName)) {
            /* Never truncated: everything below judges the package through
             * `entryPath` (the real directory) but every later call — the
             * install included — finds it again by this name, so a truncated
             * one would install a different image than the one listed. */
            ESP_LOGW(TAG, "firmware: skipping '%s', a package name is at most %d characters",
                     ent->d_name, (int)sizeof(dirName) - 1);
            continue;
        }

        fill_package(entryPath, dirName, &out[n]);
        n++;
        sample_player_port_yield_to_streamer(3);
    }
    closedir(d);
    return n;
}

bool fw_library_find(const char* name, FwPackage* out)
{
    /* One directory, not a 16-entry scan: fw_library_scan()'s stack
     * FwPackage[16] is 2.2 kB, and fw_install_start() (fw_slots.cpp) already
     * needs a dedicated task just to call this — even one FwPackage plus
     * fill_package()'s own locals (esp_app_desc_t alone is 256 B) is too much
     * for the CDC drain loop's own stack; see fw_install_cmd_task() in
     * hil_control.cpp. This also means a loose *.bin that has not yet been
     * scanned once will not be found by name — every package this installs
     * was already listed by FW_LIST, which folds it. */
    char boundedName[FW_PKG_NAME_LEN];
    strlcpy(boundedName, name, sizeof(boundedName));

    char dirPath[FW_PKG_DIR_MAX];
    snprintf(dirPath, sizeof(dirPath), "%s/%s", FW_PKG_ROOT, boundedName);

    struct stat st;
    if (stat(dirPath, &st) != 0 || !S_ISDIR(st.st_mode)) return false;

    fill_package(dirPath, boundedName, out);
    return true;
}

/* "<FW_PKG_DIR_MAX>/<readdir() entry>" -- d_name is char[256]. */
static constexpr size_t kPkgFilePathMax = FW_PKG_DIR_MAX + 1 + 256;
/* A package holds a handful of files; the bound only stops a runaway loop. */
static constexpr int kMaxPackageFiles = 64;

bool fw_library_remove(const char* name)
{
    char dirPath[FW_PKG_DIR_MAX];
    snprintf(dirPath, sizeof(dirPath), "%s/%s", FW_PKG_ROOT, name);
    /* One file per pass, each pass a fresh opendir(): FATFS readdir() is
     * position-based, and unlinking inside the pass that lists the directory
     * skips entries (the same trap fw_library_scan() works around). */
    for (int i = 0; i < kMaxPackageFiles; i++) {
        DIR* d = opendir(dirPath);
        if (!d) return errno == ENOENT;
        char victim[kPkgFilePathMax] = "";
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            snprintf(victim, sizeof(victim), "%s/%s", dirPath, ent->d_name);
            break;
        }
        closedir(d);
        if (!victim[0]) break;
        if (unlink(victim) != 0) {
            ESP_LOGW(TAG, "remove %s: %s", victim, strerror(errno));
            return false;
        }
        sample_player_port_yield_to_streamer(1);
    }
    return rmdir(dirPath) == 0;
}

uint64_t fw_library_package_bytes(const char* name)
{
    char dirPath[FW_PKG_DIR_MAX];
    snprintf(dirPath, sizeof(dirPath), "%s/%s", FW_PKG_ROOT, name);
    DIR* d = opendir(dirPath);
    if (!d) return 0;
    uint64_t sum = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        char path[kPkgFilePathMax];
        snprintf(path, sizeof(path), "%s/%s", dirPath, ent->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) sum += static_cast<uint64_t>(st.st_size);
    }
    closedir(d);
    return sum;
}
