#pragma once
#include "crosspad/fw/FwCatalog.hpp"
#include "crosspad/fw/FwDownloader.hpp"

/* One catalog version -> /sdcard/crosspad/firmware/<kit>-<version>/ (spec §3):
 * crosspad::FwDownloader on the board's card, HTTP, SHA-256 accelerator and
 * the keys in main/fw_sign_pubkey.h. A bad signature or a second SHA mismatch
 * blocks the version until reboot. Blocking, SD- and network-heavy: the
 * updater's job task only. */

using FwDlResult = crosspad::FwDlResult;
using FwDlProgress = crosspad::FwDlProgress;
/* "ok" "no_card" "card_full" "network" "http" "signature" "mismatch" "sha" "io" "blocked" "no_mem" */
inline const char* fw_dl_result_name(FwDlResult r) { return crosspad::fwDlResultName(r); }

/* slotA / slotB: the package names in the two slots, which the cleanup keeps.
 * Every name taken from the catalog (kit, version, tag, file names) is checked
 * before it reaches a path or a URL; one that fails is Mismatch. */
FwDlResult fw_download_run(const char* baseUrl, const char* kit, const crosspad::FwCatalogVersion& v,
                           FwDlProgress& progress, const char* slotA, const char* slotB);

/* .complete is there and the manifest on the card still verifies and matches. */
bool fw_download_verify_on_card(const char* kit, const char* version);

bool fw_download_blocked(const char* kit, const char* version);
