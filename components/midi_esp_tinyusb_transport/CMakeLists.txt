idf_component_register(
    SRCS "midi_esp_tinyusb_transport.cpp" "midi_esp_tinyusb_task.cpp"
    INCLUDE_DIRS "include"
    REQUIRES arduino_midi_library freertos esp_timer espressif__tinyusb
)

# # Ensure tinyusb include directory is added
# idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
# target_include_directories(${tusb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")

# Add espressif__esp_tinyusb include path for tusb_config.h
idf_component_get_property(esp_tinyusb_dir espressif__esp_tinyusb COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${esp_tinyusb_dir}/include")