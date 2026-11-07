/**
 * @file ota_cdc.h
 * @brief OTA firmware update over USB CDC
 *
 * Handles the OTA protocol: begin/data/complete lifecycle,
 * NVS auto-accept preference. UI is decoupled via callbacks.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief UI callback structure for OTA events.
 * Register via ota_set_ui_callbacks() to receive OTA lifecycle notifications.
 * All callbacks are invoked from the CDC processing task — use lv_async_call()
 * inside them if you need to update LVGL objects.
 */
typedef struct {
    void (*on_begin)(size_t total_size, const char *version);              /**< OTA transfer started */
    void (*on_progress)(size_t received, size_t total); /**< Data progress update */
    void (*on_complete)(void);                        /**< OTA finished successfully */
    void (*on_error)(void);                           /**< OTA failed */
    void (*on_confirm_request)(size_t size, const char *version);           /**< User confirmation needed (non-auto-accept) */
} ota_ui_callbacks_t;

/**
 * @brief Register UI callbacks for OTA events.
 * @param cbs Pointer to callback struct (copied internally). Pass NULL to unregister.
 */
void ota_set_ui_callbacks(const ota_ui_callbacks_t *cbs);

/**
 * @brief Called by UI code to respond to a confirmation request.
 * @param accepted true if user accepted the update, false if rejected.
 */
void ota_user_respond(bool accepted);

/**
 * @brief Initialise OTA subsystem (create semaphore, etc.)
 * Call once from app_main() before entering the main loop.
 */
void ota_init(void);

/**
 * @brief Check if an OTA transfer is currently in progress.
 */
bool ota_is_active(void);

/**
 * @brief Check incoming CDC data for an "OTA_BEGIN <size>" command.
 * @return true if the message was an OTA command (handled), false otherwise.
 */
bool ota_check_begin(const char *buf, size_t len);

/**
 * @brief Check incoming CDC data for an "OTA_DELTA <patch_size>" command.
 * Initiates a delta OTA update using esp_delta_ota (detools patch applied
 * against the currently running partition).
 * @return true if the message was a delta OTA command (handled), false otherwise.
 */
bool ota_check_delta_begin(const char *buf, size_t len);

/**
 * @brief Check if the current OTA transfer is a delta update.
 */
bool ota_is_delta(void);

/**
 * @brief Whether an OTA over CDC starts without the on-screen Allow prompt
 * (NVS ota_prefs/auto_accept, the Firmware app's "Auto-accept updates over
 * USB"). Read-only; false when the key was never written.
 */
bool ota_auto_accept_enabled(void);

/**
 * @brief Feed received binary data into the OTA writer.
 * Call this in OTA-active mode for every CDC message.
 */
void ota_process_data(const uint8_t *data, size_t len);

/**
 * @brief Handle OTA timeout (no data received within deadline).
 * Aborts the current OTA transaction and responds with an error.
 */
void ota_abort_timeout(void);

#ifdef __cplusplus
}
#endif
