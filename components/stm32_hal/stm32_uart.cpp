/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "stm32_uart.h"
#include "stm32_pwr_sync.h"
#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "crosspad_s3.h"
#endif

static const char *TAG = "STM32_UART";

/* Written by the UART event task, read (and cleared) from the CDC/main task —
 * atomic rather than volatile, which C++20 deprecates for read-modify-write. */
static std::atomic<uint32_t> stm32_rx_msg_count{0};
/* Overflow events on the STM link — each one flushed the whole RX ring (PAD_STATS uart_ovf). */
std::atomic<uint32_t> g_stm_uart_ovf{0};
static std::atomic<uint32_t> stm32_rx_byte_count{0};
static std::atomic<uint32_t> s_last_rx_us{0};

#ifdef CONFIG_IDF_TARGET_ESP32S3

// UART Configuration
#define STM32_UART_NUM         UART_NUM_1
#define STM32_UART_BAUD_RATE   460800
#define STM32_UART_BUF_SIZE    (1024)
#define MIDI_PROCESS_BUF_SIZE  64

static QueueHandle_t stm32_uart_queue;
static TaskHandle_t uart_task_handle = NULL;

static void stm32_uart_event_task(void *pvParameters);

// MIDI callback — raw UART bytes delivered to Arduino MIDI library parser
static void (*midi_rx_callback)(const uint8_t*, size_t, void*) = NULL;
static void* midi_rx_user_data = NULL;

static bool stm32_uart_initialized = false;
static std::atomic<bool> s_raw_mode{false};

esp_err_t stm32_uart_init(void)
{
    if (stm32_uart_initialized) {
        ESP_LOGW(TAG, "STM32 UART already initialized");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Initializing STM32 UART communication");

    uart_config_t uart_config = {
        .baud_rate = STM32_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,   // unused: flow control disabled
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {},
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(STM32_UART_NUM, STM32_UART_BUF_SIZE * 2,
                                          STM32_UART_BUF_SIZE * 2, 20, &stm32_uart_queue, 0),
                       TAG, "Failed to install UART driver");

    ESP_RETURN_ON_ERROR(uart_param_config(STM32_UART_NUM, &uart_config),
                       TAG, "Failed to configure UART parameters");

    /* Hand a message to the event task 2 symbol times after its last byte
     * (~48 us at 460800) instead of the driver's 10 (~0.23 ms); after
     * uart_param_config, which the symbol length comes from. */
    ESP_RETURN_ON_ERROR(uart_set_rx_timeout(STM32_UART_NUM, 2),
                       TAG, "Failed to set UART RX timeout");

    ESP_RETURN_ON_ERROR(uart_set_pin(STM32_UART_NUM, BSP_UART_TX, BSP_UART_RX,
                                    UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                       TAG, "Failed to set UART pins");

    if (xTaskCreate(stm32_uart_event_task, "stm32_uart_event", 3072, NULL, 8, &uart_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UART event task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "STM32 UART initialized on TX:%d, RX:%d", BSP_UART_TX, BSP_UART_RX);
    stm32_uart_initialized = true;
    return ESP_OK;
}

esp_err_t stm32_uart_deinit(void)
{
    if (!stm32_uart_initialized) {
        ESP_LOGW(TAG, "STM32 UART wasn't initialized");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Deinitializing STM32 UART");

    if (uart_task_handle != NULL) {
        vTaskDelete(uart_task_handle);
        uart_task_handle = NULL;
    }

    ESP_RETURN_ON_ERROR(uart_driver_delete(STM32_UART_NUM), TAG, "Failed to delete UART driver");

    stm32_uart_initialized = false;
    return ESP_OK;
}

int stm32_uart_send(const uint8_t* data, size_t len)
{
    if (!stm32_uart_initialized || !data || len == 0) {
        return -1;
    }
    if (s_raw_mode.load(std::memory_order_acquire)) {
        return 0;   /* link owned by the raw-mode caller */
    }
    return uart_write_bytes(STM32_UART_NUM, data, len);
}

void stm32_uart_raw_begin(void)
{
    s_raw_mode.store(true, std::memory_order_release);
    uart_flush_input(STM32_UART_NUM);
}

void stm32_uart_raw_end(void)
{
    uart_flush_input(STM32_UART_NUM);
    xQueueReset(stm32_uart_queue);
    s_raw_mode.store(false, std::memory_order_release);
}

void stm32_uart_raw_flush(void)
{
    uart_flush_input(STM32_UART_NUM);
}

int stm32_uart_raw_read(uint8_t *dst, size_t len, uint32_t timeout_ms)
{
    if (!stm32_uart_initialized || !s_raw_mode.load(std::memory_order_acquire)) return -1;
    return uart_read_bytes(STM32_UART_NUM, dst, len, pdMS_TO_TICKS(timeout_ms));
}

int stm32_uart_raw_write(const uint8_t *src, size_t len)
{
    if (!stm32_uart_initialized || !s_raw_mode.load(std::memory_order_acquire)) return -1;
    return uart_write_bytes(STM32_UART_NUM, src, len);
}

static void stm32_uart_event_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t *buf = static_cast<uint8_t*>(malloc(MIDI_PROCESS_BUF_SIZE));
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate process buffer");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        if (xQueueReceive(stm32_uart_queue, &event, portMAX_DELAY)) {
            switch (event.type) {
                case UART_DATA:
                    s_last_rx_us.store((uint32_t)esp_timer_get_time(), std::memory_order_relaxed);
                    if (s_raw_mode.load(std::memory_order_acquire)) {
                        break;   /* leave the bytes for stm32_uart_raw_read() */
                    }
                    stm32_rx_msg_count.fetch_add(1, std::memory_order_relaxed);
                    {
                        /* Everything buffered, not event.size. The driver's ISR
                         * drops an event silently when this queue (20) is full
                         * -- this task held off behind higher priorities on both
                         * cores, or stopped by a flash write while the IRAM ISR
                         * runs -- and its bytes then stayed behind for good:
                         * every later message waited for the next one to push it
                         * out. An event whose bytes an earlier pass took finds
                         * nothing here and reads nothing, without waiting. */
                        size_t remaining = 0;
                        uart_get_buffered_data_len(STM32_UART_NUM, &remaining);
                        while (remaining > 0) {
                            size_t chunk = (remaining > MIDI_PROCESS_BUF_SIZE) ? MIDI_PROCESS_BUF_SIZE : remaining;
                            int len = uart_read_bytes(STM32_UART_NUM, buf, chunk, 0);
                            if (len > 0) {
                                stm32_rx_byte_count.fetch_add((uint32_t)len, std::memory_order_relaxed);
                                /* Power-state SysEx sniffer, tapped before the
                                 * MIDI parser so it works with no application
                                 * callback. Bytes are not consumed. */
                                stm32_pwr_sync_feed(buf, len);
                                if (midi_rx_callback) {
                                    midi_rx_callback(buf, len, midi_rx_user_data);
                                }
                                // No callback registered → data discarded (BSP-only mode)
                                remaining -= len;
                            } else {
                                break;
                            }
                        }
                    }
                    break;

                case UART_FIFO_OVF:
                case UART_BUFFER_FULL:
                    g_stm_uart_ovf.fetch_add(1, std::memory_order_relaxed);
                    ESP_LOGW(TAG, "UART overflow/full");
                    uart_flush_input(STM32_UART_NUM);
                    xQueueReset(stm32_uart_queue);
                    break;

                case UART_BREAK:
                case UART_PARITY_ERR:
                case UART_FRAME_ERR:
                    ESP_LOGW(TAG, "UART error: %d", event.type);
                    break;

                default:
                    break;
            }
        }
    }

    free(buf);
    vTaskDelete(NULL);
}

void stm32_register_midi_callback(void (*callback)(const uint8_t*, size_t, void*), void* user_data)
{
    midi_rx_callback = callback;
    midi_rx_user_data = user_data;
    if (callback) {
        ESP_LOGI(TAG, "External MIDI callback registered");
    } else {
        ESP_LOGI(TAG, "External MIDI callback unregistered");
    }
}

#else // !CONFIG_IDF_TARGET_ESP32S3 — no STM32 co-processor

esp_err_t stm32_uart_init(void)   { return ESP_OK; }
esp_err_t stm32_uart_deinit(void) { return ESP_OK; }
int stm32_uart_send(const uint8_t*, size_t) { return -1; }
void stm32_uart_raw_begin(void) {}
void stm32_uart_raw_end(void) {}
void stm32_uart_raw_flush(void) {}
int  stm32_uart_raw_read(uint8_t*, size_t, uint32_t) { return -1; }
int  stm32_uart_raw_write(const uint8_t*, size_t) { return -1; }
void stm32_register_midi_callback(void (*)(const uint8_t*, size_t, void*), void*) {}

#endif // CONFIG_IDF_TARGET_ESP32S3

uint32_t stm32_get_rx_msg_count(bool reset)
{
    return reset ? stm32_rx_msg_count.exchange(0, std::memory_order_relaxed)
                 : stm32_rx_msg_count.load(std::memory_order_relaxed);
}

uint32_t stm32_get_rx_byte_count(bool reset)
{
    return reset ? stm32_rx_byte_count.exchange(0, std::memory_order_relaxed)
                 : stm32_rx_byte_count.load(std::memory_order_relaxed);
}

uint32_t stm32_uart_last_rx_us(void)
{
    return s_last_rx_us.load(std::memory_order_relaxed);
}
