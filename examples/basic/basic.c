#include <zipc/zipc.h>

#include <stdio.h>

#define CHECK(x) do { zipc_status_t s_ = (x); if (s_ != ZIPC_OK) return 1; } while (0)

enum { PRODUCER = 1, CONSUMER = 2 };

int main(void)
{
    zipc_platform_memory_t *memory = NULL;
    zipc_link_t *producer = NULL;
    zipc_link_t *consumer = NULL;
    zipc_pool_t pool;
    zipc_buffer_t buffer;

    const size_t control_size = zipc_pool_required_control_size(4U);
    const size_t payload_offset = (control_size + 63U) & ~(size_t)63U;
    const zipc_platform_memory_config_t memory_cfg = {
        .type = ZIPC_SHM_POSIX,
        .size = payload_offset + 4U * 1024U,
        .backend.posix = {
            .name = "/zipc_v0_basic",
            .create = true,
            .unlink_on_close = true,
        },
    };
    CHECK(zipc_platform_memory_open(&memory, &memory_cfg));

    const zipc_pool_config_t pool_cfg = {
        .control_memory = memory,
        .payload_memory = NULL,
        .slot_count = 4U,
        .slot_capacity = 1024U,
        .payload_alignment = 64U,
        .payload_offset = payload_offset,
    };
    CHECK(zipc_pool_format(&pool_cfg));
    CHECK(zipc_pool_attach(&pool, &pool_cfg));

    const zipc_link_config_t producer_cfg = {
        .pool = &pool,
        .local_component = PRODUCER,
        .remote_component = CONSUMER,
        .transport = {
            .type = ZIPC_TRANSPORT_MESSAGE_QUEUE,
            .endpoint_name = "/zipc_v0_basic_link",
            .create_endpoint = true,
            .queue_length = 8U,
        },
    };
    const zipc_link_config_t consumer_cfg = {
        .pool = &pool,
        .local_component = CONSUMER,
        .remote_component = PRODUCER,
        .transport = {
            .type = ZIPC_TRANSPORT_MESSAGE_QUEUE,
            .endpoint_name = "/zipc_v0_basic_link",
            .create_endpoint = false,
        },
    };

    CHECK(zipc_link_create(&producer, &producer_cfg));
    CHECK(zipc_link_create(&consumer, &consumer_cfg));

    CHECK(zipc_buffer_alloc_ex(producer, 0U, 16U, 0U, &buffer, NULL));
    CHECK(zipc_buffer_append(&buffer, "hello zIPC", sizeof("hello zIPC")));
    CHECK(zipc_send(producer, &buffer));
    printf("producer sent: %s\n", "hello zIPC");

    CHECK(zipc_recv(consumer, &buffer));
    printf("consumer received: %s\n", (char *)zipc_buffer_data(&buffer));
    CHECK(zipc_buffer_release(&buffer));

    zipc_link_destroy(consumer);
    zipc_link_destroy(producer);
    zipc_platform_memory_close(memory);
    return 0;
}
