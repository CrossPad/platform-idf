/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* C standard library */
#include <string.h>
#include <stdlib.h>
#include <assert.h>

/* ESP-IDF */
#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Component */
#include "bsp_sdmmc.h"


#define BSP_SDMMC_FREQ_KHZ_MAX	40000




static const char TAG[] = "bsp_sdmmc";
static sdmmc_card_t *bsp_sdcard = NULL;
static sdmmc_card_t *bsp_raw_card = NULL;
/* The bus the card came up on, set at the mount (or the raw init) and cleared
 * at the unmount: a reader from another task never dereferences a card the
 * hotplug may be freeing. */
static uint8_t s_bus_width;
static uint16_t s_bus_khz;

static void note_bus(const sdmmc_card_t *card)
{
    s_bus_width = card ? (uint8_t)(1u << card->log_bus_width) : 0;
    s_bus_khz = card ? (uint16_t)card->real_freq_khz : 0;
}

void bsp_sdcard_get_bus(int *width, int *freq_khz)
{
    *width = s_bus_width;
    *freq_khz = s_bus_khz;
}





sdmmc_card_t *bsp_sdcard_get_handle(void)
{
    return bsp_sdcard;
}

void bsp_sdcard_get_sdmmc_host(const int slot, sdmmc_host_t *config)
{
    assert(config);
    sdmmc_host_t host_config = SDMMC_HOST_DEFAULT();
    host_config.slot = slot;
    host_config.max_freq_khz = BSP_SDMMC_FREQ_KHZ_MAX;
    host_config.flags |= SDMMC_HOST_FLAG_4BIT;
    *config = host_config;
}

void bsp_sdcard_sdmmc_get_slot(const int slot, sdmmc_slot_config_t *config)
{
    assert(config);
    memset(config, 0, sizeof(sdmmc_slot_config_t));

    /* SD card is connected to Slot 0 pins. Slot 0 uses IO MUX, so not specifying the pins here */
    config->cd = SDMMC_SLOT_NO_CD;
    config->wp = SDMMC_SLOT_NO_WP;
    config->cmd = BSP_SD_CMD;
    config->clk = BSP_SD_CLK;
    config->d0 = BSP_SD_D0;
    config->d1 = BSP_SD_D1;
    config->d2 = BSP_SD_D2;
    config->d3 = BSP_SD_D3;
    config->width = BSP_SD_WIDTH;
    config->flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
}

esp_err_t bsp_sdcard_sdmmc_mount(bsp_sdcard_cfg_t *cfg)
{
    sdmmc_host_t sdhost = {0};
    sdmmc_slot_config_t sdslot = {0};
    const esp_vfs_fat_sdmmc_mount_config_t mount_config =
    {
        .format_if_mount_failed = false,
        /* 32, not 20. Each descriptor is a 4 kB FIL in PSRAM at mount, which
         * is cheap (PSRAM had ~525 kB free with a kit up); what it buys is a
         * file-handle cache in sample_player_port.cpp big enough to hold a
         * whole kit. At 20 the cache was capped at 12 against kits of 16+
         * samples, so a 16-pad pattern evicted a sounding voice's handle on
         * nearly every note -- measured 204 evictions per 240 notes -- and
         * each eviction is a FAT directory walk on the next refill
         * (40..160 ms on a 1-bit card): a late voice start, or a hole in
         * whichever voice was nearest empty. CONFIG_FATFS_FS_LOCK moves with
         * it; it caps concurrently open files independently. */
        .max_files = 32,
        .allocation_unit_size = 16 * 1024
    };
    assert(cfg);


    if (!cfg->mount)
    {
        cfg->mount = &mount_config;
    }

    if (!cfg->host)
    {
        // SLOT_0: the SD signals are wired to arbitrary GPIOs, not its native pins.
        bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &sdhost);
        cfg->host = &sdhost;
    }

    if (!cfg->slot.sdmmc)
    {
        bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &sdslot);
        cfg->slot.sdmmc = &sdslot;
        ESP_LOGI(TAG, "SDMMC SLOT_0 pin map: CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d width=%d",
                 cfg->slot.sdmmc->clk, cfg->slot.sdmmc->cmd, cfg->slot.sdmmc->d0, cfg->slot.sdmmc->d1,
                 cfg->slot.sdmmc->d2, cfg->slot.sdmmc->d3, cfg->slot.sdmmc->width);
    }

    /* Per attempt and after any override of the clock (the 20 MHz retry), so
     * the line says what this attempt asks for. */
    ESP_LOGI(TAG, "SDMMC host prepared: slot=%d max_freq=%ukHz", cfg->host->slot, cfg->host->max_freq_khz);
    esp_err_t err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, cfg->host, cfg->slot.sdmmc, cfg->mount, &bsp_sdcard);
    return err;
}

esp_err_t bsp_sdcard_mount_ex(bool thorough)
{
    bsp_sdcard_cfg_t cfg = {0};
    /* The wide attempt may be retried below, so its failure is not an error
     * yet: keep the driver's own E-lines out of the log until it is final. */
    esp_log_level_set("sdmmc_sd", ESP_LOG_NONE);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
    esp_err_t ret = bsp_sdcard_sdmmc_mount(&cfg);
    if (ret != ESP_OK && thorough && BSP_SD_WIDTH > 1)
    {
        /* Once more wide, at half the clock and on a fresh host (a failed
         * esp_vfs_fat_sdmmc_mount() deinits its own), before the width is
         * given up for the whole boot. */
        ESP_LOGW(TAG, "SD %d-bit init failed (%s), retrying %d-bit 20 MHz",
                 BSP_SD_WIDTH, esp_err_to_name(ret), BSP_SD_WIDTH);
        /* This one says which step fails: the first attempt's usual failure
         * is the SSR read (ACMD13) at 400 kHz, before the clock is raised, and
         * there a slower clock is only a second try. */
        esp_log_level_set("sdmmc_sd", ESP_LOG_WARN);
        vTaskDelay(pdMS_TO_TICKS(50));
        sdmmc_host_t host20;
        bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &host20);
        host20.max_freq_khz = SDMMC_FREQ_DEFAULT;
        bsp_sdcard_cfg_t cfg20 = { .host = &host20 };
        ret = bsp_sdcard_sdmmc_mount(&cfg20);
    }
    esp_log_level_set("sdmmc_sd", ESP_LOG_INFO);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_INFO);
    if (ret != ESP_OK && BSP_SD_WIDTH > 1)
    {
        /* CMD and D0 answered but the wide transfer did not (a card that sits
         * badly in the socket, D1-D3 marginal): a slow card beats no card. */
        ESP_LOGW(TAG, "SD %d-bit init failed (%s), retrying 1-bit", BSP_SD_WIDTH, esp_err_to_name(ret));
        sdmmc_slot_config_t slot1;
        bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &slot1);
        slot1.width = 1;
        bsp_sdcard_cfg_t cfg1 = { .slot.sdmmc = &slot1 };
        ret = bsp_sdcard_sdmmc_mount(&cfg1);
    }
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SD Card mount failed, error: %s", esp_err_to_name(ret));
    }
    else
    {
        note_bus(bsp_sdcard);
        ESP_LOGI(TAG, "SD Card mounted successfully at %s (%d-bit, %d kHz)",
                 BSP_SD_MOUNT_POINT, s_bus_width, s_bus_khz);
    }
    return ret;
}

esp_err_t bsp_sdcard_mount(void)
{
    return bsp_sdcard_mount_ex(false);
}


/* One complete attempt: host, slot, card. Every attempt starts from a fresh
 * host — a card that failed halfway leaves the peripheral in a state where the
 * next sdmmc_host_init_slot() cannot even update the clock (0x107). The
 * mounted path gets this for free because esp_vfs_fat_sdmmc_mount() deinits on
 * its way out of a failure. */
static esp_err_t bsp_sdcard_raw_try(int width, sdmmc_host_t *host, sdmmc_card_t *card)
{
    sdmmc_slot_config_t sdslot = {0};

    bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, host);
    bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &sdslot);
    sdslot.width = width;
    memset(card, 0, sizeof(*card));

    esp_err_t err = (*host->init)();
    if (err != ESP_OK)
    {
        return err;
    }

    err = sdmmc_host_init_slot(host->slot, &sdslot);
    if (err == ESP_OK)
    {
        err = sdmmc_card_init(host, card);
    }
    if (err != ESP_OK)
    {
        sdmmc_host_deinit();
    }
    return err;
}

esp_err_t bsp_sdcard_raw_init(sdmmc_card_t **out_card)
{
    sdmmc_host_t sdhost = {0};

    assert(out_card);
    if (bsp_raw_card)
    {
        *out_card = bsp_raw_card;
        return ESP_OK;
    }

    sdmmc_card_t *card = calloc(1, sizeof(sdmmc_card_t));
    if (!card)
    {
        return ESP_ERR_NO_MEM;
    }

    /* The wide attempt may be retried below, so its failure is not an error
     * yet: keep the driver's own E-lines out of the log until it is final. */
    esp_log_level_set("sdmmc_sd", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_periph", ESP_LOG_NONE);
    esp_err_t err = bsp_sdcard_raw_try(BSP_SD_WIDTH, &sdhost, card);
    esp_log_level_set("sdmmc_sd", ESP_LOG_INFO);
    esp_log_level_set("sdmmc_periph", ESP_LOG_INFO);
    if (err != ESP_OK && BSP_SD_WIDTH > 1)
    {
        /* Same fallback as the mounted path: a card that sits badly in the
         * socket answers CMD and D0 but not the wide transfer. */
        ESP_LOGW(TAG, "SD %d-bit raw init failed (%s), retrying 1-bit", BSP_SD_WIDTH, esp_err_to_name(err));
        err = bsp_sdcard_raw_try(1, &sdhost, card);
    }

    if (err != ESP_OK)
    {
        free(card);
        ESP_LOGE(TAG, "SD Card raw init failed, error: %s", esp_err_to_name(err));
        return err;
    }

    bsp_raw_card = card;
    note_bus(card);
    *out_card = card;
    ESP_LOGI(TAG, "SD Card raw: %d sectors x %d B, bus %d-bit",
             card->csd.capacity, card->csd.sector_size, 1 << card->log_bus_width);
    return ESP_OK;
}


esp_err_t bsp_sdcard_unmount(void)
{
    esp_err_t ret = ESP_OK;

    note_bus(NULL);
    ret |= esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, bsp_sdcard);
    bsp_sdcard = NULL;
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SD Card unmount failed, error: %s", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "SD Card unmounted successfully from %s", BSP_SD_MOUNT_POINT);
    }

    return ret;
}


