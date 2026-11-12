/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_spiffs.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_vfs_fat.h"

#include "iot_button.h"
#include "button_gpio.h"
#include "crosspad_s3.h"
#include "display.h"
#include "lcd_touch_ft6x36.h"
#include "esp_lcd_ili9341.h"
#include "src/display/lv_display_private.h"   /* disp->flushing, for the flush wait */
#include "bsp_err_check.h"
#include "stm32_hal_init.h"


static const char *TAG = "CROSSPAD_S3";

/** @cond */
_Static_assert(CONFIG_ESP_LCD_TOUCH_MAX_BUTTONS > 0, "Touch buttons must be supported for this BSP");
/** @endcond */

static const ili9341_lcd_init_cmd_t vendor_specific_init[] = {
    {0xC8, (uint8_t []){0xFF, 0x93, 0x42}, 3, 0}, // same
    {0xC0, (uint8_t []){0x0E, 0x0E}, 2, 0},
    {0xC5, (uint8_t []){0xD0}, 1, 0},  //VCOM
    {0xC1, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39, 0x48, 0x02, 0x0a, 0x08, 0x17, 0x17, 0x0F}, 15, 0},
    {0xE1, (uint8_t []){0x00, 0x28, 0x29, 0x01, 0x0d, 0x03, 0x3f, 0x33, 0x52, 0x04, 0x0f, 0x0e, 0x37, 0x38, 0x0F}, 15, 0},

    {0xB1, (uint8_t []){00, 0x1B}, 2, 0},
    {0x36, (uint8_t []){0x08}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0}, //Pixel Format Set //DPI [2:0],DBI [2:0]
    {0xB7, (uint8_t []){0x06}, 1, 0},

    {0x11, (uint8_t []){0}, 0x80, 0}, //Exit Sleep
    {0x29, (uint8_t []){0}, 0x80, 0}, //Display on

    {0, (uint8_t []){0}, 0xff, 0},
};


#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
static lv_display_t *disp;
static lv_indev_t *disp_indev = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;
#endif // (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

static esp_lcd_touch_handle_t tp;   // LCD touch handle

/**
 * @brief I2C handle for BSP usage
 *
 * In IDF v5.4 you can call i2c_master_get_bus_handle(BSP_I2C_NUM, i2c_master_bus_handle_t *ret_handle)
 * from #include "esp_private/i2c_platform.h" to get this handle
 *
 * For IDF 5.2 and 5.3 you must call bsp_i2c_get_handle()
 */
static i2c_master_bus_handle_t i2c_handle = NULL;
static bool i2c_initialized = false;


typedef enum {
    BSP_BUTTON_TYPE_GPIO,
    BSP_BUTTON_TYPE_CUSTOM
} bsp_button_type_t;

typedef struct {
    bsp_button_type_t type;
    union {
        button_gpio_config_t gpio;
        button_driver_t      custom;
    } cfg;
} bsp_button_config_t;

static const bsp_button_config_t bsp_button_config[BSP_BUTTON_NUM] = {
    {
        .type = BSP_BUTTON_TYPE_GPIO,
        .cfg.gpio = {
            .gpio_num = BSP_BUTTON_CONFIG_IO,
            .active_level = 0,
        }
    }
};

esp_err_t bsp_i2c_init(void)
{
    /* I2C was initialized before */
    if (i2c_initialized) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t i2c_config = {
        .i2c_port = BSP_I2C_NUM,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = 1,
            .allow_pd = 0,
        },
    };
    BSP_ERROR_CHECK_RETURN_ERR(i2c_new_master_bus(&i2c_config, &i2c_handle));

    i2c_initialized = true;
    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
    BSP_ERROR_CHECK_RETURN_ERR(i2c_del_master_bus(i2c_handle));
    i2c_initialized = false;
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    bsp_i2c_init();
    return i2c_handle;
}

static esp_err_t bsp_i2c_device_probe(uint8_t addr)
{
    if (ESP_LCD_TOUCH_IO_I2C_FT6X36_ADDRESS == addr) return ESP_OK; // FT6x36 is optional / sometimes buggy
    return i2c_master_probe(i2c_handle, addr, 100);
}

esp_err_t bsp_spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_BSP_SPIFFS_MOUNT_POINT,
        .partition_label = CONFIG_BSP_SPIFFS_PARTITION_LABEL,
        .max_files = CONFIG_BSP_SPIFFS_MAX_FILES,
#ifdef CONFIG_BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
    };

    esp_err_t ret_val = esp_vfs_spiffs_register(&conf);

    BSP_ERROR_CHECK_RETURN_ERR(ret_val);

    size_t total = 0, used = 0;
    ret_val = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret_val != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret_val));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    return ret_val;
}

esp_err_t bsp_spiffs_unmount(void)
{
    return esp_vfs_spiffs_unregister(CONFIG_BSP_SPIFFS_PARTITION_LABEL);
}


// Bit number used to represent command and parameter
#define LCD_CMD_BITS           8
#define LCD_PARAM_BITS         8
#define LCD_LEDC_CH            CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH

esp_err_t bsp_display_brightness_init(void)
{
    // Initialize STM32 manager if not already done
    if (!stm32_hal_is_initialized()) {
        esp_err_t ret = stm32_hal_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize STM32 HAL for brightness control: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "STM32 HAL initialized for brightness control");
    }

    return bsp_display_brightness_set(5); // Set default brightness to 5% to show that display is on
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    if (brightness_percent < 0) {
        brightness_percent = 0;
    }

    ESP_LOGI(TAG, "Setting LCD backlight: %d%%", brightness_percent);

    // Send brightness command to STM32 via HAL
    esp_err_t ret = stm32_hal_set_brightness((uint8_t)brightness_percent);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send brightness to STM32: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t bsp_display_backlight_off(void)
{
    return bsp_display_brightness_set(0);
}

esp_err_t bsp_display_backlight_on(void)
{
    return bsp_display_brightness_set(100);
}

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
static esp_err_t bsp_lcd_enter_sleep(void)
{
    assert(panel_handle);
    return esp_lcd_panel_disp_on_off(panel_handle, false);
}

static esp_err_t bsp_lcd_exit_sleep(void)
{
    assert(panel_handle);
    return esp_lcd_panel_disp_on_off(panel_handle, true);
}
#endif // (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io)
{
    esp_err_t ret = ESP_OK;
    assert(config != NULL && config->max_transfer_sz > 0);

    ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "Brightness init failed");

    /* Initilize I2C */
    BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());

    ESP_LOGD(TAG, "Initialize SPI bus");
    const spi_bus_config_t buscfg = {
        .sclk_io_num = BSP_LCD_PCLK,
        .mosi_io_num = BSP_LCD_DATA0,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = config->max_transfer_sz,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI init failed");

    ESP_LOGD(TAG, "Install panel IO");
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BSP_LCD_DC,
        .cs_gpio_num = BSP_LCD_CS,
        .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, ret_io), err, TAG, "New panel IO failed");

    ESP_LOGD(TAG, "Install LCD driver");
    const ili9341_vendor_config_t vendor_config = {
        .init_cmds = &vendor_specific_init[0],
        .init_cmds_size = sizeof(vendor_specific_init) / sizeof(ili9341_lcd_init_cmd_t),
    };

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST, // Shared with Touch reset
        .flags.reset_active_high = 1,
        .rgb_ele_order = BSP_LCD_COLOR_SPACE,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
    };

    /* Touch presence is reported, never a veto.
     *
     * The FT6x36 shares BSP_LCD_RST with the panel, and this probe runs
     * before the panel driver has taken that line, so a controller still
     * held in reset does not ACK. Treating that as "no display module"
     * blanks a perfectly good screen -- which is what it did. The ILI9341
     * cannot confirm itself either way (MISO is NC, so it is write-only),
     * and writes aimed at a panel that is not there go nowhere and cost
     * nothing. So create it regardless: a genuinely absent screen is
     * survivable now because setupLVGL() guards the NULL touch indev,
     * which is the thing that actually hung the board -- not the panel.
     *
     * UI_STATE reports touch= separately, so a bench session still sees
     * that this flex did not answer. */
    if (ESP_OK != bsp_i2c_device_probe(ESP_LCD_TOUCH_IO_I2C_FT6X36_ADDRESS)) {
        ESP_LOGW(TAG, "No FT6x36 at 0x%02X - touch unavailable, LCD still driven",
                 ESP_LCD_TOUCH_IO_I2C_FT6X36_ADDRESS);
    }

    panel_config.vendor_config = (void *)&vendor_config;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_ili9341(*ret_io, (const esp_lcd_panel_dev_config_t *)&panel_config, ret_panel), err, TAG, "New panel failed");

    /* Complain about these, but do not fail on them. They were unchecked
     * for as long as this board has worked, so a panel that returns an
     * error here has still been driving a visible screen; refusing it
     * would blank a display over a fault it evidently survives. */
    esp_err_t perr = esp_lcd_panel_init(*ret_panel);
    if (perr != ESP_OK) {
        ESP_LOGW(TAG, "Panel init: %s (continuing)", esp_err_to_name(perr));
    }
    perr = esp_lcd_panel_mirror(*ret_panel, true, true);
    if (perr != ESP_OK) {
        ESP_LOGW(TAG, "Panel mirror: %s (continuing)", esp_err_to_name(perr));
    }
    return ret;

err:
    if (*ret_panel) {
        esp_lcd_panel_del(*ret_panel);
    }
    if (*ret_io) {
        esp_lcd_panel_io_del(*ret_io);
    }
    spi_bus_free(BSP_LCD_SPI_NUM);
    return ret;
}

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg)
{
    assert(cfg != NULL);
    esp_lcd_panel_io_handle_t io_handle = NULL;
    const bsp_display_config_t bsp_disp_cfg = {
        .max_transfer_sz = (BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT) * sizeof(uint16_t),
    };
    if (bsp_display_new(&bsp_disp_cfg, &panel_handle, &io_handle) != ESP_OK) {
        ESP_LOGE(TAG, "bsp_display_new failed - LCD unavailable");
        return NULL;
    }

    esp_lcd_panel_disp_on_off(panel_handle, true);

    /* Add LCD screen */
    ESP_LOGD(TAG, "Add LCD screen");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = cfg->buffer_size,
        .double_buffer = cfg->double_buffer,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma = cfg->flags.buff_dma,
            .buff_spiram = cfg->flags.buff_spiram,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = (BSP_LCD_BIGENDIAN ? true : false),
#endif
        }
    };

    return lvgl_port_add_disp(&disp_cfg);
}

__attribute__((weak)) esp_err_t esp_lcd_touch_enter_sleep(esp_lcd_touch_handle_t tp)
{
    ESP_LOGE(TAG, "Sleep mode not supported!");
    return ESP_FAIL;
}

__attribute__((weak)) esp_err_t esp_lcd_touch_exit_sleep(esp_lcd_touch_handle_t tp)
{
    ESP_LOGE(TAG, "Sleep mode not supported!");
    return ESP_FAIL;
}

static esp_err_t bsp_touch_enter_sleep(void)
{
    assert(tp);
    return esp_lcd_touch_enter_sleep(tp);
}

static esp_err_t bsp_touch_exit_sleep(void)
{
    assert(tp);
    return esp_lcd_touch_exit_sleep(tp);
}
#endif // (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

esp_err_t bsp_touch_new(esp_lcd_touch_handle_t *ret_touch)
{
    /* Initilize I2C */
    BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());

    /* Initialize touch */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC, // Shared with LCD reset
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 1,
            .mirror_y = 0,
        },
    };
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config;

    vTaskDelay(100 / portTICK_PERIOD_MS);

    esp_err_t ret = bsp_i2c_device_probe(ESP_LCD_TOUCH_IO_I2C_FT6X36_ADDRESS);
    if (ESP_OK == ret) {
        esp_lcd_panel_io_i2c_config_t cfg = ESP_LCD_TOUCH_IO_I2C_FT6X36_CONFIG();
        memcpy(&tp_io_config, &cfg, sizeof(cfg));
        tp_io_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ; // This parameter was introduced together with I2C Driver-NG in IDF v5.2
    } else {
        ESP_LOGE(TAG, "Touch not found in time, be sure that your display is connected properly: %s", esp_err_to_name(ret));
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle), TAG, "");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft6x36(tp_io_handle, &tp_cfg, ret_touch), TAG, "New ft6x36 failed");

    return ESP_OK;
}

/* ID_G_THGROUP per level, lower = lighter touch. Low is the value
 * touch_ft6x36_init() has always written. */
static const uint8_t k_touch_thgroup[] = {
    [BSP_TOUCH_SENSITIVITY_LOW]    = 70,
    [BSP_TOUCH_SENSITIVITY_MEDIUM] = 50,
    [BSP_TOUCH_SENSITIVITY_HIGH]   = 30,
};

esp_err_t bsp_touch_set_sensitivity(bsp_touch_sensitivity_t level)
{
    ESP_RETURN_ON_FALSE(tp, ESP_ERR_INVALID_STATE, TAG, "no touch controller");
    ESP_RETURN_ON_FALSE((unsigned)level < sizeof(k_touch_thgroup) / sizeof(k_touch_thgroup[0]),
                        ESP_ERR_INVALID_ARG, TAG, "touch sensitivity %d", (int)level);
    ESP_RETURN_ON_ERROR(esp_lcd_touch_ft6x36_set_threshold(tp, k_touch_thgroup[level]), TAG,
                        "touch sensitivity");
    ESP_LOGI(TAG, "Touch sensitivity %d (THGROUP %u)", (int)level, k_touch_thgroup[level]);
    return ESP_OK;
}

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
static lv_indev_t *bsp_display_indev_init(lv_display_t *disp)
{
    esp_err_t err = bsp_touch_new(&tp);
    if (err != ESP_OK || tp == NULL) {
        ESP_LOGW(TAG, "Touch init failed (%s) - continuing without touch", esp_err_to_name(err));
        return NULL;
    }

    /* Add touch input (for selected screen) */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = tp,
    };

    return lvgl_port_add_touch(&touch_cfg);
}

/* Tick at which the LCD/TS reset was released, 0 while it has not happened.
 * The FT6x36 needs 300 ms after reset before it answers; the panel less.
 * Pulsed early in app_main, that settle overlaps the rest of the bring-up and
 * bsp_display_start() only waits for whatever of it is still outstanding. */
#define BSP_DISPLAY_RESET_SETTLE_MS 300
static TickType_t s_lcd_reset_released = 0;

void bsp_display_reset_early(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BSP_LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(BSP_LCD_RST, 0);
    vTaskDelay(40 / portTICK_PERIOD_MS);
    gpio_set_level(BSP_LCD_RST, 1);
    s_lcd_reset_released = xTaskGetTickCount();
    if (s_lcd_reset_released == 0) s_lcd_reset_released = 1;
}

lv_display_t *bsp_display_start(void)
{
    if (s_lcd_reset_released == 0) {
        bsp_display_reset_early();
    }
    const TickType_t settle = pdMS_TO_TICKS(BSP_DISPLAY_RESET_SETTLE_MS);
    const TickType_t since = xTaskGetTickCount() - s_lcd_reset_released;
    if (since < settle) {
        vTaskDelay(settle - since);
    }

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
#if CONFIG_BSP_LCD_DRAW_BUF_DOUBLE
        .double_buffer = 1,
#else
        .double_buffer = 0,
#endif
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        }
    };
    cfg.lvgl_port_cfg.task_affinity = 0;
    /* 8192 left 608 bytes free at idle — measured by the perfmon task-stack
     * dump, not guessed. Anything built from inside an lv_async_call() runs
     * here, and a screen with a widget per SD-card entry (the kit browser)
     * walks straight through that margin: it reset the device on open with no
     * usable coredump, which is what an overflow into the TCB looks like. */
    cfg.lvgl_port_cfg.task_stack = 16384;
    return bsp_display_start_with_config(&cfg);
}

/* LVGL waits here for the panel's DMA to take a band. Without a callback it
 * spins on disp->flushing at its own priority (4, core 0) -- and a whole
 * screen is ~31 ms over the SPI, so nothing below it on core 0 ran meanwhile:
 * a screen redrawn every frame kept LVGL busy 100 % and held a CDC
 * verb for 1.4 s. Sleeping a tick at a time gives that time back, for ~0.5 ms
 * a wait on average. */
static void bsp_flush_wait(lv_display_t *d)
{
    while (d->flushing) vTaskDelay(1);
}

lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg)
{
    assert(cfg != NULL);

    if (lvgl_port_init(&cfg->lvgl_port_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed - display unavailable");
        return NULL;
    }

    if (bsp_display_brightness_init() != ESP_OK) {
        ESP_LOGE(TAG, "Display brightness init failed - display unavailable");
        return NULL;
    }

    disp = bsp_display_lcd_init(cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "Display LCD init failed - running headless");
        return NULL;
    }
    if (lvgl_port_lock(0)) {
        lv_display_set_flush_wait_cb(disp, bsp_flush_wait);
        lvgl_port_unlock();
    }

    disp_indev = bsp_display_indev_init(disp);
    if (disp_indev == NULL) {
        ESP_LOGW(TAG, "Touch input unavailable - encoder-only navigation");
    }

    return disp;
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return disp_indev;
}

void bsp_display_rotate(lv_display_t *disp, lv_disp_rotation_t rotation)
{
    lv_disp_set_rotation(disp, rotation);
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    lvgl_port_unlock();
}

esp_err_t bsp_display_enter_sleep(void)
{
    BSP_ERROR_CHECK_RETURN_ERR(bsp_lcd_enter_sleep());
    BSP_ERROR_CHECK_RETURN_ERR(bsp_display_backlight_off());
    BSP_ERROR_CHECK_RETURN_ERR(bsp_touch_enter_sleep());
    return ESP_OK;
}

esp_err_t bsp_display_exit_sleep(void)
{
    BSP_ERROR_CHECK_RETURN_ERR(bsp_lcd_exit_sleep());
    BSP_ERROR_CHECK_RETURN_ERR(bsp_display_backlight_on());
    BSP_ERROR_CHECK_RETURN_ERR(bsp_touch_exit_sleep());
    return ESP_OK;
}
#endif // (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

esp_err_t bsp_iot_button_create(button_handle_t btn_array[], int *btn_cnt, int btn_array_size)
{
    esp_err_t ret = ESP_OK;
    const button_config_t btn_config = {0};
    if ((btn_array_size < BSP_BUTTON_NUM) ||
            (btn_array == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (btn_cnt) {
        *btn_cnt = 0;
    }
    for (int i = 0; i < BSP_BUTTON_NUM; i++) {
        if (bsp_button_config[i].type == BSP_BUTTON_TYPE_CUSTOM) {
            if (tp == NULL) {
                BSP_ERROR_CHECK_RETURN_ERR(bsp_touch_new(&tp));
            }
            ret |= iot_button_create(&btn_config, &bsp_button_config[i].cfg.custom, &btn_array[i]);
        } else if (bsp_button_config[i].type == BSP_BUTTON_TYPE_GPIO) {
            ret |= iot_button_new_gpio_device(&btn_config, &bsp_button_config[i].cfg.gpio, &btn_array[i]);
        } else {
            ESP_LOGW(TAG, "Unsupported button type!");
        }
        if (btn_cnt) {
            (*btn_cnt)++;
        }
    }
    return ret;
}

esp_err_t bsp_stm32_init(void)
{
    ESP_LOGI(TAG, "Setting up  GPIO pins");
    // Set up the BUTTON GPIO pin.
    // gpio_pad_select_gpio(BUTTON_2);
    gpio_set_direction((gpio_num_t)BSP_BUTTON_2, GPIO_MODE_INPUT); // Convert BUTTON_2 to gpio_num_t type
    // Set up the STM32 reset GPIO pin.
    // gpio_pad_select_gpio(STM_RST_PIN);
    gpio_set_level((gpio_num_t)BSP_STM_RST_PIN, 0);                    // Convert STM_RST_PIN to gpio_num_t type
    gpio_set_direction((gpio_num_t)BSP_STM_RST_PIN, GPIO_MODE_OUTPUT); // Convert STM_RST_PIN to gpio_num_t type

    ESP_LOGI(TAG, "Initializing STM32 communication subsystem via HAL");
    esp_err_t ret = stm32_hal_init();
    
    return ret;
}

void bsp_stm32_reset(void)
{
    gpio_set_level((gpio_num_t)BSP_STM_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level((gpio_num_t)BSP_STM_RST_PIN, 0);
}

esp_err_t bsp_stm32_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing STM32 communication subsystem via HAL");
    return stm32_hal_deinit();
}
