#pragma once
#include <stdbool.h>
#include <stdint.h>

enum class FwGcResult : uint8_t { Ok, CardFull, NoMem, NoCard, Io };

/* Space-only cleanup of FW_PKG_ROOT (spec §5): the library stays under the
 * limit (512 MB unless FW_UPD_CFG gc_mb= overrides it until reboot) and the
 * card keeps 10 % free. Never deletes the packages named slotA / slotB (the
 * caller reads them from fw_slots: this runs on a stack that must not touch
 * flash; names compare case-insensitively, as FATFS does), one with a
 * .pinned file, or `keep` (the download in progress). CardFull when that
 * cannot be met without a protected package -- then nothing is deleted. NoCard:
 * the card's size is unknown; NoMem: no room for the package list; Io: an
 * eviction failed, so the space was not freed. Blocking, SD-heavy: the
 * updater's job task only. */
FwGcResult fw_gc_run(uint64_t incoming, const char *keep, const char *slotA, const char *slotB);
void fw_gc_set_library_limit_mb(uint32_t mb);   /* 0 = the default */
uint32_t fw_gc_library_limit_mb(void);
