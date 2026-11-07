// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "freertos/FreeRTOS.h"

typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
TickType_t xTaskGetTickCount(void);
