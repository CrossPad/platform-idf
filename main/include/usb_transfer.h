/**
 * @file usb_transfer.h
 * @brief USB Transfer mode — the SD card exposed to the host as a USB drive.
 *
 * A separate boot mode. Entering it arms a one-shot flag in RTC memory and
 * restarts; on that boot the firmware brings up the display, USB and the card
 * and starts nothing that opens a file on it. Leaving it is another restart.
 * The flag never survives a power cycle, so a cold boot is always normal.
 *
 * Design: docs/design/2026-09-21-usb-transfer-mode-design.md
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What the screen shows and USB_MSC reports. */
typedef enum {
    USB_TRANSFER_NO_CARD = 0, /**< no SD card — MSC is not enumerated at all */
    USB_TRANSFER_WAITING,     /**< the host has not claimed the drive yet */
    USB_TRANSFER_MOUNTED,     /**< the host is reading or writing the card */
    USB_TRANSFER_BUSY,        /**< a write landed within the last two seconds */
    USB_TRANSFER_EJECTED,     /**< the host ejected the drive — safe to leave */
} usb_transfer_state_t;

typedef struct {
    usb_transfer_state_t state;
    bool     cardPresent;
    uint64_t cardBytes;   /**< capacity reported by the card's CSD */
    uint32_t readKiB;     /**< moved to the host since this boot */
    uint32_t writeKiB;    /**< taken from the host since this boot */
} usb_transfer_status_t;

/**
 * Read and clear the one-shot boot flag. Call exactly once, first thing in
 * app_main(): true means this boot was asked for by usb_transfer_request().
 */
bool usb_transfer_boot_claim(void);

/** True for the whole of a boot claimed above. */
bool usb_transfer_active(void);

/** Arm the flag and restart into transfer mode. Does not return. */
void usb_transfer_request(void);

/** Restart without the flag, back into the normal profile. Does not return. */
void usb_transfer_leave(void);

/**
 * Bring the card up raw (no FATFS) and hand it to the MSC class driver.
 * Called from the transfer profile's TinyUSB install, before the driver is
 * installed — the MSC callbacks need the storage handle from the first
 * enumeration. Failure means "no card": install the plain CDC profile.
 */
esp_err_t usb_transfer_storage_attach(void);

/** Snapshot for the screen and the USB_MSC verb. Any task. */
void usb_transfer_get_status(usb_transfer_status_t *out);

#ifdef __cplusplus
}
#endif
