#include <zipc/zipc.h>

#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed: %s:%d: %s\n", \
                __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

#define CONTROL_CAPS (ZIPC_MEM_CAP_CPU_READ | ZIPC_MEM_CAP_CPU_WRITE | \
                      ZIPC_MEM_CAP_ATOMIC32 | ZIPC_MEM_CAP_ATOMIC64)

alignas(64) static uint8_t shared_memory[64U * 1024U];
alignas(64) static uint8_t atomic32_memory[64U * 1024U];

static size_t align_up_test(size_t value, size_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static bool trace_has_error(const zipc_buffer_t *buffer)
{
    zipc_trace_entry_t entries[ZIPC_TRACE_DEPTH];
    const uint32_t count = zipc_buffer_trace_copy(buffer, entries,
                                                  ZIPC_TRACE_DEPTH);
    for (uint32_t i = 0U; i < count; ++i)
        if (entries[i].event == ZIPC_TRACE_ERROR)
            return true;
    return false;
}

int main(void)
{
    const uint32_t slot_count = 4U;
    const uint32_t slot_capacity = 256U;
    const size_t control_size = zipc_pool_required_control_size(slot_count);
    const size_t payload_offset = align_up_test(control_size, 64U);
    const size_t payload_size = zipc_pool_required_payload_size(
        slot_count, slot_capacity, 0U, 64U);
    CHECK(control_size != 0U && payload_size != 0U);
    CHECK(payload_offset + payload_size <= sizeof(shared_memory));
    CHECK(zipc_pool_required_payload_size(1U, UINT32_MAX, 0U, 2U) == 0U);

    zipc_platform_memory_t *memory = NULL;
    zipc_platform_memory_config_t memory_config = {
        .type = ZIPC_SHM_PREALLOCATED,
        .size = sizeof(shared_memory),
        .backend.preallocated = {
            .address = shared_memory,
            .physical_address = ZIPC_PHYS_ADDR_INVALID,
            .capabilities = CONTROL_CAPS,
        },
    };
    CHECK(zipc_platform_memory_open(&memory, &memory_config) == ZIPC_OK);

    zipc_pool_config_t config = {
        .control_memory = memory,
        .payload_memory = memory,
        .payload_offset = payload_offset,
        .slot_count = slot_count,
        .slot_capacity = slot_capacity,
        .payload_alignment = 64U,
        .pool_id = 17U,
    };

    zipc_pool_config_t bad = config;
    bad.control_offset = 1U;
    CHECK(zipc_pool_format(&bad) == ZIPC_ERR_INVALID_ARGUMENT);
    bad = config;
    bad.payload_offset++;
    CHECK(zipc_pool_format(&bad) == ZIPC_ERR_INVALID_ARGUMENT);
    bad = config;
    bad.payload_offset = 0U;
    CHECK(zipc_pool_format(&bad) == ZIPC_ERR_INVALID_ARGUMENT);

    CHECK(zipc_pool_format(&config) == ZIPC_OK);
    zipc_pool_t pool;
    CHECK(zipc_pool_attach(&pool, &config) == ZIPC_OK);

    zipc_pool_header_t *header =
        (zipc_pool_header_t *)(void *)shared_memory;
    const uint16_t header_size = header->header_size;
    const uint32_t controls_offset = header->controls_offset;
    header->header_size--;
    CHECK(zipc_pool_attach(&pool, &config) == ZIPC_ERR_INVALID_POOL);
    header->header_size = header_size;
    header->controls_offset += (uint32_t)_Alignof(zipc_slot_control_t);
    CHECK(zipc_pool_attach(&pool, &config) == ZIPC_ERR_INVALID_POOL);
    header->controls_offset = controls_offset;
    CHECK(zipc_pool_attach(&pool, &config) == ZIPC_OK);

    zipc_platform_memory_t *atomic32_only = NULL;
    zipc_platform_memory_config_t atomic32_config = memory_config;
    atomic32_config.backend.preallocated.address = atomic32_memory;
    atomic32_config.backend.preallocated.capabilities =
        ZIPC_MEM_CAP_CPU_READ | ZIPC_MEM_CAP_CPU_WRITE | ZIPC_MEM_CAP_ATOMIC32;
    CHECK(zipc_platform_memory_open(&atomic32_only, &atomic32_config) == ZIPC_OK);
    bad = config;
    bad.control_memory = atomic32_only;
    bad.payload_memory = atomic32_only;
    CHECK(zipc_pool_format(&bad) == ZIPC_ERR_UNSUPPORTED_MEMORY);
    zipc_platform_memory_close(atomic32_only);

    atomic_store_explicit(&pool.header->components[3U].epoch, UINT32_MAX,
                          memory_order_release);
    CHECK(zipc_component_register(&pool, 3U, NULL) == ZIPC_ERR_COMPONENT_STALE);
    CHECK(atomic_load_explicit(&pool.header->components[3U].epoch,
                               memory_order_acquire) == UINT32_MAX);

    uint32_t epoch = 0U;
    CHECK(zipc_component_register(&pool, 1U, &epoch) == ZIPC_OK);
    zipc_buffer_t buffer;
    CHECK(zipc_buffer_allocate(&pool, 1U, &buffer) == ZIPC_OK);
    zipc_recovery_result_t recovered;
    CHECK(zipc_pool_recover_owner(&pool, 1U, epoch, 0U, &recovered) ==
          ZIPC_ERR_COMPONENT_STALE);
    CHECK(zipc_buffer_slot_state(&buffer) == ZIPC_SLOT_OWNED);

    CHECK(zipc_buffer_set_limits(&buffer, 1U, 0U) == ZIPC_OK);
    const uint64_t errors_before = atomic_load_explicit(
        &pool.header->protocol_error_count, memory_order_relaxed);
    zipc_message_t message;
    CHECK(zipc_buffer_prepare_transfer(&pool, zipc_buffer_handle(&buffer),
                                       1U, 2U, &message) ==
          ZIPC_ERR_HOP_LIMIT);
    CHECK(atomic_load_explicit(&pool.header->protocol_error_count,
                               memory_order_relaxed) == errors_before + 1U);
    CHECK(trace_has_error(&buffer));

    CHECK(zipc_buffer_set_limits(&buffer, 8U, 0U) == ZIPC_OK);
    CHECK(zipc_buffer_prepare_transfer(&pool, zipc_buffer_handle(&buffer),
                                       1U, 2U, &message) == ZIPC_OK);
    const zipc_slot_id_t transfer_slot = zipc_handle_slot_id(message.handle);
    const uint32_t trace_count = pool.controls[transfer_slot].trace_count;
    zipc_message_t bad_message = message;
    bad_message.transfer_sequence++;
    CHECK(zipc_buffer_claim(&pool, &bad_message, 2U, &buffer) ==
          ZIPC_ERR_SEQUENCE_MISMATCH);
    CHECK(atomic_load_explicit(&pool.header->protocol_error_count,
                               memory_order_relaxed) == errors_before + 2U);
    CHECK(pool.controls[transfer_slot].trace_count == trace_count);
    zipc_buffer_t claimed;
    CHECK(zipc_buffer_claim(&pool, &message, 2U, &claimed) == ZIPC_OK);
    CHECK(zipc_buffer_release(&claimed) == ZIPC_OK);

    CHECK(zipc_buffer_allocate(&pool, 1U, &buffer) == ZIPC_OK);
    CHECK(zipc_component_unregister(&pool, 1U, epoch) == ZIPC_OK);
    CHECK(zipc_pool_recover_owner(&pool, 1U, epoch, 0U, &recovered) == ZIPC_OK);
    CHECK(recovered.recovered_owned == 1U);

    const uint32_t ring_depth = 4U;
    zipc_transport_spsc_ring_t *ring = calloc(
        1U, zipc_transport_spsc_ring_size(ring_depth));
    CHECK(ring != NULL);
    CHECK(zipc_transport_spsc_ring_initialize(ring, ring_depth) == ZIPC_OK);
    zipc_platform_transport_config_t transport = {
        .type = ZIPC_TRANSPORT_SHM_RING_POLLING,
        .platform_handle = ring,
        .ring_depth = ring_depth,
        .poll_timeout_ns = UINT64_C(1000000),
    };
    zipc_link_config_t tx_config = {
        .pool = &pool,
        .local_component = 1U,
        .remote_component = 2U,
        .default_deadline_ns = UINT64_MAX,
        .transport = transport,
    };
    zipc_link_config_t rx_config = tx_config;
    rx_config.local_component = 2U;
    rx_config.remote_component = 1U;
    rx_config.default_deadline_ns = 0U;
    zipc_link_t *tx = NULL;
    zipc_link_t *rx = NULL;
    CHECK(zipc_link_create(&tx, &tx_config) == ZIPC_OK);
    CHECK(zipc_link_create(&rx, &rx_config) == ZIPC_OK);

    CHECK(zipc_buffer_alloc(tx, 8U, &buffer) == ZIPC_OK);
    const zipc_slot_id_t slot_id = zipc_handle_slot_id(zipc_buffer_handle(&buffer));
    CHECK(pool.controls[slot_id].deadline_ns == ZIPC_DEADLINE_NONE);
    const zipc_handle_t sent_handle = zipc_buffer_handle(&buffer);
    CHECK(zipc_send(tx, &buffer) == ZIPC_OK);

    pool.flags = ZIPC_POOL_F_STRICT_OWNERSHIP;
    CHECK(zipc_recv(rx, &buffer) == ZIPC_ERR_UNSUPPORTED_MEMORY);
    CHECK(atomic_load_explicit(
              &pool.controls[zipc_handle_slot_id(sent_handle)].state,
              memory_order_acquire) == ZIPC_SLOT_FREE);
    pool.flags = ZIPC_POOL_F_NONE;

    zipc_link_destroy(rx);
    zipc_link_destroy(tx);
    free(ring);
    zipc_platform_memory_close(memory);
    puts("PASS: ABI-1 geometry, capabilities, epochs, recovery and errors");
    return 0;
}
