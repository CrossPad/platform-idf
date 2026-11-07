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

include(CTest)
include(Catch)
catch_discover_tests(hil_text_tests)
catch_discover_tests(lv_async_safe_tests)
catch_discover_tests(cdc_line_tests)
