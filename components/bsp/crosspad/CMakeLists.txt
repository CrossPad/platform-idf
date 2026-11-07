idf_component_register(
    SRCS
        crosspad_s3.c
        bsp_sdmmc.c
        crosspad_s3_idf5.c
        bsp_imu.cpp
        i2s_driver.c
        bsp_es8388.c
        performance_monitor.c
        bsp_worker.c
        bsp_ulp.c

    INCLUDE_DIRS
        include

    PRIV_INCLUDE_DIRS
        priv_include

    REQUIRES
        driver
        spiffs
        fatfs
        lcd_touch_ft6x36
        stm32_hal
        arduino_json_file_adapter
        bblanchon__ArduinoJson
        nvs_flash
        crosspad-core
        ulp

    PRIV_REQUIRES
        esp_lcd
        esp_psram
)

# The board's name is a build fact, not a header's: every component that
# requires the BSP gets it on its command line, so `#ifdef BSP_BOARD_CROSSPAD_S3`
# holds in a file that includes nothing from here before testing it.
target_compile_definitions(${COMPONENT_LIB} PUBLIC BSP_BOARD_CROSSPAD_S3)

ulp_embed_binary(ulp_bsp_background "ulp/ulp_background.c" "bsp_ulp.c")
