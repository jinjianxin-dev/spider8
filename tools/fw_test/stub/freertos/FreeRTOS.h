/* PC 单测桩：FreeRTOS 的最小类型集，够 motion.c 编译即可。 */
#ifndef SPIDER8_STUB_FREERTOS_H
#define SPIDER8_STUB_FREERTOS_H

#include <stdint.h>
#include <stddef.h>

typedef uint32_t TickType_t;
typedef int32_t  BaseType_t;
typedef uint32_t UBaseType_t;
typedef uint32_t StackType_t;
typedef void    *TaskHandle_t;
typedef void    *QueueHandle_t;
typedef void   (*TaskFunction_t)(void *);

typedef struct { uint32_t dummy[8]; } StaticTask_t;
typedef struct { uint32_t dummy[8]; } StaticQueue_t;

#define pdTRUE              1
#define pdFALSE             0
#define pdPASS              1
#define pdMS_TO_TICKS(ms)   ((TickType_t)(ms))
#define portTICK_PERIOD_MS  1
#define portMAX_DELAY       ((TickType_t)0xffffffffu)

#endif
