

/**
 * @file
 * @brief BSP I2S Dual Driver
 */

#pragma once

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_codec_dev.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief I2S interface identifier
 */
typedef enum {
    BSP_I2S_INTERFACE_0 = 0,  /*!< Primary I2S interface (I2S_NUM_0) */
    BSP_I2S_INTERFACE_1 = 1,  /*!< Secondary I2S interface (I2S_NUM_1) */
    BSP_I2S_INTERFACE_MAX
} bsp_i2s_interface_t;

/**
 * @brief Initialize dual I2S interfaces for codec input
 * 
 * This function initializes both I2S interfaces (I2S0 and I2S1) for stereo codec input.
 * - I2S0 operates in MASTER mode and provides clock signals
 * - I2S1 operates in SLAVE mode and shares clock signals from I2S0
 * 
 * @param[in] i2s_config I2S standard configuration. Pass NULL to use default values
 *            (Stereo, 16-bit, bsp_i2s_get_sample_rate() Hz, Philips mode)
 * 
 * @return 
 *    - ESP_OK on success
 *    - ESP_ERR_INVALID_ARG if configuration is invalid
 *    - ESP_ERR_NO_MEM if memory allocation failed
 *    - ESP_FAIL on other errors
 */
esp_err_t bsp_i2s_init_dual(const i2s_std_config_t *i2s_config);

/* Rate the default I2S configuration is clocked at, when no explicit
 * i2s_std_config_t is passed. Set it before the first init; the codecs and
 * the USB audio bridge read it back through the getter. */
#define BSP_I2S_DEFAULT_SAMPLE_RATE 48000
void bsp_i2s_set_sample_rate(uint32_t hz);
uint32_t bsp_i2s_get_sample_rate(void);

/**
 * @brief Deinitialize all I2S interfaces
 * 
 * @return 
 *    - ESP_OK on success
 *    - ESP_FAIL on failure
 */
esp_err_t bsp_i2s_deinit_all(void);

/**
 * @brief Get I2S TX channel handle for specific interface
 * 
 * @param[in] interface I2S interface identifier
 * @return i2s_chan_handle_t TX channel handle or NULL if not initialized
 */
i2s_chan_handle_t bsp_i2s_get_tx_handle(bsp_i2s_interface_t interface);

/**
 * @brief Get I2S RX channel handle for specific interface
 * 
 * @param[in] interface I2S interface identifier
 * @return i2s_chan_handle_t RX channel handle or NULL if not initialized
 */
i2s_chan_handle_t bsp_i2s_get_rx_handle(bsp_i2s_interface_t interface);

/* The DMA ring of every channel, TX and RX of both interfaces: buffers of
 * BSP_I2S_DMA_FRAME_NUM stereo 16-bit frames. Each buffer is internal RAM, on
 * four channels; i2s_driver.c says why there are this many. */
#define BSP_I2S_DMA_DESC_NUM  6
#define BSP_I2S_DMA_FRAME_NUM 64

/**
 * @brief The TX DMA's own clock, from its end-of-buffer interrupt.
 *
 * `sent` counts buffers the DMA finished playing and `last_us` is when the
 * last one finished (esp_timer, low 32 bits). `unfilled` counts the buffers it
 * then started that nobody had written since their previous turn -- the DAC
 * played silence there (auto_clear). While a render is feeding the channel
 * that is an underrun; before the first write and while the render is parked
 * it counts every buffer.
 */
typedef struct {
    uint32_t sent;
    uint32_t last_us;
    uint32_t unfilled;
} bsp_i2s_tx_clock_t;

/** A consistent snapshot of one interface's TX clock (zeros before init). */
void bsp_i2s_tx_clock(bsp_i2s_interface_t interface, bsp_i2s_tx_clock_t *out);

/**
 * @brief Give `sem` (a FreeRTOS counting semaphore) from the TX interrupt
 *        every time the DMA finishes a buffer; NULL stops it. For a render
 *        paced by the DMA's own clock instead of by a blocking write.
 */
void bsp_i2s_tx_set_waiter(bsp_i2s_interface_t interface, void *sem);

/**
 * @brief Get codec data interface for specific I2S interface
 * 
 * This function returns the ESP-ADF codec data interface for the specified I2S interface,
 * which can be used with other ESP-ADF audio components.
 * 
 * @param[in] interface I2S interface identifier
 * @return Pointer to audio codec data interface or NULL if not initialized
 */
const audio_codec_data_if_t *bsp_i2s_get_codec_itf(bsp_i2s_interface_t interface);

/**
 * @brief Initialize I2S with ES8388 codec
 * 
 * This is a convenience function that initializes both I2S interfaces and ES8388 codecs.
 * It configures:
 * - I2S0 as MASTER with ES8388 codec 0
 * - I2S1 as SLAVE with ES8388 codec 1 (if enabled)
 * 
 * @param[in] i2c_handle I2C master bus handle (must be initialized)
 * @param[in] i2s_config I2S configuration. Pass NULL to use defaults
 * @param[in] dual_codec Enable second ES8388 codec on I2S1 interface
 * 
 * @return 
 *    - ESP_OK on success
 *    - ESP_ERR_INVALID_ARG if i2c_handle is NULL
 *    - ESP_FAIL on other errors
 */
esp_err_t bsp_i2s_es8388_init(i2c_master_bus_handle_t i2c_handle, 
                              const i2s_std_config_t *i2s_config,
                              bool dual_codec);

/**
 * @brief Deinitialize I2S and ES8388 codecs
 * 
 * @return ESP_OK on success
 */
esp_err_t bsp_i2s_es8388_deinit(void);

#ifdef __cplusplus
}
#endif
