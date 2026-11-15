/* PC 单测桩：队列 API 声明（实现在 test_settle.c）。 */
#ifndef SPIDER8_STUB_QUEUE_H
#define SPIDER8_STUB_QUEUE_H

#include "freertos/FreeRTOS.h"

QueueHandle_t xQueueCreateStatic(UBaseType_t len, UBaseType_t item,
                                 uint8_t *storage, StaticQueue_t *buf);
BaseType_t    xQueueOverwrite(QueueHandle_t q, const void *item);
BaseType_t    xQueuePeek(QueueHandle_t q, void *buf, TickType_t ticks);

#endif
