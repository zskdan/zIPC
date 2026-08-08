#include <zipc/zipc.h>

#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include <openamp/open_amp.h>

#ifndef ZIPC_FREERTOS_DEFAULT_QUEUE_LENGTH
#define ZIPC_FREERTOS_DEFAULT_QUEUE_LENGTH 8U
#endif

#ifndef ZIPC_FREERTOS_PHYS_TO_VIRT
#define ZIPC_FREERTOS_PHYS_TO_VIRT(address) ((void *)(uintptr_t)(address))
#endif

struct zipc_platform_memory {
    zipc_platform_memory_type_t type;
    void *base;
    size_t size;
    uint64_t physical_base;
    uint32_t capabilities;
};

zipc_status_t zipc_platform_memory_open(
    zipc_platform_memory_t **memory_out,
    const zipc_platform_memory_config_t *config)
{
    if (memory_out == NULL || config == NULL || config->size == 0U)
        return ZIPC_ERR_INVALID_ARGUMENT;

    if (config->type != ZIPC_SHM_DTREVMEM_CACHED &&
        config->type != ZIPC_SHM_DTREVMEM_UNCACHED &&
        config->type != ZIPC_SHM_PREALLOCATED)
        return ZIPC_ERR_UNSUPPORTED_MEMORY;

    zipc_platform_memory_t *memory = pvPortMalloc(sizeof(*memory));
    if (memory == NULL)
        return ZIPC_ERR_PLATFORM;

    memset(memory, 0, sizeof(*memory));
    memory->type = config->type;
    memory->size = config->size;

    if (config->type == ZIPC_SHM_PREALLOCATED) {
        if (config->backend.preallocated.address == NULL) {
            vPortFree(memory);
            return ZIPC_ERR_INVALID_ARGUMENT;
        }
        memory->base = config->backend.preallocated.address;
        memory->physical_base = config->backend.preallocated.physical_address;
        memory->capabilities = config->backend.preallocated.capabilities;
    } else {
        const uint64_t physical = config->backend.dtrevmem.physical_address;
        if (physical == ZIPC_PHYS_ADDR_INVALID) {
            vPortFree(memory);
            return ZIPC_ERR_INVALID_ARGUMENT;
        }
        memory->base = ZIPC_FREERTOS_PHYS_TO_VIRT(physical);
        memory->physical_base = physical;
        memory->capabilities = ZIPC_MEM_CAP_CPU_READ |
                               ZIPC_MEM_CAP_CPU_WRITE |
                               ZIPC_MEM_CAP_FIXED_PHYS;

        if (config->type == ZIPC_SHM_DTREVMEM_CACHED)
            memory->capabilities |= ZIPC_MEM_CAP_CACHEABLE;

        if (config->backend.dtrevmem.supports_cpu_atomics)
            memory->capabilities |= ZIPC_MEM_CAP_ATOMIC32 |
                                    ZIPC_MEM_CAP_ATOMIC64;

        if (config->backend.dtrevmem.remote_accessible)
            memory->capabilities |= ZIPC_MEM_CAP_REMOTE_ACCESS;

        if (config->backend.dtrevmem.device_memory)
            memory->capabilities |= ZIPC_MEM_CAP_DEVICE_MEMORY;
    }

    *memory_out = memory;
    return ZIPC_OK;
}

void zipc_platform_memory_close(zipc_platform_memory_t *memory)
{
    if (memory != NULL)
        vPortFree(memory);
}

void *zipc_platform_memory_base(const zipc_platform_memory_t *memory)
{
    return memory != NULL ? memory->base : NULL;
}

size_t zipc_platform_memory_size(const zipc_platform_memory_t *memory)
{
    return memory != NULL ? memory->size : 0U;
}

uint64_t zipc_platform_memory_physical_base(const zipc_platform_memory_t *memory)
{
    return memory != NULL ? memory->physical_base : ZIPC_PHYS_ADDR_INVALID;
}

uint32_t zipc_platform_memory_capabilities(const zipc_platform_memory_t *memory)
{
    return memory != NULL ? memory->capabilities : 0U;
}

struct zipc_platform_transport {
    zipc_platform_transport_type_t type;
    TickType_t timeout_ticks;
    bool owns_handle;
    union {
        QueueHandle_t queue;
        struct {
            struct rpmsg_endpoint *endpoint;
            QueueHandle_t receive_queue;
        } rpmsg;
        struct {
            TaskHandle_t destination_task;
            zipc_message_t *mailbox;
            UBaseType_t notification_index;
        } task_notification;
        struct {
            zipc_message_t *mailbox;
            zipc_platform_signal_send_fn send;
            zipc_platform_signal_wait_fn wait;
            void *context;
        } ipi;
        struct {
            zipc_transport_spsc_ring_t *ring;
            zipc_platform_signal_send_fn send;
            zipc_platform_signal_wait_fn wait;
            void *context;
        } pl_ring_irq;
        struct {
            zipc_platform_secure_call_fn call;
            void *context;
            uint32_t service_id;
            zipc_message_t response;
            bool response_pending;
        } secure;
    } handle;
};

static zipc_status_t open_queue(zipc_platform_transport_t *transport,
                               const zipc_platform_transport_config_t *config)
{
    transport->timeout_ticks = (TickType_t)config->timeout_ticks;

    if (config->create_endpoint) {
        const UBaseType_t length = config->queue_length != 0U
                                 ? (UBaseType_t)config->queue_length
                                 : (UBaseType_t)ZIPC_FREERTOS_DEFAULT_QUEUE_LENGTH;
        transport->handle.queue = xQueueCreate(length, sizeof(zipc_message_t));
        transport->owns_handle = true;
    } else {
        transport->handle.queue = (QueueHandle_t)config->platform_handle;
        transport->owns_handle = false;
    }

    return transport->handle.queue != NULL ? ZIPC_OK : ZIPC_ERR_PLATFORM;
}

static zipc_status_t open_rpmsg(zipc_platform_transport_t *transport,
                               const zipc_platform_transport_config_t *config)
{
    transport->handle.rpmsg.endpoint =
        (struct rpmsg_endpoint *)config->platform_handle;
    transport->handle.rpmsg.receive_queue =
        (QueueHandle_t)config->receive_handle;
    transport->timeout_ticks = (TickType_t)config->timeout_ticks;
    transport->owns_handle = false;

    return transport->handle.rpmsg.endpoint != NULL
         ? ZIPC_OK : ZIPC_ERR_INVALID_ARGUMENT;
}

static zipc_status_t open_task_notification(
    zipc_platform_transport_t *transport,
    const zipc_platform_transport_config_t *config)
{
    if (config->platform_handle == NULL || config->shared_mailbox == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    transport->handle.task_notification.destination_task =
        (TaskHandle_t)config->platform_handle;
    transport->handle.task_notification.mailbox = config->shared_mailbox;
    transport->handle.task_notification.notification_index =
        (UBaseType_t)config->notification_index;
    transport->timeout_ticks = (TickType_t)config->timeout_ticks;
    transport->owns_handle = false;
    return ZIPC_OK;
}

static zipc_status_t open_ipi(zipc_platform_transport_t *transport,
                             const zipc_platform_transport_config_t *config)
{
    if (config->shared_mailbox == NULL ||
        config->signal_send == NULL || config->signal_wait == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    transport->handle.ipi.mailbox = config->shared_mailbox;
    transport->handle.ipi.send = config->signal_send;
    transport->handle.ipi.wait = config->signal_wait;
    transport->handle.ipi.context = config->signal_context;
    transport->timeout_ticks = (TickType_t)config->timeout_ticks;
    transport->owns_handle = false;
    return ZIPC_OK;
}

static zipc_status_t open_pl_ring_irq(
    zipc_platform_transport_t *transport,
    const zipc_platform_transport_config_t *config)
{
    zipc_transport_spsc_ring_t *ring =
        (zipc_transport_spsc_ring_t *)config->platform_handle;
    if (ring == NULL || config->signal_send == NULL ||
        config->signal_wait == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    if (config->create_endpoint) {
        zipc_status_t status = zipc_transport_spsc_ring_initialize(
            ring, config->ring_depth);
        if (status != ZIPC_OK)
            return status;
    } else if (ring->depth < 2U ||
               (config->ring_depth != 0U &&
                ring->depth != config->ring_depth)) {
        return ZIPC_ERR_INVALID_ARGUMENT;
    }

    transport->handle.pl_ring_irq.ring = ring;
    transport->handle.pl_ring_irq.send = config->signal_send;
    transport->handle.pl_ring_irq.wait = config->signal_wait;
    transport->handle.pl_ring_irq.context = config->signal_context;
    transport->timeout_ticks = (TickType_t)config->timeout_ticks;
    transport->owns_handle = false;
    return ZIPC_OK;
}

static zipc_status_t open_secure(
    zipc_platform_transport_t *transport,
    const zipc_platform_transport_config_t *config)
{
    if (config->secure_call == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    transport->handle.secure.call = config->secure_call;
    transport->handle.secure.context = config->secure_context;
    transport->handle.secure.service_id = config->secure_service_id;
    transport->handle.secure.response_pending = false;
    return ZIPC_OK;
}

zipc_status_t zipc_platform_transport_open(
    zipc_platform_transport_t **transport_out,
    const zipc_platform_transport_config_t *config)
{
    if (transport_out == NULL || config == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    zipc_platform_transport_t *transport = pvPortMalloc(sizeof(*transport));
    if (transport == NULL)
        return ZIPC_ERR_PLATFORM;

    memset(transport, 0, sizeof(*transport));
    transport->type = config->type;

    zipc_status_t status;
    switch (config->type) {
    case ZIPC_TRANSPORT_MESSAGE_QUEUE:
        status = open_queue(transport, config);
        break;
    case ZIPC_TRANSPORT_RPMSG:
        status = open_rpmsg(transport, config);
        break;
    case ZIPC_TRANSPORT_TASK_NOTIFICATION:
        status = open_task_notification(transport, config);
        break;
    case ZIPC_TRANSPORT_IPI:
        status = open_ipi(transport, config);
        break;
    case ZIPC_TRANSPORT_PL_RING_IRQ:
        status = open_pl_ring_irq(transport, config);
        break;
    case ZIPC_TRANSPORT_SMC:
    case ZIPC_TRANSPORT_FFA:
        status = open_secure(transport, config);
        break;
    default:
        status = ZIPC_ERR_TRANSPORT;
        break;
    }

    if (status != ZIPC_OK) {
        vPortFree(transport);
        return status;
    }

    *transport_out = transport;
    return ZIPC_OK;
}

zipc_status_t zipc_platform_transport_send(
    zipc_platform_transport_t *transport,
    const zipc_message_t *message)
{
    if (transport == NULL || message == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    switch (transport->type) {
    case ZIPC_TRANSPORT_MESSAGE_QUEUE:
        return xQueueSend(transport->handle.queue, message,
                          transport->timeout_ticks) == pdPASS
             ? ZIPC_OK : ZIPC_ERR_TRANSPORT;

    case ZIPC_TRANSPORT_RPMSG: {
        const int result = rpmsg_send(transport->handle.rpmsg.endpoint,
                                      message, sizeof(*message));
        return result >= 0 ? ZIPC_OK : ZIPC_ERR_TRANSPORT;
    }

    case ZIPC_TRANSPORT_TASK_NOTIFICATION:
        *transport->handle.task_notification.mailbox = *message;
        atomic_thread_fence(memory_order_release);
        return xTaskNotifyGiveIndexed(
                   transport->handle.task_notification.destination_task,
                   transport->handle.task_notification.notification_index) == pdPASS
             ? ZIPC_OK : ZIPC_ERR_TRANSPORT;

    case ZIPC_TRANSPORT_IPI:
        *transport->handle.ipi.mailbox = *message;
        atomic_thread_fence(memory_order_release);
        return transport->handle.ipi.send(transport->handle.ipi.context);


    case ZIPC_TRANSPORT_PL_RING_IRQ: {
        zipc_transport_spsc_ring_t *ring =
            transport->handle.pl_ring_irq.ring;
        const uint32_t producer = atomic_load_explicit(
            &ring->producer, memory_order_relaxed);
        const uint32_t consumer = atomic_load_explicit(
            &ring->consumer, memory_order_acquire);
        if ((uint32_t)(producer - consumer) >= ring->depth)
            return ZIPC_ERR_TRANSPORT;
        ring->entries[producer % ring->depth] = *message;
        atomic_store_explicit(&ring->producer, producer + 1U,
                              memory_order_release);
        return transport->handle.pl_ring_irq.send(
            transport->handle.pl_ring_irq.context);
    }
    case ZIPC_TRANSPORT_SMC:
    case ZIPC_TRANSPORT_FFA: {
        zipc_status_t status = transport->handle.secure.call(
            transport->handle.secure.context,
            transport->handle.secure.service_id,
            message,
            &transport->handle.secure.response);
        if (status == ZIPC_OK)
            transport->handle.secure.response_pending = true;
        return status;
    }

    default:
        return ZIPC_ERR_TRANSPORT;
    }
}

zipc_status_t zipc_platform_transport_receive(
    zipc_platform_transport_t *transport,
    zipc_message_t *message)
{
    if (transport == NULL || message == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    switch (transport->type) {
    case ZIPC_TRANSPORT_MESSAGE_QUEUE:
        return xQueueReceive(transport->handle.queue, message,
                             transport->timeout_ticks) == pdPASS
             ? ZIPC_OK : ZIPC_ERR_TRANSPORT;

    case ZIPC_TRANSPORT_RPMSG:
        if (transport->handle.rpmsg.receive_queue == NULL)
            return ZIPC_ERR_TRANSPORT;
        return xQueueReceive(transport->handle.rpmsg.receive_queue, message,
                             transport->timeout_ticks) == pdPASS
             ? ZIPC_OK : ZIPC_ERR_TRANSPORT;

    case ZIPC_TRANSPORT_TASK_NOTIFICATION:
        if (ulTaskNotifyTakeIndexed(
                transport->handle.task_notification.notification_index,
                pdTRUE, transport->timeout_ticks) == 0U)
            return ZIPC_ERR_TRANSPORT;
        atomic_thread_fence(memory_order_acquire);
        *message = *transport->handle.task_notification.mailbox;
        return ZIPC_OK;

    case ZIPC_TRANSPORT_IPI: {
        const zipc_status_t status = transport->handle.ipi.wait(
            transport->handle.ipi.context,
            (uint32_t)transport->timeout_ticks);
        if (status != ZIPC_OK)
            return status;
        atomic_thread_fence(memory_order_acquire);
        *message = *transport->handle.ipi.mailbox;
        return ZIPC_OK;
    }


    case ZIPC_TRANSPORT_PL_RING_IRQ: {
        const zipc_status_t status = transport->handle.pl_ring_irq.wait(
            transport->handle.pl_ring_irq.context,
            (uint32_t)transport->timeout_ticks);
        if (status != ZIPC_OK)
            return status;
        zipc_transport_spsc_ring_t *ring =
            transport->handle.pl_ring_irq.ring;
        const uint32_t consumer = atomic_load_explicit(
            &ring->consumer, memory_order_relaxed);
        const uint32_t producer = atomic_load_explicit(
            &ring->producer, memory_order_acquire);
        if (consumer == producer)
            return ZIPC_ERR_TRANSPORT;
        *message = ring->entries[consumer % ring->depth];
        atomic_store_explicit(&ring->consumer, consumer + 1U,
                              memory_order_release);
        return ZIPC_OK;
    }
    case ZIPC_TRANSPORT_SMC:
    case ZIPC_TRANSPORT_FFA:
        if (!transport->handle.secure.response_pending)
            return ZIPC_ERR_TRANSPORT;
        *message = transport->handle.secure.response;
        transport->handle.secure.response_pending = false;
        return ZIPC_OK;

    default:
        return ZIPC_ERR_TRANSPORT;
    }
}

void zipc_platform_transport_close(zipc_platform_transport_t *transport)
{
    if (transport == NULL)
        return;

    if (transport->type == ZIPC_TRANSPORT_MESSAGE_QUEUE &&
        transport->owns_handle && transport->handle.queue != NULL)
        vQueueDelete(transport->handle.queue);

    /* RPMsg endpoints, tasks, mailboxes and IPI channels are externally owned. */
    vPortFree(transport);
}

uint32_t zipc_platform_transport_xen_local_port(
    const zipc_platform_transport_t *transport)
{
    (void)transport;
    return 0U;
}

void *zipc_platform_alloc(size_t size)
{
    void *pointer = pvPortMalloc(size);
    if (pointer != NULL)
        memset(pointer, 0, size);
    return pointer;
}

void zipc_platform_free(void *pointer)
{
    vPortFree(pointer);
}

uint64_t zipc_platform_time_ns(void)
{
    return (uint64_t)xTaskGetTickCount() * UINT64_C(1000000000) / (uint64_t)configTICK_RATE_HZ;
}
