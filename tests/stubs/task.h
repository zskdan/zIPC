#ifndef TEST_TASK_H
#define TEST_TASK_H
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
int xTaskNotifyGiveIndexed(TaskHandle_t task, unsigned index);
uint32_t ulTaskNotifyTakeIndexed(unsigned index, int clear, TickType_t timeout);
#endif
