/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

 /*
    Edited by Mateusz Czarnecki for CrossPad project
 */

/**
 * @file
 * @brief ESP BSP: CrossPad
 */

#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
//#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "esp_codec_dev.h"
#include "iot_button.h"
#include "config.h"
#include "display.h"

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
#include "lvgl.h"
#include "esp_lvgl_port.h"
#endif // BSP_CONFIG_NO_GRAPHIC_LIB == 0

#define CONFIG_BSP_SPIFFS_MOUNT_POINT "/spiffs"
#define CONFIG_BSP_SPIFFS_PARTITION_LABEL "spiffs"
#define CONFIG_BSP_SPIFFS_MAX_FILES 5

/**************************************************************************************************
 *  BSP Board Name
 **************************************************************************************************/

/* BSP_BOARD_CROSSPAD_S3 is not defined here: CMakeLists.txt puts it on the
 * command line of every component that requires this one, so a test of it
 * never depends on which headers came first. */

/**************************************************************************************************
 *  BSP Capabilities
 **************************************************************************************************/

/** @defgroup capabilities Capabilities
 *  @brief BSP Capabilities
 *  @{
 */
#define BSP_CAPS_DISPLAY        1
#define BSP_CAPS_TOUCH          1
#define BSP_CAPS_BUTTONS        1
#define BSP_CAPS_AUDIO          1
#define BSP_CAPS_AUDIO_SPEAKER  1
#define BSP_CAPS_AUDIO_MIC      1
#define BSP_CAPS_AUDIO_DUAL     1
#define BSP_CAPS_SDCARD         1
#define BSP_CAPS_IMU            1
#define BSP_LED_NUM             16
#define BSP_CAPS_LED            BSP_LED_NUM
/** @} */ // end of capabilities

/**************************************************************************************************
 *  CrossPad pinout - selected per PCB revision (Kconfig BSP_BOARD_REV)
 *
 *  Each revision lives in its own header under include/pinout/. To add a new
 *  rev: copy pinout_v2.h, fill in the GPIOs, add a Kconfig choice entry, and
 *  add the matching #elif branch below.
 **************************************************************************************************/
#if defined(CONFIG_BSP_BOARD_REV_V2)
#  include "pinout/pinout_v2.h"
#  define BSP_BOARD_REV_NAME "v2"
#  define BSP_BOARD_REV_NUM 2
#elif defined(CONFIG_BSP_BOARD_REV_V1)
#  include "pinout/pinout_v1.h"
#  define BSP_BOARD_REV_NAME "v1"
#  define BSP_BOARD_REV_NUM 1
#else
#  error "No CrossPad PCB revision selected - set BSP_BOARD_REV in menuconfig"
#endif

// Capabilities derived from pinout (1 if pin is wired, 0 if GPIO_NUM_NC)
#define BSP_HAS_JACK_ADC          (ADC_PIN != GPIO_NUM_NC)
#define BSP_HAS_BACK_COVER_DETECT (BSP_BACK_COVER_PIN != GPIO_NUM_NC)
#define BSP_HAS_IMU_INT2          (BSP_IMU_INT2_PIN != GPIO_NUM_NC)

// ADC oversampling (revision-independent)
#define NUM_ADC_SAMPLES 8

typedef enum {
#ifdef BSP_BUTTON_CONFIG_IO
    BSP_BUTTON_1, // encoder momentary switch
#endif
#if CONFIG_BSP_BUTTONS_NUM > 1
    BSP_BUTTON_2,
#endif
    BSP_BUTTON_NUM
} bsp_button_t;
/** @} */ // end of buttons



#ifdef __cplusplus
extern "C" {
#endif

/** \addtogroup g03_audio
 *  @{
 */

/**************************************************************************************************
 *
 * I2S audio interface
 *
 * CrossPad has dual ES8388 audio codecs connected via I2S:
 *  - I2S0 (Master): primary codec for playback and recording
 *  - I2S1 (Slave): secondary codec, shares clock from I2S0
 *
 * For dual-codec initialization use bsp_i2s_es8388_init() from i2s_driver.h.
 * For individual codec/I2S control, see bsp_es8388.h and i2s_driver.h.
 *
 * Legacy single-codec functions (bsp_audio_init / bsp_audio_get_codec_itf) are
 * still available in crosspad_s3_idf5.c for backward compatibility.
 **************************************************************************************************/

/**
 * @brief Init audio (legacy single-codec path)
 *
 * @note Prefer bsp_i2s_es8388_init() from i2s_driver.h for dual-codec support.
 * @param[in]  i2s_config I2S configuration. Pass NULL to use default values (Mono, duplex, 16bit, 22050 Hz)
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_NOT_SUPPORTED The communication mode is not supported on the current chip
 *      - ESP_ERR_INVALID_ARG   NULL pointer or invalid configuration
 *      - ESP_ERR_NOT_FOUND     No available I2S channel found
 *      - ESP_ERR_NO_MEM        No memory for storing the channel information
 *      - ESP_ERR_INVALID_STATE This channel has not initialized or already started
 */
esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config);

/**
 * @brief Get codec I2S interface (legacy single-codec path)
 *
 * @note Prefer bsp_i2s_get_codec_itf() from i2s_driver.h for dual-codec support.
 * @return
 *      - Pointer to codec I2S interface handle or NULL when error occurred
 */
const audio_codec_data_if_t *bsp_audio_get_codec_itf(void);

/** @} */ // end of audio

/** \addtogroup g01_i2c
 *  @{
 */

/**************************************************************************************************
 *
 * I2C interface
 *
 * Devices connected to I2C peripheral:
 *  - Dual ES8388 audio codecs (configuration only, data via I2S)
 *  - LCD Touch controller FT6x36
 *  - IMU LSM6DSO (6-axis: accelerometer + gyroscope)
 **************************************************************************************************/
#define BSP_I2C_NUM     CONFIG_BSP_I2C_NUM

/**
 * @brief Init I2C driver
 *
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_ARG   I2C parameter error
 *      - ESP_FAIL              I2C driver installation error
 *
 */
esp_err_t bsp_i2c_init(void);

/**
 * @brief Deinit I2C driver and free its resources
 *
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_ARG   I2C parameter error
 *
 */
esp_err_t bsp_i2c_deinit(void);

/**
 * @brief Get I2C driver handle
 *
 * @return
 *      - I2C handle
 */
i2c_master_bus_handle_t bsp_i2c_get_handle(void);

/** @} */ // end of i2c

/** \addtogroup g02_storage
 *  @{
 */

/**************************************************************************************************
 *
 * SPIFFS
 *
 * After mounting the SPIFFS, it can be accessed with stdio functions ie.:
 * \code{.c}
 * FILE* f = fopen(BSP_SPIFFS_MOUNT_POINT"/hello.txt", "w");
 * fprintf(f, "Hello World!\n");
 * fclose(f);
 * \endcode
 **************************************************************************************************/
#define BSP_SPIFFS_MOUNT_POINT      CONFIG_BSP_SPIFFS_MOUNT_POINT

/**
 * @brief Mount SPIFFS to virtual file system
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if esp_vfs_spiffs_register was already called
 *      - ESP_ERR_NO_MEM if memory can not be allocated
 *      - ESP_FAIL if partition can not be mounted
 *      - other error codes
 */
esp_err_t bsp_spiffs_mount(void);

/**
 * @brief Unmount SPIFFS from virtual file system
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_FOUND if the partition table does not contain SPIFFS partition with given label
 *      - ESP_ERR_INVALID_STATE if esp_vfs_spiffs_unregister was already called
 *      - ESP_ERR_NO_MEM if memory can not be allocated
 *      - ESP_FAIL if partition can not be mounted
 *      - other error codes
 */
esp_err_t bsp_spiffs_unmount(void);

/** @} */ // end of storage

/** \addtogroup g04_display
 *  @{
 */

/**************************************************************************************************
 *
 * LCD interface
 *
 * CrossPad uses a 2.4" ILI9341 display (320x240, 16-bit RGB565) with FT6x36 capacitive touch.
 * Backlight brightness is controlled via STM32 co-processor.
 *
 * LVGL is used as graphics library. LVGL is NOT thread safe, therefore the user must take LVGL mutex
 * by calling bsp_display_lock() before calling any LVGL API (lv_...) and then give the mutex with
 * bsp_display_unlock().
 *
 * Display's backlight must be enabled explicitly by calling bsp_display_backlight_on()
 **************************************************************************************************/
#define BSP_LCD_PIXEL_CLOCK_HZ     (40 * 1000 * 1000)
#define BSP_LCD_SPI_NUM            (SPI3_HOST)

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

/**
 * @brief BSP display configuration structure
 */
typedef struct {
    lvgl_port_cfg_t lvgl_port_cfg;  /*!< LVGL port configuration */
    uint32_t        buffer_size;    /*!< Size of the buffer for the screen in pixels */
    bool            double_buffer;  /*!< True, if should be allocated two buffers */
    struct {
        unsigned int buff_dma: 1;    /*!< Allocated LVGL buffer will be DMA capable */
        unsigned int buff_spiram: 1; /*!< Allocated LVGL buffer will be in PSRAM */
    } flags;
} bsp_display_cfg_t;

/**
 * @brief Initialize display
 *
 * This function initializes SPI, display controller and starts LVGL handling task.
 * LCD backlight must be enabled separately by calling bsp_display_brightness_set()
 *
 * @return Pointer to LVGL display or NULL when error occurred
 */
lv_display_t *bsp_display_start(void);

/**
 * @brief Pulse the LCD/touch reset line now, so its settle time overlaps the
 *        rest of the boot instead of being waited out inside bsp_display_start().
 *
 * Optional: bsp_display_start() resets the panel itself when this was not
 * called, and otherwise waits only for what is left of the settle window.
 */
void bsp_display_reset_early(void);

/**
 * @brief Initialize display
 *
 * This function initializes SPI, display controller and starts LVGL handling task.
 * LCD backlight must be enabled separately by calling bsp_display_brightness_set()
 *
 * @param cfg display configuration
 *
 * @return Pointer to LVGL display or NULL when error occurred
 */
lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg);

/**
 * @brief Get pointer to input device (touch, buttons, ...)
 *
 * @note The LVGL input device is initialized in bsp_display_start() function.
 *
 * @return Pointer to LVGL input device or NULL when not initialized
 */
lv_indev_t *bsp_display_get_input_dev(void);

/**
 * @brief Take LVGL mutex
 *
 * @param timeout_ms Timeout in [ms]. 0 will block indefinitely.
 * @return true  Mutex was taken
 * @return false Mutex was NOT taken
 */
bool bsp_display_lock(uint32_t timeout_ms);

/**
 * @brief Give LVGL mutex
 *
 */
void bsp_display_unlock(void);

/**
 * @brief Set display enter sleep mode
 *
 * All the display (LCD, backlight, touch) will enter sleep mode.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_SUPPORTED if this function is not supported by the panel
 */
esp_err_t bsp_display_enter_sleep(void);

/**
 * @brief Set display exit sleep mode
 *
 * All the display (LCD, backlight, touch) will exit sleep mode.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_SUPPORTED if this function is not supported by the panel
 */
esp_err_t bsp_display_exit_sleep(void);

/**
 * @brief Rotate screen
 *
 * Display must be already initialized by calling bsp_display_start()
 *
 * @param[in] disp Pointer to LVGL display
 * @param[in] rotation Angle of the display rotation
 */
void bsp_display_rotate(lv_display_t *disp, lv_disp_rotation_t rotation);
#endif // BSP_CONFIG_NO_GRAPHIC_LIB == 0

/** @} */ // end of display

/** \addtogroup g05_touch
 *  @{
 */

/**************************************************************************************************
 *
 * Touch controller
 *
 * FT6x36 capacitive touch controller via I2C
 **************************************************************************************************/

#define BSP_TOUCH_I2C_CLK           BSP_I2C_SCL
#define BSP_TOUCH_I2C_DATA          BSP_I2C_SDA

/**
 * @brief Initialize touch controller
 *
 * Initialize I2C touch controller. This function can be called only once,
 * before any other touch function.
 *
 * @param[out] ret_touch    Pointer to touch controller handle. Can be used with other esp_lcd_touch_* functions.
 * @return
 *      - ESP_OK                on success
 *      - ESP_ERR_INVALID_ARG   when the parameter ret_touch is NULL
 *      - ESP_FAIL              on other errors
 */
esp_err_t bsp_touch_new(esp_lcd_touch_handle_t *ret_touch);

/**
 * @brief Touch sensitivity levels; the BSP maps each to a controller threshold
 */
typedef enum {
    BSP_TOUCH_SENSITIVITY_LOW = 0,  ///< the threshold the panel has always run at
    BSP_TOUCH_SENSITIVITY_MEDIUM,
    BSP_TOUCH_SENSITIVITY_HIGH,
} bsp_touch_sensitivity_t;

/**
 * @brief Change the sensitivity of the running touch controller, no reset
 *
 * @return
 *      - ESP_OK                on success
 *      - ESP_ERR_INVALID_STATE when no touch controller came up
 *      - ESP_ERR_INVALID_ARG   for an unknown level
 *      - controller/I2C errors otherwise
 */
esp_err_t bsp_touch_set_sensitivity(bsp_touch_sensitivity_t level);

/** @} */ // end of touch

/** \addtogroup g06_buttons
 *  @{
 */

/**************************************************************************************************
 *
 * Button
 *
 * CrossPad has a rotary encoder with momentary switch on GPIO_NUM_0.
 **************************************************************************************************/

/**
 * @brief Initialize all buttons
 *
 * Returned button handlers must be used with espressif/button component API
 *
 * @note For LCD panel button which is defined as BSP_BUTTON_MAIN, bsp_display_start should
 *       be called before call this function.
 *
 * @param[out] btn_array      Output button array
 * @param[out] btn_cnt        Number of button handlers saved to btn_array, can be NULL
 * @param[in]  btn_array_size Size of output button array. Must be at least BSP_BUTTON_NUM
 * @return
 *     - ESP_OK               All buttons initialized
 *     - ESP_ERR_INVALID_ARG  btn_array is too small or NULL
 *     - ESP_FAIL             Underlaying iot_button_create failed
 */
esp_err_t bsp_iot_button_create(button_handle_t btn_array[], int *btn_cnt, int btn_array_size);

/** @} */ // end of buttons

/** @defgroup g99_stm32 STM32 Communication
 *  @brief STM32 communication BSP API
 *  @{
 */

/**
 * @brief Initialize STM32 communication subsystem
 *
 * This function initializes UART communication with STM32 microcontroller
 * and starts the communication task for handling MIDI sysex messages.
 * It should be called during BSP initialization.
 *
 * @return
 *     - ESP_OK               STM32 communication initialized successfully
 *     - ESP_FAIL             Initialization failed
 */
esp_err_t bsp_stm32_init(void);

/**
 * @brief Deinitialize STM32 communication subsystem
 *
 * @return
 *     - ESP_OK               STM32 communication deinitialized successfully
 *     - ESP_FAIL             Deinitialization failed
 */
esp_err_t bsp_stm32_deinit(void);

/** @} */ // end of stm32



#ifdef __cplusplus
}
#endif
