#include <zipc/zipc.h>
#include "queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    return 1; } } while (0)

int main(void)
{
    enum { SLOT_COUNT = 8, SLOT_CAPACITY = 256 };
    const size_t control_size = zipc_pool_required_control_size(SLOT_COUNT);
    const size_t payload_offset = (control_size + 63U) & ~(size_t)63U;
    const size_t total_size = payload_offset +
        zipc_pool_required_payload_size(SLOT_COUNT, SLOT_CAPACITY, 0U, 64U);
    void *shared = aligned_alloc(64U, (total_size + 63U) & ~(size_t)63U);
    CHECK(shared != NULL);
    memset(shared, 0, total_size);

    zipc_platform_memory_t *memory = NULL;
    zipc_platform_memory_config_t memory_cfg = {
        .type = ZIPC_SHM_PREALLOCATED,
        .size = total_size,
        .backend.preallocated = {
            .address = shared,
            .physical_address = ZIPC_PHYS_ADDR_INVALID,
            .capabilities = ZIPC_MEM_CAP_CPU_READ |
                            ZIPC_MEM_CAP_CPU_WRITE |
                            ZIPC_MEM_CAP_ATOMIC32 |
                            ZIPC_MEM_CAP_ATOMIC64 |
                            ZIPC_MEM_CAP_CACHEABLE,
        },
    };
    CHECK(zipc_platform_memory_open(&memory, &memory_cfg) == ZIPC_OK);

    zipc_pool_config_t pool_cfg = {
        .control_memory = memory,
        .payload_memory = memory,
        .payload_offset = payload_offset,
        .slot_count = SLOT_COUNT,
        .slot_capacity = SLOT_CAPACITY,
        .payload_alignment = 64U,
    };
    CHECK(zipc_pool_format(&pool_cfg) == ZIPC_OK);
    zipc_pool_t pool;
    CHECK(zipc_pool_attach(&pool, &pool_cfg) == ZIPC_OK);

    uint32_t epoch1 = 0U, epoch2 = 0U;
    CHECK(zipc_component_register(&pool, 1U, &epoch1) == ZIPC_OK);
    CHECK(zipc_component_register(&pool, 2U, &epoch2) == ZIPC_OK);

    QueueHandle_t queue = xQueueCreate(4U, sizeof(zipc_message_t));
    CHECK(queue != NULL);
    zipc_link_t *tx = NULL, *rx = NULL;
    zipc_link_config_t tx_cfg = {
        .pool = &pool, .local_component = 1U, .remote_component = 2U,
        .local_epoch = epoch1, .hop_limit = 4U,
        .transport = {.type = ZIPC_TRANSPORT_MESSAGE_QUEUE,
                      .platform_handle = queue, .create_endpoint = false},
    };
    zipc_link_config_t rx_cfg = {
        .pool = &pool, .local_component = 2U, .remote_component = 1U,
        .local_epoch = epoch2,
        .transport = {.type = ZIPC_TRANSPORT_MESSAGE_QUEUE,
                      .platform_handle = queue, .create_endpoint = false},
    };
    CHECK(zipc_link_create(&tx, &tx_cfg) == ZIPC_OK);
    CHECK(zipc_link_create(&rx, &rx_cfg) == ZIPC_OK);

    zipc_buffer_t buffer;
    CHECK(zipc_buffer_get(tx, 16U, &buffer) == ZIPC_OK);
    CHECK(zipc_buffer_append(&buffer, "freertos", 9U) == ZIPC_OK);
    CHECK(zipc_send(tx, &buffer) == ZIPC_OK);
    CHECK(zipc_receive(rx, &buffer) == ZIPC_OK);
    CHECK(zipc_buffer_length(&buffer) == 9U);
    CHECK(memcmp(zipc_buffer_const_data(&buffer), "freertos", 9U) == 0);
    CHECK(buffer.control->hop_count == 2U);
    CHECK(zipc_buffer_put(rx, &buffer) == ZIPC_OK);

    zipc_link_destroy(rx);
    zipc_link_destroy(tx);
    vQueueDelete(queue);
    zipc_platform_memory_close(memory);
    free(shared);
    puts("PASS: FreeRTOS preallocated memory, queue transport, epochs and high-level API");
    return 0;
}
