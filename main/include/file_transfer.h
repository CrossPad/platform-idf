/**
 * @file file_transfer.h
 * @brief Bulk transfer over the CDC control port: whole-partition assets
 *        flashing, and arbitrary files in both directions.
 *
 * MSC is not an option on this board — the TinyUSB descriptor has no endpoints
 * left (MIDI + CDC + UAC2) — so everything rides the one CDC pipe that already
 * carries OTA and the HIL verbs.
 *
 * Framing: the command protocol is newline-framed text, binary payloads are
 * not escaped. A transfer switches the pipe into binary mode for exactly the
 * byte count announced in its header line and switches back when that many
 * bytes have arrived, which is the same contract OTA_BEGIN already has with
 * cdc_dispatch_lines(). Escaping (SLIP/COBS) was rejected: it expands the
 * payload, costs CPU on both ends, and buys nothing when the length is known
 * up front in both directions.
 *
 * Threading: every filesystem call runs on a worker task, never on the
 * priority-1 CDC drain loop. A read off the SD card takes the SDMMC
 * transaction lock, which behind a kit load has been measured at 3.1 s — that
 * is exactly the wait that must not land on the loop that answers every
 * off-device query. The drain loop only parses, memcpys into a staging slot
 * and posts; the worker writes and answers.
 *
 * Verbs (replies terminated \r\n):
 *   ASSETS_BEGIN <size> <crc32hex> [FORCE]
 *                            -> ASSETS_READY, then <size> raw bytes,
 *                               "OK n/size" per 4096-byte chunk,
 *                               then ASSETS_OK (device reboots) or
 *                               ASSETS_ERROR <reason>
 *   FS_ROOTS                 -> one line per mount, "<root> <total> <used>"
 *                               ("- -" when it is not mounted), then FS_END
 *   FS_LIST <dir>            -> "FS_LIST", lines "F|D <size> <name>", "FS_END"
 *   FS_STAT <path>           -> "FS_STAT <size> F|D"
 *   FS_MKDIR <path>          -> OK
 *   FS_DELETE <path>         -> OK
 *   FS_GET <path> [off] [len]
 *                            -> "FS_DATA <len>", <len> raw bytes,
 *                               "FS_OK <crc32hex>" (or "FS_TRUNC <got>/<len>"
 *                               if the read came up short)
 *   FS_PUT <size> <crc32hex> <path>
 *                            -> FS_READY, then <size> raw bytes,
 *                               "OK n/size" per 4096-byte chunk,
 *                               then "FS_OK <crc32hex>" or "FS_ERR <reason>"
 *
 * Roots: /assets (LittleFS), /spiflash (FAT "storage", mounted on first use),
 * /sdcard. Anything else is refused.
 *
 * The host side is tools/fs_transfer.py.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Offer one CDC command line to the transfer handlers.
 * @return true if the line was consumed.
 */
bool fx_handle_cdc(const char *buf, size_t len);

/**
 * @brief True while the pipe is in binary mode (a host->device stream is in
 * flight). cdc_dispatch_lines() must not frame on newlines while this holds,
 * and the RX callback must not drop on a full app_queue.
 */
bool fx_stream_active(void);

/**
 * @brief Feed received bytes into the active host->device stream.
 */
void fx_process_data(const uint8_t *data, size_t len);

/**
 * @brief Abandon a stream that stopped mid-transfer (idle tick).
 */
void fx_abort_timeout(void);

/**
 * @brief Erase the assets partition and write `image` into it (page 0 held
 * back and written last, then the whole partition read back and checked
 * against `crc32` — the same shape ASSETS_BEGIN/assets_write_slot/
 * assets_finish use for a streamed transfer, factored out for a caller that
 * already holds the whole image in one buffer.
 *
 * Does not touch the LittleFS mount (the caller unregisters "assets" first
 * and re-registers it afterwards) and does not restart the device. Blocking;
 * call off the LVGL thread and off the priority-1 CDC drain loop.
 */
bool fx_assets_write_image(const uint8_t *image, uint32_t size, uint32_t crc32);

#ifdef __cplusplus
} /* extern "C" */
#endif
