#include <zipc/zipc.h>

#include <string.h>

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

static size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static zipc_status_t normalize_config(const zipc_pool_config_t *config,
                                     zipc_platform_memory_t **payload_memory,
                                     uint32_t *slot_stride)
{
    if (config == NULL || config->control_memory == NULL ||
        config->slot_count == 0U || config->slot_capacity == 0U ||
        !is_power_of_two(config->payload_alignment))
        return ZIPC_ERR_INVALID_ARGUMENT;

    *payload_memory = config->payload_memory != NULL
                    ? config->payload_memory
                    : config->control_memory;

    *slot_stride = config->slot_stride != 0U
                 ? config->slot_stride
                 : (uint32_t)align_up(config->slot_capacity,
                                      config->payload_alignment);

    if (*slot_stride < config->slot_capacity ||
        (*slot_stride % config->payload_alignment) != 0U)
        return ZIPC_ERR_INVALID_ARGUMENT;

    const uint32_t control_caps =
        zipc_platform_memory_capabilities(config->control_memory);
    if ((control_caps & (ZIPC_MEM_CAP_CPU_READ | ZIPC_MEM_CAP_CPU_WRITE |
                         ZIPC_MEM_CAP_ATOMIC32)) !=
        (ZIPC_MEM_CAP_CPU_READ | ZIPC_MEM_CAP_CPU_WRITE |
         ZIPC_MEM_CAP_ATOMIC32))
        return ZIPC_ERR_UNSUPPORTED_MEMORY;

    const uint32_t payload_caps =
        zipc_platform_memory_capabilities(*payload_memory);
    if ((payload_caps & (ZIPC_MEM_CAP_CPU_READ | ZIPC_MEM_CAP_CPU_WRITE)) !=
        (ZIPC_MEM_CAP_CPU_READ | ZIPC_MEM_CAP_CPU_WRITE))
        return ZIPC_ERR_UNSUPPORTED_MEMORY;

    return ZIPC_OK;
}

size_t zipc_pool_required_control_size(uint32_t slot_count)
{
    if (slot_count == 0U)
        return 0U;

    const size_t controls_offset =
        align_up(sizeof(zipc_pool_header_t), _Alignof(zipc_slot_control_t));
    if ((size_t)slot_count > (SIZE_MAX - controls_offset) /
                             sizeof(zipc_slot_control_t))
        return 0U;

    return controls_offset + (size_t)slot_count * sizeof(zipc_slot_control_t);
}

size_t zipc_pool_required_payload_size(uint32_t slot_count,
                                      uint32_t slot_capacity,
                                      uint32_t slot_stride,
                                      uint32_t payload_alignment)
{
    if (slot_count == 0U || slot_capacity == 0U ||
        !is_power_of_two(payload_alignment))
        return 0U;

    uint32_t stride = slot_stride != 0U
                    ? slot_stride
                    : (uint32_t)align_up(slot_capacity, payload_alignment);
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
        config->control_offset > zipc_platform_memory_size(config->control_memory) ||
        control_required > zipc_platform_memory_size(config->control_memory) -
                           config->control_offset ||
        config->payload_offset > zipc_platform_memory_size(payload_memory) ||
        payload_required > zipc_platform_memory_size(payload_memory) -
                           config->payload_offset)
        return ZIPC_ERR_INVALID_ARGUMENT;

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
    header->controls_offset = (uint32_t)align_up(sizeof(*header),
                                                 _Alignof(zipc_slot_control_t));
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
        header->slot_count != config->slot_count ||
        header->slot_capacity != config->slot_capacity ||
        header->slot_stride != requested_stride ||
        header->payload_alignment != config->payload_alignment)
        return ZIPC_ERR_INVALID_POOL;

    const size_t control_required =
        zipc_pool_required_control_size(header->slot_count);
    const size_t payload_required =
        zipc_pool_required_payload_size(header->slot_count,
                                       header->slot_capacity,
                                       header->slot_stride,
                                       header->payload_alignment);
    if (control_required == 0U || payload_required == 0U ||
        control_required > zipc_platform_memory_size(config->control_memory) -
                           config->control_offset ||
        config->payload_offset > zipc_platform_memory_size(payload_memory) ||
        payload_required > zipc_platform_memory_size(payload_memory) -
                           config->payload_offset)
        return ZIPC_ERR_INVALID_POOL;

    memset(pool, 0, sizeof(*pool));
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

    buffer->handle = handle;
    buffer->slot_id = slot_id;
    buffer->control = slot;
    buffer->slot_base = pool->payload_base +
                        (size_t)slot_id * pool->header->slot_stride;
    buffer->data = buffer->slot_base + slot->region.offset;
    buffer->length = slot->region.length;
    buffer->capacity = pool->header->slot_capacity;
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
    if (buffer == NULL || buffer->control == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;
    if (offset > buffer->capacity || length > buffer->capacity - offset)
        return ZIPC_ERR_REGION_OVERFLOW;

    buffer->control->region.offset = offset;
    buffer->control->region.length = length;
    buffer->data = buffer->slot_base + offset;
    buffer->length = length;
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
    const uint64_t now_ns = zipc_now_ns();
    if (slot->deadline_ns != ZIPC_DEADLINE_NONE && now_ns > slot->deadline_ns)
        return ZIPC_ERR_DEADLINE;
    if (slot->hop_limit != ZIPC_HOP_LIMIT_UNLIMITED && slot->hop_count >= slot->hop_limit)
        return ZIPC_ERR_HOP_LIMIT;

    slot->next_owner_id = next_owner;
    slot->transfer_sequence++;

    message->handle = handle;
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
    if (message->destination_component != receiver)
        return ZIPC_ERR_INVALID_RECEIVER;

    const zipc_slot_id_t slot_id = zipc_handle_slot_id(message->handle);
    if (slot_id >= pool->header->slot_count)
        return ZIPC_ERR_INVALID_HANDLE;

    zipc_slot_control_t *slot = &pool->controls[slot_id];
    if (slot->generation != zipc_handle_generation(message->handle))
        return ZIPC_ERR_STALE_HANDLE;
    if (slot->owner_id != message->source_component ||
        slot->next_owner_id != receiver)
        return ZIPC_ERR_INVALID_RECEIVER;
    if (slot->transfer_sequence != message->transfer_sequence)
        return ZIPC_ERR_SEQUENCE_MISMATCH;
    const uint64_t now_ns = zipc_now_ns();
    if (slot->deadline_ns != ZIPC_DEADLINE_NONE && now_ns > slot->deadline_ns)
        return ZIPC_ERR_DEADLINE;
    if (slot->hop_limit != ZIPC_HOP_LIMIT_UNLIMITED && slot->hop_count >= slot->hop_limit)
        return ZIPC_ERR_HOP_LIMIT;

    uint32_t expected = ZIPC_SLOT_TRANSFER;
    if (!atomic_compare_exchange_strong_explicit(
            &slot->state, &expected, ZIPC_SLOT_OWNED,
            memory_order_acquire, memory_order_relaxed))
        return ZIPC_ERR_INVALID_STATE;

    slot->owner_id = receiver;
    slot->next_owner_id = ZIPC_INVALID_COMPONENT_ID;
    slot->owner_epoch = registered_epoch(pool, receiver);
    slot->hop_count++;
    slot->visited_mask |= UINT64_C(1) << receiver;
    slot->acquired_ns = now_ns;
    trace_slot(slot, receiver, ZIPC_TRACE_RECEIVE);
    return make_view(pool, slot_id, message->handle, buffer);
}

zipc_status_t zipc_buffer_release(zipc_pool_t *pool,
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
    const uint32_t epoch = atomic_fetch_add_explicit(&entry->epoch, 1U, memory_order_acq_rel) + 1U;
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
    link->local_epoch = config->local_epoch != 0U ? config->local_epoch : registered_epoch(config->pool, config->local_component);
    link->hop_limit = config->hop_limit != 0U ? config->hop_limit : ZIPC_HOP_LIMIT_UNLIMITED;
    link->default_deadline_ns = config->default_deadline_ns;
    link->timeout_ticks = config->timeout_ticks;

    zipc_status_t status = zipc_platform_transport_open(
        &link->transport, &config->transport);
    if (status != ZIPC_OK) {
        zipc_platform_free(link);
        return status;
    }

    *link_out = link;
    return ZIPC_OK;
}

void zipc_link_destroy(zipc_link_t *link)
{
    if (link == NULL)
        return;
    zipc_platform_transport_close(link->transport);
    zipc_platform_free(link);
}

zipc_status_t zipc_buffer_get(zipc_link_t *link,
                              uint32_t headroom,
                              zipc_buffer_t *buffer)
{
    if (link == NULL || buffer == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    zipc_status_t status = zipc_buffer_allocate(
        link->pool, link->local_component, buffer);
    if (status != ZIPC_OK)
        return status;

    if (headroom > buffer->capacity) {
        (void)zipc_buffer_release(link->pool, buffer->handle,
                                  link->local_component);
        memset(buffer, 0, sizeof(*buffer));
        return ZIPC_ERR_REGION_OVERFLOW;
    }

    status = zipc_buffer_set_region(buffer, headroom, 0U);
    if (status != ZIPC_OK) return status;
    buffer->control->owner_epoch = link->local_epoch;
    buffer->control->hop_limit = link->hop_limit;
    buffer->control->deadline_ns = link->default_deadline_ns != 0U
        ? zipc_now_ns() + link->default_deadline_ns : ZIPC_DEADLINE_NONE;
    return ZIPC_OK;
}

static void rollback_transfer(zipc_link_t *link,
                              zipc_buffer_t *buffer,
                              uint32_t previous_sequence)
{
    zipc_slot_control_t *slot = buffer->control;
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
    if (link == NULL || buffer == NULL || buffer->control == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    zipc_message_t message;
    const uint32_t previous_sequence = buffer->control->transfer_sequence;
    zipc_status_t status = zipc_buffer_prepare_transfer(
        link->pool, buffer->handle, link->local_component,
        link->remote_component, &message);
    if (status != ZIPC_OK)
        return status;

    status = zipc_platform_transport_send(link->transport, &message);
    if (status != ZIPC_OK) {
        rollback_transfer(link, buffer, previous_sequence);
        return status;
    }

    memset(buffer, 0, sizeof(*buffer));
    return ZIPC_OK;
}

zipc_status_t zipc_send_timeout(zipc_link_t *link, zipc_buffer_t *buffer, uint32_t timeout_ticks)
{
    (void)timeout_ticks;
    return zipc_send(link, buffer);
}

zipc_status_t zipc_receive(zipc_link_t *link, zipc_buffer_t *buffer)
{
    if (link == NULL || buffer == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    zipc_message_t message;
    zipc_status_t status = zipc_platform_transport_receive(
        link->transport, &message);
    if (status != ZIPC_OK)
        return status;
    if (message.source_component != link->remote_component)
        return ZIPC_ERR_INVALID_RECEIVER;

    return zipc_buffer_claim(link->pool, &message,
                             link->local_component, buffer);
}

zipc_status_t zipc_receive_timeout(zipc_link_t *link, zipc_buffer_t *buffer, uint32_t timeout_ticks)
{
    (void)timeout_ticks;
    return zipc_receive(link, buffer);
}

zipc_status_t zipc_buffer_put(zipc_link_t *link, zipc_buffer_t *buffer)
{
    if (link == NULL || buffer == NULL || buffer->control == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;

    zipc_status_t status = zipc_buffer_release(
        link->pool, buffer->handle, link->local_component);
    if (status == ZIPC_OK)
        memset(buffer, 0, sizeof(*buffer));
    return status;
}

zipc_status_t zipc_buffer_set_limits(zipc_buffer_t *buffer, uint32_t hop_limit,
                                     uint64_t absolute_deadline_ns)
{
    if (buffer == NULL || buffer->control == NULL) return ZIPC_ERR_INVALID_ARGUMENT;
    buffer->control->hop_limit = hop_limit != 0U ? hop_limit : ZIPC_HOP_LIMIT_UNLIMITED;
    buffer->control->deadline_ns = absolute_deadline_ns != 0U ? absolute_deadline_ns : ZIPC_DEADLINE_NONE;
    return ZIPC_OK;
}

zipc_status_t zipc_buffer_append(zipc_buffer_t *buffer,
                                 const void *data,
                                 uint32_t length)
{
    if (buffer == NULL || buffer->control == NULL ||
        (length != 0U && data == NULL))
        return ZIPC_ERR_INVALID_ARGUMENT;
    if (length > zipc_buffer_tailroom(buffer))
        return ZIPC_ERR_REGION_OVERFLOW;

    if (length != 0U)
        memcpy(buffer->data + buffer->length, data, length);
    return zipc_buffer_set_region(buffer, buffer->control->region.offset,
                                  buffer->length + length);
}

zipc_status_t zipc_buffer_prepend(zipc_buffer_t *buffer,
                                  const void *data,
                                  uint32_t length)
{
    if (buffer == NULL || buffer->control == NULL ||
        (length != 0U && data == NULL))
        return ZIPC_ERR_INVALID_ARGUMENT;
    if (length > zipc_buffer_headroom(buffer))
        return ZIPC_ERR_REGION_OVERFLOW;

    const uint32_t offset = buffer->control->region.offset - length;
    if (length != 0U)
        memcpy(buffer->slot_base + offset, data, length);
    return zipc_buffer_set_region(buffer, offset, buffer->length + length);
}

zipc_status_t zipc_buffer_trim_front(zipc_buffer_t *buffer, uint32_t length)
{
    if (buffer == NULL || buffer->control == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;
    if (length > buffer->length)
        return ZIPC_ERR_REGION_OVERFLOW;
    return zipc_buffer_set_region(buffer,
                                  buffer->control->region.offset + length,
                                  buffer->length - length);
}

zipc_status_t zipc_buffer_trim_back(zipc_buffer_t *buffer, uint32_t length)
{
    if (buffer == NULL || buffer->control == NULL)
        return ZIPC_ERR_INVALID_ARGUMENT;
    if (length > buffer->length)
        return ZIPC_ERR_REGION_OVERFLOW;
    return zipc_buffer_set_region(buffer, buffer->control->region.offset,
                                  buffer->length - length);
}

void *zipc_buffer_data(zipc_buffer_t *buffer)
{
    return buffer != NULL ? buffer->data : NULL;
}

const void *zipc_buffer_const_data(const zipc_buffer_t *buffer)
{
    return buffer != NULL ? buffer->data : NULL;
}

uint32_t zipc_buffer_length(const zipc_buffer_t *buffer)
{
    return buffer != NULL ? buffer->length : 0U;
}

uint32_t zipc_buffer_headroom(const zipc_buffer_t *buffer)
{
    return buffer != NULL && buffer->control != NULL
         ? buffer->control->region.offset : 0U;
}

uint32_t zipc_buffer_tailroom(const zipc_buffer_t *buffer)
{
    if (buffer == NULL || buffer->control == NULL ||
        buffer->control->region.offset > buffer->capacity ||
        buffer->length > buffer->capacity - buffer->control->region.offset)
        return 0U;
    return buffer->capacity - buffer->control->region.offset - buffer->length;
}
