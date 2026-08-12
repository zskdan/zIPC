#include <zipc/zipc.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

const char *zipc_version_string(void)
{
    return ZIPC_VERSION_STRING;
}

typedef enum {
    ZIPC_BUFFER_LOCAL_INVALID = 0,
    ZIPC_BUFFER_LOCAL_OWNED = 1
} zipc_buffer_local_state_t;

typedef struct {
    zipc_handle_t handle;
    zipc_slot_id_t slot_id;
    zipc_slot_control_t *control;
    uint8_t *slot_base;
    uint8_t *data;
    uint32_t length;
    uint32_t capacity;
    zipc_pool_t *pool;
    zipc_component_id_t owner;
    uint16_t local_state;
    uint32_t pool_id;
} zipc_buffer_impl_t;

_Static_assert(sizeof(zipc_buffer_impl_t) <= sizeof(zipc_buffer_t),
               "zipc_buffer_t opaque storage too small");

static zipc_buffer_impl_t *buffer_impl(zipc_buffer_t *buffer)
{
    return (zipc_buffer_impl_t *)(void *)buffer;
}

static const zipc_buffer_impl_t *buffer_impl_const(const zipc_buffer_t *buffer)
{
    return (const zipc_buffer_impl_t *)(const void *)buffer;
}

#define BI(b) buffer_impl((b))
#define BIC(b) buffer_impl_const((b))

static bool buffer_valid(const zipc_buffer_t *buffer)
{
    return buffer != NULL && BIC(buffer)->local_state == ZIPC_BUFFER_LOCAL_OWNED &&
           BIC(buffer)->control != NULL && BIC(buffer)->pool != NULL;
}

static void buffer_invalidate(zipc_buffer_t *buffer)
{
    if (buffer != NULL)
        memset(buffer, 0, sizeof(*buffer));
}

static uint64_t zipc_now_ns(void)
{
    return zipc_platform_time_ns();
}

static void trace_slot(zipc_slot_control_t *slot,
                       zipc_component_id_t component,
                       zipc_trace_event_t event)
{
    if (slot == NULL) return;
    const uint32_t index = slot->trace_count % ZIPC_TRACE_DEPTH;
    slot->trace[index] = (zipc_trace_entry_t){
        .timestamp_ns = zipc_now_ns(),
        .sequence = slot->transfer_sequence,
        .component_id = component,
        .event = (uint16_t)event
    };
    slot->trace_count++;
}

static void record_protocol_error(zipc_pool_t *pool,
                                  zipc_slot_control_t *owned_slot,
                                  zipc_component_id_t component)
{
    atomic_fetch_add_explicit(&pool->header->protocol_error_count, 1U,
                              memory_order_relaxed);
    if (owned_slot != NULL)
        trace_slot(owned_slot, component, ZIPC_TRACE_ERROR);
}

static uint32_t registered_epoch(const zipc_pool_t *pool, zipc_component_id_t component)
{
    if (pool == NULL || component >= ZIPC_MAX_COMPONENTS) return 0U;
    return atomic_load_explicit(&pool->header->components[component].epoch, memory_order_acquire);
}


size_t zipc_transport_spsc_ring_size(uint32_t depth)
{
    if (depth < 2U)
        return 0U;
    return sizeof(zipc_transport_spsc_ring_t) +
           ((size_t)depth * sizeof(zipc_message_t));
}

zipc_status_t zipc_transport_spsc_ring_initialize(
    zipc_transport_spsc_ring_t *ring,
    uint32_t depth)
{
    if (ring == NULL || depth < 2U)
        return ZIPC_ERR_INVALID_ARGUMENT;

    atomic_store_explicit(&ring->producer, 0U, memory_order_relaxed);
    atomic_store_explicit(&ring->consumer, 0U, memory_order_relaxed);
    ring->depth = depth;
    ring->reserved = 0U;
    atomic_thread_fence(memory_order_release);
    return ZIPC_OK;
}

static bool is_power_of_two(size_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static bool align_up_checked(size_t value, size_t alignment, size_t *result)
{
    if (result == NULL || !is_power_of_two(alignment) ||
        value > SIZE_MAX - (alignment - 1U))
        return false;

    *result = (value + alignment - 1U) & ~(alignment - 1U);
    return true;
}

static bool memory_address_aligned(const zipc_platform_memory_t *memory,
                                   size_t offset, size_t alignment)
{
    const uintptr_t base = (uintptr_t)zipc_platform_memory_base(memory);

    return base != 0U && is_power_of_two(alignment) &&
           offset <= UINTPTR_MAX - base &&
           ((base + offset) & (alignment - 1U)) == 0U;
}

static bool memory_range_fits(const zipc_platform_memory_t *memory,
                              size_t offset, size_t length)
{
    const size_t size = zipc_platform_memory_size(memory);
    const uintptr_t base = (uintptr_t)zipc_platform_memory_base(memory);

    return base != 0U && offset <= size && length <= size - offset &&
           offset <= UINTPTR_MAX - base &&
           length <= UINTPTR_MAX - (base + offset);
}

static bool pool_ranges_valid(const zipc_pool_config_t *config,
                              const zipc_platform_memory_t *payload_memory,
                              size_t control_required,
                              size_t payload_required)
{
    if (!memory_range_fits(config->control_memory, config->control_offset,
                           control_required) ||
        !memory_range_fits(payload_memory, config->payload_offset,
                           payload_required))
        return false;

    if (config->control_memory == payload_memory) {
        const size_t control_end = config->control_offset + control_required;
        const size_t payload_end = config->payload_offset + payload_required;

        if (config->control_offset < payload_end &&
            config->payload_offset < control_end)
            return false;
    }

    return true;
}

static zipc_status_t normalize_config(const zipc_pool_config_t *config,
                                     zipc_platform_memory_t **payload_memory,
                                     uint32_t *slot_stride)
{
    if (config == NULL || config->control_memory == NULL ||
        config->slot_count == 0U || config->slot_capacity == 0U ||
        !is_power_of_two(config->payload_alignment) ||
        (config->flags & ~(ZIPC_POOL_F_GUARD_PAGES | ZIPC_POOL_F_STRICT_OWNERSHIP)) != 0U)
        return ZIPC_ERR_INVALID_ARGUMENT;

    *payload_memory = config->payload_memory != NULL
                    ? config->payload_memory
                    : config->control_memory;

    if ((config->control_offset % _Alignof(zipc_pool_header_t)) != 0U ||
        !memory_address_aligned(config->control_memory,
                                config->control_offset,
                                _Alignof(zipc_pool_header_t)) ||
        (config->payload_offset % config->payload_alignment) != 0U ||
        !memory_address_aligned(*payload_memory, config->payload_offset,
                                config->payload_alignment))
        return ZIPC_ERR_INVALID_ARGUMENT;

    if ((config->flags & ZIPC_POOL_F_STRICT_OWNERSHIP) != 0U) {
        const size_t page_size = zipc_platform_memory_page_size(*payload_memory);
        const uint32_t payload_caps = zipc_platform_memory_capabilities(*payload_memory);
        if (page_size == 0U || page_size > UINT32_MAX ||
            (payload_caps & ZIPC_MEM_CAP_PAGE_PROTECT) == 0U ||
            (config->payload_offset % page_size) != 0U ||
            !memory_address_aligned(*payload_memory, config->payload_offset,
                                    page_size) ||
            ((size_t)config->slot_capacity % page_size) != 0U)
            return ZIPC_ERR_UNSUPPORTED_MEMORY;
    }

    if ((config->flags & ZIPC_POOL_F_GUARD_PAGES) != 0U) {
        const size_t page_size = zipc_platform_memory_page_size(*payload_memory);
        const uint32_t payload_caps =
            zipc_platform_memory_capabilities(*payload_memory);

        if (page_size == 0U || page_size > UINT32_MAX ||
            (payload_caps & ZIPC_MEM_CAP_PAGE_PROTECT) == 0U ||
            (config->payload_offset % page_size) != 0U ||
            !memory_address_aligned(*payload_memory, config->payload_offset,
                                    page_size) ||
            ((size_t)config->slot_capacity % page_size) != 0U ||
            config->slot_capacity > UINT32_MAX - (uint32_t)page_size)
            return ZIPC_ERR_UNSUPPORTED_MEMORY;

        const uint32_t guarded_stride =
            config->slot_capacity + (uint32_t)page_size;
        if (config->slot_stride != 0U &&
            config->slot_stride != guarded_stride)
            return ZIPC_ERR_INVALID_ARGUMENT;
        *slot_stride = guarded_stride;
    } else {
        if (config->slot_stride != 0U) {
            *slot_stride = config->slot_stride;
        } else {
            size_t aligned_capacity = 0U;
            if (!align_up_checked(config->slot_capacity,
                                  config->payload_alignment,
                                  &aligned_capacity) ||
                aligned_capacity > UINT32_MAX)
                return ZIPC_ERR_INVALID_ARGUMENT;
            *slot_stride = (uint32_t)aligned_capacity;
        }
    }

    if (*slot_stride < config->slot_capacity ||
        (*slot_stride % config->payload_alignment) != 0U)
        return ZIPC_ERR_INVALID_ARGUMENT;

    const uint32_t control_caps =
        zipc_platform_memory_capabilities(config->control_memory);
    if ((control_caps & (ZIPC_MEM_CAP_CPU_READ | ZIPC_MEM_CAP_CPU_WRITE |
                          ZIPC_MEM_CAP_ATOMIC32 | ZIPC_MEM_CAP_ATOMIC64)) !=
        (ZIPC_MEM_CAP_CPU_READ | ZIPC_MEM_CAP_CPU_WRITE |
         ZIPC_MEM_CAP_ATOMIC32 | ZIPC_MEM_CAP_ATOMIC64))
        return ZIPC_ERR_UNSUPPORTED_MEMORY;

    const uint32_t payload_caps =
        zipc_platform_memory_capabilities(*payload_memory);
    if ((payload_caps & (ZIPC_MEM_CAP_CPU_READ | ZIPC_MEM_CAP_CPU_WRITE)) !=
        (ZIPC_MEM_CAP_CPU_READ | ZIPC_MEM_CAP_CPU_WRITE))
        return ZIPC_ERR_UNSUPPORTED_MEMORY;

    return ZIPC_OK;
}

static zipc_status_t apply_guard_pages(const zipc_pool_config_t *config,
                                      zipc_platform_memory_t *payload_memory,
                                      uint32_t slot_stride)
{
    if ((config->flags & ZIPC_POOL_F_GUARD_PAGES) == 0U)
        return ZIPC_OK;

    const size_t page_size = zipc_platform_memory_page_size(payload_memory);
    if (page_size == 0U)
        return ZIPC_ERR_UNSUPPORTED_MEMORY;

    for (uint32_t i = 0; i < config->slot_count; ++i) {
        const size_t guard_offset = config->payload_offset +
            (size_t)i * slot_stride + config->slot_capacity;
        zipc_status_t status = zipc_platform_memory_protect_none(
            payload_memory, guard_offset, page_size);
        if (status != ZIPC_OK)
            return status;
    }

    return ZIPC_OK;
}

size_t zipc_pool_required_control_size(uint32_t slot_count)
{
    if (slot_count == 0U)
        return 0U;

    size_t controls_offset = 0U;
    if (!align_up_checked(sizeof(zipc_pool_header_t),
                          _Alignof(zipc_slot_control_t), &controls_offset))
        return 0U;
    if ((size_t)slot_count > (SIZE_MAX - controls_offset) /
                             sizeof(zipc_slot_control_t))
        return 0U;

    return controls_offset + (size_t)slot_count * sizeof(zipc_slot_control_t);
}

uint32_t zipc_pool_guarded_slot_stride(uint32_t slot_capacity)
{
    const size_t page_size = zipc_platform_page_size();
    if (page_size == 0U || page_size > UINT32_MAX ||
        slot_capacity == 0U || ((size_t)slot_capacity % page_size) != 0U ||
        slot_capacity > UINT32_MAX - (uint32_t)page_size)
        return 0U;

    return slot_capacity + (uint32_t)page_size;
}

size_t zipc_pool_guarded_payload_size(uint32_t slot_count,
                                     uint32_t slot_capacity)
{
    const uint32_t stride = zipc_pool_guarded_slot_stride(slot_capacity);
    if (slot_count == 0U || stride == 0U ||
        (size_t)slot_count > SIZE_MAX / stride)
        return 0U;
    return (size_t)slot_count * stride;
}

size_t zipc_pool_required_payload_size(uint32_t slot_count,
                                      uint32_t slot_capacity,
                                      uint32_t slot_stride,
                                      uint32_t payload_alignment)
{
    if (slot_count == 0U || slot_capacity == 0U ||
        !is_power_of_two(payload_alignment))
        return 0U;

    size_t aligned_capacity = 0U;
    if (slot_stride == 0U &&
        (!align_up_checked(slot_capacity, payload_alignment,
                           &aligned_capacity) ||
         aligned_capacity > UINT32_MAX))
        return 0U;

    const uint32_t stride = slot_stride != 0U
                          ? slot_stride : (uint32_t)aligned_capacity;
    if (stride < slot_capacity || (stride % payload_alignment) != 0U ||
        (size_t)slot_count > SIZE_MAX / stride)
        return 0U;

    return (size_t)slot_count * stride;
}

zipc_status_t zipc_pool_format(const zipc_pool_config_t *config)
{
    zipc_platform_memory_t *payload_memory = NULL;
    uint32_t slot_stride = 0U;
    zipc_status_t status = normalize_config(config, &payload_memory, &slot_stride);
    if (status != ZIPC_OK)
        return status;

    const size_t control_required =
        zipc_pool_required_control_size(config->slot_count);
    const size_t payload_required =
        zipc_pool_required_payload_size(config->slot_count,
                                       config->slot_capacity,
                                       slot_stride,
                                       config->payload_alignment);
    if (control_required == 0U || payload_required == 0U ||
        !pool_ranges_valid(config, payload_memory, control_required,
                           payload_required))
        return ZIPC_ERR_INVALID_ARGUMENT;

    status = apply_guard_pages(config, payload_memory, slot_stride);
    if (status != ZIPC_OK)
        return status;

    uint8_t *control_base =
        (uint8_t *)zipc_platform_memory_base(config->control_memory) +
        config->control_offset;
    memset(control_base, 0, control_required);

    zipc_pool_header_t *header = (zipc_pool_header_t *)control_base;
    header->magic = ZIPC_POOL_MAGIC;
    header->abi_version = ZIPC_POOL_ABI_VERSION;
    header->header_size = (uint16_t)sizeof(*header);
    header->slot_count = config->slot_count;
    header->slot_capacity = config->slot_capacity;
    header->slot_stride = slot_stride;
    size_t controls_offset = 0U;
    if (!align_up_checked(sizeof(*header), _Alignof(zipc_slot_control_t),
                          &controls_offset) || controls_offset > UINT32_MAX ||
        config->control_offset > SIZE_MAX - controls_offset ||
        !memory_address_aligned(config->control_memory,
                                config->control_offset + controls_offset,
                                _Alignof(zipc_slot_control_t)))
        return ZIPC_ERR_INVALID_ARGUMENT;
    header->controls_offset = (uint32_t)controls_offset;
    header->payload_alignment = config->payload_alignment;

    zipc_slot_control_t *controls = (zipc_slot_control_t *)(
        control_base + header->controls_offset);
    for (uint32_t i = 0; i < config->slot_count; ++i) {
        atomic_init(&controls[i].state, ZIPC_SLOT_FREE);
        controls[i].owner_id = ZIPC_INVALID_COMPONENT_ID;
        controls[i].next_owner_id = ZIPC_INVALID_COMPONENT_ID;
    }

    atomic_init(&header->allocation_cursor, 0U);
    atomic_init(&header->allocation_count, 0U);
    atomic_init(&header->release_count, 0U);
    atomic_init(&header->allocation_failure_count, 0U);
    atomic_init(&header->protocol_error_count, 0U);
    atomic_init(&header->recovery_count, 0U);
    for (uint32_t i = 0; i < ZIPC_MAX_COMPONENTS; ++i) {
        atomic_init(&header->components[i].epoch, 0U);
        atomic_init(&header->components[i].active, 0U);
        atomic_init(&header->components[i].last_heartbeat_ns, 0U);
        atomic_init(&header->components[i].recovered_slots, 0U);
    }
    return ZIPC_OK;
}

zipc_status_t zipc_pool_attach(zipc_pool_t *pool,
                             const zipc_pool_config_t *config)
{
    if (pool == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    zipc_platform_memory_t *payload_memory = NULL;
    uint32_t requested_stride = 0U;
    zipc_status_t status = normalize_config(config, &payload_memory,
                                           &requested_stride);
    if (status != ZIPC_OK)
        return status;

    if (config->control_offset > zipc_platform_memory_size(config->control_memory) ||
        sizeof(zipc_pool_header_t) >
            zipc_platform_memory_size(config->control_memory) -
            config->control_offset)
        return ZIPC_ERR_INVALID_POOL;

    uint8_t *control_base =
        (uint8_t *)zipc_platform_memory_base(config->control_memory) +
        config->control_offset;
    zipc_pool_header_t *header = (zipc_pool_header_t *)control_base;

    if (header->magic != ZIPC_POOL_MAGIC ||
        header->abi_version != ZIPC_POOL_ABI_VERSION ||
        header->header_size != sizeof(*header) ||
        header->slot_count != config->slot_count ||
        header->slot_capacity != config->slot_capacity ||
        header->slot_stride != requested_stride ||
        header->payload_alignment != config->payload_alignment)
        return ZIPC_ERR_INVALID_POOL;

    size_t expected_controls_offset = 0U;
    if (!align_up_checked(sizeof(*header), _Alignof(zipc_slot_control_t),
                          &expected_controls_offset) ||
        expected_controls_offset > UINT32_MAX ||
        header->controls_offset != expected_controls_offset ||
        (header->controls_offset % _Alignof(zipc_slot_control_t)) != 0U ||
        config->control_offset > SIZE_MAX - header->controls_offset ||
        !memory_address_aligned(
            config->control_memory,
            config->control_offset + header->controls_offset,
            _Alignof(zipc_slot_control_t)))
        return ZIPC_ERR_INVALID_POOL;

    const size_t control_required =
        zipc_pool_required_control_size(header->slot_count);
    const size_t payload_required =
        zipc_pool_required_payload_size(header->slot_count,
                                       header->slot_capacity,
                                       header->slot_stride,
                                       header->payload_alignment);
    if (control_required == 0U || payload_required == 0U ||
        header->controls_offset > control_required ||
        (size_t)header->slot_count >
            (control_required - header->controls_offset) /
            sizeof(zipc_slot_control_t) ||
        !pool_ranges_valid(config, payload_memory, control_required,
                           payload_required))
        return ZIPC_ERR_INVALID_POOL;

    memset(pool, 0, sizeof(*pool));
    status = apply_guard_pages(config, payload_memory, requested_stride);
    if (status != ZIPC_OK)
        return status;

    pool->control_memory = config->control_memory;
    pool->payload_memory = payload_memory;
    pool->control_offset = config->control_offset;
    pool->payload_offset = config->payload_offset;
    pool->header = header;
    pool->controls = (zipc_slot_control_t *)(control_base +
                                            header->controls_offset);
    pool->payload_base =
        (uint8_t *)zipc_platform_memory_base(payload_memory) +
        config->payload_offset;
    pool->flags = config->flags;
    pool->pool_id = config->pool_id;
    if ((pool->flags & ZIPC_POOL_F_STRICT_OWNERSHIP) != 0U) {
        for (uint32_t i = 0; i < pool->header->slot_count; ++i) {
            const size_t slot_offset = pool->payload_offset +
                (size_t)i * pool->header->slot_stride;
            status = zipc_platform_memory_protect_none(pool->payload_memory,
                                                       slot_offset,
                                                       pool->header->slot_capacity);
            if (status != ZIPC_OK)
                return status;
        }
    }
    return ZIPC_OK;
}

static zipc_status_t make_view(zipc_pool_t *pool,
                              zipc_slot_id_t slot_id,
                              zipc_handle_t handle,
                              zipc_buffer_t *buffer)
{
    zipc_slot_control_t *slot = &pool->controls[slot_id];
    if (slot->region.offset > pool->header->slot_capacity ||
        slot->region.length > pool->header->slot_capacity -
                              slot->region.offset)
        return ZIPC_ERR_REGION_OVERFLOW;

    BI(buffer)->handle = handle;
    BI(buffer)->slot_id = slot_id;
    BI(buffer)->control = slot;
    BI(buffer)->slot_base = pool->payload_base +
                        (size_t)slot_id * pool->header->slot_stride;
    BI(buffer)->data = BI(buffer)->slot_base + slot->region.offset;
    BI(buffer)->length = slot->region.length;
    BI(buffer)->capacity = pool->header->slot_capacity;
    BI(buffer)->pool = pool;
    BI(buffer)->owner = slot->owner_id;
    BI(buffer)->local_state = ZIPC_BUFFER_LOCAL_OWNED;
    BI(buffer)->pool_id = pool->pool_id;
    return ZIPC_OK;
}

zipc_status_t zipc_buffer_allocate(zipc_pool_t *pool,
                                 zipc_component_id_t allocator,
                                 zipc_buffer_t *buffer)
{
    if (pool == NULL || buffer == NULL || allocator >= ZIPC_MAX_COMPONENTS)
        return ZIPC_ERR_INVALID_ARGUMENT;

    const uint32_t count = pool->header->slot_count;
    const uint32_t start =
        atomic_load_explicit(&pool->header->allocation_cursor,
                             memory_order_relaxed) % count;

    for (uint32_t n = 0; n < count; ++n) {
        const uint32_t slot_id = (start + n) % count;
        zipc_slot_control_t *slot = &pool->controls[slot_id];
        uint32_t expected = ZIPC_SLOT_FREE;

        if (!atomic_compare_exchange_strong_explicit(
                &slot->state, &expected, ZIPC_SLOT_OWNED,
                memory_order_acquire, memory_order_relaxed))
            continue;

        slot->generation++;
        if (slot->generation == 0U)
            slot->generation++;
        slot->owner_id = allocator;
        slot->next_owner_id = ZIPC_INVALID_COMPONENT_ID;
        slot->owner_epoch = registered_epoch(pool, allocator);
        slot->hop_count = 1U;
        slot->hop_limit = ZIPC_HOP_LIMIT_UNLIMITED;
        slot->visited_mask = UINT64_C(1) << allocator;
        slot->region.offset = 0U;
        slot->region.length = 0U;
        slot->transfer_sequence = 0U;
        slot->payload_type = 0U;
        slot->flags = 0U;
        slot->error_flags = 0U;
        slot->acquired_ns = zipc_now_ns();
        slot->deadline_ns = ZIPC_DEADLINE_NONE;
        slot->trace_count = 0U;
        slot->recovery_count = 0U;
        memset(slot->trace, 0, sizeof(slot->trace));
        trace_slot(slot, allocator, ZIPC_TRACE_ALLOCATE);

        atomic_store_explicit(&pool->header->allocation_cursor,
                              (slot_id + 1U) % count,
                              memory_order_relaxed);
        atomic_fetch_add_explicit(&pool->header->allocation_count, 1U,
                                  memory_order_relaxed);

        return make_view(pool, slot_id,
                         zipc_handle_make(slot_id, slot->generation), buffer);
    }

    atomic_fetch_add_explicit(&pool->header->allocation_failure_count, 1U,
                              memory_order_relaxed);
    return ZIPC_ERR_NO_BUFFER;
}

zipc_status_t zipc_buffer_from_handle(zipc_pool_t *pool,
                                    zipc_handle_t handle,
                                    zipc_component_id_t expected_owner,
                                    zipc_buffer_t *buffer)
{
    if (pool == NULL || buffer == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    const zipc_slot_id_t slot_id = zipc_handle_slot_id(handle);
    if (slot_id >= pool->header->slot_count)
        return ZIPC_ERR_INVALID_HANDLE;

    zipc_slot_control_t *slot = &pool->controls[slot_id];
    if (slot->generation != zipc_handle_generation(handle))
        return ZIPC_ERR_STALE_HANDLE;
    if (atomic_load_explicit(&slot->state, memory_order_acquire) !=
        ZIPC_SLOT_OWNED)
        return ZIPC_ERR_INVALID_STATE;
    if (slot->owner_id != expected_owner)
        return ZIPC_ERR_NOT_OWNER;

    return make_view(pool, slot_id, handle, buffer);
}

zipc_status_t zipc_buffer_set_region(zipc_buffer_t *buffer,
                                   uint32_t offset,
                                   uint32_t length)
{
    if (buffer == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;
    if (!buffer_valid(buffer))
        return ZIPC_ERR_INVALID_BUFFER;
    if (offset > BI(buffer)->capacity || length > BI(buffer)->capacity - offset)
        return ZIPC_ERR_REGION_OVERFLOW;

    BI(buffer)->control->region.offset = offset;
    BI(buffer)->control->region.length = length;
    BI(buffer)->data = BI(buffer)->slot_base + offset;
    BI(buffer)->length = length;
    return ZIPC_OK;
}

zipc_status_t zipc_buffer_prepare_transfer(zipc_pool_t *pool,
                                         zipc_handle_t handle,
                                         zipc_component_id_t current_owner,
                                         zipc_component_id_t next_owner,
                                         zipc_message_t *message)
{
    if (pool == NULL || message == NULL ||
        current_owner >= ZIPC_MAX_COMPONENTS ||
        next_owner >= ZIPC_MAX_COMPONENTS)
        return ZIPC_ERR_INVALID_ARGUMENT;

    const zipc_slot_id_t slot_id = zipc_handle_slot_id(handle);
    if (slot_id >= pool->header->slot_count) {
        record_protocol_error(pool, NULL, current_owner);
        return ZIPC_ERR_INVALID_HANDLE;
    }

    zipc_slot_control_t *slot = &pool->controls[slot_id];
    if (slot->generation != zipc_handle_generation(handle)) {
        record_protocol_error(pool, NULL, current_owner);
        return ZIPC_ERR_STALE_HANDLE;
    }
    if (atomic_load_explicit(&slot->state, memory_order_relaxed) !=
        ZIPC_SLOT_OWNED) {
        record_protocol_error(pool, NULL, current_owner);
        return ZIPC_ERR_INVALID_STATE;
    }
    if (slot->owner_id != current_owner) {
        record_protocol_error(pool, NULL, current_owner);
        return ZIPC_ERR_NOT_OWNER;
    }
    const uint64_t now_ns = zipc_now_ns();
    if (slot->deadline_ns != ZIPC_DEADLINE_NONE && now_ns > slot->deadline_ns) {
        record_protocol_error(pool, slot, current_owner);
        return ZIPC_ERR_DEADLINE;
    }
    if (slot->hop_limit != ZIPC_HOP_LIMIT_UNLIMITED &&
        slot->hop_count >= slot->hop_limit) {
        record_protocol_error(pool, slot, current_owner);
        return ZIPC_ERR_HOP_LIMIT;
    }

    slot->next_owner_id = next_owner;
    slot->transfer_sequence++;

    message->handle = handle;
    message->pool_id = pool->pool_id;
    message->source_component = current_owner;
    message->destination_component = next_owner;
    message->transfer_sequence = slot->transfer_sequence;
    message->flags = 0U;
    trace_slot(slot, current_owner, ZIPC_TRACE_SEND);

    atomic_store_explicit(&slot->state, ZIPC_SLOT_TRANSFER,
                          memory_order_release);
    return ZIPC_OK;
}

zipc_status_t zipc_buffer_claim(zipc_pool_t *pool,
                              const zipc_message_t *message,
                              zipc_component_id_t receiver,
                              zipc_buffer_t *buffer)
{
    if (pool == NULL || message == NULL || buffer == NULL ||
        receiver >= ZIPC_MAX_COMPONENTS)
        return ZIPC_ERR_INVALID_ARGUMENT;
    if (message->pool_id != pool->pool_id) {
        record_protocol_error(pool, NULL, receiver);
        return ZIPC_ERR_INVALID_HANDLE;
    }
    if (message->destination_component != receiver) {
        record_protocol_error(pool, NULL, receiver);
        return ZIPC_ERR_INVALID_RECEIVER;
    }

    const zipc_slot_id_t slot_id = zipc_handle_slot_id(message->handle);
    if (slot_id >= pool->header->slot_count) {
        record_protocol_error(pool, NULL, receiver);
        return ZIPC_ERR_INVALID_HANDLE;
    }

    zipc_slot_control_t *slot = &pool->controls[slot_id];
    if (atomic_load_explicit(&slot->state, memory_order_acquire) !=
        ZIPC_SLOT_TRANSFER) {
        record_protocol_error(pool, NULL, receiver);
        return ZIPC_ERR_INVALID_STATE;
    }
    if (slot->generation != zipc_handle_generation(message->handle)) {
        record_protocol_error(pool, NULL, receiver);
        return ZIPC_ERR_STALE_HANDLE;
    }
    if (slot->owner_id != message->source_component ||
        slot->next_owner_id != receiver) {
        record_protocol_error(pool, NULL, receiver);
        return ZIPC_ERR_INVALID_RECEIVER;
    }
    if (slot->transfer_sequence != message->transfer_sequence) {
        record_protocol_error(pool, NULL, receiver);
        return ZIPC_ERR_SEQUENCE_MISMATCH;
    }
    const uint64_t now_ns = zipc_now_ns();
    if (slot->deadline_ns != ZIPC_DEADLINE_NONE && now_ns > slot->deadline_ns) {
        record_protocol_error(pool, NULL, receiver);
        return ZIPC_ERR_DEADLINE;
    }
    if (slot->hop_limit != ZIPC_HOP_LIMIT_UNLIMITED &&
        slot->hop_count >= slot->hop_limit) {
        record_protocol_error(pool, NULL, receiver);
        return ZIPC_ERR_HOP_LIMIT;
    }

    uint32_t expected = ZIPC_SLOT_TRANSFER;
    if (!atomic_compare_exchange_strong_explicit(
            &slot->state, &expected, ZIPC_SLOT_OWNED,
            memory_order_acquire, memory_order_relaxed)) {
        record_protocol_error(pool, NULL, receiver);
        return ZIPC_ERR_INVALID_STATE;
    }

    slot->owner_id = receiver;
    slot->next_owner_id = ZIPC_INVALID_COMPONENT_ID;
    slot->owner_epoch = registered_epoch(pool, receiver);
    slot->hop_count++;
    slot->visited_mask |= UINT64_C(1) << receiver;
    slot->acquired_ns = now_ns;
    trace_slot(slot, receiver, ZIPC_TRACE_RECEIVE);
    zipc_status_t status = make_view(pool, slot_id, message->handle, buffer);
    if (status != ZIPC_OK) {
        record_protocol_error(pool, slot, receiver);
        (void)zipc_pool_buffer_release(pool, message->handle, receiver);
    }
    return status;
}

zipc_status_t zipc_pool_buffer_release(zipc_pool_t *pool,
                                zipc_handle_t handle,
                                zipc_component_id_t current_owner)
{
    if (pool == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    const zipc_slot_id_t slot_id = zipc_handle_slot_id(handle);
    if (slot_id >= pool->header->slot_count)
        return ZIPC_ERR_INVALID_HANDLE;

    zipc_slot_control_t *slot = &pool->controls[slot_id];
    if (slot->generation != zipc_handle_generation(handle))
        return ZIPC_ERR_STALE_HANDLE;
    if (atomic_load_explicit(&slot->state, memory_order_relaxed) !=
        ZIPC_SLOT_OWNED)
        return ZIPC_ERR_INVALID_STATE;
    if (slot->owner_id != current_owner)
        return ZIPC_ERR_NOT_OWNER;

    trace_slot(slot, current_owner, ZIPC_TRACE_RELEASE);
    slot->owner_id = ZIPC_INVALID_COMPONENT_ID;
    slot->next_owner_id = ZIPC_INVALID_COMPONENT_ID;
    slot->region.offset = 0U;
    slot->region.length = 0U;
    atomic_store_explicit(&slot->state, ZIPC_SLOT_FREE, memory_order_release);
    atomic_fetch_add_explicit(&pool->header->release_count, 1U,
                              memory_order_relaxed);
    return ZIPC_OK;
}

uint64_t zipc_buffer_payload_physical_address(const zipc_pool_t *pool,
                                             zipc_slot_id_t slot_id,
                                             uint32_t offset)
{
    if (pool == NULL || slot_id >= pool->header->slot_count ||
        offset > pool->header->slot_capacity)
        return ZIPC_PHYS_ADDR_INVALID;

    const uint64_t base =
        zipc_platform_memory_physical_base(pool->payload_memory);
    if (base == ZIPC_PHYS_ADDR_INVALID)
        return ZIPC_PHYS_ADDR_INVALID;

    return base + pool->payload_offset +
           (uint64_t)slot_id * pool->header->slot_stride + offset;
}

/* Component lifecycle, recovery and tracing ------------------------------ */

zipc_status_t zipc_component_register(zipc_pool_t *pool, zipc_component_id_t component,
                                      uint32_t *epoch_out)
{
    if (pool == NULL || component >= ZIPC_MAX_COMPONENTS) return ZIPC_ERR_INVALID_ARGUMENT;
    zipc_component_status_t *entry = &pool->header->components[component];
    uint32_t current = atomic_load_explicit(&entry->epoch, memory_order_acquire);
    for (;;) {
        if (current == UINT32_MAX)
            return ZIPC_ERR_COMPONENT_STALE;
        if (atomic_compare_exchange_weak_explicit(
                &entry->epoch, &current, current + 1U,
                memory_order_acq_rel, memory_order_acquire))
            break;
    }
    const uint32_t epoch = current + 1U;
    atomic_store_explicit(&entry->last_heartbeat_ns, zipc_now_ns(), memory_order_release);
    atomic_store_explicit(&entry->active, 1U, memory_order_release);
    if (epoch_out != NULL) *epoch_out = epoch;
    return ZIPC_OK;
}

zipc_status_t zipc_component_heartbeat(zipc_pool_t *pool, zipc_component_id_t component,
                                       uint32_t epoch)
{
    if (pool == NULL || component >= ZIPC_MAX_COMPONENTS || epoch == 0U) return ZIPC_ERR_INVALID_ARGUMENT;
    zipc_component_status_t *entry = &pool->header->components[component];
    if (atomic_load_explicit(&entry->active, memory_order_acquire) == 0U ||
        atomic_load_explicit(&entry->epoch, memory_order_acquire) != epoch)
        return ZIPC_ERR_COMPONENT_STALE;
    atomic_store_explicit(&entry->last_heartbeat_ns, zipc_now_ns(), memory_order_release);
    return ZIPC_OK;
}

zipc_status_t zipc_component_unregister(zipc_pool_t *pool, zipc_component_id_t component,
                                        uint32_t epoch)
{
    if (pool == NULL || component >= ZIPC_MAX_COMPONENTS || epoch == 0U) return ZIPC_ERR_INVALID_ARGUMENT;
    zipc_component_status_t *entry = &pool->header->components[component];
    if (atomic_load_explicit(&entry->epoch, memory_order_acquire) != epoch) return ZIPC_ERR_COMPONENT_STALE;
    atomic_store_explicit(&entry->active, 0U, memory_order_release);
    return ZIPC_OK;
}

zipc_status_t zipc_component_snapshot(const zipc_pool_t *pool, zipc_component_id_t component,
                                      zipc_component_snapshot_t *snapshot)
{
    if (pool == NULL || component >= ZIPC_MAX_COMPONENTS || snapshot == NULL) return ZIPC_ERR_INVALID_ARGUMENT;
    const zipc_component_status_t *entry = &pool->header->components[component];
    snapshot->epoch = atomic_load_explicit(&entry->epoch, memory_order_acquire);
    snapshot->active = atomic_load_explicit(&entry->active, memory_order_acquire) != 0U;
    snapshot->last_heartbeat_ns = atomic_load_explicit(&entry->last_heartbeat_ns, memory_order_acquire);
    snapshot->recovered_slots = atomic_load_explicit(&entry->recovered_slots, memory_order_relaxed);
    return ZIPC_OK;
}

zipc_status_t zipc_pool_recover_owner(zipc_pool_t *pool, zipc_component_id_t component,
                                      uint32_t dead_epoch, uint64_t minimum_age_ns,
                                      zipc_recovery_result_t *result)
{
    if (pool == NULL || component >= ZIPC_MAX_COMPONENTS || dead_epoch == 0U) return ZIPC_ERR_INVALID_ARGUMENT;
    const zipc_component_status_t *entry = &pool->header->components[component];
    if (atomic_load_explicit(&entry->active, memory_order_acquire) != 0U &&
        atomic_load_explicit(&entry->epoch, memory_order_acquire) == dead_epoch)
        return ZIPC_ERR_COMPONENT_STALE;
    zipc_recovery_result_t local = {0};
    const uint64_t now = zipc_now_ns();
    for (uint32_t i = 0; i < pool->header->slot_count; ++i) {
        zipc_slot_control_t *slot = &pool->controls[i];
        uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
        if ((state != ZIPC_SLOT_OWNED && state != ZIPC_SLOT_TRANSFER) || slot->owner_id != component) continue;
        if (slot->owner_epoch != dead_epoch) { local.skipped_newer_epoch++; continue; }
        if (minimum_age_ns != 0U && now >= slot->acquired_ns && now - slot->acquired_ns < minimum_age_ns) continue;
        uint32_t expected = state;
        if (!atomic_compare_exchange_strong_explicit(&slot->state, &expected, ZIPC_SLOT_ERROR,
                                                      memory_order_acq_rel, memory_order_relaxed)) continue;
        trace_slot(slot, component, ZIPC_TRACE_RECOVER);
        slot->recovery_count++;
        slot->error_flags |= UINT32_C(1);
        slot->owner_id = ZIPC_INVALID_COMPONENT_ID;
        slot->next_owner_id = ZIPC_INVALID_COMPONENT_ID;
        slot->region = (zipc_buffer_region_t){0};
        atomic_store_explicit(&slot->state, ZIPC_SLOT_FREE, memory_order_release);
        if (state == ZIPC_SLOT_OWNED) local.recovered_owned++; else local.recovered_transfer++;
        atomic_fetch_add_explicit(&pool->header->recovery_count, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&pool->header->components[component].recovered_slots, 1U, memory_order_relaxed);
    }
    if (result != NULL) *result = local;
    return ZIPC_OK;
}

uint32_t zipc_slot_trace_copy(const zipc_slot_control_t *slot, zipc_trace_entry_t *entries,
                              uint32_t capacity)
{
    if (slot == NULL || entries == NULL || capacity == 0U) return 0U;
    uint32_t count = slot->trace_count < ZIPC_TRACE_DEPTH ? slot->trace_count : ZIPC_TRACE_DEPTH;
    if (count > capacity) count = capacity;
    const uint32_t start = slot->trace_count > ZIPC_TRACE_DEPTH ? slot->trace_count % ZIPC_TRACE_DEPTH : 0U;
    for (uint32_t i = 0; i < count; ++i) entries[i] = slot->trace[(start + i) % ZIPC_TRACE_DEPTH];
    return count;
}

/* High-level link and buffer API ----------------------------------------- */

struct zipc_link {
    zipc_pool_t *pool;
    zipc_platform_transport_t *transport;
    zipc_component_id_t local_component;
    zipc_component_id_t remote_component;
    uint32_t local_epoch;
    uint32_t hop_limit;
    uint64_t default_deadline_ns;
    uint32_t timeout_ticks;
};

typedef struct {
    char name[ZIPC_TOPOLOGY_NAME_MAX + 1U];
    zipc_pool_t *pool;
} zipc_pool_binding_t;

typedef struct {
    char name[ZIPC_TOPOLOGY_NAME_MAX + 1U];
    zipc_platform_transport_config_t config;
} zipc_transport_binding_t;

typedef struct {
    char name[ZIPC_TOPOLOGY_NAME_MAX + 1U];
    uint64_t link_id;
    char pool_name[ZIPC_TOPOLOGY_NAME_MAX + 1U];
    char transport_name[ZIPC_TOPOLOGY_NAME_MAX + 1U];
    zipc_component_id_t local_component;
    zipc_component_id_t remote_component;
    uint32_t local_epoch;
    uint32_t hop_limit;
    uint64_t default_deadline_ns;
    uint32_t timeout_ticks;
} zipc_topology_link_entry_t;

static const zipc_link_definition_t *g_legacy_topology_definitions;
static size_t g_legacy_topology_count;
static zipc_pool_binding_t g_pool_bindings[ZIPC_TOPOLOGY_MAX_POOLS];
static size_t g_pool_binding_count;
static zipc_transport_binding_t g_transport_bindings[ZIPC_TOPOLOGY_MAX_TRANSPORTS];
static size_t g_transport_binding_count;
static zipc_topology_link_entry_t g_topology_links[ZIPC_TOPOLOGY_MAX_LINKS];
static size_t g_topology_link_count;

static zipc_status_t protect_slot(zipc_pool_t *pool, zipc_slot_id_t slot_id,
                                  bool read_write)
{
    if (pool == NULL || (pool->flags & ZIPC_POOL_F_STRICT_OWNERSHIP) == 0U)
        return ZIPC_OK;
    if (slot_id >= pool->header->slot_count)
        return ZIPC_ERR_INVALID_HANDLE;
    const size_t offset = pool->payload_offset +
                          (size_t)slot_id * pool->header->slot_stride;
    return read_write
        ? zipc_platform_memory_protect_rw(pool->payload_memory, offset,
                                          pool->header->slot_capacity)
        : zipc_platform_memory_protect_none(pool->payload_memory, offset,
                                            pool->header->slot_capacity);
}

zipc_status_t zipc_link_create(zipc_link_t **link_out,
                               const zipc_link_config_t *config)
{
    if (link_out == NULL || config == NULL || config->pool == NULL ||
        config->local_component >= ZIPC_MAX_COMPONENTS ||
        config->remote_component >= ZIPC_MAX_COMPONENTS)
        return ZIPC_ERR_INVALID_ARGUMENT;

    zipc_link_t *link = zipc_platform_alloc(sizeof(*link));
    if (link == NULL)
        return ZIPC_ERR_PLATFORM;
    memset(link, 0, sizeof(*link));
    link->pool = config->pool;
    link->local_component = config->local_component;
    link->remote_component = config->remote_component;
    link->local_epoch = config->local_epoch != 0U
                      ? config->local_epoch
                      : registered_epoch(config->pool, config->local_component);
    link->hop_limit = config->hop_limit != 0U
                    ? config->hop_limit : ZIPC_HOP_LIMIT_UNLIMITED;
    link->default_deadline_ns = config->default_deadline_ns;
    link->timeout_ticks = config->timeout_ticks;

    zipc_status_t status = zipc_platform_transport_open(&link->transport,
                                                        &config->transport);
    if (status != ZIPC_OK) {
        zipc_platform_free(link);
        return status;
    }
    *link_out = link;
    return ZIPC_OK;
}

void zipc_link_destroy(zipc_link_t *link)
{
    if (link == NULL) return;
    zipc_platform_transport_close(link->transport);
    zipc_platform_free(link);
}

static bool topology_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0') return false;
    return strlen(name) <= ZIPC_TOPOLOGY_NAME_MAX;
}

static zipc_pool_t *topology_find_pool(const char *name)
{
    for (size_t i = 0; i < g_pool_binding_count; ++i)
        if (strcmp(g_pool_bindings[i].name, name) == 0)
            return g_pool_bindings[i].pool;
    return NULL;
}

static const zipc_platform_transport_config_t *topology_find_transport(const char *name)
{
    for (size_t i = 0; i < g_transport_binding_count; ++i)
        if (strcmp(g_transport_bindings[i].name, name) == 0)
            return &g_transport_bindings[i].config;
    return NULL;
}

static int topology_find_link_index(const char *name)
{
    for (size_t i = 0; i < g_topology_link_count; ++i)
        if (strcmp(g_topology_links[i].name, name) == 0)
            return (int)i;
    return -1;
}

void zipc_topology_reset(void)
{
    g_legacy_topology_definitions = NULL;
    g_legacy_topology_count = 0U;
    g_pool_binding_count = 0U;
    g_transport_binding_count = 0U;
    g_topology_link_count = 0U;
    memset(g_pool_bindings, 0, sizeof(g_pool_bindings));
    memset(g_transport_bindings, 0, sizeof(g_transport_bindings));
    memset(g_topology_links, 0, sizeof(g_topology_links));
}

zipc_status_t zipc_topology_bind_pool(const char *name, zipc_pool_t *pool)
{
    if (!topology_name_valid(name) || pool == NULL) return ZIPC_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < g_pool_binding_count; ++i) {
        if (strcmp(g_pool_bindings[i].name, name) == 0) {
            g_pool_bindings[i].pool = pool;
            return ZIPC_OK;
        }
    }
    if (g_pool_binding_count >= ZIPC_TOPOLOGY_MAX_POOLS) return ZIPC_ERR_NO_BUFFER;
    zipc_pool_binding_t *entry = &g_pool_bindings[g_pool_binding_count++];
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    entry->pool = pool;
    return ZIPC_OK;
}

zipc_status_t zipc_topology_bind_transport(
    const char *name, const zipc_platform_transport_config_t *transport)
{
    if (!topology_name_valid(name) || transport == NULL) return ZIPC_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < g_transport_binding_count; ++i) {
        if (strcmp(g_transport_bindings[i].name, name) == 0) {
            g_transport_bindings[i].config = *transport;
            return ZIPC_OK;
        }
    }
    if (g_transport_binding_count >= ZIPC_TOPOLOGY_MAX_TRANSPORTS) return ZIPC_ERR_NO_BUFFER;
    zipc_transport_binding_t *entry = &g_transport_bindings[g_transport_binding_count++];
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    entry->config = *transport;
    return ZIPC_OK;
}

zipc_status_t zipc_topology_validate(const zipc_topology_config_t *config)
{
    if (config == NULL || config->links == NULL || config->link_count == 0U ||
        config->link_count > ZIPC_TOPOLOGY_MAX_LINKS)
        return ZIPC_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < config->link_count; ++i) {
        const zipc_topology_link_config_t *l = &config->links[i];
        if (!topology_name_valid(l->name) || !topology_name_valid(l->pool_name) ||
            !topology_name_valid(l->transport_name) || l->link_id == 0U ||
            l->local_component >= ZIPC_MAX_COMPONENTS ||
            l->remote_component >= ZIPC_MAX_COMPONENTS ||
            l->local_component == l->remote_component)
            return ZIPC_ERR_INVALID_ARGUMENT;
        if (topology_find_pool(l->pool_name) == NULL ||
            topology_find_transport(l->transport_name) == NULL)
            return ZIPC_ERR_INVALID_ARGUMENT;
        for (size_t j = i + 1U; j < config->link_count; ++j) {
            if (strcmp(l->name, config->links[j].name) == 0 ||
                (l->link_id != 0U && l->link_id == config->links[j].link_id))
                return ZIPC_ERR_INVALID_ARGUMENT;
        }
    }
    return ZIPC_OK;
}

static void topology_copy_link(zipc_topology_link_entry_t *dst,
                               const zipc_topology_link_config_t *src)
{
    memset(dst, 0, sizeof(*dst));
    snprintf(dst->name, sizeof(dst->name), "%s", src->name);
    snprintf(dst->pool_name, sizeof(dst->pool_name), "%s", src->pool_name);
    snprintf(dst->transport_name, sizeof(dst->transport_name), "%s", src->transport_name);
    dst->link_id = src->link_id;
    dst->local_component = src->local_component;
    dst->remote_component = src->remote_component;
    dst->local_epoch = src->local_epoch;
    dst->hop_limit = src->hop_limit;
    dst->default_deadline_ns = src->default_deadline_ns;
    dst->timeout_ticks = src->timeout_ticks;
}

zipc_status_t zipc_topology_register_config(
    const zipc_topology_config_t *config, zipc_topology_policy_t policy)
{
    zipc_status_t status = zipc_topology_validate(config);
    if (status != ZIPC_OK) return status;
    if (policy != ZIPC_TOPOLOGY_REJECT_DUPLICATES &&
        policy != ZIPC_TOPOLOGY_EXTEND && policy != ZIPC_TOPOLOGY_OVERRIDE)
        return ZIPC_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < config->link_count; ++i) {
        const zipc_topology_link_config_t *src = &config->links[i];
        int existing = topology_find_link_index(src->name);
        if (existing >= 0) {
            if (policy != ZIPC_TOPOLOGY_OVERRIDE) return ZIPC_ERR_INVALID_ARGUMENT;
            topology_copy_link(&g_topology_links[(size_t)existing], src);
            continue;
        }
        if (src->link_id != 0U) {
            for (size_t j = 0; j < g_topology_link_count; ++j) {
                if (g_topology_links[j].link_id == src->link_id) {
                    if (policy != ZIPC_TOPOLOGY_OVERRIDE) return ZIPC_ERR_INVALID_ARGUMENT;
                    topology_copy_link(&g_topology_links[j], src);
                    existing = (int)j;
                    break;
                }
            }
            if (existing >= 0) continue;
        }
        if (g_topology_link_count >= ZIPC_TOPOLOGY_MAX_LINKS) return ZIPC_ERR_NO_BUFFER;
        topology_copy_link(&g_topology_links[g_topology_link_count++], src);
    }
    return ZIPC_OK;
}

static char *topology_trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) --end;
    *end = '\0';
    return s;
}

static bool topology_parse_u64(const char *s, uint64_t *value)
{
    if (s == NULL || value == NULL || *s == '\0') return false;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 0);
    if (end == s || *topology_trim(end) != '\0') return false;
    *value = (uint64_t)v;
    return true;
}

zipc_status_t zipc_topology_load_string(
    const char *text, size_t length, zipc_topology_policy_t policy)
{
    if (text == NULL || length == 0U) return ZIPC_ERR_INVALID_ARGUMENT;
    char *copy = zipc_platform_alloc(length + 1U);
    if (copy == NULL) return ZIPC_ERR_PLATFORM;
    memcpy(copy, text, length);
    copy[length] = '\0';

    zipc_topology_link_config_t parsed[ZIPC_TOPOLOGY_MAX_LINKS];
    char names[ZIPC_TOPOLOGY_MAX_LINKS][ZIPC_TOPOLOGY_NAME_MAX + 1U];
    char pools[ZIPC_TOPOLOGY_MAX_LINKS][ZIPC_TOPOLOGY_NAME_MAX + 1U];
    char transports[ZIPC_TOPOLOGY_MAX_LINKS][ZIPC_TOPOLOGY_NAME_MAX + 1U];
    memset(parsed, 0, sizeof(parsed));
    memset(names, 0, sizeof(names));
    memset(pools, 0, sizeof(pools));
    memset(transports, 0, sizeof(transports));
    size_t count = 0U;
    zipc_topology_link_config_t *current = NULL;

    for (char *line = strtok(copy, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        char *p = topology_trim(line);
        if (*p == '\0' || *p == '#' || *p == ';') continue;
        if (*p == '[') {
            char *close = strchr(p, ']');
            if (close == NULL) { zipc_platform_free(copy); return ZIPC_ERR_INVALID_ARGUMENT; }
            *close = '\0';
            char *section = topology_trim(p + 1);
            char *name = NULL;
            if (strncmp(section, "link.", 5U) == 0) name = topology_trim(section + 5);
            else if (strncmp(section, "link ", 5U) == 0) name = topology_trim(section + 5);
            else { current = NULL; continue; }
            if (!topology_name_valid(name) || count >= ZIPC_TOPOLOGY_MAX_LINKS) {
                zipc_platform_free(copy); return ZIPC_ERR_INVALID_ARGUMENT;
            }
            current = &parsed[count];
            snprintf(names[count], sizeof(names[count]), "%s", name);
            current->name = names[count];
            ++count;
            continue;
        }
        if (current == NULL) continue;
        char *eq = strchr(p, '=');
        if (eq == NULL) { zipc_platform_free(copy); return ZIPC_ERR_INVALID_ARGUMENT; }
        *eq = '\0';
        char *key = topology_trim(p);
        char *value = topology_trim(eq + 1);
        size_t idx = (size_t)(current - parsed);
        uint64_t n = 0U;
        if (strcmp(key, "id") == 0 || strcmp(key, "link_id") == 0) {
            if (!topology_parse_u64(value, &n)) goto parse_error;
            current->link_id = n;
        } else if (strcmp(key, "pool") == 0) {
            if (!topology_name_valid(value)) goto parse_error;
            snprintf(pools[idx], sizeof(pools[idx]), "%s", value); current->pool_name = pools[idx];
        } else if (strcmp(key, "transport") == 0) {
            if (!topology_name_valid(value)) goto parse_error;
            snprintf(transports[idx], sizeof(transports[idx]), "%s", value); current->transport_name = transports[idx];
        } else if (strcmp(key, "local") == 0 || strcmp(key, "local_component") == 0) {
            if (!topology_parse_u64(value, &n) || n > UINT16_MAX) goto parse_error;
            current->local_component = (zipc_component_id_t)n;
        } else if (strcmp(key, "remote") == 0 || strcmp(key, "remote_component") == 0) {
            if (!topology_parse_u64(value, &n) || n > UINT16_MAX) goto parse_error;
            current->remote_component = (zipc_component_id_t)n;
        } else if (strcmp(key, "local_epoch") == 0) {
            if (!topology_parse_u64(value, &n) || n > UINT32_MAX) goto parse_error;
            current->local_epoch = (uint32_t)n;
        } else if (strcmp(key, "hop_limit") == 0) {
            if (!topology_parse_u64(value, &n) || n > UINT32_MAX) goto parse_error;
            current->hop_limit = (uint32_t)n;
        } else if (strcmp(key, "default_deadline_ns") == 0) {
            if (!topology_parse_u64(value, &n)) goto parse_error;
            current->default_deadline_ns = n;
        } else if (strcmp(key, "timeout_ticks") == 0) {
            if (!topology_parse_u64(value, &n) || n > UINT32_MAX) goto parse_error;
            current->timeout_ticks = (uint32_t)n;
        } else {
            goto parse_error;
        }
    }
    if (count == 0U) { zipc_platform_free(copy); return ZIPC_ERR_INVALID_ARGUMENT; }
    {
        zipc_topology_config_t cfg = {.links = parsed, .link_count = count};
        zipc_status_t status = zipc_topology_register_config(&cfg, policy);
        zipc_platform_free(copy);
        return status;
    }
parse_error:
    zipc_platform_free(copy);
    return ZIPC_ERR_INVALID_ARGUMENT;
}

zipc_status_t zipc_topology_register(const zipc_link_definition_t *definitions,
                                     size_t count)
{
    if ((definitions == NULL && count != 0U) || count == 0U)
        return ZIPC_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < count; ++i) {
        if (definitions[i].name == NULL || definitions[i].config.pool == NULL)
            return ZIPC_ERR_INVALID_ARGUMENT;
        for (size_t j = i + 1U; j < count; ++j)
            if (definitions[j].name != NULL &&
                strcmp(definitions[i].name, definitions[j].name) == 0)
                return ZIPC_ERR_INVALID_ARGUMENT;
    }
    g_legacy_topology_definitions = definitions;
    g_legacy_topology_count = count;
    return ZIPC_OK;
}

zipc_status_t zipc_link_open(zipc_link_t **link, const char *name)
{
    if (link == NULL || name == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;
    int index = topology_find_link_index(name);
    if (index >= 0) {
        const zipc_topology_link_entry_t *entry = &g_topology_links[(size_t)index];
        zipc_pool_t *pool = topology_find_pool(entry->pool_name);
        const zipc_platform_transport_config_t *transport =
            topology_find_transport(entry->transport_name);
        if (pool == NULL || transport == NULL) return ZIPC_ERR_INVALID_ARGUMENT;
        zipc_link_config_t config = {
            .pool = pool,
            .local_component = entry->local_component,
            .remote_component = entry->remote_component,
            .local_epoch = entry->local_epoch,
            .hop_limit = entry->hop_limit,
            .default_deadline_ns = entry->default_deadline_ns,
            .timeout_ticks = entry->timeout_ticks,
            .transport = *transport,
        };
        return zipc_link_create(link, &config);
    }
    for (size_t i = 0; i < g_legacy_topology_count; ++i)
        if (strcmp(g_legacy_topology_definitions[i].name, name) == 0)
            return zipc_link_create(link, &g_legacy_topology_definitions[i].config);
    return ZIPC_ERR_INVALID_ARGUMENT;
}

static void apply_link_limits(zipc_link_t *link, zipc_buffer_t *buffer)
{
    BI(buffer)->control->owner_epoch = link->local_epoch;
    BI(buffer)->control->hop_limit = link->hop_limit;
    const uint64_t now_ns = zipc_now_ns();
    BI(buffer)->control->deadline_ns =
        link->default_deadline_ns == 0U ||
        link->default_deadline_ns >= ZIPC_DEADLINE_NONE - now_ns
        ? ZIPC_DEADLINE_NONE : now_ns + link->default_deadline_ns;
    BI(buffer)->owner = link->local_component;
}

zipc_status_t zipc_buffer_alloc_ex(zipc_link_t *link, size_t size,
                                   size_t headroom, size_t tailroom,
                                   zipc_buffer_t *buffer)
{
    if (link == NULL || buffer == NULL || size > UINT32_MAX ||
        headroom > UINT32_MAX || tailroom > UINT32_MAX)
        return ZIPC_ERR_INVALID_ARGUMENT;
    if (headroom + size < headroom || headroom + size + tailroom < size ||
        headroom + size + tailroom > link->pool->header->slot_capacity)
        return ZIPC_ERR_REGION_OVERFLOW;

    buffer_invalidate(buffer);
    zipc_status_t status = zipc_buffer_allocate(link->pool,
                                                link->local_component, buffer);
    if (status != ZIPC_OK) return status;
    status = protect_slot(link->pool, BI(buffer)->slot_id, true);
    if (status != ZIPC_OK) {
        (void)zipc_pool_buffer_release(link->pool, BI(buffer)->handle,
                                       link->local_component);
        buffer_invalidate(buffer);
        return status;
    }
    status = zipc_buffer_set_region(buffer, (uint32_t)headroom, (uint32_t)size);
    if (status != ZIPC_OK) {
        (void)zipc_pool_buffer_release(link->pool, BI(buffer)->handle,
                                       link->local_component);
        (void)protect_slot(link->pool, BI(buffer)->slot_id, false);
        buffer_invalidate(buffer);
        return status;
    }
    apply_link_limits(link, buffer);
    return ZIPC_OK;
}

zipc_status_t zipc_buffer_alloc(zipc_link_t *link, size_t size,
                                zipc_buffer_t *buffer)
{
    return zipc_buffer_alloc_ex(link, size, 0U, 0U, buffer);
}

/* compatibility: old get() allocates an empty buffer with headroom. */
zipc_status_t zipc_buffer_get(zipc_link_t *link, uint32_t headroom,
                              zipc_buffer_t *buffer)
{
    return zipc_buffer_alloc_ex(link, 0U, headroom, 0U, buffer);
}

static void rollback_transfer(zipc_link_t *link, zipc_buffer_t *buffer,
                              uint32_t previous_sequence)
{
    zipc_slot_control_t *slot = BI(buffer)->control;
    uint32_t expected = ZIPC_SLOT_TRANSFER;
    if (slot != NULL && atomic_compare_exchange_strong_explicit(
            &slot->state, &expected, ZIPC_SLOT_OWNED,
            memory_order_acquire, memory_order_relaxed)) {
        slot->next_owner_id = ZIPC_INVALID_COMPONENT_ID;
        slot->transfer_sequence = previous_sequence;
        slot->owner_id = link->local_component;
    }
}

zipc_status_t zipc_send(zipc_link_t *link, zipc_buffer_t *buffer)
{
    if (link == NULL || buffer == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;
    if (!buffer_valid(buffer))
        return ZIPC_ERR_INVALID_BUFFER;
    if (BI(buffer)->pool != link->pool || BI(buffer)->owner != link->local_component)
        return ZIPC_ERR_NOT_OWNER;

    zipc_message_t message;
    const uint32_t previous_sequence = BI(buffer)->control->transfer_sequence;
    zipc_status_t status = zipc_buffer_prepare_transfer(
        link->pool, BI(buffer)->handle, link->local_component,
        link->remote_component, &message);
    if (status != ZIPC_OK) return status;

    status = protect_slot(link->pool, BI(buffer)->slot_id, false);
    if (status != ZIPC_OK) {
        rollback_transfer(link, buffer, previous_sequence);
        return status;
    }

    status = zipc_platform_transport_send(link->transport, &message);
    if (status != ZIPC_OK) {
        rollback_transfer(link, buffer, previous_sequence);
        (void)protect_slot(link->pool, BI(buffer)->slot_id, true);
        return status;
    }

    buffer_invalidate(buffer);
    return ZIPC_OK;
}

zipc_status_t zipc_send_timeout(zipc_link_t *link, zipc_buffer_t *buffer,
                                uint32_t timeout_ticks)
{
    (void)timeout_ticks;
    return zipc_send(link, buffer);
}

zipc_status_t zipc_recv(zipc_link_t *link, zipc_buffer_t *buffer)
{
    if (link == NULL || buffer == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;
    buffer_invalidate(buffer);
    zipc_message_t message;
    zipc_status_t status = zipc_platform_transport_receive(link->transport,
                                                           &message);
    if (status != ZIPC_OK) return status;
    if (message.source_component != link->remote_component) {
        record_protocol_error(link->pool, NULL, link->local_component);
        return ZIPC_ERR_INVALID_RECEIVER;
    }
    status = zipc_buffer_claim(link->pool, &message,
                               link->local_component, buffer);
    if (status != ZIPC_OK) return status;
    BI(buffer)->owner = link->local_component;
    status = protect_slot(link->pool, BI(buffer)->slot_id, true);
    if (status != ZIPC_OK) {
        (void)zipc_pool_buffer_release(link->pool, BI(buffer)->handle,
                                       link->local_component);
        buffer_invalidate(buffer);
        return status;
    }
    return ZIPC_OK;
}

zipc_status_t zipc_recv_timeout(zipc_link_t *link, zipc_buffer_t *buffer,
                                uint32_t timeout_ticks)
{
    (void)timeout_ticks;
    return zipc_recv(link, buffer);
}

zipc_status_t zipc_receive(zipc_link_t *link, zipc_buffer_t *buffer)
{
    return zipc_recv(link, buffer);
}

zipc_status_t zipc_receive_timeout(zipc_link_t *link, zipc_buffer_t *buffer,
                                   uint32_t timeout_ticks)
{
    return zipc_recv_timeout(link, buffer, timeout_ticks);
}

zipc_status_t zipc_buffer_release(zipc_buffer_t *buffer)
{
    if (buffer == NULL) return ZIPC_ERR_INVALID_ARGUMENT;
    if (!buffer_valid(buffer)) return ZIPC_ERR_INVALID_BUFFER;
    zipc_pool_t *pool = BI(buffer)->pool;
    const zipc_slot_id_t slot_id = BI(buffer)->slot_id;
    zipc_status_t status = zipc_pool_buffer_release(pool, BI(buffer)->handle,
                                                    BI(buffer)->owner);
    if (status == ZIPC_OK) {
        (void)protect_slot(pool, slot_id, false);
        buffer_invalidate(buffer);
    }
    return status;
}

zipc_status_t zipc_buffer_put(zipc_link_t *link, zipc_buffer_t *buffer)
{
    if (link == NULL || buffer == NULL) return ZIPC_ERR_INVALID_ARGUMENT;
    if (!buffer_valid(buffer)) return ZIPC_ERR_INVALID_BUFFER;
    if (BI(buffer)->pool != link->pool || BI(buffer)->owner != link->local_component)
        return ZIPC_ERR_NOT_OWNER;
    return zipc_buffer_release(buffer);
}

zipc_status_t zipc_send_copy(zipc_link_t *link, const void *data, size_t size)
{
    if (size != 0U && data == NULL) return ZIPC_ERR_INVALID_ARGUMENT;
    zipc_buffer_t buffer;
    zipc_status_t status = zipc_buffer_alloc(link, size, &buffer);
    if (status != ZIPC_OK) return status;
    if (size != 0U) memcpy(zipc_buffer_data(&buffer), data, size);
    status = zipc_send(link, &buffer);
    if (status != ZIPC_OK && zipc_buffer_is_valid(&buffer))
        (void)zipc_buffer_release(&buffer);
    return status;
}

zipc_status_t zipc_recv_copy(zipc_link_t *link, void *data, size_t capacity,
                             size_t *actual_size)
{
    if (actual_size == NULL || (capacity != 0U && data == NULL))
        return ZIPC_ERR_INVALID_ARGUMENT;
    zipc_buffer_t buffer;
    zipc_status_t status = zipc_recv(link, &buffer);
    if (status != ZIPC_OK) return status;
    *actual_size = zipc_buffer_size(&buffer);
    if (*actual_size > capacity) {
        (void)zipc_buffer_release(&buffer);
        return ZIPC_ERR_BUFFER_TOO_SMALL;
    }
    if (*actual_size != 0U) memcpy(data, zipc_buffer_data(&buffer), *actual_size);
    return zipc_buffer_release(&buffer);
}

zipc_status_t zipc_buffer_set_limits(zipc_buffer_t *buffer, uint32_t hop_limit,
                                     uint64_t absolute_deadline_ns)
{
    if (!buffer_valid(buffer)) return ZIPC_ERR_INVALID_BUFFER;
    BI(buffer)->control->hop_limit = hop_limit != 0U
        ? hop_limit : ZIPC_HOP_LIMIT_UNLIMITED;
    BI(buffer)->control->deadline_ns = absolute_deadline_ns != 0U
        ? absolute_deadline_ns : ZIPC_DEADLINE_NONE;
    return ZIPC_OK;
}

zipc_status_t zipc_buffer_resize(zipc_buffer_t *buffer, size_t size)
{
    if (!buffer_valid(buffer)) return ZIPC_ERR_INVALID_BUFFER;
    if (size > UINT32_MAX || size > zipc_buffer_headroom(buffer) +
        zipc_buffer_tailroom(buffer) + BI(buffer)->length)
        return ZIPC_ERR_REGION_OVERFLOW;
    if (size > BI(buffer)->capacity - BI(buffer)->control->region.offset)
        return ZIPC_ERR_REGION_OVERFLOW;
    return zipc_buffer_set_region(buffer, BI(buffer)->control->region.offset,
                                  (uint32_t)size);
}

zipc_status_t zipc_buffer_append(zipc_buffer_t *buffer,
                                 const void *data, uint32_t length)
{
    if (!buffer_valid(buffer)) return ZIPC_ERR_INVALID_BUFFER;
    if (length != 0U && data == NULL) return ZIPC_ERR_INVALID_ARGUMENT;
    if (length > zipc_buffer_tailroom(buffer)) return ZIPC_ERR_REGION_OVERFLOW;
    if (length != 0U) memcpy(BI(buffer)->data + BI(buffer)->length, data, length);
    return zipc_buffer_set_region(buffer, BI(buffer)->control->region.offset,
                                  BI(buffer)->length + length);
}

zipc_status_t zipc_buffer_prepend(zipc_buffer_t *buffer,
                                  const void *data, uint32_t length)
{
    if (!buffer_valid(buffer)) return ZIPC_ERR_INVALID_BUFFER;
    if (length != 0U && data == NULL) return ZIPC_ERR_INVALID_ARGUMENT;
    if (length > zipc_buffer_headroom(buffer)) return ZIPC_ERR_REGION_OVERFLOW;
    const uint32_t offset = BI(buffer)->control->region.offset - length;
    if (length != 0U) memcpy(BI(buffer)->slot_base + offset, data, length);
    return zipc_buffer_set_region(buffer, offset, BI(buffer)->length + length);
}

zipc_status_t zipc_buffer_trim_front(zipc_buffer_t *buffer, uint32_t length)
{
    if (!buffer_valid(buffer)) return ZIPC_ERR_INVALID_BUFFER;
    if (length > BI(buffer)->length) return ZIPC_ERR_REGION_OVERFLOW;
    return zipc_buffer_set_region(buffer,
        BI(buffer)->control->region.offset + length, BI(buffer)->length - length);
}

zipc_status_t zipc_buffer_trim_back(zipc_buffer_t *buffer, uint32_t length)
{
    if (!buffer_valid(buffer)) return ZIPC_ERR_INVALID_BUFFER;
    if (length > BI(buffer)->length) return ZIPC_ERR_REGION_OVERFLOW;
    return zipc_buffer_set_region(buffer, BI(buffer)->control->region.offset,
                                  BI(buffer)->length - length);
}

void *zipc_buffer_data(zipc_buffer_t *buffer)
{
    return buffer_valid(buffer) ? BI(buffer)->data : NULL;
}

const void *zipc_buffer_const_data(const zipc_buffer_t *buffer)
{
    return buffer_valid(buffer) ? BIC(buffer)->data : NULL;
}

size_t zipc_buffer_size(const zipc_buffer_t *buffer)
{
    return buffer_valid(buffer) ? BIC(buffer)->length : 0U;
}

void *zipc_buffer_at(zipc_buffer_t *buffer, size_t offset, size_t length)
{
    if (!buffer_valid(buffer)) return NULL;

    const size_t capacity = BI(buffer)->capacity;
    if (offset > capacity || length > capacity - offset) return NULL;

    return BI(buffer)->slot_base + offset;
}

size_t zipc_buffer_capacity(const zipc_buffer_t *buffer)
{
    return buffer_valid(buffer) ? BIC(buffer)->capacity : 0U;
}

uint32_t zipc_buffer_length(const zipc_buffer_t *buffer)
{
    return (uint32_t)zipc_buffer_size(buffer);
}

uint32_t zipc_buffer_headroom(const zipc_buffer_t *buffer)
{
    return buffer_valid(buffer) ? BIC(buffer)->control->region.offset : 0U;
}

uint32_t zipc_buffer_tailroom(const zipc_buffer_t *buffer)
{
    if (!buffer_valid(buffer) || BIC(buffer)->control->region.offset > BIC(buffer)->capacity ||
        BIC(buffer)->length > BIC(buffer)->capacity - BIC(buffer)->control->region.offset)
        return 0U;
    return BIC(buffer)->capacity - BIC(buffer)->control->region.offset - BIC(buffer)->length;
}

bool zipc_buffer_is_valid(const zipc_buffer_t *buffer)
{
    return buffer_valid(buffer);
}

zipc_handle_t zipc_buffer_handle(const zipc_buffer_t *buffer)
{
    return buffer_valid(buffer) ? BIC(buffer)->handle : ZIPC_INVALID_HANDLE;
}

uint32_t zipc_buffer_pool_id(const zipc_buffer_t *buffer)
{
    return buffer_valid(buffer) ? BIC(buffer)->pool_id : UINT32_MAX;
}

uint32_t zipc_buffer_hop_count(const zipc_buffer_t *buffer)
{
    return buffer_valid(buffer) ? BIC(buffer)->control->hop_count : 0U;
}

zipc_visited_mask_t zipc_buffer_visited_mask(const zipc_buffer_t *buffer)
{
    return buffer_valid(buffer) ? BIC(buffer)->control->visited_mask : 0U;
}

uint32_t zipc_buffer_owner_epoch(const zipc_buffer_t *buffer)
{
    return buffer_valid(buffer) ? BIC(buffer)->control->owner_epoch : 0U;
}

zipc_slot_state_t zipc_buffer_slot_state(const zipc_buffer_t *buffer)
{
    if (!buffer_valid(buffer)) return ZIPC_SLOT_ERROR;
    return (zipc_slot_state_t)atomic_load_explicit(&BIC(buffer)->control->state,
                                                   memory_order_acquire);
}

uint32_t zipc_buffer_trace_copy(const zipc_buffer_t *buffer,
                                zipc_trace_entry_t *entries,
                                uint32_t capacity)
{
    return buffer_valid(buffer)
         ? zipc_slot_trace_copy(BIC(buffer)->control, entries, capacity) : 0U;
}
