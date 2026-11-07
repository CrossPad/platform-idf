#pragma once
#include <stddef.h>
#include <stdint.h>
#include "crosspad/fw/FwKit.hpp"
#include "crosspad/fw/FwLibraryGc.hpp"
#include "crosspad/fw/FwStore.hpp"
#include "crosspad-gui/fw/IFwViewService.h"
#include "fw_library.h"

/**
 * @file fw_view_data.h
 * @brief The Firmware views' card work (docs/subsystems/firmware-app.md): what
 *        is on the card, the cached catalog, a kit card's text, icons, and the
 *        writes the views make (pin, delete, the community acknowledgement).
 *
 * Everything that touches the SD card runs on one worker task with a PSRAM
 * stack that exists only while there is work -- it never touches flash (no
 * NVS, no fw_slots_get()). Results are moved in by fw_view_data_poll() from
 * the views' LVGL timer; what a getter returns stays valid until the next
 * poll. LVGL thread only, except fw_view_data_busy(); getters only between
 * fw_view_data_open() and fw_view_data_close(). The board's side of
 * crosspad-gui's IFwViewService (fw_view_service.cpp), whose header has the
 * snapshot's types.
 */

using crosspad_gui::FwIconSrc;
using crosspad_gui::FwLibItem;
using crosspad_gui::FwLibrarySnap;
using crosspad_gui::kFwCommunityOkMax;
using crosspad_gui::kFwLibMax;

/** A view starts / stops using the data. The last close frees everything;
 *  work still running is dropped by the worker when it finishes. */
void fw_view_data_open(void);
void fw_view_data_close(void);

/** Requests queued or running, or results not polled yet. Any task (FW_UI busy=). */
bool fw_view_data_busy(void);

/** From the views' timer: moves finished results in. */
void fw_view_data_poll(void);

void fw_view_data_scan_library(void);
FwLibrarySnap *fw_view_data_library(void);           /* nullptr until the first scan landed; the view sets slot/running */
uint32_t fw_view_data_library_gen(void);             /* changes with every scan that landed */

void fw_view_data_load_store(void);                  /* FW_CATALOG_CACHE -> FwStore */
void fw_view_data_drop_store(void);                  /* no view needs it: ~5.4 kB back */
const crosspad::FwStore *fw_view_data_store(void);   /* nullptr unless the last load was Ok */
crosspad::FwStoreLoad fw_view_data_store_state(void);
bool fw_view_data_store_loaded(void);                /* a load finished, whatever its result */
uint32_t fw_view_data_store_gen(void);

/** A kit card's text: the package's manifest (pkg may be ""), filled in from
 *  .details/<kit>.json. Only the latest request's answer is kept. */
void fw_view_data_load_detail(const char *kit, const char *version, const char *pkg);
const crosspad::FwKitDetail *fw_view_data_detail(void);   /* nullptr while it loads */
uint32_t fw_view_data_detail_gen(void);

/** Card writes; each ends with a library scan. A remove of a package named by a slot is refused here too. */
void fw_view_data_set_pinned(const char *pkg, bool pinned);
void fw_view_data_remove(const char *pkg, const char *slotA, const char *slotB);
void fw_view_data_ack_community(const char *kit);
bool fw_view_data_community_ok(const char *kit);

/** <pkg>/icon.bin, else .icons/<kit>-<version>.bin; the answer reaches
 *  crosspad-gui's fw_icons_deliver(ticket, ...) from fw_view_data_poll(). */
bool fw_view_data_request_icon(uint32_t ticket, const char *pkg, const char *kit, const char *version);
/** A request still queued for `ticket` is dropped unread. */
void fw_view_data_cancel_icon(uint32_t ticket);
