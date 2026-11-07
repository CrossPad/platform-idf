/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief BSP ES8388 Codec Driver
 * 
 * This driver provides ES8388 audio codec support for dual I2S interface setup.
 * Compatible with ESP-ADF codec abstraction layer.
 */

#pragma once

#include "esp_err.h"
#include "esp_codec_dev.h"
#include "driver/i2c_master.h"
#include "i2s_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/** ES8388 I2C address (CE pin configuration dependent) */
#define BSP_ES8388_CODEC_ADDR    0x10  /*!< ES8388 I2C address when CE=0 */
#define BSP_ES8388_CODEC_ADDR_ALT 0x11  /*!< ES8388 I2C address when CE=1 */

/**
 * @brief ES8388 codec mode
 */
typedef enum {
    BSP_ES8388_MODE_ENCODE = 1,     /*!< ADC working */
    BSP_ES8388_MODE_DECODE = 2,     /*!< DAC working */
    BSP_ES8388_MODE_BOTH = 3,       /*!< Both ADC and DAC working */
    BSP_ES8388_MODE_LINE_IN = 4,    /*!< Line in mode */
} bsp_es8388_mode_t;

/**
 * @brief ES8388 ADC input selection
 */
typedef enum {
    BSP_ES8388_ADC_INPUT_LINE1 = 0x00,      /*!< Line 1 input */
    BSP_ES8388_ADC_INPUT_LINE2 = 0x50,      /*!< Line 2 input */
    BSP_ES8388_ADC_INPUT_DIFFERENCE = 0xf0, /*!< Differential input */
} bsp_es8388_adc_input_t;

/**
 * @brief ES8388 DAC output selection: which output drivers are powered
 *        (DACPOWER bits, LOUTx/ROUTx enable). A driver powered down still
 *        holds its pins on VREF through 1.5 kOhm unless
 *        bsp_es8388_set_off_outputs_hiz() lets go of them.
 */
typedef enum {
    BSP_ES8388_DAC_OUTPUT_LINE1 = 0x30,     /*!< LOUT1/ROUT1 */
    BSP_ES8388_DAC_OUTPUT_LINE2 = 0x0C,     /*!< LOUT2/ROUT2 */
    BSP_ES8388_DAC_OUTPUT_ALL = 0x3C,       /*!< All outputs */
} bsp_es8388_dac_output_t;

/**
 * @brief ES8388 codec configuration
 */
typedef struct {
    i2c_master_bus_handle_t i2c_handle;    /*!< I2C master bus handle */
    uint8_t i2c_addr;                      /*!< I2C device address */
    bsp_i2s_interface_t i2s_interface;     /*!< Associated I2S interface */
    bsp_es8388_mode_t codec_mode;          /*!< Codec operating mode */
    bsp_es8388_adc_input_t adc_input;      /*!< ADC input selection */
    bsp_es8388_dac_output_t dac_output;    /*!< DAC output selection */
    uint8_t mic_amp_gain;                  /*!< MicAmp PGA gain stage (0..8 → 0..+24dB in 3dB steps). 0 = line-level input. */
} bsp_es8388_config_t;

/**
 * @brief Initialize ES8388 codec
 * 
 * This function initializes the ES8388 codec with the specified configuration.
 * The I2S interface must be initialized before calling this function.
 * Can be called multiple times for different I2S interfaces.
 * 
 * @param[in] config ES8388 configuration
 * 
 * @return 
 *    - ESP_OK on success
 *    - ESP_ERR_INVALID_ARG if config is NULL or invalid
 *    - ESP_ERR_NOT_FOUND if ES8388 not found on I2C bus
 *    - ESP_FAIL on other errors
 */
esp_err_t bsp_es8388_init(const bsp_es8388_config_t *config);

/**
 * @brief Deinitialize ES8388 codec
 * 
 * @return 
 *    - ESP_OK on success
 *    - ESP_FAIL on failure
 */
esp_err_t bsp_es8388_deinit(void);

/**
 * @brief Start ES8388 codec
 * 
 * @param[in] interface I2S interface identifier
 * @param[in] mode Codec mode (encode/decode/both)
 * 
 * @return 
 *    - ESP_OK on success
 *    - ESP_FAIL on failure
 */
esp_err_t bsp_es8388_start(bsp_i2s_interface_t interface, bsp_es8388_mode_t mode);

/**
 * @brief Stop ES8388 codec
 * 
 * @param[in] interface I2S interface identifier
 * @param[in] mode Codec mode to stop
 * 
 * @return 
 *    - ESP_OK on success
 *    - ESP_FAIL on failure
 */
esp_err_t bsp_es8388_stop(bsp_i2s_interface_t interface, bsp_es8388_mode_t mode);

/**
 * @brief Set the ADC's MicAmp PGA gain, both channels, while running
 *
 * @param[in] interface I2S interface identifier
 * @param[in] stage 0..8 = 0..+24 dB in 3 dB steps (clamped). Set before the
 *            codec is initialised, it is the gain its init applies.
 *
 * @return ESP_OK, or the I2C write's error
 */
esp_err_t bsp_es8388_set_mic_gain(bsp_i2s_interface_t interface, uint8_t stage);

/**
 * @brief Set ES8388 volume
 * 
 * @param[in] interface I2S interface identifier
 * @param[in] volume Volume level (0-100)
 * 
 * @return 
 *    - ESP_OK on success
 *    - ESP_ERR_INVALID_ARG if volume out of range
 *    - ESP_FAIL on failure
 */
esp_err_t bsp_es8388_set_volume(bsp_i2s_interface_t interface, int volume);

/**
 * @brief Set the analog volume of some of a codec's output pairs only
 *
 * The same 0-100 scale and analog stage as bsp_es8388_set_volume(), for the
 * pairs in @p outputs (BSP_ES8388_DAC_OUTPUT_LINE1 = LOUT1/ROUT1,
 * BSP_ES8388_DAC_OUTPUT_LINE2 = LOUT2/ROUT2, or both). Where a board wires the
 * two pairs of one codec to different places (speakers, a jack), each keeps
 * its own level on one DAC stream.
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG if volume out of range, ESP_FAIL otherwise
 */
esp_err_t bsp_es8388_set_output_volume(bsp_i2s_interface_t interface, bsp_es8388_dac_output_t outputs, int volume);

/**
 * @brief Get ES8388 volume
 * 
 * @param[in] interface I2S interface identifier
 * @param[out] volume Current volume level (0-100)
 * 
 * @return 
 *    - ESP_OK on success
 *    - ESP_ERR_INVALID_ARG if volume is NULL
 *    - ESP_FAIL on failure
 */
esp_err_t bsp_es8388_get_volume(bsp_i2s_interface_t interface, int *volume);

/**
 * @brief The largest digital peak (fraction of full scale) the analog output
 *        stage carries without clipping at @p volume (0-100).
 *
 * 1.0 up to the codes around 0 dB; lower at the top of the range, where the
 * volume adds gain the stage after it cannot swing at full scale (-3.5 dBFS
 * at 100 %). Whatever feeds the codec should keep its peaks under this.
 */
float bsp_es8388_out_ceiling(int volume);

/** Called after every successful bsp_es8388_set_volume(), on the caller's task. */
typedef void (*bsp_es8388_volume_cb_t)(bsp_i2s_interface_t interface, int volume);

/**
 * @brief Be told whenever a codec's output volume changes, from wherever it
 *        is set (one callback; NULL removes it).
 */
void bsp_es8388_set_volume_callback(bsp_es8388_volume_cb_t cb);

/**
 * @brief Set ES8388 mute state
 * 
 * @param[in] interface I2S interface identifier
 * @param[in] enable true to mute, false to unmute
 * 
 * @return 
 *    - ESP_OK on success
 *    - ESP_FAIL on failure
 */
esp_err_t bsp_es8388_set_mute(bsp_i2s_interface_t interface, bool enable);

/**
 * @brief Configure ES8388 ADC input
 * 
 * @param[in] interface I2S interface identifier
 * @param[in] input ADC input selection
 * 
 * @return 
 *    - ESP_OK on success
 *    - ESP_FAIL on failure
 */
esp_err_t bsp_es8388_config_adc_input(bsp_i2s_interface_t interface, bsp_es8388_adc_input_t input);

/**
 * @brief Configure ES8388 DAC output: which output drivers are powered
 *
 * Kept across bsp_es8388_stop()/start(): start() powers exactly these. Before
 * start() it is only remembered.
 *
 * @param[in] interface I2S interface identifier
 * @param[in] output DAC output selection, or any OR of their bits (0 = none)
 *
 * @return
 *    - ESP_OK on success
 *    - ESP_FAIL on failure
 */
esp_err_t bsp_es8388_config_dac_output(bsp_i2s_interface_t interface, bsp_es8388_dac_output_t output);

/**
 * @brief How hard a powered-down output driver holds its pins
 *
 * DACCONTROL23 VROI: an output driver that is off sits on VREF through
 * 1.5 kOhm (false, the init's choice) or 40 kOhm (true). Where a board wires
 * an output pair to an input jack, 1.5 kOhm through the coupling capacitor
 * loads whatever is plugged in; 40 kOhm lets the source drive the net.
 * Applies to every driver of the codec that is off.
 *
 * @param[in] interface I2S interface identifier
 * @param[in] hiz true for 40 kOhm, false for 1.5 kOhm
 *
 * @return ESP_OK, ESP_FAIL if the codec is not initialized or the write failed
 */
esp_err_t bsp_es8388_set_off_outputs_hiz(bsp_i2s_interface_t interface, bool hiz);

/**
 * @brief Raw register access, for bench measurements (CDC CODEC_REG)
 */
esp_err_t bsp_es8388_read_register(bsp_i2s_interface_t interface, uint8_t reg, uint8_t *value);
esp_err_t bsp_es8388_write_register(bsp_i2s_interface_t interface, uint8_t reg, uint8_t value);

/**
 * @brief Get ESP-ADF codec device handle
 * 
 * This function returns an ESP-ADF codec device handle that can be used
 * with other ESP-ADF audio components.
 * 
 * @param[in] interface I2S interface identifier
 * @return Pointer to codec device handle or NULL if not initialized
 */
esp_codec_dev_handle_t bsp_es8388_get_codec_dev(bsp_i2s_interface_t interface);

/**
 * @brief Deinitialize specific ES8388 codec
 *
 * @param[in] interface I2S interface identifier
 * @return
 *    - ESP_OK on success
 *    - ESP_FAIL on failure
 */
esp_err_t bsp_es8388_deinit_interface(bsp_i2s_interface_t interface);

#ifdef __cplusplus
}
#endif
