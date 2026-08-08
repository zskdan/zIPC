#include "zipc/zipc.h"

zipc_status_t zipc_transport_backend_roles(
    zipc_platform_transport_type_t transport,
    zipc_descriptor_backend_type_t *descriptor,
    zipc_event_backend_type_t *event)
{
    if (descriptor == NULL || event == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    switch (transport) {
    case ZIPC_TRANSPORT_MESSAGE_QUEUE:
        *descriptor = ZIPC_DESCRIPTOR_BACKEND_MESSAGE_QUEUE;
        *event = ZIPC_EVENT_BACKEND_INTEGRATED;
        break;
    case ZIPC_TRANSPORT_UNIX_DGRAM:
        *descriptor = ZIPC_DESCRIPTOR_BACKEND_UNIX_DGRAM;
        *event = ZIPC_EVENT_BACKEND_INTEGRATED;
        break;
    case ZIPC_TRANSPORT_FIFO:
        *descriptor = ZIPC_DESCRIPTOR_BACKEND_FIFO;
        *event = ZIPC_EVENT_BACKEND_INTEGRATED;
        break;
    case ZIPC_TRANSPORT_SHM_RING_EVENTFD:
        *descriptor = ZIPC_DESCRIPTOR_BACKEND_SHM_RING;
        *event = ZIPC_EVENT_BACKEND_EVENTFD;
        break;
    case ZIPC_TRANSPORT_SHM_RING_POLLING:
        *descriptor = ZIPC_DESCRIPTOR_BACKEND_SHM_RING;
        *event = ZIPC_EVENT_BACKEND_SHM_POLLING;
        break;
    case ZIPC_TRANSPORT_RPMSG:
        *descriptor = ZIPC_DESCRIPTOR_BACKEND_RPMSG;
        *event = ZIPC_EVENT_BACKEND_INTEGRATED;
        break;
    case ZIPC_TRANSPORT_TASK_NOTIFICATION:
        *descriptor = ZIPC_DESCRIPTOR_BACKEND_SHARED_MAILBOX;
        *event = ZIPC_EVENT_BACKEND_TASK_NOTIFICATION;
        break;
    case ZIPC_TRANSPORT_IPI:
        *descriptor = ZIPC_DESCRIPTOR_BACKEND_SHARED_MAILBOX;
        *event = ZIPC_EVENT_BACKEND_IPI;
        break;
    case ZIPC_TRANSPORT_XEN_RING_EVTCHN:
        *descriptor = ZIPC_DESCRIPTOR_BACKEND_XEN_RING;
        *event = ZIPC_EVENT_BACKEND_XEN_EVENT_CHANNEL;
        break;
    case ZIPC_TRANSPORT_PL_RING_IRQ:
        *descriptor = ZIPC_DESCRIPTOR_BACKEND_PL_RING;
        *event = ZIPC_EVENT_BACKEND_IRQ;
        break;
    case ZIPC_TRANSPORT_SMC:
    case ZIPC_TRANSPORT_FFA:
        *descriptor = ZIPC_DESCRIPTOR_BACKEND_SECURE_CALL;
        *event = ZIPC_EVENT_BACKEND_SYNCHRONOUS_CALL;
        break;
    default:
        return ZIPC_ERR_TRANSPORT;
    }
    return ZIPC_OK;
}

const char *zipc_payload_backend_name(zipc_payload_backend_type_t backend)
{
    switch (backend) {
    case ZIPC_SHM_POSIX: return "posix-shm";
    case ZIPC_SHM_HUGEPAGES: return "hugepages";
    case ZIPC_SHM_DTREVMEM_CACHED: return "dtrevmem-cached";
    case ZIPC_SHM_DTREVMEM_UNCACHED: return "dtrevmem-uncached";
    case ZIPC_SHM_XEN_STATIC: return "xen-static";
    case ZIPC_SHM_PREALLOCATED: return "preallocated";
    default: return "unknown";
    }
}

const char *zipc_descriptor_backend_name(zipc_descriptor_backend_type_t backend)
{
    switch (backend) {
    case ZIPC_DESCRIPTOR_BACKEND_NONE: return "none";
    case ZIPC_DESCRIPTOR_BACKEND_MESSAGE_QUEUE: return "message-queue";
    case ZIPC_DESCRIPTOR_BACKEND_UNIX_DGRAM: return "unix-dgram";
    case ZIPC_DESCRIPTOR_BACKEND_FIFO: return "fifo";
    case ZIPC_DESCRIPTOR_BACKEND_SHM_RING: return "shm-ring";
    case ZIPC_DESCRIPTOR_BACKEND_RPMSG: return "rpmsg";
    case ZIPC_DESCRIPTOR_BACKEND_SHARED_MAILBOX: return "shared-mailbox";
    case ZIPC_DESCRIPTOR_BACKEND_XEN_RING: return "xen-ring";
    case ZIPC_DESCRIPTOR_BACKEND_PL_RING: return "pl-ring";
    case ZIPC_DESCRIPTOR_BACKEND_SECURE_CALL: return "secure-call";
    default: return "unknown";
    }
}

const char *zipc_event_backend_name(zipc_event_backend_type_t backend)
{
    switch (backend) {
    case ZIPC_EVENT_BACKEND_NONE: return "none";
    case ZIPC_EVENT_BACKEND_INTEGRATED: return "integrated";
    case ZIPC_EVENT_BACKEND_EVENTFD: return "eventfd";
    case ZIPC_EVENT_BACKEND_SHM_POLLING: return "shm-polling";
    case ZIPC_EVENT_BACKEND_TASK_NOTIFICATION: return "task-notification";
    case ZIPC_EVENT_BACKEND_IPI: return "ipi";
    case ZIPC_EVENT_BACKEND_XEN_EVENT_CHANNEL: return "xen-event-channel";
    case ZIPC_EVENT_BACKEND_IRQ: return "irq";
    case ZIPC_EVENT_BACKEND_SYNCHRONOUS_CALL: return "synchronous-call";
    default: return "unknown";
    }
}
