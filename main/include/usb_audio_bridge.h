#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize USB audio bridge
 *
 * Starts the USB audio port's rings. The I2S channels and codecs are left
 * as they run (16-bit stereo, the UAC format). Must be called AFTER I2S is
 * initialized.
 */
esp_err_t usb_audio_bridge_init(void);

/**
 * @brief Deinitialize USB audio bridge
 *
 * Stops the port. Does not touch the I2S channels.
 */
esp_err_t usb_audio_bridge_deinit(void);

/**
 * @brief Speaker callback: USB host → both I2S codecs (zero-copy DMA)
 *
 * Passes 2ch 16-bit PCM directly from USB to both I2S interfaces.
 */
esp_err_t usb_audio_output_cb(uint8_t *buf, size_t len, void *ctx);

/**
 * @brief Mic callback: I2S capture codec → USB host (zero-copy DMA)
 *
 * Reads 2ch 16-bit PCM from the selected capture codec into the USB buffer.
 */
esp_err_t usb_audio_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *ctx);
size_t usb_audio_output_space_cb(void *ctx);
/** The host opened or closed the mic stream (uac_device_config_t.input_open_cb). */
void usb_audio_input_open_cb(bool open, void *ctx);
/** The 'SPK #' line, when one is due: from the worker, once a second. */
void usb_audio_bridge_report(void);

/** Capture source: both codecs summed on the board (still a stereo stream). */
#define USB_AUDIO_CAPTURE_MIX 2
/* Pads-only mixer bus, fed digitally by the render loop instead of an ADC. */
#define USB_AUDIO_CAPTURE_SAMPLER 3

/**
 * @brief Select which codec feeds the USB mic path (default 0)
 *
 * 0 or 1 pick a codec, USB_AUDIO_CAPTURE_MIX sums both.
 * Runtime-switchable; takes effect on the next mic callback.
 */
esp_err_t usb_audio_bridge_set_capture_codec(int codec);

/**
 * @brief Currently selected USB mic capture source (0, 1 or USB_AUDIO_CAPTURE_MIX)
 */
int usb_audio_bridge_get_capture_codec(void);

#ifdef __cplusplus
}
#endif
