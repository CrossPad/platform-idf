/**
 * @file fw_library.h
 * @brief Scans /sdcard/crosspad/firmware for installable packages and judges
 *        each against the running image (project, board revision, OTA slot
 *        size). Read-only: installing a package into the non-running OTA
 *        slot is fw_slots.h.
 *
 * Threading: fw_library_scan() opens up to 16 image files off the SD card —
 * call it off the LVGL thread and off the priority-1 CDC drain loop.
 * fw_library_find() only opens the one named directory, so it is lighter
 * than a full scan — but still opens a manifest and reads an image header
 * off the card, and the bench measured that alone as enough to overflow the
 * CDC drain loop's own stack (see fw_install_cmd_task() in hil_control.cpp,
 * which is why FW_INSTALL never calls fw_install_start() — and by extension
 * fw_library_find() — from that loop either).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "crosspad/fw/FwPackage.hpp"
#include "fw_image_desc.h"

#define FW_PKG_ROOT      "/sdcard/crosspad/firmware"
#define FW_PKG_NAME_LEN  48

/* A download (fw_download.h) puts .downloading in the directory before its
 * first file, writes every file as <file>.part and renames it when its
 * SHA-256 matches the signed manifest; .complete comes last, then
 * .downloading goes. A directory with .downloading and no .complete is a
 * download to resume (verdict Downloading), never installable. A package
 * with .pinned is never evicted by the space cleanup (fw_gc.h). */
#define FW_PKG_COMPLETE     ".complete"
#define FW_PKG_DOWNLOADING  ".downloading"
#define FW_PKG_PINNED       ".pinned"
#define FW_PKG_PART_SUFFIX  ".part"

using FwPkgVerdict = crosspad::FwPkgVerdict;

struct FwPackage {
    char     name[FW_PKG_NAME_LEN];
    char     version[32];       // the image's; the manifest's when there is no image
    char     kit[FW_KIT_ID_LEN];    // the image's CPFI (none: ""); the manifest's when there is no image
    uint8_t  boardRev;          // 0 = image without CPFI
    uint16_t stmProtoMin;
    uint8_t  assetsId[FW_ASSETS_ID_LEN];
    uint32_t imageSize;
    bool     hasAssets;
    bool     hasSettings;
    FwPkgVerdict verdict;
};

/** Scans FW_PKG_ROOT, one FwPackage per package directory (a loose *.bin at
 * the root is folded into its own directory first). Returns the count
 * written, capped at capacity. */
size_t fw_library_scan(FwPackage* out, size_t capacity);

/** Looks at the one package directory named `name`, without scanning the
 * rest of FW_PKG_ROOT. False if not found. */
bool fw_library_find(const char* name, FwPackage* out);

/** Builds "FW_PKG_ROOT/<pkg.name>/<file>" into buf. */
void fw_library_path(const FwPackage& pkg, const char* file, char* buf, size_t len);

/** "<kit>-<version>" -- the directory a catalog version downloads into. */
void fw_library_package_name(const char* kit, const char* version, char* out, size_t n);

/** True when FW_PKG_ROOT/<name>/.complete exists: a download finished and verified. */
bool fw_library_is_complete(const char* name);

/** Deletes FW_PKG_ROOT/<name> and the files in it (packages are flat). True
 * when it is gone. SD-heavy: never on the LVGL thread or the CDC drain loop. */
bool fw_library_remove(const char* name);

/** Bytes of the regular files in FW_PKG_ROOT/<name>; 0 when it does not exist. */
uint64_t fw_library_package_bytes(const char* name);
