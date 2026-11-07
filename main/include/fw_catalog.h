#pragma once
#include <stddef.h>
#include <stdint.h>
#include "crosspad/fw/FwCatalog.hpp"
#include "fw_library.h"

/* crosspad.app's /api/device/catalog for this board's revision
 * (docs/subsystems/firmware-updates.md). Blocking: a held WiFi lease, the
 * updater's job task, never the LVGL thread or the CDC drain loop. */

/* The last good fetch, for normal mode (C's Store/Updates views show it aged):
 * {"v":1,"fetched":<unix>,"rev":"v<N>","catalog":<the Worker's JSON verbatim>}.
 * A dot name: fw_library_scan() and the cleanup skip it. */
#define FW_CATALOG_CACHE FW_PKG_ROOT "/.catalog.json"

enum class FwCatalogFetch : uint8_t { Ok, Network, Http, Parse, NoMem, Cancelled };
const char* fw_catalog_fetch_name(FwCatalogFetch r);   // "ok" "network" "http" "parse" "no_mem" "cancelled"

/* `out` is ~50 kB: the caller keeps it in PSRAM. checkedAt gets the
 * response's Date header (unix seconds) or 0. A catalog that parses is also
 * written to FW_CATALOG_CACHE; a failed fetch leaves the cache alone. `cancel`
 * (when given) is asked between reads and once more before the cache is
 * written: true ends the fetch as Cancelled with nothing written. */
FwCatalogFetch fw_catalog_fetch(const char* baseUrl, crosspad::FwCatalog& out, int64_t* checkedAt,
                                bool (*cancel)() = nullptr);

/* <baseUrl>/api/device/file/<tag>/<name>; tag and name were checked to be
 * URL-safe by the catalog parser, so nothing is escaped. False when it did
 * not fit `n`: a cut-off URL names another file. */
bool fw_catalog_file_url(const char* baseUrl, const char* tag, const char* name, char* out, size_t n);
