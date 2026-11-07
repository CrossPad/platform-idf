/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
   Edited by Mateusz Czarnecki for CrossPad project
*/

/**
 * @file
 * @brief ESP BSP: CrossPad
 */

#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "config.h"

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
#include "lvgl.h"
#include "esp_lvgl_port.h"
#endif // BSP_CONFIG_NO_GRAPHIC_LIB == 0

#define CONFIG_BSP_SPIFFS_MOUNT_POINT "/spiffs"
#define CONFIG_BSP_SPIFFS_PARTITION_LABEL "spiffs"
#define CONFIG_BSP_SPIFFS_MAX_FILES 5

/** @defgroup g02_storage SD Card and SPIFFS
 *  @brief SPIFFS and SD card BSP API
 *  @{
 */
/* uSD card MMC pins are defined per-revision in pinout/pinout_v*.h
   (BSP_SD_D0..D3, BSP_SD_CLK, BSP_SD_CMD, BSP_SD_DET, BSP_SD_WIDTH) */
#if defined(CONFIG_BSP_BOARD_REV_V2)
#  include "pinout/pinout_v2.h"
#elif defined(CONFIG_BSP_BOARD_REV_V1)
#  include "pinout/pinout_v1.h"
#endif

/** @} */ // end of storage

#ifdef __cplusplus
extern "C" {
#endif



/**************************************************************************************************
 *
 * SD card
 *
 * After mounting the SD card, it can be accessed with stdio functions ie.:
 * \code{.c}
 * FILE* f = fopen(BSP_SD_MOUNT_POINT"/hello.txt", "w");
 * fprintf(f, "Hello %s!\n", bsp_sdcard->cid.name);
 * fclose(f);
 * \endcode
 *
 * @attention IO2 is also routed to RGB LED and push button
 **************************************************************************************************/
#define BSP_SD_MOUNT_POINT      CONFIG_BSP_SD_MOUNT_POINT
#define BSP_SDSPI_HOST          (SPI2_HOST)

/**
 * @brief BSP SD card configuration structure
 */
typedef struct
{
    const esp_vfs_fat_sdmmc_mount_config_t *mount;
    sdmmc_host_t *host;
    union
    {
        const sdmmc_slot_config_t *sdmmc;
        const sdspi_device_config_t *sdspi;
    } slot;
} bsp_sdcard_cfg_t;

/**
 * @brief Mount microSD card to virtual file system
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if esp_vfs_fat_sdmmc_mount was already called
 *      - ESP_ERR_NO_MEM if memory can not be allocated
 *      - ESP_FAIL if partition can not be mounted
 *      - other error codes from SDMMC or SPI drivers, SDMMC protocol, or FATFS drivers
 */
esp_err_t bsp_sdcard_mount(void);

/**
 * @brief bsp_sdcard_mount(), with one more attempt when @p thorough
 *
 * 4-bit at 40 MHz; when @p thorough, 4-bit at 20 MHz on a fresh host after
 * 50 ms; then 1-bit at 40 MHz. The boot mounts thoroughly; the hotplug probe
 * shares the board's worker with the pad LEDs and the power button, so it
 * makes the two quick attempts only. The success line names the bus:
 * "SD Card mounted successfully at /sdcard (4-bit, 40000 kHz)".
 */
esp_err_t bsp_sdcard_mount_ex(bool thorough);

/**
 * @brief The bus width (1/4) and clock (kHz) the card came up on; 0 and 0
 *        while no card is mounted. Values kept at the mount -- safe from any
 *        task, never touches the card.
 */
void bsp_sdcard_get_bus(int *width, int *freq_khz);

/**
 * @brief Unmount micorSD card from virtual file system
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if the partition table does not contain FATFS partition with given label
 *      - ESP_ERR_INVALID_STATE if esp_vfs_fat_spiflash_mount was already called
 *      - ESP_ERR_NO_MEM if memory can not be allocated
 *      - ESP_FAIL if partition can not be mounted
 *      - other error codes from wear levelling library, SPI flash driver, or FATFS drivers
 */
esp_err_t bsp_sdcard_unmount(void);

/**
 * @brief Get SD card handle
 *
 * @return SD card handle
 */
sdmmc_card_t *bsp_sdcard_get_handle(void);

/**
 * @brief Bring the card up without mounting a filesystem on it
 *
 * Same host and slot configuration as bsp_sdcard_mount(), including the
 * 4-bit -> 1-bit retry, stopping after sdmmc_card_init(). For USB transfer
 * mode, where the host owns the FAT volume and the firmware must only move
 * sectors (see main/usb_transfer.cpp). Never call it beside bsp_sdcard_mount():
 * two owners of one card is what this exists to avoid.
 *
 * @param out_card Receives the card handle on success
 *
 * @return ESP_OK, ESP_ERR_NO_MEM, or an SDMMC driver/protocol error
 */
esp_err_t bsp_sdcard_raw_init(sdmmc_card_t **out_card);

/**
 * @brief Get SD card MMC host config
 *
 * @param slot SD card slot
 * @param config Structure which will be filled
 */
void bsp_sdcard_get_sdmmc_host(const int slot, sdmmc_host_t *config);

/**
 * @brief Get SD card MMC slot config
 *
 * @param slot SD card slot
 * @param config Structure which will be filled
 */
void bsp_sdcard_sdmmc_get_slot(const int slot, sdmmc_slot_config_t *config);

/**
 * @brief Mount microSD card to virtual file system (MMC mode)
 *
 * @param cfg SD card configuration
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if esp_vfs_fat_sdmmc_mount was already called
 *      - ESP_ERR_NO_MEM if memory cannot be allocated
 *      - ESP_FAIL if partition cannot be mounted
 *      - other error codes from SDMMC or SPI drivers, SDMMC protocol, or FATFS drivers
 */
esp_err_t bsp_sdcard_sdmmc_mount(bsp_sdcard_cfg_t *cfg);

/** @} */ // end of storage

/** \addtogroup g04_display
 *  @{
 */



#ifdef __cplusplus
}
#endif
