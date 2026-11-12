/**
 * @file cdc_rx.h
 * @brief The CDC port's receive side (cdc_rx.cpp): the queue the TinyUSB task
 *        fills and the command loop drains, and the line dispatcher.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "sdkconfig.h"
#include "tusb_cdc_acm.h"

typedef struct {
    uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];     // Data buffer
    size_t buf_len;                                     // Number of bytes received
    uint8_t itf;                                        // Index of CDC device interface
} app_message_t;

/* Created by the boot before USB starts; a message with buf_len 0 only wakes
 * the command loop. */
extern QueueHandle_t app_queue;

/* One CDC read is not one command: splits @p buf into lines and hands each to
 * its handler. */
void cdc_dispatch_lines(const uint8_t *buf, size_t len, uint8_t itf);

#ifdef __cplusplus
extern "C" {
#endif
void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event);
void tinyusb_cdc_line_state_changed_callback(int itf, cdcacm_event_t *event);
#ifdef __cplusplus
}
#endif
