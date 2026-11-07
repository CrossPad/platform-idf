/*
 * In-field STM32 firmware update, driven from the ESP over the STM UART link.
 *
 * The image is a raw CrossPad_STM32_r2 .bin carrying a descriptor at offset
 * 0x100 (magic "CPFW", major, minor, protocol). The bundled copy lives in
 * LittleFS as /assets/stm32_fw.bin (staged from main/stm32_fw/ at configure
 * time); any other path — an SD card file, for instance — works the same.
 *
 * Protocol and STM side: CrossPad_STM32_r2/app/inc/fw_update.h.
 */
#pragma once

#include "esp_err.h"
#include "stm32_hal_init.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STM32_FW_BUNDLED_PATH   "/assets/stm32_fw.bin"

typedef struct {
    uint8_t  major;
    uint8_t  minor;
    uint16_t proto;
    uint8_t  pcb;           /* PCB revision the image targets; 0 = unknown (pre-1.3 image) */
    size_t   size;          /* bytes in the file */
} stm32_fw_image_info_t;

/** Read and validate the descriptor of an image file. */
esp_err_t stm32_fw_image_info(const char *path, stm32_fw_image_info_t *out);

/** Progress callback: pages written so far / total pages. */
typedef void (*stm32_fw_progress_cb)(uint16_t done, uint16_t total, void *ctx);

/**
 * Reflash the STM32 from an image file. Blocks for the whole transfer
 * (~10 s), including the STM's reboot and a fresh identity probe. On success
 * the cached link info (stm32_hal_get_link_info) reflects the new firmware.
 *
 * Failure modes are logged; ESP_ERR_INVALID_RESPONSE means the STM did not
 * enter or did not accept the image, ESP_ERR_TIMEOUT that it went silent, and
 * ESP_ERR_INVALID_STATE that the link was not up to begin with.
 */
esp_err_t stm32_fw_update(const char *path, stm32_fw_progress_cb cb, void *ctx);

/**
 * stm32_fw_update() with the board-revision gate optional. An image built
 * for another PCB (descriptor pcb != REG_IDENT_PCB_REV, both known) is
 * refused with ESP_ERR_NOT_SUPPORTED unless `force`.
 */
esp_err_t stm32_fw_update_ex(const char *path, bool force, stm32_fw_progress_cb cb, void *ctx);

typedef struct {
    stm32_link_info_t     running;   /* what the STM reports (valid when .linked) */
    stm32_fw_image_info_t bundled;   /* descriptor of the image at `path` */
    bool have_image;                 /* `path` holds a valid image */
    bool due;                        /* bundled is newer, or speaks another protocol */
    bool battery_low;                /* < 20 % with no charger: ask the user to plug in first */
    bool pcb_mismatch;               /* image targets another PCB than this board: never due */
} stm32_fw_update_status_t;

/**
 * Compare the running STM firmware with the image at `path`. Never flashes.
 * `due` is set when the bundle is newer (major.minor) or speaks a different
 * protocol; the UI decides what to do with it. Returns false when the link is
 * down or the image is missing (then `due` is false as well).
 */
bool stm32_fw_update_check(const char *path, stm32_fw_update_status_t *out);

/**
 * Headless policy: stm32_fw_update_check() and, when due and the battery
 * allows, stm32_fw_update() straight away. Progress is always logged; `cb`
 * (may be NULL) additionally receives it. Returns true when an update ran
 * and succeeded.
 */
bool stm32_fw_update_auto(const char *path, stm32_fw_progress_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif
