#define _POSIX_C_SOURCE 200809L
#include <zipc/zipc.h>
#include "../common/platform-linux-common.h"

#include <fcntl.h>
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>

struct zipc_platform_memory {
    zipc_platform_memory_type_t type;
    int fd;
    void *mapping_base;
    size_t mapping_size;
    void *base;
    size_t size;
    uint64_t physical_base;
    uint32_t capabilities;
    bool unlink_on_close;
    char object_name[256];
};

static zipc_status_t map_fd(zipc_platform_memory_t *memory,
                           size_t mapping_size,
                           off_t offset,
                           size_t page_offset)
{
    memory->mapping_base = mmap(NULL, mapping_size,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                memory->fd, offset);
    if (memory->mapping_base == MAP_FAILED) {
        memory->mapping_base = NULL;
        return ZIPC_ERR_PLATFORM;
    }
    memory->mapping_size = mapping_size;
    memory->base = (uint8_t *)memory->mapping_base + page_offset;
    return ZIPC_OK;
}

static zipc_status_t open_posix(zipc_platform_memory_t *memory,
                               const zipc_platform_memory_config_t *config)
{
    const char *name = config->backend.posix.name;
    if (name == NULL || name[0] == '\0')
        return ZIPC_ERR_INVALID_ARGUMENT;

    const bool create = config->backend.posix.create;
    memory->unlink_on_close = config->backend.posix.unlink_on_close;
    snprintf(memory->object_name, sizeof(memory->object_name), "%s", name);

    memory->fd = shm_open(name, O_RDWR | (create ? O_CREAT : 0), 0600);
    if (memory->fd < 0)
        return ZIPC_ERR_PLATFORM;
    if (create && ftruncate(memory->fd, (off_t)config->size) != 0)
        return ZIPC_ERR_PLATFORM;

    zipc_status_t status = map_fd(memory, config->size, 0, 0U);
    if (status != ZIPC_OK)
        return status;

    memory->size = config->size;
    memory->physical_base = ZIPC_PHYS_ADDR_INVALID;
    memory->capabilities = ZIPC_MEM_CAP_CPU_READ | ZIPC_MEM_CAP_CPU_WRITE |
                           ZIPC_MEM_CAP_ATOMIC32 | ZIPC_MEM_CAP_ATOMIC64 |
                           ZIPC_MEM_CAP_CACHEABLE | ZIPC_MEM_CAP_PAGE_PROTECT;
    return ZIPC_OK;
}

static zipc_status_t open_hugepages(zipc_platform_memory_t *memory,
                                   const zipc_platform_memory_config_t *config)
{
    const char *path = config->backend.hugepages.path;
    if (path == NULL || path[0] == '\0')
        return ZIPC_ERR_INVALID_ARGUMENT;

    const bool create = config->backend.hugepages.create;
    memory->unlink_on_close = config->backend.hugepages.unlink_on_close;
    snprintf(memory->object_name, sizeof(memory->object_name), "%s", path);

    memory->fd = open(path, O_RDWR | (create ? O_CREAT : 0), 0600);
    if (memory->fd < 0)
        return ZIPC_ERR_PLATFORM;
    if (create && ftruncate(memory->fd, (off_t)config->size) != 0)
        return ZIPC_ERR_PLATFORM;

    zipc_status_t status = map_fd(memory, config->size, 0, 0U);
    if (status != ZIPC_OK)
        return status;

    memory->size = config->size;
    memory->physical_base = ZIPC_PHYS_ADDR_INVALID;
    memory->capabilities = ZIPC_MEM_CAP_CPU_READ | ZIPC_MEM_CAP_CPU_WRITE |
                           ZIPC_MEM_CAP_ATOMIC32 | ZIPC_MEM_CAP_ATOMIC64 |
                           ZIPC_MEM_CAP_CACHEABLE;
    return ZIPC_OK;
}

static zipc_status_t open_dtrevmem(zipc_platform_memory_t *memory,
                                  const zipc_platform_memory_config_t *config,
                                  bool cached)
{
    const char *path = config->backend.dtrevmem.device_path;
    const uint64_t physical = config->backend.dtrevmem.physical_address;
    if (path == NULL || path[0] == '\0')
        return ZIPC_ERR_INVALID_ARGUMENT;

    const long page_size_long = sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0)
        return ZIPC_ERR_PLATFORM;

    const uint64_t page_size = (uint64_t)page_size_long;
    const uint64_t aligned = physical & ~(page_size - 1U);
    const size_t page_offset = (size_t)(physical - aligned);
    if (config->size > SIZE_MAX - page_offset)
        return ZIPC_ERR_INVALID_ARGUMENT;

    const int open_flags = O_RDWR | (cached ? 0 : O_SYNC);
    memory->fd = open(path, open_flags);
    if (memory->fd < 0)
        return ZIPC_ERR_PLATFORM;

    zipc_status_t status = map_fd(memory, config->size + page_offset,
                                 (off_t)aligned, page_offset);
    if (status != ZIPC_OK)
        return status;

    memory->size = config->size;
    memory->physical_base = physical;
    memory->capabilities = ZIPC_MEM_CAP_CPU_READ | ZIPC_MEM_CAP_CPU_WRITE |
                           ZIPC_MEM_CAP_FIXED_PHYS;
    if (cached)
        memory->capabilities |= ZIPC_MEM_CAP_CACHEABLE;
    if (config->backend.dtrevmem.supports_cpu_atomics)
        memory->capabilities |= ZIPC_MEM_CAP_ATOMIC32 |
                                ZIPC_MEM_CAP_ATOMIC64;
    if (config->backend.dtrevmem.remote_accessible)
        memory->capabilities |= ZIPC_MEM_CAP_REMOTE_ACCESS;
    if (config->backend.dtrevmem.device_memory)
        memory->capabilities |= ZIPC_MEM_CAP_DEVICE_MEMORY;
    return ZIPC_OK;
}

zipc_status_t zipc_platform_memory_open(
    zipc_platform_memory_t **memory_out,
    const zipc_platform_memory_config_t *config)
{
    if (memory_out == NULL || config == NULL || config->size == 0U)
        return ZIPC_ERR_INVALID_ARGUMENT;

    zipc_platform_memory_t *memory = calloc(1, sizeof(*memory));
    if (memory == NULL)
        return ZIPC_ERR_PLATFORM;
    memory->type = config->type;
    memory->fd = -1;
    memory->physical_base = ZIPC_PHYS_ADDR_INVALID;

    zipc_status_t status;
    switch (config->type) {
    case ZIPC_SHM_POSIX:
        status = open_posix(memory, config);
        break;
    case ZIPC_SHM_HUGEPAGES:
        status = open_hugepages(memory, config);
        break;
    case ZIPC_SHM_DTREVMEM_CACHED:
        status = open_dtrevmem(memory, config, true);
        break;
    case ZIPC_SHM_DTREVMEM_UNCACHED:
        status = open_dtrevmem(memory, config, false);
        break;
    case ZIPC_SHM_PREALLOCATED:
        if (config->backend.preallocated.address == NULL) {
            status = ZIPC_ERR_INVALID_ARGUMENT;
            break;
        }
        memory->base = config->backend.preallocated.address;
        memory->size = config->size;
        memory->physical_base = config->backend.preallocated.physical_address;
        memory->capabilities = config->backend.preallocated.capabilities;
        status = ZIPC_OK;
        break;
    default:
        status = ZIPC_ERR_INVALID_ARGUMENT;
        break;
    }

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
        munmap(memory->mapping_base, memory->mapping_size);
    if (memory->fd >= 0)
        close(memory->fd);
    if (memory->unlink_on_close && memory->object_name[0] != '\0') {
        if (memory->type == ZIPC_SHM_POSIX)
            shm_unlink(memory->object_name);
        else if (memory->type == ZIPC_SHM_HUGEPAGES)
            unlink(memory->object_name);
    }
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

size_t zipc_platform_page_size(void)
{
    const long page_size = sysconf(_SC_PAGESIZE);
    return page_size > 0 ? (size_t)page_size : 0U;
}

size_t zipc_platform_memory_page_size(const zipc_platform_memory_t *memory)
{
    if (memory == NULL ||
        (memory->capabilities & ZIPC_MEM_CAP_PAGE_PROTECT) == 0U)
        return 0U;

    return zipc_platform_page_size();
}

zipc_status_t zipc_platform_memory_protect_none(zipc_platform_memory_t *memory,
                                               size_t offset,
                                               size_t length)
{
    if (memory == NULL || length == 0U ||
        (memory->capabilities & ZIPC_MEM_CAP_PAGE_PROTECT) == 0U)
        return ZIPC_ERR_UNSUPPORTED_MEMORY;

    const size_t page_size = zipc_platform_memory_page_size(memory);
    if (page_size == 0U || (offset % page_size) != 0U ||
        (length % page_size) != 0U || offset > memory->size ||
        length > memory->size - offset)
        return ZIPC_ERR_INVALID_ARGUMENT;

    if (mprotect((uint8_t *)memory->base + offset, length, PROT_NONE) != 0)
        return ZIPC_ERR_PLATFORM;

    return ZIPC_OK;
}

zipc_status_t zipc_platform_memory_protect_rw(zipc_platform_memory_t *memory,
                                             size_t offset,
                                             size_t length)
{
    if (memory == NULL || length == 0U ||
        (memory->capabilities & ZIPC_MEM_CAP_PAGE_PROTECT) == 0U)
        return ZIPC_ERR_UNSUPPORTED_MEMORY;
    const size_t page_size = zipc_platform_memory_page_size(memory);
    if (page_size == 0U || (offset % page_size) != 0U ||
        (length % page_size) != 0U || offset > memory->size ||
        length > memory->size - offset)
        return ZIPC_ERR_INVALID_ARGUMENT;
    if (mprotect((uint8_t *)memory->base + offset, length, PROT_READ | PROT_WRITE) != 0)
        return ZIPC_ERR_PLATFORM;
    return ZIPC_OK;
}



typedef zipc_transport_spsc_ring_t zipc_linux_spsc_ring_t;

static uint64_t zipc_monotonic_time_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0U;

    return ((uint64_t)ts.tv_sec * UINT64_C(1000000000)) +
           (uint64_t)ts.tv_nsec;
}

static inline void zipc_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ volatile("yield" ::: "memory");
#else
    atomic_signal_fence(memory_order_seq_cst);
#endif
}

static size_t ring_mapping_size(uint32_t depth)
{
    return zipc_transport_spsc_ring_size(depth);
}

struct zipc_platform_transport {
    zipc_platform_transport_type_t type;
    bool created;
    char endpoint_name[108];
    char device_path[256];
    bool owns_ring;
    bool owns_eventfd;
    union {
        mqd_t mq;
        int fd;
        struct {
            zipc_linux_spsc_ring_t *ring;
            size_t mapping_size;
            int event_fd;
            uint64_t pending_events;
        } ring_eventfd;
        struct {
            zipc_linux_spsc_ring_t *ring;
            size_t mapping_size;
            uint64_t timeout_ns;
        } ring_polling;
        struct {
            zipc_message_t *mailbox;
            zipc_platform_signal_send_fn send;
            zipc_platform_signal_wait_fn wait;
            void *context;
            uint32_t timeout_ticks;
        } ipi;
        struct {
            zipc_transport_spsc_ring_t *ring;
            zipc_platform_signal_send_fn send;
            zipc_platform_signal_wait_fn wait;
            void *context;
            uint32_t timeout_ticks;
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

static zipc_status_t open_mqueue(zipc_platform_transport_t *transport,
                                const zipc_platform_transport_config_t *config)
{
    struct mq_attr attr = {
        .mq_flags = 0,
        .mq_maxmsg = 8,
        .mq_msgsize = sizeof(zipc_message_t),
        .mq_curmsgs = 0
    };
    const int flags = O_RDWR | (config->create_endpoint ? O_CREAT : 0);
    transport->handle.mq = mq_open(config->endpoint_name, flags, 0600, &attr);
    return transport->handle.mq == (mqd_t)-1 ? ZIPC_ERR_PLATFORM : ZIPC_OK;
}

static zipc_status_t open_unix_dgram(zipc_platform_transport_t *transport,
                                    const zipc_platform_transport_config_t *config)
{
    transport->handle.fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (transport->handle.fd < 0)
        return ZIPC_ERR_PLATFORM;

    if (config->create_endpoint) {
        struct sockaddr_un address;
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        snprintf(address.sun_path, sizeof(address.sun_path), "%s",
                 config->endpoint_name);
        unlink(address.sun_path);
        if (bind(transport->handle.fd,
                 (const struct sockaddr *)&address,
                 sizeof(address)) != 0)
            return ZIPC_ERR_PLATFORM;
    }
    return ZIPC_OK;
}



static zipc_status_t open_fifo(zipc_platform_transport_t *transport,
                              const zipc_platform_transport_config_t *config)
{
    if (config->create_endpoint) {
        if (mkfifo(config->endpoint_name, 0600) != 0 && errno != EEXIST)
            return ZIPC_ERR_PLATFORM;
    }

    /* O_RDWR avoids open-order deadlocks for the generic bidirectional API. */
    transport->handle.fd = open(config->endpoint_name, O_RDWR);
    return transport->handle.fd < 0 ? ZIPC_ERR_PLATFORM : ZIPC_OK;
}

static zipc_status_t open_ring_eventfd(
    zipc_platform_transport_t *transport,
    const zipc_platform_transport_config_t *config)
{
    const uint32_t depth = config->ring_depth != 0U ? config->ring_depth : 64U;
    if (depth < 2U)
        return ZIPC_ERR_INVALID_ARGUMENT;

    zipc_linux_spsc_ring_t *ring = config->platform_handle;
    if (ring == NULL) {
        const size_t size = ring_mapping_size(depth);
        ring = mmap(NULL, size, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (ring == MAP_FAILED)
            return ZIPC_ERR_PLATFORM;
        memset(ring, 0, size);
        (void)zipc_transport_spsc_ring_initialize(ring, depth);
        transport->owns_ring = true;
        transport->handle.ring_eventfd.mapping_size = size;
    } else if (ring->depth < 2U) {
        return ZIPC_ERR_INVALID_ARGUMENT;
    }

    int event_fd = -1;
    if (config->receive_handle != NULL)
        event_fd = (int)(intptr_t)config->receive_handle;
    else {
        event_fd = eventfd(0, EFD_CLOEXEC);
        if (event_fd < 0) {
            if (transport->owns_ring)
                munmap(ring, transport->handle.ring_eventfd.mapping_size);
            transport->owns_ring = false;
            return ZIPC_ERR_PLATFORM;
        }
        transport->owns_eventfd = true;
    }

    transport->handle.ring_eventfd.ring = ring;
    transport->handle.ring_eventfd.event_fd = event_fd;
    return ZIPC_OK;
}


static zipc_status_t open_ring_polling(
    zipc_platform_transport_t *transport,
    const zipc_platform_transport_config_t *config)
{
    const uint32_t depth = config->ring_depth != 0U ? config->ring_depth : 64U;
    if (depth < 2U)
        return ZIPC_ERR_INVALID_ARGUMENT;

    zipc_linux_spsc_ring_t *ring = config->platform_handle;
    if (ring == NULL) {
        const size_t size = ring_mapping_size(depth);
        ring = mmap(NULL, size, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (ring == MAP_FAILED)
            return ZIPC_ERR_PLATFORM;
        memset(ring, 0, size);
        (void)zipc_transport_spsc_ring_initialize(ring, depth);
        transport->owns_ring = true;
        transport->handle.ring_polling.mapping_size = size;
    } else if (ring->depth < 2U) {
        return ZIPC_ERR_INVALID_ARGUMENT;
    }

    transport->handle.ring_polling.ring = ring;
    transport->handle.ring_polling.timeout_ns = config->poll_timeout_ns;
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
    transport->handle.ipi.timeout_ticks = config->timeout_ticks;
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
        const uint32_t depth = config->ring_depth;
        zipc_status_t status =
            zipc_transport_spsc_ring_initialize(ring, depth);
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
    transport->handle.pl_ring_irq.timeout_ticks = config->timeout_ticks;
    return ZIPC_OK;
}

static zipc_status_t open_rpmsg(zipc_platform_transport_t *transport,
                               const zipc_platform_transport_config_t *config)
{
    if (config->device_path == NULL || config->device_path[0] == '\0')
        return ZIPC_ERR_INVALID_ARGUMENT;

    transport->handle.fd = open(config->device_path, O_RDWR);
    if (transport->handle.fd < 0)
        return ZIPC_ERR_PLATFORM;

    snprintf(transport->device_path, sizeof(transport->device_path), "%s",
             config->device_path);
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

    if ((config->type == ZIPC_TRANSPORT_POSIX_MQUEUE ||
         config->type == ZIPC_TRANSPORT_UNIX_DGRAM ||
         config->type == ZIPC_TRANSPORT_FIFO) &&
        config->endpoint_name == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    if (config->type == ZIPC_TRANSPORT_RPMSG &&
        config->device_path == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    if (config->type == ZIPC_TRANSPORT_IPI &&
        (config->shared_mailbox == NULL ||
         config->signal_send == NULL || config->signal_wait == NULL))
        return ZIPC_ERR_INVALID_ARGUMENT;

    if (config->type == ZIPC_TRANSPORT_PL_RING_IRQ &&
        (config->platform_handle == NULL ||
         config->signal_send == NULL || config->signal_wait == NULL))
        return ZIPC_ERR_INVALID_ARGUMENT;

    zipc_platform_transport_t *transport = calloc(1, sizeof(*transport));
    if (transport == NULL)
        return ZIPC_ERR_PLATFORM;
    transport->type = config->type;
    transport->created = config->create_endpoint;
    transport->handle.fd = -1;
    if (config->endpoint_name != NULL) {
        snprintf(transport->endpoint_name, sizeof(transport->endpoint_name), "%s",
                 config->endpoint_name);
    }

    zipc_status_t status;
    switch (config->type) {
    case ZIPC_TRANSPORT_POSIX_MQUEUE:
        transport->handle.mq = (mqd_t)-1;
        status = open_mqueue(transport, config);
        break;
    case ZIPC_TRANSPORT_UNIX_DGRAM:
        status = open_unix_dgram(transport, config);
        break;
    case ZIPC_TRANSPORT_FIFO:
        status = open_fifo(transport, config);
        break;
    case ZIPC_TRANSPORT_SHM_RING_EVENTFD:
        status = open_ring_eventfd(transport, config);
        break;
    case ZIPC_TRANSPORT_SHM_RING_POLLING:
        status = open_ring_polling(transport, config);
        break;
    case ZIPC_TRANSPORT_RPMSG:
        status = open_rpmsg(transport, config);
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
        status = ZIPC_ERR_INVALID_ARGUMENT;
        break;
    }

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
    if (transport == NULL || message == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    if (transport->type == ZIPC_TRANSPORT_POSIX_MQUEUE)
        return mq_send(transport->handle.mq, (const char *)message,
                       sizeof(*message), 0) == 0
             ? ZIPC_OK : ZIPC_ERR_TRANSPORT;


    if (transport->type == ZIPC_TRANSPORT_FIFO) {
        const ssize_t sent = write(transport->handle.fd, message,
                                   sizeof(*message));
        return sent == (ssize_t)sizeof(*message)
             ? ZIPC_OK : ZIPC_ERR_TRANSPORT;
    }

    if (transport->type == ZIPC_TRANSPORT_SHM_RING_EVENTFD) {
        zipc_status_t status = zipc_linux_common_ring_push(
            transport->handle.ring_eventfd.ring, message);
        if (status != ZIPC_OK)
            return status;
        const uint64_t one = 1U;
        return write(transport->handle.ring_eventfd.event_fd,
                     &one, sizeof(one)) == (ssize_t)sizeof(one)
             ? ZIPC_OK : ZIPC_ERR_TRANSPORT;
    }

    if (transport->type == ZIPC_TRANSPORT_SHM_RING_POLLING)
        return zipc_linux_common_ring_push(
            transport->handle.ring_polling.ring, message);

    if (transport->type == ZIPC_TRANSPORT_RPMSG) {
        const ssize_t sent = write(transport->handle.fd, message,
                                   sizeof(*message));
        return sent == (ssize_t)sizeof(*message)
             ? ZIPC_OK : ZIPC_ERR_TRANSPORT;
    }

    if (transport->type == ZIPC_TRANSPORT_IPI) {
        *transport->handle.ipi.mailbox = *message;
        return transport->handle.ipi.send(transport->handle.ipi.context);
    }

    if (transport->type == ZIPC_TRANSPORT_PL_RING_IRQ) {
        zipc_status_t status = zipc_linux_common_ring_push(
            transport->handle.pl_ring_irq.ring, message);
        if (status != ZIPC_OK)
            return status;
        return transport->handle.pl_ring_irq.send(
            transport->handle.pl_ring_irq.context);
    }

    if (transport->type == ZIPC_TRANSPORT_SMC ||
        transport->type == ZIPC_TRANSPORT_FFA) {
        zipc_status_t status = transport->handle.secure.call(
            transport->handle.secure.context,
            transport->handle.secure.service_id,
            message,
            &transport->handle.secure.response);
        if (status == ZIPC_OK)
            transport->handle.secure.response_pending = true;
        return status;
    }

    if (transport->type == ZIPC_TRANSPORT_UNIX_DGRAM) {
        struct sockaddr_un address;
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        snprintf(address.sun_path, sizeof(address.sun_path), "%s",
                 transport->endpoint_name);
        const ssize_t sent = sendto(transport->handle.fd, message,
                                    sizeof(*message), 0,
                                    (const struct sockaddr *)&address,
                                    sizeof(address));
        return sent == (ssize_t)sizeof(*message)
             ? ZIPC_OK : ZIPC_ERR_TRANSPORT;
    }

    return ZIPC_ERR_INVALID_ARGUMENT;
}

zipc_status_t zipc_platform_transport_receive(
    zipc_platform_transport_t *transport,
    zipc_message_t *message)
{
    if (transport == NULL || message == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    if (transport->type == ZIPC_TRANSPORT_POSIX_MQUEUE) {
        const ssize_t received = mq_receive(transport->handle.mq,
                                            (char *)message,
                                            sizeof(*message), NULL);
        return received == (ssize_t)sizeof(*message)
             ? ZIPC_OK : ZIPC_ERR_TRANSPORT;
    }


    if (transport->type == ZIPC_TRANSPORT_FIFO) {
        const ssize_t received = read(transport->handle.fd, message,
                                      sizeof(*message));
        return received == (ssize_t)sizeof(*message)
             ? ZIPC_OK : ZIPC_ERR_TRANSPORT;
    }

    if (transport->type == ZIPC_TRANSPORT_SHM_RING_EVENTFD) {
        if (transport->handle.ring_eventfd.pending_events == 0U) {
            uint64_t counter = 0U;
            if (read(transport->handle.ring_eventfd.event_fd,
                     &counter, sizeof(counter)) != (ssize_t)sizeof(counter) ||
                counter == 0U)
                return ZIPC_ERR_TRANSPORT;
            transport->handle.ring_eventfd.pending_events = counter;
        }

        zipc_status_t status = zipc_linux_common_ring_pop(
            transport->handle.ring_eventfd.ring, message);
        if (status != ZIPC_OK)
            return status;
        --transport->handle.ring_eventfd.pending_events;
        return ZIPC_OK;
    }

    if (transport->type == ZIPC_TRANSPORT_SHM_RING_POLLING) {
        const uint64_t timeout_ns =
            transport->handle.ring_polling.timeout_ns;
        uint64_t deadline_ns = 0U;

        if (timeout_ns != 0U) {
            const uint64_t now_ns = zipc_monotonic_time_ns();
            deadline_ns = UINT64_MAX - now_ns < timeout_ns
                        ? UINT64_MAX : now_ns + timeout_ns;
        }

        for (;;) {
            zipc_status_t status = zipc_linux_common_ring_pop(
                transport->handle.ring_polling.ring, message);
            if (status != ZIPC_ERR_NO_BUFFER)
                return status;

            if (timeout_ns != 0U &&
                zipc_monotonic_time_ns() >= deadline_ns)
                return ZIPC_ERR_TIMEOUT;

            zipc_cpu_relax();
        }
    }

    if (transport->type == ZIPC_TRANSPORT_RPMSG) {
        const ssize_t received = read(transport->handle.fd, message,
                                      sizeof(*message));
        return received == (ssize_t)sizeof(*message)
             ? ZIPC_OK : ZIPC_ERR_TRANSPORT;
    }

    if (transport->type == ZIPC_TRANSPORT_IPI) {
        zipc_status_t status = transport->handle.ipi.wait(
            transport->handle.ipi.context,
            transport->handle.ipi.timeout_ticks);
        if (status != ZIPC_OK)
            return status;
        *message = *transport->handle.ipi.mailbox;
        return ZIPC_OK;
    }

    if (transport->type == ZIPC_TRANSPORT_PL_RING_IRQ) {
        zipc_status_t status = transport->handle.pl_ring_irq.wait(
            transport->handle.pl_ring_irq.context,
            transport->handle.pl_ring_irq.timeout_ticks);
        if (status != ZIPC_OK)
            return status;

        return zipc_linux_common_ring_pop(
            transport->handle.pl_ring_irq.ring, message);
    }

    if (transport->type == ZIPC_TRANSPORT_SMC ||
        transport->type == ZIPC_TRANSPORT_FFA) {
        if (!transport->handle.secure.response_pending)
            return ZIPC_ERR_TRANSPORT;
        *message = transport->handle.secure.response;
        transport->handle.secure.response_pending = false;
        return ZIPC_OK;
    }

    if (transport->type == ZIPC_TRANSPORT_UNIX_DGRAM) {
        const ssize_t received = recv(transport->handle.fd, message,
                                      sizeof(*message), 0);
        return received == (ssize_t)sizeof(*message)
             ? ZIPC_OK : ZIPC_ERR_TRANSPORT;
    }

    return ZIPC_ERR_INVALID_ARGUMENT;
}

void zipc_platform_transport_close(zipc_platform_transport_t *transport)
{
    if (transport == NULL)
        return;

    if (transport->type == ZIPC_TRANSPORT_POSIX_MQUEUE) {
        if (transport->handle.mq != (mqd_t)-1)
            mq_close(transport->handle.mq);
        if (transport->created)
            mq_unlink(transport->endpoint_name);
    } else if (transport->type == ZIPC_TRANSPORT_UNIX_DGRAM) {
        if (transport->handle.fd >= 0)
            close(transport->handle.fd);
        if (transport->created)
            unlink(transport->endpoint_name);
    } else if (transport->type == ZIPC_TRANSPORT_FIFO) {
        if (transport->handle.fd >= 0)
            close(transport->handle.fd);
        if (transport->created)
            unlink(transport->endpoint_name);
    } else if (transport->type == ZIPC_TRANSPORT_SHM_RING_EVENTFD) {
        if (transport->owns_eventfd &&
            transport->handle.ring_eventfd.event_fd >= 0)
            close(transport->handle.ring_eventfd.event_fd);
        if (transport->owns_ring &&
            transport->handle.ring_eventfd.ring != NULL)
            munmap(transport->handle.ring_eventfd.ring,
                   transport->handle.ring_eventfd.mapping_size);
    } else if (transport->type == ZIPC_TRANSPORT_SHM_RING_POLLING) {
        if (transport->owns_ring &&
            transport->handle.ring_polling.ring != NULL)
            munmap(transport->handle.ring_polling.ring,
                   transport->handle.ring_polling.mapping_size);
    } else if (transport->type == ZIPC_TRANSPORT_RPMSG) {
        if (transport->handle.fd >= 0)
            close(transport->handle.fd);
    } else if (transport->type == ZIPC_TRANSPORT_IPI) {
        /* Mailbox and platform IPI resources are externally owned. */
    } else if (transport->type == ZIPC_TRANSPORT_PL_RING_IRQ) {
        /* Shared ring and IRQ/doorbell resources are externally owned. */
    }
    free(transport);
}

uint32_t zipc_platform_transport_xen_local_port(
    const zipc_platform_transport_t *transport)
{
    (void)transport;
    return 0U;
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
