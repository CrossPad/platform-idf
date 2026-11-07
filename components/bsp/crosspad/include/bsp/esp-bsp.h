/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef CONFIG_IDF_TARGET_ESP32P4
#include "bsp/esp32_p4_function_ev_board.h"
#else
#include "../crosspad_s3.h"
#include "../imu.h"
#endif
