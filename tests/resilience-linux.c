#include <zipc/zipc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; } } while (0)

int main(void)
{
    const uint32_t slots = 4U, capacity = 256U;
    const size_t control_size = zipc_pool_required_control_size(slots);
    const size_t payload_offset = (control_size + 63U) & ~(size_t)63U;
    const size_t total = payload_offset + zipc_pool_required_payload_size(slots, capacity, 0U, 64U);
    zipc_platform_memory_t *memory = NULL;
    zipc_platform_memory_config_t memory_cfg = {
        .type = ZIPC_SHM_POSIX, .size = total,
        .backend.posix = {.name = "/zipc_v01_resilience", .create = true, .unlink_on_close = true}
    };
    CHECK(zipc_platform_memory_open(&memory, &memory_cfg) == ZIPC_OK);
    zipc_pool_config_t cfg = {
        .control_memory = memory, .payload_memory = memory,
        .payload_offset = payload_offset, .slot_count = slots,
        .slot_capacity = capacity, .payload_alignment = 64U
    };
    CHECK(zipc_pool_format(&cfg) == ZIPC_OK);
    zipc_pool_t pool;
    CHECK(zipc_pool_attach(&pool, &cfg) == ZIPC_OK);

    uint32_t epoch = 0U;
    CHECK(zipc_component_register(&pool, 1U, &epoch) == ZIPC_OK && epoch != 0U);
    CHECK(zipc_component_heartbeat(&pool, 1U, epoch) == ZIPC_OK);

    zipc_buffer_t buffer;
    CHECK(zipc_buffer_allocate(&pool, 1U, 0U, &buffer) == ZIPC_OK);
    CHECK(zipc_buffer_owner_epoch(&buffer) == epoch);
    CHECK(zipc_buffer_set_limits(&buffer, 1U, 0U) == ZIPC_OK);
    zipc_message_t message;
    CHECK(zipc_buffer_prepare_transfer(&pool, zipc_buffer_handle(&buffer), 1U, 2U, &message) == ZIPC_ERR_HOP_LIMIT);

    CHECK(zipc_buffer_set_limits(&buffer, 8U, 0U) == ZIPC_OK);
    CHECK(zipc_buffer_prepare_transfer(&pool, zipc_buffer_handle(&buffer), 1U, 2U, &message) == ZIPC_OK);
    zipc_trace_entry_t trace[ZIPC_TRACE_DEPTH];
    CHECK(zipc_buffer_trace_copy(&buffer, trace, ZIPC_TRACE_DEPTH) >= 2U);

    zipc_buffer_t wrong_owner;
    CHECK(zipc_buffer_allocate(&pool, 2U, 0U, &wrong_owner) == ZIPC_OK);
    const zipc_slot_id_t wrong_owner_slot =
        zipc_handle_slot_id(zipc_buffer_handle(&wrong_owner));
    zipc_message_t wrong_owner_message;
    CHECK(zipc_buffer_prepare_transfer(
              &pool, zipc_buffer_handle(&wrong_owner), 2U, 3U,
              &wrong_owner_message) == ZIPC_OK);
    zipc_buffer_t newer_epoch;
    CHECK(zipc_buffer_allocate(&pool, 1U, 0U, &newer_epoch) == ZIPC_OK);
    const zipc_slot_id_t newer_epoch_slot =
        zipc_handle_slot_id(zipc_buffer_handle(&newer_epoch));
    pool.controls[newer_epoch_slot].owner_epoch = epoch + 1U;

    CHECK(zipc_component_unregister(&pool, 1U, epoch) == ZIPC_OK);
    zipc_recovery_result_t recovered;
    CHECK(zipc_pool_recover_owner(&pool, 1U, epoch, 0U, &recovered) == ZIPC_OK);
    CHECK(recovered.recovered_transfer == 1U);
    CHECK(recovered.recovered_owned == 0U);
    CHECK(recovered.skipped_newer_epoch == 1U);
    CHECK(zipc_buffer_slot_state(&buffer) == ZIPC_SLOT_FREE);
    CHECK(atomic_load_explicit(&pool.controls[wrong_owner_slot].state,
                               memory_order_acquire) == ZIPC_SLOT_TRANSFER);
    CHECK(atomic_load_explicit(&pool.controls[newer_epoch_slot].state,
                               memory_order_acquire) == ZIPC_SLOT_OWNED);
    CHECK(zipc_buffer_claim(&pool, &wrong_owner_message, 3U, &wrong_owner) ==
          ZIPC_OK);
    CHECK(zipc_pool_buffer_release(&pool, zipc_buffer_handle(&wrong_owner), 3U) ==
          ZIPC_OK);
    uint32_t newer_registered_epoch = 0U;
    CHECK(zipc_component_register(&pool, 1U, &newer_registered_epoch) == ZIPC_OK);
    CHECK(newer_registered_epoch == epoch + 1U);
    CHECK(zipc_pool_buffer_release(&pool, zipc_buffer_handle(&newer_epoch), 1U) ==
          ZIPC_OK);

    uint32_t restart_epoch = 0U;
    CHECK(zipc_component_register(&pool, 4U, &restart_epoch) == ZIPC_OK);
    zipc_buffer_t interrupted;
    CHECK(zipc_buffer_allocate(&pool, 4U, 0U, &interrupted) == ZIPC_OK);
    const zipc_buffer_id_t interrupted_id = zipc_buffer_id(&interrupted);
    const zipc_handle_t interrupted_handle = zipc_buffer_handle(&interrupted);
    zipc_restart_t restart;
    CHECK(zipc_component_restart_begin(&pool, 4U, restart_epoch, &restart) ==
          ZIPC_OK);
    CHECK(!zipc_buffer_is_valid(&interrupted));
    CHECK(zipc_buffer_set_region(&interrupted, 0U, 1U) ==
          ZIPC_ERR_INVALID_BUFFER);
    zipc_buffer_t adopted;
    CHECK(zipc_component_restart_next(&restart, &adopted) == ZIPC_OK);
    CHECK(zipc_buffer_id(&adopted) == interrupted_id);
    CHECK(zipc_buffer_handle(&adopted) != interrupted_handle);
    CHECK(zipc_component_restart_next(&restart, &interrupted) ==
          ZIPC_ERR_NO_BUFFER);
    CHECK(zipc_component_restart_finish(&restart) == ZIPC_OK);
    CHECK(zipc_component_unregister(&pool, 4U, restart_epoch) ==
          ZIPC_ERR_COMPONENT_STALE);
    CHECK(zipc_buffer_release(&adopted) == ZIPC_OK);

    zipc_descriptor_backend_type_t descriptor;
    zipc_event_backend_type_t event;
    CHECK(zipc_transport_backend_roles(ZIPC_TRANSPORT_SHM_RING_EVENTFD,
                                       &descriptor, &event) == ZIPC_OK);
    CHECK(descriptor == ZIPC_DESCRIPTOR_BACKEND_SHM_RING);
    CHECK(event == ZIPC_EVENT_BACKEND_EVENTFD);
    CHECK(zipc_transport_backend_roles(ZIPC_TRANSPORT_SHM_RING_POLLING,
                                       &descriptor, &event) == ZIPC_OK);
    CHECK(descriptor == ZIPC_DESCRIPTOR_BACKEND_SHM_RING);
    CHECK(event == ZIPC_EVENT_BACKEND_SHM_POLLING);
    CHECK(strcmp(zipc_event_backend_name(event), "shm-polling") == 0);
    CHECK(strcmp(zipc_payload_backend_name(ZIPC_SHM_POSIX), "posix-shm") == 0);

    const uint32_t ring_depth = 8U;
    const size_t ring_size = zipc_transport_spsc_ring_size(ring_depth);
    zipc_transport_spsc_ring_t *ring = calloc(1U, ring_size);
    CHECK(ring != NULL);
    CHECK(zipc_transport_spsc_ring_initialize(ring, ring_depth) == ZIPC_OK);

    zipc_platform_transport_config_t poll_cfg = {
        .type = ZIPC_TRANSPORT_SHM_RING_POLLING,
        .platform_handle = ring,
        .ring_depth = ring_depth,
        .poll_timeout_ns = UINT64_C(1000000)
    };
    zipc_platform_transport_t *poll_tx = NULL;
    zipc_platform_transport_t *poll_rx = NULL;
    CHECK(zipc_platform_transport_open(&poll_tx, &poll_cfg) == ZIPC_OK);
    CHECK(zipc_platform_transport_open(&poll_rx, &poll_cfg) == ZIPC_OK);

    zipc_message_t sent = {.handle = UINT64_C(0x1122334455667788)};
    zipc_message_t received = {0};
    CHECK(zipc_platform_transport_receive(poll_rx, &received) ==
          ZIPC_ERR_TIMEOUT);
    CHECK(zipc_platform_transport_send(poll_tx, &sent) == ZIPC_OK);
    CHECK(zipc_platform_transport_receive(poll_rx, &received) == ZIPC_OK);
    CHECK(received.handle == sent.handle);
    zipc_platform_transport_close(poll_tx);
    zipc_platform_transport_close(poll_rx);
    free(ring);

    puts("PASS: epochs, heartbeat, hop limit, trace, recovery, backend roles and timed SHM polling");
    zipc_platform_memory_close(memory);
    return 0;
}
