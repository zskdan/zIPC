#include <zipc/zipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    return 1; } } while (0)

typedef struct { unsigned pending; } test_ipi_t;
static zipc_status_t signal_send(void *context) {
    ((test_ipi_t *)context)->pending++;
    return ZIPC_OK;
}
static zipc_status_t signal_wait(void *context, uint32_t timeout) {
    (void)timeout;
    test_ipi_t *ipi = context;
    if (ipi->pending == 0U) return ZIPC_ERR_TIMEOUT;
    ipi->pending--;
    return ZIPC_OK;
}

int main(void)
{
    enum { SLOT_COUNT = 4, SLOT_CAPACITY = 256 };
    const size_t control_size = zipc_pool_required_control_size(SLOT_COUNT);
    const size_t payload_offset = (control_size + 63U) & ~(size_t)63U;
    const size_t total_size = payload_offset +
        zipc_pool_required_payload_size(SLOT_COUNT, SLOT_CAPACITY, 0U, 64U);
    void *shared = aligned_alloc(64U, (total_size + 63U) & ~(size_t)63U);
    CHECK(shared != NULL);
    memset(shared, 0, total_size);

    zipc_platform_memory_t *memory = NULL;
    zipc_platform_memory_config_t memory_cfg = {
        .type = ZIPC_SHM_DTREVMEM_CACHED,
        .size = total_size,
        .backend.dtrevmem = {
            .physical_address = (uint64_t)(uintptr_t)shared,
            .supports_cpu_atomics = true,
            .remote_accessible = true,
        },
    };
    CHECK(zipc_platform_memory_open(&memory, &memory_cfg) == ZIPC_OK);
    zipc_pool_config_t pool_cfg = {
        .control_memory = memory, .payload_memory = memory,
        .payload_offset = payload_offset, .slot_count = SLOT_COUNT,
        .slot_capacity = SLOT_CAPACITY, .payload_alignment = 64U,
    };
    CHECK(zipc_pool_format(&pool_cfg) == ZIPC_OK);
    zipc_pool_t pool;
    CHECK(zipc_pool_attach(&pool, &pool_cfg) == ZIPC_OK);

    uint32_t epoch1 = 0U, epoch2 = 0U;
    CHECK(zipc_component_register(&pool, 10U, &epoch1) == ZIPC_OK);
    CHECK(zipc_component_register(&pool, 11U, &epoch2) == ZIPC_OK);

    zipc_message_t mailbox;
    test_ipi_t ipi = {0};
    zipc_platform_transport_config_t transport_cfg = {
        .type = ZIPC_TRANSPORT_IPI,
        .shared_mailbox = &mailbox,
        .signal_send = signal_send,
        .signal_wait = signal_wait,
        .signal_context = &ipi,
    };
    zipc_link_t *tx = NULL, *rx = NULL;
    zipc_link_config_t tx_cfg = {
        .pool = &pool, .local_component = 10U, .remote_component = 11U,
        .local_epoch = epoch1, .hop_limit = 3U, .transport = transport_cfg,
    };
    zipc_link_config_t rx_cfg = {
        .pool = &pool, .local_component = 11U, .remote_component = 10U,
        .local_epoch = epoch2, .transport = transport_cfg,
    };
    CHECK(zipc_link_create(&tx, &tx_cfg) == ZIPC_OK);
    CHECK(zipc_link_create(&rx, &rx_cfg) == ZIPC_OK);

    zipc_buffer_t buffer;
    CHECK(zipc_buffer_alloc_ex(tx, 0U, 8U, 0U, &buffer, NULL) == ZIPC_OK);
    CHECK(zipc_buffer_append(&buffer, "baremetal", 10U) == ZIPC_OK);
    CHECK(zipc_send(tx, &buffer) == ZIPC_OK);
    CHECK(zipc_recv(rx, &buffer) == ZIPC_OK);
    CHECK(memcmp(zipc_buffer_data(&buffer), "baremetal", 10U) == 0);
    CHECK(zipc_buffer_hop_count(&buffer) == 2U);
    CHECK(zipc_buffer_release(&buffer) == ZIPC_OK);

    zipc_link_destroy(rx);
    zipc_link_destroy(tx);
    zipc_platform_memory_close(memory);
    free(shared);
    puts("PASS: bare-metal reserved memory, IPI transport, epochs and high-level API");
    return 0;
}
