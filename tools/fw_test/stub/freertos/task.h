/* PC 单测桩：任务 API 声明（实现在 test_settle.c）。 */
#ifndef SPIDER8_STUB_TASK_H
#define SPIDER8_STUB_TASK_H

#include "freertos/FreeRTOS.h"

TaskHandle_t xTaskCreateStatic(TaskFunction_t fn, const char *name,
                               uint32_t stack, void *param,
                               UBaseType_t prio, StackType_t *stack_buf,
                               StaticTask_t *tcb);
void       vTaskDelayUntil(TickType_t *last, TickType_t inc);
TickType_t xTaskGetTickCount(void);
uint32_t   uxTaskGetStackHighWaterMark(TaskHandle_t h);

#endif
