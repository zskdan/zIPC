#define _POSIX_C_SOURCE 200809L
#include <zipc/zipc.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
typedef uint16_t domid_t;
#include <xen/evtchn.h>

struct zipc_platform_memory {
    zipc_platform_memory_type_t type;
    int fd;
    void *mapping_base;
    size_t mapping_size;
    void *base;
    size_t size;
    uint64_t physical_base;
    uint32_t capabilities;
};

typedef struct {
    _Atomic uint32_t producer;
    _Atomic uint32_t consumer;
    uint32_t depth;
    uint32_t reserved;
    zipc_message_t entries[];
} zipc_xen_spsc_ring_t;

struct zipc_platform_transport {
    zipc_platform_transport_type_t type;
    int evtchn_fd;
    uint32_t local_port;
    bool owns_binding;
    zipc_xen_spsc_ring_t *ring;
    uint32_t depth;
};

static zipc_status_t xen_map_static(zipc_platform_memory_t *memory,
                                   const zipc_platform_memory_config_t *config)
{
    const char *path = config->backend.xen_static.device_path;
    const uint64_t physical =
        config->backend.xen_static.guest_physical_address;

    if (path == NULL || path[0] == '\0')
        return ZIPC_ERR_INVALID_ARGUMENT;

    const long page_size_long = sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0)
        return ZIPC_ERR_PLATFORM;

    const uint64_t page_size = (uint64_t)page_size_long;
    const uint64_t aligned = physical & ~(page_size - UINT64_C(1));
    const size_t page_offset = (size_t)(physical - aligned);
    if (config->size > SIZE_MAX - page_offset)
        return ZIPC_ERR_INVALID_ARGUMENT;

    memory->fd = open(path, O_RDWR);
    if (memory->fd < 0)
        return ZIPC_ERR_PLATFORM;

    memory->mapping_size = config->size + page_offset;
    memory->mapping_base = mmap(NULL, memory->mapping_size,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                memory->fd, (off_t)aligned);
    if (memory->mapping_base == MAP_FAILED) {
        memory->mapping_base = NULL;
        return ZIPC_ERR_PLATFORM;
    }

    memory->base = (uint8_t *)memory->mapping_base + page_offset;
    memory->size = config->size;
    memory->physical_base = physical;
    memory->capabilities = ZIPC_MEM_CAP_CPU_READ | ZIPC_MEM_CAP_CPU_WRITE |
                           ZIPC_MEM_CAP_FIXED_PHYS |
                           ZIPC_MEM_CAP_REMOTE_ACCESS;
    if (config->backend.xen_static.supports_cpu_atomics)
        memory->capabilities |= ZIPC_MEM_CAP_ATOMIC32 |
                                ZIPC_MEM_CAP_ATOMIC64;
    if (config->backend.xen_static.cacheable)
        memory->capabilities |= ZIPC_MEM_CAP_CACHEABLE;
    return ZIPC_OK;
}

zipc_status_t zipc_platform_memory_open(
    zipc_platform_memory_t **memory_out,
    const zipc_platform_memory_config_t *config)
{
    if (memory_out == NULL || config == NULL || config->size == 0U)
        return ZIPC_ERR_INVALID_ARGUMENT;
    if (config->type != ZIPC_SHM_XEN_STATIC)
        return ZIPC_ERR_UNSUPPORTED_MEMORY;

    zipc_platform_memory_t *memory = calloc(1U, sizeof(*memory));
    if (memory == NULL)
        return ZIPC_ERR_PLATFORM;
    memory->fd = -1;
    memory->physical_base = ZIPC_PHYS_ADDR_INVALID;
    memory->type = config->type;

    const zipc_status_t status = xen_map_static(memory, config);
    if (status != ZIPC_OK) {
        zipc_platform_memory_close(memory);
        return status;
    }

    *memory_out = memory;
    return ZIPC_OK;
}

void zipc_platform_memory_close(zipc_platform_memory_t *memory)
{
    if (memory == NULL)
        return;
    if (memory->mapping_base != NULL)
        (void)munmap(memory->mapping_base, memory->mapping_size);
    if (memory->fd >= 0)
        (void)close(memory->fd);
    free(memory);
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

static zipc_status_t xen_bind_evtchn(zipc_platform_transport_t *transport,
                                    const zipc_platform_transport_config_t *cfg)
{
    const char *path = cfg->device_path != NULL ?
                       cfg->device_path : "/dev/xen/evtchn";
    transport->evtchn_fd = open(path, O_RDWR | O_CLOEXEC);
    if (transport->evtchn_fd < 0)
        return ZIPC_ERR_PLATFORM;

    int port;
    if (cfg->xen_static_port != 0U) {
        struct ioctl_evtchn_bind arg = { .port = cfg->xen_static_port };
        port = ioctl(transport->evtchn_fd, IOCTL_EVTCHN_BIND_STATIC, &arg);
    } else if (cfg->xen_remote_port != 0U) {
        struct ioctl_evtchn_bind_interdomain arg = {
            .remote_domain = cfg->xen_remote_domid,
            .remote_port = cfg->xen_remote_port
        };
        port = ioctl(transport->evtchn_fd,
                     IOCTL_EVTCHN_BIND_INTERDOMAIN, &arg);
    } else {
        struct ioctl_evtchn_bind_unbound_port arg = {
            .remote_domain = cfg->xen_remote_domid
        };
        port = ioctl(transport->evtchn_fd,
                     IOCTL_EVTCHN_BIND_UNBOUND_PORT, &arg);
    }

    if (port < 0)
        return ZIPC_ERR_PLATFORM;

    transport->local_port = (uint32_t)port;
    transport->owns_binding = true;
    return ZIPC_OK;
}

zipc_status_t zipc_platform_transport_open(
    zipc_platform_transport_t **transport_out,
    const zipc_platform_transport_config_t *config)
{
    if (transport_out == NULL || config == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;
    if (config->type != ZIPC_TRANSPORT_XEN_RING_EVTCHN ||
        config->platform_handle == NULL || config->ring_depth < 2U)
        return ZIPC_ERR_INVALID_ARGUMENT;

    zipc_platform_transport_t *transport = calloc(1U, sizeof(*transport));
    if (transport == NULL)
        return ZIPC_ERR_PLATFORM;
    transport->type = config->type;
    transport->evtchn_fd = -1;
    transport->ring = (zipc_xen_spsc_ring_t *)config->platform_handle;
    transport->depth = config->ring_depth;

    if (config->create_endpoint) {
        atomic_store_explicit(&transport->ring->producer, 0U,
                              memory_order_relaxed);
        atomic_store_explicit(&transport->ring->consumer, 0U,
                              memory_order_relaxed);
        transport->ring->depth = config->ring_depth;
        transport->ring->reserved = 0U;
    } else if (transport->ring->depth != config->ring_depth) {
        free(transport);
        return ZIPC_ERR_INVALID_ARGUMENT;
    }

    const zipc_status_t status = xen_bind_evtchn(transport, config);
    if (status != ZIPC_OK) {
        zipc_platform_transport_close(transport);
        return status;
    }

    *transport_out = transport;
    return ZIPC_OK;
}

zipc_status_t zipc_platform_transport_send(
    zipc_platform_transport_t *transport,
    const zipc_message_t *message)
{
    if (transport == NULL || message == NULL ||
        transport->type != ZIPC_TRANSPORT_XEN_RING_EVTCHN)
        return ZIPC_ERR_INVALID_ARGUMENT;

    zipc_xen_spsc_ring_t *ring = transport->ring;
    const uint32_t producer = atomic_load_explicit(&ring->producer,
                                                   memory_order_relaxed);
    const uint32_t consumer = atomic_load_explicit(&ring->consumer,
                                                   memory_order_acquire);
    if ((producer - consumer) >= transport->depth)
        return ZIPC_ERR_TRANSPORT;

    ring->entries[producer % transport->depth] = *message;
    atomic_store_explicit(&ring->producer, producer + 1U,
                          memory_order_release);

    struct ioctl_evtchn_notify arg = { .port = transport->local_port };
    if (ioctl(transport->evtchn_fd, IOCTL_EVTCHN_NOTIFY, &arg) < 0)
        return ZIPC_ERR_TRANSPORT;
    return ZIPC_OK;
}

zipc_status_t zipc_platform_transport_receive(
    zipc_platform_transport_t *transport,
    zipc_message_t *message)
{
    if (transport == NULL || message == NULL ||
        transport->type != ZIPC_TRANSPORT_XEN_RING_EVTCHN)
        return ZIPC_ERR_INVALID_ARGUMENT;

    zipc_xen_spsc_ring_t *ring = transport->ring;
    for (;;) {
        const uint32_t consumer = atomic_load_explicit(&ring->consumer,
                                                       memory_order_relaxed);
        const uint32_t producer = atomic_load_explicit(&ring->producer,
                                                       memory_order_acquire);
        if (consumer != producer) {
            *message = ring->entries[consumer % transport->depth];
            atomic_store_explicit(&ring->consumer, consumer + 1U,
                                  memory_order_release);
            return ZIPC_OK;
        }

        uint32_t pending_port = 0U;
        ssize_t count;
        do {
            count = read(transport->evtchn_fd, &pending_port,
                         sizeof(pending_port));
        } while (count < 0 && errno == EINTR);
        if (count != (ssize_t)sizeof(pending_port))
            return ZIPC_ERR_TRANSPORT;

        /* Writing the port back unmasks it for subsequent notifications. */
        do {
            count = write(transport->evtchn_fd, &pending_port,
                          sizeof(pending_port));
        } while (count < 0 && errno == EINTR);
        if (count != (ssize_t)sizeof(pending_port))
            return ZIPC_ERR_TRANSPORT;
    }
}

void zipc_platform_transport_close(zipc_platform_transport_t *transport)
{
    if (transport == NULL)
        return;

    if (transport->evtchn_fd >= 0 && transport->owns_binding) {
        struct ioctl_evtchn_unbind arg = { .port = transport->local_port };
        (void)ioctl(transport->evtchn_fd, IOCTL_EVTCHN_UNBIND, &arg);
    }
    if (transport->evtchn_fd >= 0)
        (void)close(transport->evtchn_fd);
    free(transport);
}

uint32_t zipc_platform_transport_xen_local_port(
    const zipc_platform_transport_t *transport)
{
    if (transport == NULL ||
        transport->type != ZIPC_TRANSPORT_XEN_RING_EVTCHN)
        return 0U;
    return transport->local_port;
}

void *zipc_platform_alloc(size_t size)
{
    return calloc(1U, size);
}

void zipc_platform_free(void *pointer)
{
    free(pointer);
}

uint64_t zipc_platform_time_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0U;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}
