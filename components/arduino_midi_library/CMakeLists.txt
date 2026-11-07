# cmake_minimum_required(VERSION 3.5.0)
# project(arduino_midi_library CXX)

# add_subdirectory(builder)

# setup_builder()

# add_subdirectory(external)
# add_subdirectory(src)
# add_subdirectory(test)

cmake_minimum_required(VERSION 3.5)

idf_component_register(
    SRCS 
        "src/MIDI.cpp"
    INCLUDE_DIRS 
        "src"
)

project(arduino_midi)

target_compile_definitions(${COMPONENT_LIB} PRIVATE 
    ESP_PLATFORM
)
