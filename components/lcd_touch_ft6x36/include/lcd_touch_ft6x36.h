#ifndef LCD_TOUCH_FT6X36_H
#define LCD_TOUCH_FT6X36_H

#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a new FT6x36 touch driver
 *
 * @note  The I2C communication should be initialized before use this function.
 *
 * @param io LCD panel IO handle, it should be created by `esp_lcd_new_panel_io_i2c()`
 * @param config Touch panel configuration
 * @param out_touch Touch panel handle
 * @return
 *      - ESP_OK: on success
 *      - ESP_ERR_NO_MEM: if there is no memory for allocating main structure
 */
esp_err_t esp_lcd_touch_new_i2c_ft6x36(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config, esp_lcd_touch_handle_t *out_touch);

/**
 * @brief Set the valid-touch threshold (ID_G_THGROUP) of a running controller
 *
 * Lower is more sensitive. Takes effect on the next scan, no reset needed.
 *
 * @param tp Touch panel handle from esp_lcd_touch_new_i2c_ft6x36()
 * @param threshold Register value written to ID_G_THGROUP
 * @return
 *      - ESP_OK: written and read back unchanged
 *      - ESP_ERR_INVALID_RESPONSE: the read-back differs
 *      - I2C errors otherwise
 */
esp_err_t esp_lcd_touch_ft6x36_set_threshold(esp_lcd_touch_handle_t tp, uint8_t threshold);

/**
 * @brief I2C address of the FT6x36 controller
 *
 */
#define ESP_LCD_TOUCH_IO_I2C_FT6X36_ADDRESS     (0x38)

/**
 * @brief Touch IO configuration structure
 *
 */
#define ESP_LCD_TOUCH_IO_I2C_FT6X36_CONFIG()       \
    {                                               \
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_FT6X36_ADDRESS, \
        .control_phase_bytes = 1,                   \
        .dc_bit_offset = 0,                         \
        .lcd_cmd_bits = 8,                          \
        .flags =                                    \
        {                                           \
            .disable_control_phase = 1,             \
        }                                           \
    }

#ifdef __cplusplus
}
#endif

#endif // LCD_TOUCH_FT6X36_H
