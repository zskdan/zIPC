#include "platform-linux-common.h"

zipc_status_t zipc_linux_common_ring_push(
    zipc_transport_spsc_ring_t *ring,
    const zipc_message_t *message)
{
    if (ring == NULL || message == NULL || ring->depth < 2U)
        return ZIPC_ERR_INVALID_ARGUMENT;

    const uint32_t producer = atomic_load_explicit(
        &ring->producer, memory_order_relaxed);
    const uint32_t consumer = atomic_load_explicit(
        &ring->consumer, memory_order_acquire);

    if ((uint32_t)(producer - consumer) >= ring->depth)
        return ZIPC_ERR_TRANSPORT;

    for (uint32_t position = consumer; position != producer; ++position) {
        const zipc_message_t *queued = &ring->entries[position % ring->depth];
        if (queued->handle == message->handle &&
            queued->pool_id == message->pool_id &&
            queued->source_component == message->source_component &&
            queued->destination_component == message->destination_component &&
            queued->transfer_sequence == message->transfer_sequence)
            return ZIPC_OK;
    }

    ring->entries[producer % ring->depth] = *message;
    atomic_store_explicit(&ring->producer, producer + 1U,
                          memory_order_release);
    return ZIPC_OK;
}

zipc_status_t zipc_linux_common_ring_peek(
    zipc_transport_spsc_ring_t *ring,
    zipc_message_t *message,
    uint32_t *position)
{
    if (ring == NULL || message == NULL || position == NULL || ring->depth < 2U)
        return ZIPC_ERR_INVALID_ARGUMENT;
    const uint32_t consumer = atomic_load_explicit(
        &ring->consumer, memory_order_relaxed);
    const uint32_t producer = atomic_load_explicit(
        &ring->producer, memory_order_acquire);
    if (consumer == producer)
        return ZIPC_ERR_NO_BUFFER;
    *message = ring->entries[consumer % ring->depth];
    *position = consumer;
    return ZIPC_OK;
}

zipc_status_t zipc_linux_common_ring_commit(
    zipc_transport_spsc_ring_t *ring,
    uint32_t position)
{
    if (ring == NULL || ring->depth < 2U)
        return ZIPC_ERR_INVALID_ARGUMENT;
    const uint32_t consumer = atomic_load_explicit(
        &ring->consumer, memory_order_relaxed);
    if (consumer != position)
        return ZIPC_ERR_RECOVERY_REQUIRED;
    atomic_store_explicit(&ring->consumer, consumer + 1U,
                          memory_order_release);
    return ZIPC_OK;
}

zipc_status_t zipc_linux_common_ring_pop(
    zipc_transport_spsc_ring_t *ring,
    zipc_message_t *message)
{
    if (ring == NULL || message == NULL || ring->depth < 2U)
        return ZIPC_ERR_INVALID_ARGUMENT;

    const uint32_t consumer = atomic_load_explicit(
        &ring->consumer, memory_order_relaxed);
    const uint32_t producer = atomic_load_explicit(
        &ring->producer, memory_order_acquire);

    if (consumer == producer)
        return ZIPC_ERR_NO_BUFFER;

    *message = ring->entries[consumer % ring->depth];
    atomic_store_explicit(&ring->consumer, consumer + 1U,
                          memory_order_release);
    return ZIPC_OK;
}
