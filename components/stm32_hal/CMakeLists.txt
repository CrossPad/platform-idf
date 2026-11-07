if(IDF_TARGET STREQUAL "esp32s3")
    set(STM32_HAL_BSP_REQUIRES crosspad)
else()
    set(STM32_HAL_BSP_REQUIRES "")
endif()

idf_component_register(
    SRCS
        "stm32_uart.cpp"
        "stm32_i2c.c"
        "stm32_hal_init.c"
        "stm32_pwr_sync.c"
        "stm32_fw_update.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        driver
        crosspad-core
        esp_common
        freertos
        esp_timer
        esp_rom
        ${STM32_HAL_BSP_REQUIRES}
)
