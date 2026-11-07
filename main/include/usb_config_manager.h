#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * USB_MODE_AUDIO has no CDC. The ESP32-S3 DWC2 core offers five IN endpoints
 * including EP0, and MIDI (1) plus UAC2 (feedback + mic = 2) already claim all
 * of them — see main/usb_descriptors_audio.c. Control that needs to work while
 * capturing therefore goes over SysEx 0x1D through USB MIDI, which exists in
 * both modes; the HIL tools rely on that.
 */
typedef enum {
    USB_MODE_DEFAULT = 0,   // CDC + MIDI
    USB_MODE_AUDIO,         // MIDI + UAC2 — no CDC (see below)
    USB_MODE_TRANSFER,      // MSC + CDC — the SD card as a USB drive, boot mode only
    USB_MODE_COUNT
} usb_mode_t;

/**
 * @brief Initialize USB config manager, installing TinyUSB in @p mode.
 *        Replaces the inline USB init that was previously in main.cpp.
 *        USB_MODE_TRANSFER falls back to USB_MODE_DEFAULT when there is no
 *        card to expose; usb_config_manager_get_mode() says which happened.
 */
esp_err_t usb_config_manager_init(usb_mode_t mode);

/**
 * @brief Switch USB device mode. Triggers USB re-enumeration (device disconnects briefly).
 * @param mode Target USB mode
 * @return ESP_OK on success
 */
esp_err_t usb_config_manager_set_mode(usb_mode_t mode);

/**
 * @brief Get current USB mode
 */
usb_mode_t usb_config_manager_get_mode(void);

/**
 * @brief Get human-readable name for a USB mode
 */
const char* usb_config_manager_get_mode_name(usb_mode_t mode);

/**
 * @brief Check incoming CDC data for USB mode switch commands.
 *        Handles "USB_AUDIO" and "USB_DEFAULT" commands.
 */
void check_usb_mode_command(const char* buff, size_t len);

#ifdef __cplusplus
}
#endif
