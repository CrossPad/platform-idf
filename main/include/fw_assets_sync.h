/**
 * @file fw_assets_sync.h
 * @brief Keeps the shared `assets` LittleFS partition in step with the
 *        running firmware after an A/B switch.
 *
 * Both OTA slots read and write the one `assets` partition (icons, fonts,
 * the bundled STM32 image) — collect_assets.cmake stamps every build's image
 * with its own `.assets_id` marker (also embedded in the firmware's own CPFI
 * descriptor, fw_image_desc.h). A switch to a slot built against a different
 * asset set leaves the old marker in flash; fw_assets_sync() notices and
 * rewrites the partition from the running slot's own package on the SD card,
 * if one is there with the matching assets.bin.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

enum class FwAssetsResult : uint8_t { Match, Rewritten, MismatchNoSource, Failed };

/** Blocking. Compares /assets/.assets_id against
 * fw_image_desc_running()->assets_id. Equal -> Match. Otherwise looks up the
 * running slot's own package (fw_slots_get()/fw_library_find()) on the SD
 * card: if its assets.bin exists and its own assets_id is the wanted one,
 * reads it into a PSRAM buffer, unregisters LittleFS, rewrites the partition
 * (fx_assets_write_image()) and re-registers LittleFS, re-reading the marker
 * to confirm -> Rewritten, or Failed if any of that did not land. No matching
 * package on the card -> MismatchNoSource (see fw_assets_sync_toast_pending()).
 *
 * The actual work runs on a dedicated internal-RAM-stack task (the caller's
 * own stack never carries it) — the same fw_library_find()/fopen/fread chain
 * fw_install_start() already needs a task of its own for. This call blocks
 * the caller until that task finishes.
 *
 * Call once, after the SD card is mounted and fw_switch_finish_boot(), and
 * before bsp_display_start()/AudioTasks_Start() — the ~1 MB PSRAM staging
 * buffer this needs must come out of the same pool the sample engine's kit
 * takes from. Never in USB transfer mode, where the SD card belongs to
 * the host. */
FwAssetsResult fw_assets_sync(void);

/** True once, right after a fw_assets_sync() call that returned
 * MismatchNoSource — never twice for the same event. Copies the one-line
 * toast text into buf: "Icons are from another firmware - insert the card
 * with <pkg>" when the running slot is a package of the library, else (an
 * OTA over USB, the browser updater before it wrote assets) "Icons are from
 * an older firmware - update them at play.crosspad.app/device". Meant to be
 * polled once the launcher is up, the same shape as
 * fw_rollback_toast_pending(). */
bool fw_assets_sync_toast_pending(char *buf, size_t len);
