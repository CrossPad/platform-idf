/**
 * @file host_info.h
 * @brief What a host companion tells the board: HOST_INFO and FW_LATEST.
 *
 * Two companion verbs (CDC, BLE and SysEx 0x1F alike), pushed by a host-side
 * worker such as the FL Studio script's:
 *   HOST_INFO cpu=<pct> ram=<used_mb>/<total_mb> time=<unix_s> tz=<min_east>
 *             [daws=<installed>/<running>]
 *       every 2 s; sets the board clock and keeps the status bar's USB glyph
 *       lit (CrosspadStatus::hostAlive). `daws=` are bitmasks by DAW-protocol
 *       daw_id of the DAWs the host can start / sees running (DAW Control's
 *       launch button); absent = 0/0. Bare HOST_INFO only reports.
 *   FW_LATEST <x.y.z>
 *       newest firmware the host knows of; applied (newer wins) only on a board
 *       that has never completed its own catalog check, whose result is then
 *       authoritative (host_info_offer_latest); persisted in NVS so Settings → Info
 *       still says an update is waiting after a reset with no host. Bare
 *       FW_LATEST only reports.
 * Both answer one row: "HOST: ..." / "FW_LATEST: ...".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Boot, after NVS: load the persisted FW_LATEST into the status. */
void host_info_init(void);

/** True when `cmd` is HOST_INFO or FW_LATEST, answered through `reply`. The
 * text buffer lives only on that path: the companion runs this on the main
 * task's 5 kB stack. */
bool host_info_handle(const char *cmd, void (*reply)(const char *text));

/** A newest firmware version from anywhere (the hub's FW_LATEST, the board's
 * own catalog check): kept when it is newer than the one already known,
 * persisted, and status.fwUpdateAvailable recomputed against the running
 * version. True when it changed. Thread-safe. */
bool host_info_offer_latest(const char *version);

/** The board's own successful catalog check, authoritative for its running kit
 * and channel: replaces the known latest version, "" (nothing newer) included,
 * so the update glyph can also go out. Persists, so only from a task that may
 * write NVS. */
void host_info_set_latest(const char *version);

#ifdef __cplusplus
}
#endif
