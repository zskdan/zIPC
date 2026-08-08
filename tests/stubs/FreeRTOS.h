#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <stddef.h>
#include <stdint.h>
#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
typedef uint32_t TickType_t;
void *pvPortMalloc(size_t size);
void vPortFree(void *pointer);
#endif
typedef unsigned UBaseType_t;
#define configTICK_RATE_HZ 1000U
TickType_t xTaskGetTickCount(void);
