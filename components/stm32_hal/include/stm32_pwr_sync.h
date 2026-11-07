/*
 * STM32 ↔ ESP power-state synchronization.
 *
 * The STM announces sleep / ship / wake over the UART link as a vendor SysEx:
 *
 *     F0 7D 50 <state> F7
 *
 * This module parses that stream, calls the user callback for
 * SLEEP_PENDING / SHIP_PENDING / WOKE, and offers stm32_pwr_sync_ack(), which
 * writes PWR_ACK_MAGIC to REG_CMD_PWR_ACK over I2C so the STM can continue.
 *
 * The STM waits 3 s for that ACK and then continues on its own, so a late one
 * cannot hang the device — it only costs the ESP its graceful flush.
 *
 * Feed every UART RX byte to stm32_pwr_sync_feed_byte(); bytes are not
 * consumed and still reach the MIDI parser.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called when a PWR_IRQ SysEx is decoded. state is one of PWR_STATE_*. */
typedef void (*stm32_pwr_state_cb_t)(uint8_t state, void *user_data);

/** Register the callback; user_data is passed through to it. */
void stm32_pwr_sync_set_callback(stm32_pwr_state_cb_t cb, void *user_data);

/** Feed one UART RX byte to the parser. Safe from a task or an ISR. */
void stm32_pwr_sync_feed_byte(uint8_t b);

/** Feed a buffer of bytes. */
void stm32_pwr_sync_feed(const uint8_t *buf, size_t len);

/** Write PWR_ACK_MAGIC to REG_CMD_PWR_ACK. Call once the graceful cleanup is
 *  done — from the callback or asynchronously after it. */
esp_err_t stm32_pwr_sync_ack(void);

#ifdef __cplusplus
}
#endif
