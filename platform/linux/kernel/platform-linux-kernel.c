/* SPDX-License-Identifier: MIT */
#include <zipc/zipc-kernel.h>

#include <linux/ktime.h>
#include <linux/random.h>
#ifdef __KERNEL__
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/jiffies.h>

struct zipc_kernel_transport {
    zipc_kernel_transport_type_t type;
    zipc_kernel_spsc_ring_t *ring;
    wait_queue_head_t *waitq;
    struct kfifo *fifo;
    int (*notify)(void *context);
    int (*wait)(void *context, unsigned long timeout_jiffies);
    void *context;
};

static zipc_status_t ring_push(zipc_kernel_spsc_ring_t *ring,
                               const zipc_message_t *message)
{
    u32 producer = (u32)atomic_read(&ring->producer);
    u32 consumer = (u32)atomic_read_acquire(&ring->consumer);

    if ((u32)(producer - consumer) >= ring->depth)
        return ZIPC_ERR_TRANSPORT;

    ring->entries[producer % ring->depth] = *message;
    atomic_set_release(&ring->producer, producer + 1U);
    return ZIPC_OK;
}

static zipc_status_t ring_pop(zipc_kernel_spsc_ring_t *ring,
                              zipc_message_t *message)
{
    u32 consumer = (u32)atomic_read(&ring->consumer);
    u32 producer = (u32)atomic_read_acquire(&ring->producer);

    if (consumer == producer)
        return ZIPC_ERR_NO_BUFFER;

    *message = ring->entries[consumer % ring->depth];
    atomic_set_release(&ring->consumer, consumer + 1U);
    return ZIPC_OK;
}

zipc_status_t zipc_kernel_transport_open(
    zipc_kernel_transport_t **out,
    const zipc_kernel_transport_config_t *cfg)
{
    zipc_kernel_transport_t *t;

    if (!out || !cfg)
        return ZIPC_ERR_INVALID_ARGUMENT;

    t = kzalloc(sizeof(*t), GFP_KERNEL);
    if (!t)
        return ZIPC_ERR_PLATFORM;

    t->type = cfg->type;
    t->ring = cfg->ring;
    t->waitq = cfg->waitq;
    t->fifo = cfg->fifo;
    t->notify = cfg->notify;
    t->wait = cfg->wait;
    t->context = cfg->context;

    if (t->type != ZIPC_KERNEL_TRANSPORT_KFIFO &&
        (!t->ring || t->ring->depth < 2U)) {
        kfree(t);
        return ZIPC_ERR_INVALID_ARGUMENT;
    }
    if (t->type == ZIPC_KERNEL_TRANSPORT_RING_WAITQUEUE && !t->waitq) {
        kfree(t);
        return ZIPC_ERR_INVALID_ARGUMENT;
    }

    *out = t;
    return ZIPC_OK;
}

zipc_status_t zipc_kernel_transport_send(zipc_kernel_transport_t *t,
                                         const zipc_message_t *message)
{
    zipc_status_t status;

    if (!t || !message)
        return ZIPC_ERR_INVALID_ARGUMENT;

    if (t->type == ZIPC_KERNEL_TRANSPORT_KFIFO) {
        unsigned int copied;

        if (!t->fifo || kfifo_avail(t->fifo) < sizeof(*message))
            return ZIPC_ERR_TRANSPORT;
        copied = kfifo_in(t->fifo, message, sizeof(*message));
        if (copied != sizeof(*message))
            return copied != 0U ? ZIPC_ERR_TRANSPORT_PUBLISHED
                                : ZIPC_ERR_TRANSPORT;
        if (t->waitq)
            wake_up_interruptible(t->waitq);
        return ZIPC_OK;
    }

    status = ring_push(t->ring, message);
    if (status != ZIPC_OK)
        return status;

    if (t->notify && t->notify(t->context))
        return ZIPC_ERR_TRANSPORT_PUBLISHED;
    if (t->waitq)
        wake_up_interruptible(t->waitq);
    return ZIPC_OK;
}

zipc_status_t zipc_kernel_transport_receive(zipc_kernel_transport_t *t,
                                            zipc_message_t *message,
                                            unsigned long timeout)
{
    if (!t || !message)
        return ZIPC_ERR_INVALID_ARGUMENT;

    if (t->type == ZIPC_KERNEL_TRANSPORT_KFIFO) {
        if (!t->fifo)
            return ZIPC_ERR_INVALID_ARGUMENT;
        if (kfifo_len(t->fifo) < sizeof(*message)) {
            if (!t->waitq || !wait_event_interruptible_timeout(
                    *t->waitq, kfifo_len(t->fifo) >= sizeof(*message), timeout))
                return ZIPC_ERR_NO_BUFFER;
        }
        return kfifo_out(t->fifo, message, sizeof(*message)) == sizeof(*message)
             ? ZIPC_OK : ZIPC_ERR_TRANSPORT;
    }

    if (ring_pop(t->ring, message) == ZIPC_OK)
        return ZIPC_OK;

    if (t->wait && t->wait(t->context, timeout))
        return ZIPC_ERR_PLATFORM;
    else if (t->waitq && !wait_event_interruptible_timeout(
                 *t->waitq,
                 atomic_read(&t->ring->consumer) !=
                 atomic_read(&t->ring->producer), timeout))
        return ZIPC_ERR_NO_BUFFER;

    return ring_pop(t->ring, message);
}

void zipc_kernel_transport_close(zipc_kernel_transport_t *transport)
{
    kfree(transport);
}
#endif

uint64_t zipc_platform_time_ns(void)
{
    return (uint64_t)ktime_get_ns();
}

zipc_status_t zipc_platform_random(void *buffer, size_t length)
{
    if (!buffer && length != 0U)
        return ZIPC_ERR_INVALID_ARGUMENT;
    if (length == 0U)
        return ZIPC_OK;
    return get_random_bytes_wait(buffer, length) == 0
         ? ZIPC_OK : ZIPC_ERR_ENTROPY_UNAVAILABLE;
}
