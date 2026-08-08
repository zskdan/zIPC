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

    ring->entries[producer % ring->depth] = *message;
    atomic_store_explicit(&ring->producer, producer + 1U,
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
