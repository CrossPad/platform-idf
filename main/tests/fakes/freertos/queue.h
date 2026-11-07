// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "freertos/FreeRTOS.h"

typedef struct FakeQueue *QueueHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t itemSize);
void vQueueDelete(QueueHandle_t q);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
