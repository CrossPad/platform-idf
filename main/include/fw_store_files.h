#pragma once
#include <stddef.h>
#include <stdint.h>
#include "crosspad/fw/FwCatalog.hpp"
#include "fw_library.h"

/* What the Firmware views read from the card besides B's catalog cache
 * (docs/subsystems/firmware-app.md): the Store's kit icons and every kit's
 * text, written by the catalog check while it holds its lease, so a view with
 * no WiFi still shows them. Dot names: fw_library_scan() and the cleanup skip them. */

#define FW_ICON_DIR    FW_PKG_ROOT "/.icons"
#define FW_DETAIL_DIR  FW_PKG_ROOT "/.details"
#define FW_PKG_ICON    "icon.bin"

using crosspad::kFwIconBytes;
using crosspad::kFwIconHeaderBytes;
using crosspad::kFwIconSide;
/* /api/device/kit/<kit>: 8 versions of at most ~1.4 kB of text each. */
constexpr size_t kFwDetailMax = 16 * 1024;

/* Magic, colour format, no flags, size, stride and length -- what LVGL will trust when
 * it draws the buffer in place. Anything else never reaches an lv_image_dsc_t. */
bool fw_icon_valid(const uint8_t *data, size_t len);

void fw_icon_cache_path(const char *kit, const char *version, char *out, size_t n);   /* FW_ICON_DIR/<kit>-<version>.bin */
void fw_detail_path(const char *kit, char *out, size_t n);                             /* FW_DETAIL_DIR/<kit>.json */

using FwIsBeta = bool (*)(const char *kit);
/* True when the job was abandoned (a background check the board's use cancelled)
 * or went stale (a factory reset): asked between blocking steps and before every write. */
using FwAbandoned = bool (*)();

/* B's check job, a lease held, after a good catalog fetch: the icon of every
 * Store tile's version (fwShownVersion) the card lacks, and the kit text of
 * every kit whose newest version is not in its detail file; then both
 * directories pruned to this catalog. Failures are skipped: the next check
 * retries. An abandoned pass writes and deletes nothing more. */
void fw_store_files_fetch(const char *baseUrl, const crosspad::FwCatalog &cat, FwIsBeta isBeta,
                          FwAbandoned abandoned);
