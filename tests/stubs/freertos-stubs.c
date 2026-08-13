#include <zipc/zipc.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include <openamp/open_amp.h>
#include <stdlib.h>
#include <string.h>

zipc_status_t zipc_test_random(void *buffer, size_t length)
{
    static uint32_t value = UINT32_C(0x13579bdf);
    uint8_t *bytes = buffer;
    for (size_t i = 0U; i < length; ++i) {
        value = value * UINT32_C(1664525) + UINT32_C(1013904223);
        bytes[i] = (uint8_t)(value >> 24);
    }
    return ZIPC_OK;
}

typedef struct {
    unsigned length, item_size, head, tail, count;
    unsigned char *storage;
} test_queue_t;

void *pvPortMalloc(size_t size) { return calloc(1U, size); }
void vPortFree(void *pointer) { free(pointer); }
QueueHandle_t xQueueCreate(unsigned length, unsigned item_size) {
    test_queue_t *q = calloc(1U, sizeof(*q));
    if (!q) return NULL;
    q->storage = calloc(length, item_size);
    if (!q->storage) { free(q); return NULL; }
    q->length = length; q->item_size = item_size; return q;
}
int xQueueSend(QueueHandle_t handle, const void *item, TickType_t timeout) {
    (void)timeout; test_queue_t *q = handle;
    if (!q || q->count == q->length) return 0;
    memcpy(q->storage + q->tail * q->item_size, item, q->item_size);
    q->tail = (q->tail + 1U) % q->length; q->count++; return pdPASS;
}
int xQueueReceive(QueueHandle_t handle, void *item, TickType_t timeout) {
    (void)timeout; test_queue_t *q = handle;
    if (!q || q->count == 0U) return 0;
    memcpy(item, q->storage + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1U) % q->length; q->count--; return pdPASS;
}
void vQueueDelete(QueueHandle_t handle) {
    test_queue_t *q = handle; if (q) { free(q->storage); free(q); }
}
static uint32_t notifications[8];
int xTaskNotifyGiveIndexed(TaskHandle_t task, unsigned index) {
    (void)task; if (index >= 8U) return 0; notifications[index]++; return pdPASS;
}
uint32_t ulTaskNotifyTakeIndexed(unsigned index, int clear, TickType_t timeout) {
    (void)timeout; if (index >= 8U || notifications[index] == 0U) return 0U;
    uint32_t value = notifications[index];
    if (clear) notifications[index] = 0U; else notifications[index]--;
    return value;
}
int rpmsg_send(struct rpmsg_endpoint *endpoint, const void *data, size_t length) {
    (void)endpoint; (void)data; return (int)length;
}
TickType_t xTaskGetTickCount(void) { static TickType_t tick; return ++tick; }
