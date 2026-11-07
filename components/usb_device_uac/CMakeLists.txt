set(priv_requires esp_timer)

if(${IDF_VERSION_MAJOR} LESS 6)
    list(APPEND priv_requires usb)
endif()

# Determine whether esp_tinyusb is fetched from component registry or from local path
# We need esp_tinyusb (not raw tinyusb) because it provides tusb_config.h
idf_build_get_property(build_components BUILD_COMPONENTS)
if(esp_tinyusb IN_LIST build_components)
    set(esp_tinyusb_name esp_tinyusb)
else()
    set(esp_tinyusb_name espressif__esp_tinyusb)
endif()

# UAC2 audio class requires CFG_TUD_AUDIO which is only enabled on S3
if(IDF_TARGET STREQUAL "esp32s3")
    set(UAC_SRCS usb_device_uac.c)
else()
    set(UAC_SRCS "usb_device_uac.c" "")
endif()

idf_component_register(SRCS ${UAC_SRCS}
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES ${priv_requires} ${esp_tinyusb_name})
