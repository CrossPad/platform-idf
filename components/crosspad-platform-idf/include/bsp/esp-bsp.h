/**
 * @file esp-bsp.h
 * @brief BSP routing header — selects the correct BSP based on build target.
 *
 * Include this file instead of platform-specific BSP headers.
 */

#pragma once

#ifdef CONFIG_IDF_TARGET_ESP32P4
#include "bsp/esp32_p4_function_ev_board.h"
#define BSP_BOARD_P4_DEVKIT
#else
// CrossPad S3 BSP (components/bsp/crosspad)
#include "crosspad_s3.h"
#include "imu.h"
// BSP_BOARD_CROSSPAD_S3 comes from the BSP component's CMakeLists.txt
#endif
