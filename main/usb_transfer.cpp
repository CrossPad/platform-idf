#include "include/usb_transfer.h"

#include <atomic>
#include <string.h>

#include "bsp_sdmmc.h"
#include "class/msc/msc_device.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "tusb.h"

static const char *TAG = "usb_transfer";

/* RTC slow memory: kept across esp_restart(), uninitialised after a power
 * cycle. That is exactly the lifetime the mode needs. Uninitialised RTC RAM
 * can match the magic by chance, so the reset reason is the second half of
 * the test — otherwise one cold boot in 2^32 would strand a board here. */
static constexpr uint32_t kBootMagic = 0x43503A4Du;   /* "CP:M" */
RTC_NOINIT_ATTR static uint32_t s_bootMagic;

/* SCSI additional sense code for "invalid command operation code"; msc.h
 * carries the sense keys but not the ASC table. */
static constexpr uint8_t kScsiAscInvalidCommand = 0x20;

/* A write this recent means the host may still have more on the way. */
static constexpr int64_t kBusyUs = 2 * 1000 * 1000;
/* Enough for a CDC reply to leave before the controller is reset. */
static constexpr uint32_t kRestartDelayMs = 150;

static bool s_active;
static sdmmc_card_t *s_card;

/* Written from the TinyUSB task (the MSC callbacks below), read from the LVGL
 * timer and from the CDC loop. Sectors rather than bytes so the counters stay
 * 32-bit and lock-free on this core. */
static std::atomic<uint32_t> s_readSectors{0};
static std::atomic<uint32_t> s_writeSectors{0};
static std::atomic<int64_t>  s_lastWriteUs{0};
static std::atomic<bool>     s_ejected{false};

static uint32_t sector_size(void)
{
    return s_card ? (uint32_t)s_card->csd.sector_size : 0;
}

bool usb_transfer_boot_claim(void)
{
    s_active = (s_bootMagic == kBootMagic) && (esp_reset_reason() == ESP_RST_SW);
    s_bootMagic = 0;
    if (s_active) ESP_LOGW(TAG, "Booting into USB transfer mode");
    return s_active;
}

bool usb_transfer_active(void)
{
    return s_active;
}

void usb_transfer_request(void)
{
    ESP_LOGW(TAG, "Restarting into USB transfer mode");
    s_bootMagic = kBootMagic;
    vTaskDelay(pdMS_TO_TICKS(kRestartDelayMs));
    esp_restart();
}

void usb_transfer_leave(void)
{
    ESP_LOGW(TAG, "Leaving USB transfer mode");
    s_bootMagic = 0;
    vTaskDelay(pdMS_TO_TICKS(kRestartDelayMs));
    esp_restart();
}

esp_err_t usb_transfer_storage_attach(void)
{
    if (s_card) return ESP_OK;

    esp_err_t err = bsp_sdcard_raw_init(&s_card);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "SD card exposed over USB: %llu MB",
             (unsigned long long)((uint64_t)s_card->csd.capacity * sector_size() >> 20));
    return ESP_OK;
}

void usb_transfer_get_status(usb_transfer_status_t *out)
{
    if (!out) return;

    const uint32_t sec  = sector_size();
    const uint32_t read = s_readSectors.load(std::memory_order_relaxed);
    const uint32_t written = s_writeSectors.load(std::memory_order_relaxed);
    out->cardPresent = s_card != nullptr;
    out->cardBytes   = out->cardPresent ? (uint64_t)s_card->csd.capacity * sec : 0;
    out->readKiB     = (uint32_t)((uint64_t)read * sec / 1024);
    out->writeKiB    = (uint32_t)((uint64_t)written * sec / 1024);

    if (!out->cardPresent) {
        out->state = USB_TRANSFER_NO_CARD;
    } else if (s_ejected.load(std::memory_order_relaxed)) {
        out->state = USB_TRANSFER_EJECTED;
    } else if (written &&
               esp_timer_get_time() - s_lastWriteUs.load(std::memory_order_relaxed) < kBusyUs) {
        /* The timestamp starts at zero, so "no write yet" has to be excluded
         * explicitly or the first two seconds of every boot read as busy. */
        out->state = USB_TRANSFER_BUSY;
    } else if (tud_mounted() && (read || written)) {
        out->state = USB_TRANSFER_MOUNTED;
    } else {
        out->state = USB_TRANSFER_WAITING;
    }
}

/* ── MSC callbacks ─────────────────────────────────────────────────────────
 *
 * The SCSI target is the raw card, and nothing here ever mounts a filesystem
 * or writes one. esp_tinyusb ships a storage layer that would do most of this,
 * but it answers the host's eject by mounting the card on FATFS — and that
 * mount calls f_mkfs when f_mount reports no filesystem or an internal error,
 * which is not a thing to risk on someone's kit card. Its tud_umount_cb also
 * dereferences a storage handle that is null in every profile but this one,
 * so leaving it linked made an ordinary unplug a panic. --wrap cannot help:
 * these callbacks have weak definitions in the translation units that call
 * them, and ld only redirects undefined references. So main/CMakeLists.txt
 * drops tusb_msc_storage.c from the build and these strong definitions
 * override the weak defaults instead.
 *
 * All of them run on the TinyUSB task. The card I/O is synchronous: this is
 * the only task touching the card for the whole of a transfer boot. */
extern "C" {

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    /* Space-padded, not NUL-terminated: this is what `lsblk -o VENDOR,MODEL`
     * and every file manager put in front of the user. */
    memcpy(vendor_id,   "CrossPad", 8);
    memcpy(product_id,  "SD Card         ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    return s_card != nullptr;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = s_card ? (uint32_t)s_card->csd.capacity : 0;
    *block_size  = (uint16_t)sector_size();
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;
    if (load_eject && !start) {
        s_ejected.store(true, std::memory_order_relaxed);
        ESP_LOGI(TAG, "Host ejected the drive");
    }
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    const uint32_t sec = sector_size();
    /* CFG_TUD_MSC_EP_BUFSIZE is a whole number of sectors, so the class driver
     * never splits one: an offset here would mean a configuration change. */
    if (!s_card || offset != 0 || sec == 0 || bufsize % sec != 0) return -1;
    if (sdmmc_read_sectors(s_card, buffer, lba, bufsize / sec) != ESP_OK) return -1;
    s_readSectors.fetch_add(bufsize / sec, std::memory_order_relaxed);
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    const uint32_t sec = sector_size();
    if (!s_card || offset != 0 || sec == 0 || bufsize % sec != 0) return -1;
    if (sdmmc_write_sectors(s_card, buffer, lba, bufsize / sec) != ESP_OK) return -1;
    s_writeSectors.fetch_add(bufsize / sec, std::memory_order_relaxed);
    s_lastWriteUs.store(esp_timer_get_time(), std::memory_order_relaxed);
    s_ejected.store(false, std::memory_order_relaxed);
    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;
    if (scsi_cmd[0] == SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL) return 0;
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, kScsiAscInvalidCommand, 0);
    return -1;
}

} // extern "C"
