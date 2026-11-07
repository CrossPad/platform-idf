/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize STM32 UART communication
 */
esp_err_t stm32_uart_init(void);

/**
 * @brief Deinitialize STM32 UART communication
 */
esp_err_t stm32_uart_deinit(void);

/**
 * @brief Send raw bytes to STM32 via UART
 *
 * @param data Pointer to byte buffer
 * @param len Number of bytes to send
 * @return Number of bytes written, or -1 on error
 */
int stm32_uart_send(const uint8_t* data, size_t len);

/**
 * @brief Register a callback for received MIDI data
 *
 * Raw UART bytes are delivered to this callback. Used by EspStm32MidiTransport
 * for Arduino MIDI library parsing. If not registered, data is discarded.
 */
void stm32_register_midi_callback(void (*callback)(const uint8_t*, size_t, void*), void* user_data);

/**
 * @brief Get number of UART RX messages since last reset
 */
uint32_t stm32_get_rx_msg_count(bool reset);

/**
 * @brief Get number of UART RX bytes since last reset
 */
uint32_t stm32_get_rx_byte_count(bool reset);

/**
 * @brief When the event task last took a UART_DATA event (esp_timer, low 32
 *        bits): the earliest point on the ESP a received byte is timed at,
 *        2 symbol times (RX timeout) after the message's last byte.
 */
uint32_t stm32_uart_last_rx_us(void);

/* Raw mode — exclusive use of the link by one caller (the STM firmware
 * updater). While active the event task leaves received bytes in the driver
 * ring for stm32_uart_raw_read(), and stm32_uart_send() drops everything so
 * neither the heartbeat nor MIDI thru can corrupt the byte stream. */
void stm32_uart_raw_begin(void);
void stm32_uart_raw_end(void);
void stm32_uart_raw_flush(void);
int  stm32_uart_raw_read(uint8_t *dst, size_t len, uint32_t timeout_ms);
int  stm32_uart_raw_write(const uint8_t *src, size_t len);

#ifdef __cplusplus
}
#endif
