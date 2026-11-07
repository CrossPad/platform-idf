/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*uac_output_cb_t)(uint8_t *buf, size_t len, void *cb_ctx);
typedef esp_err_t (*uac_input_cb_t)(uint8_t *buf, size_t len, size_t *bytes_read, void *cb_ctx);
typedef size_t (*uac_output_space_cb_t)(void *cb_ctx);
typedef void (*uac_set_mute_cb_t)(uint32_t mute, void *cb_ctx);
typedef void (*uac_set_volume_cb_t)(uint32_t volume, void *cb_ctx);
typedef void (*uac_input_open_cb_t)(bool open, void *cb_ctx);

/**
 * @brief USB UAC Device Config
 */
typedef struct {
    bool skip_tinyusb_init;                      /*!< if true, the Tinyusb and usb phy will not be initialized */
    uac_output_cb_t output_cb;                   /*!< callback function for UAC data output, if NULL, output will be disabled */
    uac_input_cb_t input_cb;                     /*!< callback function for UAC data input, if NULL, input will be disabled */
    uac_output_space_cb_t output_space_cb;       /*!< bytes the output consumer can take right now; if NULL, the whole FIFO is read (the feedback endpoint then sees an empty FIFO and cannot throttle the host) */
    uac_set_mute_cb_t set_mute_cb;               /*!< callback function for set mute, if NULL, the set mute request will be ignored */
    uac_set_volume_cb_t set_volume_cb;           /*!< callback function for set volume, if NULL, the set volume request will be ignored */
    uac_input_open_cb_t input_open_cb;           /*!< the host opened (alt != 0) or closed (alt 0) the mic stream; TinyUSB task; may be NULL */
    void *cb_ctx;                                /*!< callback context, for user specific usage */
    int spk_itf_num;                             /*!< Speaker interface number (required in AS_PART mode) */
    int mic_itf_num;                             /*!< Mic interface number (required in AS_PART mode) */
    uint32_t sample_rate;                        /*!< the one rate offered to the host; 0 = CONFIG_UAC_SAMPLE_RATE (the endpoints are sized for that maximum) */
} uac_device_config_t;

/**
 * @brief Initialize the USB Audio Class (UAC) device.
 *
 * @param config Pointer to the UAC device configuration structure.
 * @return
 *       - ESP_OK on success
 *       - ESP_FAIL on failure
 */
esp_err_t uac_device_init(uac_device_config_t *config);

/**
 * @brief Deinitialize the UAC device, stopping tasks and freeing resources.
 */
void uac_device_deinit(void);

/**
 * @brief Speaker path health since the previous call, then reset.
 *
 * @param gap_max_us longest the speaker task went without running while the
 *                   stream was open (it is woken by every packet, or after 4 ms)
 * @param fifo_max   fullest TinyUSB's OUT FIFO was seen, in bytes; at
 *                   @p fifo_size the FIFO overwrites what the host sent
 * @param fifo_size  that FIFO's size, in bytes
 */
void uac_device_spk_health(uint32_t *gap_max_us, uint32_t *fifo_max, uint32_t *fifo_size);

/** The feedback value last sent to the host: samples per 1 ms frame, 16.16. */
uint32_t uac_device_feedback_q16(void);

/** The capture stream: frames handed to TinyUSB, and intervals overwritten before they were sent. */
void uac_device_mic_flow(uint32_t *sent_frames, uint32_t *lost);
/** The host the feedback endpoint is shaped for: Linux reads a value counted
 *  over 128 SOFs, macOS one counted per interval (usb_device_uac.c). Takes
 *  effect at the next SOF. */
typedef enum {
    UAC_HOST_MACOS = 0,
    UAC_HOST_LINUX = 1,
} uac_host_t;
void uac_device_set_host(uac_host_t host);
uac_host_t uac_device_get_host(void);

/** Speaker packets received since boot, and those shorter than the whole frames a millisecond holds. */
void uac_device_spk_packets(uint32_t *packets, uint32_t *short_packets);
/** Frames skipped between speaker packets, frames with two, and the latest
 *  (us after its SOF) a packet's interrupt ran since the last call. */
void uac_device_spk_timing(uint32_t *skipped, uint32_t *doubled, uint32_t *late_us);
/** The longest gap between two SOF interrupts since the last call (us), and their core. */
void uac_device_sof_timing(uint32_t *gap_max_us, uint32_t *core);
/** The tasks a late SOF (over 1.3 ms) interrupted, with counts, as text. */
int uac_device_sof_late(char *out, int len);

#ifdef __cplusplus
}
#endif
