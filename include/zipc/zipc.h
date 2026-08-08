#ifndef ZIPC_H
#define ZIPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZIPC_POOL_MAGIC             UINT32_C(0x5A495043)
#define ZIPC_POOL_ABI_VERSION       UINT16_C(1)
#define ZIPC_TRACE_DEPTH            8U
#define ZIPC_HOP_LIMIT_UNLIMITED    UINT32_MAX
#define ZIPC_DEADLINE_NONE          UINT64_MAX
#define ZIPC_MAX_COMPONENTS         64U
#define ZIPC_INVALID_COMPONENT_ID   UINT16_MAX
#define ZIPC_INVALID_HANDLE         UINT64_MAX
#define ZIPC_PHYS_ADDR_INVALID      UINT64_MAX

typedef uint64_t zipc_handle_t;
typedef uint32_t zipc_slot_id_t;
typedef uint32_t zipc_generation_t;
typedef uint16_t zipc_component_id_t;
typedef uint64_t zipc_visited_mask_t;

typedef enum {
    ZIPC_OK = 0,
    ZIPC_ERR_INVALID_ARGUMENT,
    ZIPC_ERR_INVALID_POOL,
    ZIPC_ERR_NO_BUFFER,
    ZIPC_ERR_INVALID_HANDLE,
    ZIPC_ERR_STALE_HANDLE,
    ZIPC_ERR_INVALID_STATE,
    ZIPC_ERR_NOT_OWNER,
    ZIPC_ERR_INVALID_RECEIVER,
    ZIPC_ERR_SEQUENCE_MISMATCH,
    ZIPC_ERR_REGION_OVERFLOW,
    ZIPC_ERR_UNSUPPORTED_MEMORY,
    ZIPC_ERR_TRANSPORT,
    ZIPC_ERR_PLATFORM,
    ZIPC_ERR_TIMEOUT,
    ZIPC_ERR_HOP_LIMIT,
    ZIPC_ERR_DEADLINE,
    ZIPC_ERR_COMPONENT_STALE,
    ZIPC_ERR_RECOVERY_REQUIRED
} zipc_status_t;

typedef enum {
    ZIPC_SLOT_FREE = 0,
    ZIPC_SLOT_OWNED,
    ZIPC_SLOT_TRANSFER,
    ZIPC_SLOT_ERROR
} zipc_slot_state_t;

typedef struct {
    uint32_t offset;
    uint32_t length;
} zipc_buffer_region_t;

typedef enum {
    ZIPC_TRACE_ALLOCATE = 1,
    ZIPC_TRACE_SEND,
    ZIPC_TRACE_RECEIVE,
    ZIPC_TRACE_RELEASE,
    ZIPC_TRACE_RECOVER,
    ZIPC_TRACE_ERROR
} zipc_trace_event_t;

typedef struct {
    uint64_t timestamp_ns;
    uint32_t sequence;
    uint16_t component_id;
    uint16_t event;
} zipc_trace_entry_t;

typedef struct {
    _Atomic uint32_t epoch;
    _Atomic uint32_t active;
    _Atomic uint64_t last_heartbeat_ns;
    _Atomic uint64_t recovered_slots;
} zipc_component_status_t;

/*
 * Only state is used to arbitrate slot ownership. The remaining slot fields
 * are protected by exclusive ownership and published through state release/
 * acquire. Component-table fields are atomic because independent components
 * update their own heartbeat and lifecycle concurrently.
 */
typedef struct {
    _Atomic uint32_t state;
    zipc_generation_t generation;
    zipc_component_id_t owner_id;
    zipc_component_id_t next_owner_id;
    uint32_t owner_epoch;
    uint32_t hop_count;
    uint32_t hop_limit;
    zipc_visited_mask_t visited_mask;
    zipc_buffer_region_t region;
    uint32_t transfer_sequence;
    uint16_t payload_type;
    uint16_t flags;
    uint32_t error_flags;
    uint64_t acquired_ns;
    uint64_t deadline_ns;
    uint32_t trace_count;
    uint32_t recovery_count;
    zipc_trace_entry_t trace[ZIPC_TRACE_DEPTH];
} zipc_slot_control_t;

/* Control memory only. Payload may be in the same or a different mapping. */
typedef struct {
    uint32_t magic;
    uint16_t abi_version;
    uint16_t header_size;
    uint32_t slot_count;
    uint32_t slot_capacity;
    uint32_t slot_stride;
    uint32_t controls_offset;
    uint32_t payload_alignment;
    _Atomic uint32_t allocation_cursor; /* advisory scan hint */
    _Atomic uint64_t allocation_count;
    _Atomic uint64_t release_count;
    _Atomic uint64_t allocation_failure_count;
    _Atomic uint64_t protocol_error_count;
    _Atomic uint64_t recovery_count;
    zipc_component_status_t components[ZIPC_MAX_COMPONENTS];
    uint32_t reserved[8];
} zipc_pool_header_t;

/* Platform memory API ---------------------------------------------------- */

typedef struct zipc_platform_memory zipc_platform_memory_t;
typedef struct zipc_platform_transport zipc_platform_transport_t;

typedef enum {
    ZIPC_SHM_POSIX = 0,
    ZIPC_SHM_HUGEPAGES,
    ZIPC_SHM_DTREVMEM_CACHED,
    ZIPC_SHM_DTREVMEM_UNCACHED,
    ZIPC_SHM_XEN_STATIC,
    ZIPC_SHM_PREALLOCATED
} zipc_platform_memory_type_t;

typedef enum {
    ZIPC_MEM_CAP_CPU_READ       = UINT32_C(1) << 0,
    ZIPC_MEM_CAP_CPU_WRITE      = UINT32_C(1) << 1,
    ZIPC_MEM_CAP_ATOMIC32       = UINT32_C(1) << 2,
    ZIPC_MEM_CAP_ATOMIC64       = UINT32_C(1) << 3,
    ZIPC_MEM_CAP_CACHEABLE      = UINT32_C(1) << 4,
    ZIPC_MEM_CAP_FIXED_PHYS     = UINT32_C(1) << 5,
    ZIPC_MEM_CAP_REMOTE_ACCESS  = UINT32_C(1) << 6,
    ZIPC_MEM_CAP_DEVICE_MEMORY  = UINT32_C(1) << 7
} zipc_memory_capability_t;

typedef struct {
    zipc_platform_memory_type_t type;
    size_t size;

    union {
        struct {
            const char *name;
            bool create;
            bool unlink_on_close;
        } posix;

        struct {
            const char *path;
            bool create;
            bool unlink_on_close;
        } hugepages;

        struct {
            const char *device_path;
            uint64_t physical_address;

            /* Explicit integrator declaration. False for PL BRAM. */
            bool supports_cpu_atomics;
            bool remote_accessible;
            bool device_memory;
        } dtrevmem;

        /*
         * Xen static shared-memory region exposed at a guest physical address.
         * device_path is normally /dev/mem or a dedicated UIO/character device.
         */
        struct {
            const char *device_path;
            uint64_t guest_physical_address;
            bool supports_cpu_atomics;
            bool cacheable;
        } xen_static;

        /*
         * Caller-owned, already mapped memory. zIPC wraps this region but
         * never allocates, maps, clears, or releases the supplied storage.
         * Suitable for static arrays, linker-script sections, OCRAM, TCM,
         * and BSP-provided memory.
         */
        struct {
            void *address;
            uint64_t physical_address;
            uint32_t capabilities;
        } preallocated;
    } backend;
} zipc_platform_memory_config_t;

zipc_status_t zipc_platform_memory_open(
    zipc_platform_memory_t **memory,
    const zipc_platform_memory_config_t *config);

void zipc_platform_memory_close(zipc_platform_memory_t *memory);
void *zipc_platform_memory_base(const zipc_platform_memory_t *memory);
size_t zipc_platform_memory_size(const zipc_platform_memory_t *memory);
uint64_t zipc_platform_memory_physical_base(const zipc_platform_memory_t *memory);
uint32_t zipc_platform_memory_capabilities(const zipc_platform_memory_t *memory);

/* Platform-neutral object allocation used by the high-level API. */
void *zipc_platform_alloc(size_t size);
void zipc_platform_free(void *pointer);
uint64_t zipc_platform_time_ns(void);

/* Pool API --------------------------------------------------------------- */

typedef struct {
    /* Required. Must support CPU read/write and 32-bit atomics. */
    zipc_platform_memory_t *control_memory;
    size_t control_offset;

    /* NULL means payload_memory == control_memory. */
    zipc_platform_memory_t *payload_memory;
    size_t payload_offset;

    uint32_t slot_count;
    uint32_t slot_capacity;
    uint32_t slot_stride;       /* 0 means align_up(slot_capacity, alignment). */
    uint32_t payload_alignment;
} zipc_pool_config_t;

typedef struct {
    zipc_platform_memory_t *control_memory;
    zipc_platform_memory_t *payload_memory;
    size_t control_offset;
    size_t payload_offset;
    zipc_pool_header_t *header;
    zipc_slot_control_t *controls;
    uint8_t *payload_base;
} zipc_pool_t;

typedef struct {
    zipc_handle_t handle;
    zipc_component_id_t source_component;
    zipc_component_id_t destination_component;
    uint32_t transfer_sequence;
    uint32_t flags;
} zipc_message_t;

typedef struct {
    zipc_handle_t handle;
    zipc_slot_id_t slot_id;
    zipc_slot_control_t *control;
    uint8_t *slot_base;
    uint8_t *data;
    uint32_t length;
    uint32_t capacity;
} zipc_buffer_t;

size_t zipc_pool_required_control_size(uint32_t slot_count);
size_t zipc_pool_required_payload_size(uint32_t slot_count,
                                      uint32_t slot_capacity,
                                      uint32_t slot_stride,
                                      uint32_t payload_alignment);

zipc_status_t zipc_pool_format(const zipc_pool_config_t *config);
zipc_status_t zipc_pool_attach(zipc_pool_t *pool,
                             const zipc_pool_config_t *config);

zipc_status_t zipc_buffer_allocate(zipc_pool_t *pool,
                                 zipc_component_id_t allocator,
                                 zipc_buffer_t *buffer);

zipc_status_t zipc_buffer_from_handle(zipc_pool_t *pool,
                                    zipc_handle_t handle,
                                    zipc_component_id_t expected_owner,
                                    zipc_buffer_t *buffer);

zipc_status_t zipc_buffer_set_region(zipc_buffer_t *buffer,
                                   uint32_t offset,
                                   uint32_t length);

zipc_status_t zipc_buffer_prepare_transfer(zipc_pool_t *pool,
                                         zipc_handle_t handle,
                                         zipc_component_id_t current_owner,
                                         zipc_component_id_t next_owner,
                                         zipc_message_t *message);

zipc_status_t zipc_buffer_claim(zipc_pool_t *pool,
                              const zipc_message_t *message,
                              zipc_component_id_t receiver,
                              zipc_buffer_t *buffer);

zipc_status_t zipc_buffer_release(zipc_pool_t *pool,
                                zipc_handle_t handle,
                                zipc_component_id_t current_owner);

uint64_t zipc_buffer_payload_physical_address(const zipc_pool_t *pool,
                                             zipc_slot_id_t slot_id,
                                             uint32_t offset);

/* Component lifecycle, recovery and tracing -------------------------------- */

typedef struct {
    uint32_t epoch;
    bool active;
    uint64_t last_heartbeat_ns;
    uint64_t recovered_slots;
} zipc_component_snapshot_t;

typedef struct {
    uint32_t recovered_owned;
    uint32_t recovered_transfer;
    uint32_t skipped_newer_epoch;
} zipc_recovery_result_t;

zipc_status_t zipc_component_register(zipc_pool_t *pool,
                                      zipc_component_id_t component,
                                      uint32_t *epoch_out);
zipc_status_t zipc_component_heartbeat(zipc_pool_t *pool,
                                       zipc_component_id_t component,
                                       uint32_t epoch);
zipc_status_t zipc_component_unregister(zipc_pool_t *pool,
                                        zipc_component_id_t component,
                                        uint32_t epoch);
zipc_status_t zipc_component_snapshot(const zipc_pool_t *pool,
                                      zipc_component_id_t component,
                                      zipc_component_snapshot_t *snapshot);
zipc_status_t zipc_pool_recover_owner(zipc_pool_t *pool,
                                      zipc_component_id_t component,
                                      uint32_t dead_epoch,
                                      uint64_t minimum_age_ns,
                                      zipc_recovery_result_t *result);
uint32_t zipc_slot_trace_copy(const zipc_slot_control_t *slot,
                              zipc_trace_entry_t *entries,
                              uint32_t capacity);

/* Transport API ---------------------------------------------------------- */

/* Backend-role model ----------------------------------------------------- */

/*
 * Every zIPC link depends on three logical backend roles:
 *
 *   1. payload backend: stores or carries the payload bytes;
 *   2. descriptor backend: queues/transfers zipc_message_t descriptors;
 *   3. event backend: notifies or wakes the peer.
 *
 * Several v0.1.x platform adapters bundle descriptor and event roles into a
 * single transport implementation. For example, SHM_RING_EVENTFD combines a
 * shared descriptor ring with eventfd notification. The role enums below make
 * that composition explicit without breaking the existing transport API.
 */
typedef zipc_platform_memory_type_t zipc_payload_backend_type_t;

typedef enum {
    ZIPC_DESCRIPTOR_BACKEND_NONE = 0,
    ZIPC_DESCRIPTOR_BACKEND_MESSAGE_QUEUE,
    ZIPC_DESCRIPTOR_BACKEND_UNIX_DGRAM,
    ZIPC_DESCRIPTOR_BACKEND_FIFO,
    ZIPC_DESCRIPTOR_BACKEND_SHM_RING,
    ZIPC_DESCRIPTOR_BACKEND_RPMSG,
    ZIPC_DESCRIPTOR_BACKEND_SHARED_MAILBOX,
    ZIPC_DESCRIPTOR_BACKEND_XEN_RING,
    ZIPC_DESCRIPTOR_BACKEND_PL_RING,
    ZIPC_DESCRIPTOR_BACKEND_SECURE_CALL
} zipc_descriptor_backend_type_t;

typedef enum {
    ZIPC_EVENT_BACKEND_NONE = 0,
    ZIPC_EVENT_BACKEND_INTEGRATED,
    ZIPC_EVENT_BACKEND_EVENTFD,
    ZIPC_EVENT_BACKEND_SHM_POLLING,
    ZIPC_EVENT_BACKEND_TASK_NOTIFICATION,
    ZIPC_EVENT_BACKEND_IPI,
    ZIPC_EVENT_BACKEND_XEN_EVENT_CHANNEL,
    ZIPC_EVENT_BACKEND_IRQ,
    ZIPC_EVENT_BACKEND_SYNCHRONOUS_CALL
} zipc_event_backend_type_t;

typedef struct {
    zipc_payload_backend_type_t payload;
    zipc_descriptor_backend_type_t descriptor;
    zipc_event_backend_type_t event;
} zipc_backend_roles_t;

typedef enum {
    ZIPC_TRANSPORT_MESSAGE_QUEUE = 0,
    ZIPC_TRANSPORT_POSIX_MQUEUE = ZIPC_TRANSPORT_MESSAGE_QUEUE,
    ZIPC_TRANSPORT_UNIX_DGRAM,
    ZIPC_TRANSPORT_FIFO,
    ZIPC_TRANSPORT_SHM_RING_EVENTFD,
    ZIPC_TRANSPORT_SHM_RING_POLLING,
    ZIPC_TRANSPORT_RPMSG,
    ZIPC_TRANSPORT_TASK_NOTIFICATION,
    ZIPC_TRANSPORT_IPI,
    ZIPC_TRANSPORT_XEN_RING_EVTCHN,
    ZIPC_TRANSPORT_PL_RING_IRQ,
    ZIPC_TRANSPORT_SMC,
    ZIPC_TRANSPORT_FFA
} zipc_platform_transport_type_t;

/* Classify a bundled v0.1.x transport into descriptor and event roles. */
zipc_status_t zipc_transport_backend_roles(
    zipc_platform_transport_type_t transport,
    zipc_descriptor_backend_type_t *descriptor,
    zipc_event_backend_type_t *event);

const char *zipc_payload_backend_name(zipc_payload_backend_type_t backend);
const char *zipc_descriptor_backend_name(zipc_descriptor_backend_type_t backend);
const char *zipc_event_backend_name(zipc_event_backend_type_t backend);

/*
 * Platform signal callbacks used by doorbell-style transports such as IPI.
 * The callback implementation owns interrupt triggering, acknowledgement,
 * cache maintenance, and any architecture-specific barriers.
 */
typedef zipc_status_t (*zipc_platform_signal_send_fn)(void *context);
typedef zipc_status_t (*zipc_platform_signal_wait_fn)(void *context,
                                                    uint32_t timeout_ticks);

/*
 * Secure-world invocation hook.
 *
 * Linux: normally implemented by a kernel-mediated character-device/ioctl
 * wrapper around the FF-A or SMC kernel interface.
 * FreeRTOS/bare metal: may issue an architecture-specific SMC directly.
 *
 * The callback executes one synchronous request and returns one response.
 */
typedef zipc_status_t (*zipc_platform_secure_call_fn)(
    void *context,
    uint32_t service_id,
    const zipc_message_t *request,
    zipc_message_t *response);

/*
 * Shared single-producer/single-consumer descriptor ring.
 *
 * For ZIPC_TRANSPORT_PL_RING_IRQ, place this ring in control memory that is
 * safely accessible by the PS and PL. The slot payload may independently be
 * located in reserved DDR or PL BRAM through zipc_pool_config_t.
 */
typedef struct {
    _Atomic uint32_t producer;
    _Atomic uint32_t consumer;
    uint32_t depth;
    uint32_t reserved;
    zipc_message_t entries[];
} zipc_transport_spsc_ring_t;

size_t zipc_transport_spsc_ring_size(uint32_t depth);
zipc_status_t zipc_transport_spsc_ring_initialize(
    zipc_transport_spsc_ring_t *ring,
    uint32_t depth);

typedef struct {
    zipc_platform_transport_type_t type;

    /* POSIX mqueue name, Unix-domain socket path, or FIFO path. */
    const char *endpoint_name;
    bool create_endpoint;

    /*
     * RPMsg endpoint character device, for example /dev/rpmsg0.
     * The endpoint must already exist; create_endpoint is ignored for RPMsg.
     */
    const char *device_path;

    /*
     * Platform-native transport object.
     *
     * FreeRTOS queue backend:
     *     Existing QueueHandle_t when create_endpoint is false.
     *
     * FreeRTOS RPMsg backend:
     *     Existing struct rpmsg_endpoint *.
     *
     * Ignored by the current Linux backends.
     */
    void *platform_handle;

    /*
     * Optional platform-native receive object. For FreeRTOS RPMsg this is a
     * QueueHandle_t populated by the RPMsg endpoint callback.
     */
    void *receive_handle;

    /* FreeRTOS queue creation depth; 0 selects the platform default. */
    uint32_t queue_length;

    /*
     * SHM_RING_EVENTFD / SHM_RING_POLLING backends (Linux):
     *   ring_depth      = number of zipc_message_t entries; 0 selects 64.
     *   platform_handle = optional externally shared ring mapping.
     *   receive_handle  = optional eventfd encoded as (void *)(intptr_t)fd
     *                     (EVENTFD variant only).
     *
     * When omitted, the backend creates an anonymous MAP_SHARED ring and an
     * eventfd for the EVENTFD variant. Such resources must be inherited with
     * fork() or explicitly passed to another process. The POLLING variant
     * watches the shared producer index directly and does not issue a wakeup.
     * The ring is single-producer/single-consumer.
     */
    uint32_t ring_depth;

    /*
     * SHM_RING_POLLING receive timeout in nanoseconds.
     * A value of 0 means busy-spin forever until a descriptor is available.
     */
    uint64_t poll_timeout_ns;

    /* FreeRTOS send/receive timeout in ticks. */
    uint32_t timeout_ticks;

    /*
     * TASK_NOTIFICATION backend:
     *   platform_handle    = destination TaskHandle_t
     *   shared_mailbox     = one shared zipc_message_t mailbox
     *   notification_index = indexed notification slot
     *
     * This backend is a single-entry SPSC mailbox. The sender must not
     * overwrite the mailbox while a notification is pending.
     */
    zipc_message_t *shared_mailbox;
    uint32_t notification_index;

    /*
     * IPI backend:
     *   shared_mailbox = descriptor mailbox visible to both processors
     *   signal_send    = trigger the remote IPI/doorbell
     *   signal_wait    = block until/acknowledge the local IPI
     *   signal_context = platform-specific IPI channel context
     */
    zipc_platform_signal_send_fn signal_send;
    zipc_platform_signal_wait_fn signal_wait;
    void *signal_context;

    /*
     * SMC / FF-A backends:
     *   secure_call      = synchronous platform-specific invocation
     *   secure_context   = driver, conduit, or secure-service context
     *   secure_service_id= SMC function ID or FF-A service/endpoint selector
     *
     * zipc_platform_transport_send() performs the secure call and stores the
     * returned zipc_message_t. zipc_platform_transport_receive() retrieves it.
     * This is a single-outstanding synchronous transport.
     */
    zipc_platform_secure_call_fn secure_call;
    void *secure_context;
    uint32_t secure_service_id;

    /*
     * XEN_RING_EVTCHN backend:
     *   platform_handle = shared SPSC ring address in ZIPC_SHM_XEN_STATIC memory
     *   ring_depth      = ring entry count; must match in both guests
     *   create_endpoint = initialize ring indices/depth in this guest
     *   device_path     = event-channel device, normally /dev/xen/evtchn
     *
     * Event-channel binding:
     *   xen_static_port != 0: bind that preallocated local event-channel port.
     *   otherwise, xen_remote_port != 0: bind interdomain to remote port.
     *   otherwise allocate an unbound port toward xen_remote_domid.
     *
     * zipc_platform_transport_xen_local_port() returns the resulting local port
     * so it can be communicated to the peer during platform setup.
     */
    uint32_t xen_remote_domid;
    uint32_t xen_remote_port;
    uint32_t xen_static_port;

    /*
     * PL_RING_IRQ backend:
     *   platform_handle = zipc_transport_spsc_ring_t shared with the PL
     *   ring_depth      = ring depth; must match the hardware implementation
     *   create_endpoint = initialize producer/consumer/depth on the PS side
     *   signal_send     = write the PS->PL AXI doorbell register
     *   signal_wait     = wait for and acknowledge the PL->PS IRQ
     *   signal_context  = UIO/driver/BSP-specific register and IRQ context
     *
     * Use one transport/ring per direction. Ring/control metadata should live
     * in atomic-capable reserved DDR. Slot payloads may live in the same DDR
     * or in uncached PL BRAM through split pool memory.
     */
} zipc_platform_transport_config_t;

zipc_status_t zipc_platform_transport_open(
    zipc_platform_transport_t **transport,
    const zipc_platform_transport_config_t *config);

zipc_status_t zipc_platform_transport_send(
    zipc_platform_transport_t *transport,
    const zipc_message_t *message);

zipc_status_t zipc_platform_transport_receive(
    zipc_platform_transport_t *transport,
    zipc_message_t *message);

void zipc_platform_transport_close(zipc_platform_transport_t *transport);

/* Returns 0 for non-Xen transports or when no port has been bound. */
uint32_t zipc_platform_transport_xen_local_port(
    const zipc_platform_transport_t *transport);

/* High-level link and buffer API ----------------------------------------- */

typedef struct zipc_link zipc_link_t;

typedef struct {
    zipc_pool_t *pool;
    zipc_component_id_t local_component;
    zipc_component_id_t remote_component;
    uint32_t local_epoch;       /* 0: use currently registered epoch. */
    uint32_t hop_limit;         /* 0: unlimited. */
    uint64_t default_deadline_ns; /* 0: no deadline. Relative duration. */
    uint32_t timeout_ticks;     /* Backend-specific blocking timeout. */
    zipc_platform_transport_config_t transport;
} zipc_link_config_t;

/* Creates and owns one platform transport instance. */
zipc_status_t zipc_link_create(zipc_link_t **link,
                               const zipc_link_config_t *config);
void zipc_link_destroy(zipc_link_t *link);

/* Allocate a slot owned by link->local_component with optional headroom. */
zipc_status_t zipc_buffer_get(zipc_link_t *link,
                              uint32_t headroom,
                              zipc_buffer_t *buffer);

/* Transfer an owned buffer to link->remote_component. */
zipc_status_t zipc_send(zipc_link_t *link, zipc_buffer_t *buffer);
zipc_status_t zipc_send_timeout(zipc_link_t *link, zipc_buffer_t *buffer,
                                uint32_t timeout_ticks);

/* Receive and claim the next buffer for link->local_component. */
zipc_status_t zipc_receive(zipc_link_t *link, zipc_buffer_t *buffer);
zipc_status_t zipc_receive_timeout(zipc_link_t *link, zipc_buffer_t *buffer,
                                   uint32_t timeout_ticks);

/* Release an owned buffer back to the pool. */
zipc_status_t zipc_buffer_put(zipc_link_t *link, zipc_buffer_t *buffer);
zipc_status_t zipc_buffer_set_limits(zipc_buffer_t *buffer,
                                     uint32_t hop_limit,
                                     uint64_t absolute_deadline_ns);

zipc_status_t zipc_buffer_append(zipc_buffer_t *buffer,
                                 const void *data,
                                 uint32_t length);
zipc_status_t zipc_buffer_prepend(zipc_buffer_t *buffer,
                                  const void *data,
                                  uint32_t length);
zipc_status_t zipc_buffer_trim_front(zipc_buffer_t *buffer, uint32_t length);
zipc_status_t zipc_buffer_trim_back(zipc_buffer_t *buffer, uint32_t length);

void *zipc_buffer_data(zipc_buffer_t *buffer);
const void *zipc_buffer_const_data(const zipc_buffer_t *buffer);
uint32_t zipc_buffer_length(const zipc_buffer_t *buffer);
uint32_t zipc_buffer_headroom(const zipc_buffer_t *buffer);
uint32_t zipc_buffer_tailroom(const zipc_buffer_t *buffer);

static inline zipc_handle_t zipc_handle_make(zipc_slot_id_t slot_id,
                                           zipc_generation_t generation)
{
    return ((uint64_t)generation << 32) | slot_id;
}

static inline zipc_slot_id_t zipc_handle_slot_id(zipc_handle_t handle)
{
    return (zipc_slot_id_t)(handle & UINT32_MAX);
}

static inline zipc_generation_t zipc_handle_generation(zipc_handle_t handle)
{
    return (zipc_generation_t)(handle >> 32);
}

#ifdef __cplusplus
}
#endif

#endif /* ZIPC_H */
