// SPDX-License-Identifier: GPL-3.0-or-later
// Host stand-in for the FreeRTOS subset main/lv_async_safe.cpp uses.
#pragma once
#include <cstdint>

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
void fake_critical_enter(portMUX_TYPE *mux);
void fake_critical_exit(portMUX_TYPE *mux);
#define taskENTER_CRITICAL(mux) fake_critical_enter(mux)
#define taskEXIT_CRITICAL(mux)  fake_critical_exit(mux)
