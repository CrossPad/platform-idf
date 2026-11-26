# ── Host tests for the board's CDC text (main/hil_text.cpp) ──
# The exact lines crosspad-hil parses, pinned without a board:
#   cmake -S main/tests -B main/tests/build && cmake --build main/tests/build
#   ctest --test-dir main/tests/build --output-on-failure

cmake_minimum_required(VERSION 3.14)
project(crosspad-hil-text-tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.0
)
FetchContent_MakeAvailable(Catch2)

set(MAIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(CORE_DIR ${MAIN_DIR}/../components/crosspad-core)
set(PLATFORM_DIR ${MAIN_DIR}/../components/crosspad-platform-idf)

add_executable(hil_text_tests
    test_hil_text.cpp
    ${MAIN_DIR}/hil_text.cpp
)
target_include_directories(hil_text_tests PRIVATE
    ${MAIN_DIR}/include
    ${CORE_DIR}/include
)
target_link_libraries(hil_text_tests PRIVATE Catch2::Catch2WithMain)

# The formatters write into fixed buffers on the board: every test runs
# under AddressSanitizer, so an append past a buffer fails here, not there.
if(NOT MSVC)
    target_compile_options(hil_text_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(hil_text_tests PRIVATE -fsanitize=address,undefined)
endif()

# The display-lock forwarder (main/lv_async_safe.cpp) over host stand-ins
# for FreeRTOS, esp_lvgl_port and LVGL (fakes/).
add_executable(lv_async_safe_tests
    test_lv_async_safe.cpp
    ${MAIN_DIR}/lv_async_safe.cpp
    fakes/fake_idf.cpp
)
target_include_directories(lv_async_safe_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${MAIN_DIR}/include
)
target_link_libraries(lv_async_safe_tests PRIVATE Catch2::Catch2WithMain)

# The CDC line assembler (main/include/cdc_line.h), header-only.
add_executable(cdc_line_tests test_cdc_line.cpp)
target_include_directories(cdc_line_tests PRIVATE ${MAIN_DIR}/include)
target_link_libraries(cdc_line_tests PRIVATE Catch2::Catch2WithMain)
if(NOT MSVC)
    target_compile_options(cdc_line_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(cdc_line_tests PRIVATE -fsanitize=address,undefined)
endif()

# What a host's Program Change + Bank Select names (main/include/kit_pc.h),
# header-only and free of ESP and LVGL includes for exactly this test.
add_executable(kit_pc_tests test_kit_pc.cpp)
target_include_directories(kit_pc_tests PRIVATE ${MAIN_DIR}/include)
target_link_libraries(kit_pc_tests PRIVATE Catch2::Catch2WithMain)
if(NOT MSVC)
    target_compile_options(kit_pc_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(kit_pc_tests PRIVATE -fsanitize=address,undefined)
endif()

# What a host's Control Change is worth and what one frame of pad colours is
# (main/include/host_midi_map.h), header-only for exactly this test.
add_executable(host_midi_tests test_host_midi.cpp)
target_include_directories(host_midi_tests PRIVATE ${MAIN_DIR}/include)
target_link_libraries(host_midi_tests PRIVATE Catch2::Catch2WithMain)
if(NOT MSVC)
    target_compile_options(host_midi_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(host_midi_tests PRIVATE -fsanitize=address,undefined)
endif()

# The SysEx control surface's address space, frame grammar and give-back rule
# (main/include/surface_map.h), header-only for exactly this test.
add_executable(surface_map_tests test_surface_map.cpp)
target_include_directories(surface_map_tests PRIVATE ${MAIN_DIR}/include)
target_link_libraries(surface_map_tests PRIVATE Catch2::Catch2WithMain)
if(NOT MSVC)
    target_compile_options(surface_map_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(surface_map_tests PRIVATE -fsanitize=address,undefined)
endif()

# Keeping a bench session awake and waking a dimmed board
# (main/include/host_keepawake.hpp), header-only for exactly this test: the
# nudge period, the quiet window a host falls back into and the lease that
# covers a silent build are all pinned here, because the alternative is a
# scenario that loses the board to a 1800 s soft-off halfway through.
add_executable(host_keepawake_tests test_host_keepawake.cpp)
target_include_directories(host_keepawake_tests PRIVATE ${MAIN_DIR}/include)
target_link_libraries(host_keepawake_tests PRIVATE Catch2::Catch2WithMain)
if(NOT MSVC)
    target_compile_options(host_keepawake_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(host_keepawake_tests PRIVATE -fsanitize=address,undefined)
endif()

# The settings write queue (crosspad-platform-idf/NvsWriteQueue.cpp): the part
# of the store's deferral that is pure policy, so the cooldown, the read-back,
# the erase and the retry rules are pinned without a board and without NVS.
add_executable(nvs_write_queue_tests
    test_nvs_write_queue.cpp
    ${PLATFORM_DIR}/NvsWriteQueue.cpp
)
target_include_directories(nvs_write_queue_tests PRIVATE ${PLATFORM_DIR}/include)
target_link_libraries(nvs_write_queue_tests PRIVATE Catch2::Catch2WithMain)
if(NOT MSVC)
    target_compile_options(nvs_write_queue_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(nvs_write_queue_tests PRIVATE -fsanitize=address,undefined)
endif()

include(CTest)
include(Catch)
catch_discover_tests(hil_text_tests)
catch_discover_tests(lv_async_safe_tests)
catch_discover_tests(cdc_line_tests)
catch_discover_tests(kit_pc_tests)
catch_discover_tests(host_midi_tests)
catch_discover_tests(surface_map_tests)
catch_discover_tests(host_keepawake_tests)
catch_discover_tests(nvs_write_queue_tests)
