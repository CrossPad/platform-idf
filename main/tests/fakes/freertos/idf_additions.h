// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "freertos/task.h"

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *handle, BaseType_t core,
                                           uint32_t caps);
