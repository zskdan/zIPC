#include <zipc/zipc.h>
#include "identity-test.h"

#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "check failed: %s line=%d\n", #x, __LINE__); return 1; \
} } while (0)
#define CHECK_OK(x) CHECK((x) == ZIPC_OK)

enum { THREADS = 8, IDS_PER_THREAD = 20000 };

_Static_assert(sizeof(zipc_message_t) == 24U,
               "ABI-2 descriptor layout changed unexpectedly");
_Static_assert(sizeof(zipc_visited_set_t) == 32U,
               "visited set must contain exactly eight 32-bit words");
_Static_assert(offsetof(zipc_slot_control_t, identity) % 8U == 0U,
               "buffer identity must be naturally aligned");

static uint32_t g_random_value;
static zipc_trace_record_t g_trace[16];
static uint32_t g_trace_count;
static zipc_pool_t *g_trace_pool;
static uint32_t g_allocate_claiming;
static uint32_t g_send_claiming;
static uint32_t g_receive_claiming;
static pthread_mutex_t g_rollover_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_rollover_cond = PTHREAD_COND_INITIALIZER;
static int g_hold_reservation;
static int g_reservation_held;

static zipc_status_t failing_signal_send(void *context)
{
    (void)context;
    return ZIPC_ERR_TRANSPORT;
}

static zipc_status_t successful_signal_wait(void *context,
                                            uint32_t timeout_ticks)
{
    (void)context;
    (void)timeout_ticks;
    return ZIPC_OK;
}

static zipc_status_t failing_protection(zipc_pool_t *pool,
                                        zipc_slot_id_t slot_id,
                                        bool read_write)
{
    (void)pool;
    (void)slot_id;
    (void)read_write;
    return ZIPC_ERR_PLATFORM;
}

static zipc_status_t deterministic_random(void *buffer, size_t length)
{
    if (length != sizeof(g_random_value))
        return ZIPC_ERR_ENTROPY_UNAVAILABLE;
    memcpy(buffer, &g_random_value, length);
    return ZIPC_OK;
}

static zipc_status_t failing_random(void *buffer, size_t length)
{
    (void)buffer;
    (void)length;
    return ZIPC_ERR_ENTROPY_UNAVAILABLE;
}

static void capture_trace(const zipc_trace_record_t *record, void *context)
{
    (void)context;
    if (g_trace_count < 16U)
        g_trace[g_trace_count++] = *record;
    if (g_trace_pool != NULL && record->handle != ZIPC_INVALID_HANDLE) {
        const zipc_slot_id_t slot_id = zipc_handle_slot_id(record->handle);
        if (slot_id < g_trace_pool->header->slot_count &&
            atomic_load_explicit(&g_trace_pool->controls[slot_id].state,
                                 memory_order_acquire) == ZIPC_SLOT_CLAIMING) {
            if (record->event == ZIPC_TRACE_ALLOCATE)
                g_allocate_claiming++;
            else if (record->event == ZIPC_TRACE_SEND)
                g_send_claiming++;
            else if (record->event == ZIPC_TRACE_RECEIVE)
                g_receive_claiming++;
        }
    }
}

static void hold_reserved_id(void)
{
    (void)pthread_mutex_lock(&g_rollover_lock);
    if (g_hold_reservation) {
        g_reservation_held = 1;
        (void)pthread_cond_broadcast(&g_rollover_cond);
        while (g_hold_reservation)
            (void)pthread_cond_wait(&g_rollover_cond, &g_rollover_lock);
    }
    (void)pthread_mutex_unlock(&g_rollover_lock);
}

typedef struct {
    zipc_buffer_id_t *ids;
    int failed;
} thread_context_t;

typedef struct {
    zipc_component_id_t component;
    zipc_buffer_id_t id;
    zipc_status_t status;
} one_id_context_t;

static void *generate_ids(void *argument)
{
    thread_context_t *context = argument;
    for (uint32_t i = 0U; i < IDS_PER_THREAD; ++i) {
        if (zipc_test_identity_generate(42U, &context->ids[i]) != ZIPC_OK) {
            context->failed = 1;
            break;
        }
    }
    return NULL;
}

static void *generate_one_id(void *argument)
{
    one_id_context_t *context = argument;
    context->status = zipc_test_identity_generate(context->component,
                                                   &context->id);
    return NULL;
}

static int compare_ids(const void *left, const void *right)
{
    const zipc_buffer_id_t a = *(const zipc_buffer_id_t *)left;
    const zipc_buffer_id_t b = *(const zipc_buffer_id_t *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

int main(void)
{
    const zipc_buffer_id_t packed = zipc_buffer_id_make(
        254U, UINT32_C(0xabcdef), UINT32_C(0x12345678));
    CHECK(zipc_buffer_id_component(packed) == 254U);
    CHECK(zipc_buffer_id_session(packed) == UINT32_C(0xabcdef));
    CHECK(zipc_buffer_id_sequence(packed) == UINT32_C(0x12345678));
    CHECK(zipc_buffer_id_valid(packed));
    CHECK(!zipc_buffer_id_valid(0U));
    CHECK(!zipc_buffer_id_valid(zipc_buffer_id_make(0U, 1U, 0U)));
    CHECK(!zipc_buffer_id_valid(zipc_buffer_id_make(255U, 1U, 0U)));
    CHECK(!zipc_buffer_id_valid(zipc_buffer_id_make(1U, 1U, UINT32_MAX)));
    CHECK(zipc_component_id_valid(1U) && zipc_component_id_valid(254U));
    CHECK(!zipc_component_id_valid(0U) && !zipc_component_id_valid(255U));

    zipc_visited_set_t visited = {{0U}};
    visited.words[0] = (UINT32_C(1) << 1) | (UINT32_C(1) << 31);
    visited.words[1] = UINT32_C(1);
    visited.words[7] = (UINT32_C(1) << 30);
    CHECK(zipc_visited_set_test(&visited, 1U));
    CHECK(zipc_visited_set_test(&visited, 31U));
    CHECK(zipc_visited_set_test(&visited, 32U));
    CHECK(zipc_visited_set_test(&visited, 254U));
    CHECK(!zipc_visited_set_test(&visited, 0U));
    CHECK(!zipc_visited_set_test(&visited, 255U));
    CHECK(zipc_visited_set_count(&visited) == 4U);

    zipc_test_identity_random(deterministic_random);
    zipc_test_identity_reset(7U, UINT32_C(0x123456), UINT32_MAX - 1U);
    zipc_buffer_id_t last = 0U, rotated = 0U;
    CHECK_OK(zipc_test_identity_generate(7U, &last));
    CHECK(zipc_buffer_id_sequence(last) == UINT32_MAX - 1U);
    g_random_value = UINT32_C(0x654321);
    CHECK_OK(zipc_test_identity_generate(7U, &rotated));
    CHECK(zipc_buffer_id_session(rotated) == UINT32_C(0x654321));
    CHECK(zipc_buffer_id_sequence(rotated) == 0U);
    CHECK(last != rotated);

    zipc_test_identity_reset(6U, UINT32_C(0x123456), UINT32_MAX - 1U);
    g_random_value = UINT32_C(0x654321);
    g_hold_reservation = 1;
    g_reservation_held = 0;
    zipc_test_identity_reserved(hold_reserved_id);
    one_id_context_t before_rollover = {.component = 6U};
    one_id_context_t after_rollover = {.component = 6U};
    pthread_t before_thread, after_thread;
    CHECK(pthread_create(&before_thread, NULL, generate_one_id,
                         &before_rollover) == 0);
    CHECK(pthread_mutex_lock(&g_rollover_lock) == 0);
    while (!g_reservation_held)
        CHECK(pthread_cond_wait(&g_rollover_cond, &g_rollover_lock) == 0);
    CHECK(pthread_mutex_unlock(&g_rollover_lock) == 0);
    CHECK(pthread_create(&after_thread, NULL, generate_one_id,
                         &after_rollover) == 0);
    uint32_t session_state = 0U, sequence = 0U, active = 0U;
    for (uint32_t i = 0U; i < 1000000U; ++i) {
        zipc_test_identity_snapshot(6U, &session_state, &sequence, &active);
        if ((session_state & UINT32_C(0x80000000)) != 0U)
            break;
        sched_yield();
    }
    CHECK((session_state & UINT32_C(0x80000000)) != 0U);
    CHECK(sequence == UINT32_MAX && active == 1U);
    CHECK(pthread_mutex_lock(&g_rollover_lock) == 0);
    g_hold_reservation = 0;
    CHECK(pthread_cond_broadcast(&g_rollover_cond) == 0);
    CHECK(pthread_mutex_unlock(&g_rollover_lock) == 0);
    CHECK(pthread_join(before_thread, NULL) == 0);
    CHECK(pthread_join(after_thread, NULL) == 0);
    zipc_test_identity_reserved(NULL);
    CHECK(before_rollover.status == ZIPC_OK);
    CHECK(after_rollover.status == ZIPC_OK);
    CHECK(zipc_buffer_id_session(before_rollover.id) ==
          UINT32_C(0x123456));
    CHECK(zipc_buffer_id_sequence(before_rollover.id) == UINT32_MAX - 1U);
    CHECK(zipc_buffer_id_session(after_rollover.id) ==
          UINT32_C(0x654321));
    CHECK(zipc_buffer_id_sequence(after_rollover.id) == 0U);
    zipc_test_identity_snapshot(6U, &session_state, &sequence, &active);
    CHECK(session_state == UINT32_C(0x654321));
    CHECK(sequence == 1U && active == 0U);

    zipc_test_identity_reset(8U, 0U, 0U);
    zipc_test_identity_random(failing_random);
    CHECK(zipc_test_identity_generate(8U, &last) ==
          ZIPC_ERR_ENTROPY_UNAVAILABLE);

    zipc_test_identity_random(NULL);
    zipc_test_identity_reset(42U, UINT32_C(0x234567), 0U);
    const size_t total = (size_t)THREADS * IDS_PER_THREAD;
    zipc_buffer_id_t *ids = malloc(total * sizeof(*ids));
    CHECK(ids != NULL);
    pthread_t threads[THREADS];
    thread_context_t contexts[THREADS];
    for (uint32_t i = 0U; i < THREADS; ++i) {
        contexts[i] = (thread_context_t){
            .ids = ids + (size_t)i * IDS_PER_THREAD,
        };
        CHECK(pthread_create(&threads[i], NULL, generate_ids,
                             &contexts[i]) == 0);
    }
    for (uint32_t i = 0U; i < THREADS; ++i) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        CHECK(contexts[i].failed == 0);
    }
    qsort(ids, total, sizeof(*ids), compare_ids);
    for (size_t i = 1U; i < total; ++i)
        CHECK(ids[i - 1U] != ids[i]);
    free(ids);

    const uint32_t slots = 8U, capacity = 128U, depth = 8U;
    const size_t control_size = zipc_pool_required_control_size(slots);
    const size_t payload_size = zipc_pool_required_payload_size(
        slots, capacity, 0U, 64U);
    void *control_storage = aligned_alloc(64U,
        (control_size + 63U) & ~(size_t)63U);
    void *payload_storage = aligned_alloc(64U, payload_size);
    CHECK(control_storage != NULL && payload_storage != NULL);
    zipc_platform_memory_t *control = NULL, *payload = NULL;
    zipc_platform_memory_config_t control_config = {
        .type = ZIPC_SHM_PREALLOCATED, .size = control_size,
        .backend.preallocated = {
            .address = control_storage,
            .physical_address = ZIPC_PHYS_ADDR_INVALID,
            .capabilities = ZIPC_MEM_CAP_CPU_READ | ZIPC_MEM_CAP_CPU_WRITE |
                            ZIPC_MEM_CAP_ATOMIC32 | ZIPC_MEM_CAP_ATOMIC64,
        },
    };
    zipc_platform_memory_config_t payload_config = {
        .type = ZIPC_SHM_PREALLOCATED, .size = payload_size,
        .backend.preallocated = {
            .address = payload_storage,
            .physical_address = ZIPC_PHYS_ADDR_INVALID,
            .capabilities = ZIPC_MEM_CAP_CPU_READ | ZIPC_MEM_CAP_CPU_WRITE,
        },
    };
    CHECK_OK(zipc_platform_memory_open(&control, &control_config));
    CHECK_OK(zipc_platform_memory_open(&payload, &payload_config));
    zipc_pool_config_t pool_config = {
        .control_memory = control, .payload_memory = payload,
        .slot_count = slots, .slot_capacity = capacity,
        .payload_alignment = 64U, .pool_id = 99U,
    };
    CHECK_OK(zipc_pool_format(&pool_config));
    zipc_pool_t pool;
    CHECK_OK(zipc_pool_attach(&pool, &pool_config));
    CHECK(pool.header->abi_version == 2U);
    pool.header->abi_version = 1U;
    CHECK(zipc_pool_attach(&pool, &pool_config) == ZIPC_ERR_INVALID_POOL);
    pool.header->abi_version = 2U;

    zipc_transport_spsc_ring_t *ring = calloc(
        1U, zipc_transport_spsc_ring_size(depth));
    CHECK(ring != NULL);
    CHECK_OK(zipc_transport_spsc_ring_initialize(ring, depth));
    zipc_platform_transport_config_t transport = {
        .type = ZIPC_TRANSPORT_SHM_RING_POLLING,
        .platform_handle = ring, .ring_depth = depth,
        .poll_timeout_ns = 1000000U,
    };
    zipc_link_config_t tx_config = {
        .pool = &pool, .local_component = 1U, .remote_component = 254U,
        .transport = transport,
    };
    zipc_link_config_t rx_config = {
        .pool = &pool, .local_component = 254U, .remote_component = 1U,
        .transport = transport,
    };
    zipc_link_t *tx = NULL, *rx = NULL;
    CHECK_OK(zipc_link_create(&tx, &tx_config));
    CHECK_OK(zipc_link_create(&rx, &rx_config));
    zipc_test_identity_reset(1U, 0U, 0U);
    zipc_test_identity_random(failing_random);
    zipc_buffer_t failed_allocation;
    CHECK(zipc_buffer_alloc(tx, 8U, &failed_allocation, NULL) ==
          ZIPC_ERR_ENTROPY_UNAVAILABLE);
    CHECK(!zipc_buffer_is_valid(&failed_allocation));
    for (uint32_t i = 0U; i < slots; ++i)
        CHECK(atomic_load_explicit(&pool.controls[i].state,
                                   memory_order_acquire) == ZIPC_SLOT_FREE);
    zipc_test_identity_random(NULL);
    tx_config.local_component = 0U;
    CHECK(zipc_link_create(&tx, &tx_config) == ZIPC_ERR_INVALID_ARGUMENT);
    tx_config.local_component = 255U;
    CHECK(zipc_link_create(&tx, &tx_config) == ZIPC_ERR_INVALID_ARGUMENT);
    CHECK(zipc_component_register(&pool, 0U, NULL) == ZIPC_ERR_INVALID_ARGUMENT);
    CHECK(zipc_component_register(&pool, 255U, NULL) == ZIPC_ERR_INVALID_ARGUMENT);
    zipc_buffer_t invalid;
    CHECK(zipc_buffer_allocate(&pool, 0U, 0U, &invalid) ==
          ZIPC_ERR_INVALID_ARGUMENT);
    CHECK(zipc_buffer_allocate(&pool, 255U, 0U, &invalid) ==
          ZIPC_ERR_INVALID_ARGUMENT);
    zipc_test_identity_reset(9U, 0U, 0U);
    zipc_test_identity_random(failing_random);
    CHECK(zipc_buffer_allocate(&pool, 9U, 0U, &invalid) ==
          ZIPC_ERR_ENTROPY_UNAVAILABLE);
    CHECK(atomic_load_explicit(&pool.controls[0].state,
                               memory_order_acquire) == ZIPC_SLOT_FREE);
    zipc_test_identity_random(NULL);

    zipc_buffer_t malformed;
    CHECK_OK(zipc_buffer_allocate(&pool, 1U, 0U, &malformed));
    const zipc_handle_t malformed_handle = zipc_buffer_handle(&malformed);
    const zipc_slot_id_t malformed_slot =
        zipc_handle_slot_id(malformed_handle);
    zipc_message_t malformed_message;
    CHECK_OK(zipc_buffer_prepare_transfer(
        &pool, malformed_handle, 1U, 254U, &malformed_message));
    pool.controls[malformed_slot].region.offset = capacity + 1U;
    const uint64_t releases_before_failure = atomic_load_explicit(
        &pool.header->release_count, memory_order_relaxed);
    CHECK(zipc_buffer_claim(&pool, &malformed_message, 254U, &malformed) ==
          ZIPC_ERR_REGION_OVERFLOW);
    CHECK(!zipc_buffer_is_valid(&malformed));
    CHECK(atomic_load_explicit(&pool.controls[malformed_slot].state,
                               memory_order_acquire) == ZIPC_SLOT_FREE);
    CHECK(atomic_load_explicit(&pool.header->release_count,
                               memory_order_relaxed) ==
          releases_before_failure);

    CHECK_OK(zipc_topology_bind_pool("identity-pool", &pool));
    CHECK_OK(zipc_topology_bind_transport("identity-ring", &transport));
    zipc_topology_link_config_t invalid_topology_link = {
        .name = "invalid", .link_id = 1U,
        .pool_name = "identity-pool", .transport_name = "identity-ring",
        .local_component = 0U, .remote_component = 1U,
    };
    zipc_topology_config_t invalid_topology = {
        .links = &invalid_topology_link, .link_count = 1U,
    };
    CHECK(zipc_topology_validate(&invalid_topology) ==
          ZIPC_ERR_INVALID_ARGUMENT);
    invalid_topology_link.local_component = 255U;
    CHECK(zipc_topology_validate(&invalid_topology) ==
          ZIPC_ERR_INVALID_ARGUMENT);
    zipc_topology_reset();

    g_trace_count = 0U;
    g_trace_pool = &pool;
    g_allocate_claiming = 0U;
    g_send_claiming = 0U;
    g_receive_claiming = 0U;
    zipc_trace_set_hook(capture_trace, NULL);
    zipc_buffer_t root, child;
    CHECK_OK(zipc_buffer_alloc(tx, 32U, &root, NULL));
    const zipc_buffer_id_t root_id = zipc_buffer_id(&root);
    CHECK(zipc_buffer_id_valid(root_id));
    CHECK(zipc_buffer_parent_id(&root) == 0U);
    CHECK_OK(zipc_buffer_alloc(tx, 16U, &child, &root));
    const zipc_buffer_id_t child_id = zipc_buffer_id(&child);
    CHECK(child_id != root_id);
    CHECK(zipc_buffer_parent_id(&child) == root_id);
    CHECK(zipc_buffer_is_valid(&root));
    zipc_buffer_t stale_parent = root;
    CHECK_OK(zipc_buffer_release(&root));
    zipc_buffer_t rejected_child;
    CHECK(zipc_buffer_alloc(tx, 8U, &rejected_child, &stale_parent) ==
          ZIPC_ERR_INVALID_BUFFER);
    CHECK(zipc_buffer_parent_id(&child) == root_id);
    CHECK_OK(zipc_send(tx, &child));
    CHECK_OK(zipc_recv(rx, &child));
    CHECK(zipc_buffer_id(&child) == child_id);
    CHECK(zipc_buffer_parent_id(&child) == root_id);
    const zipc_visited_set_t child_visited = zipc_buffer_visited(&child);
    CHECK(zipc_visited_set_count(&child_visited) == 2U);
    CHECK(zipc_visited_set_test(&child_visited, 1U));
    CHECK(zipc_visited_set_test(&child_visited, 254U));
    CHECK(!zipc_visited_set_test(&child_visited, 0U));
    CHECK(!zipc_visited_set_test(&child_visited, 255U));
    CHECK_OK(zipc_buffer_release(&child));
    CHECK(g_trace_count >= 6U);
    CHECK(g_trace[0].event == ZIPC_TRACE_ALLOCATE);
    CHECK(g_trace[0].buffer_id == root_id && g_trace[0].parent_id == 0U);
    CHECK(g_trace[2].event == ZIPC_TRACE_RELEASE);
    CHECK(g_trace[3].event == ZIPC_TRACE_SEND);
    CHECK(g_trace[4].event == ZIPC_TRACE_RECEIVE);
    CHECK(g_trace[5].event == ZIPC_TRACE_RELEASE);
    CHECK(g_trace[3].buffer_id == child_id &&
          g_trace[3].parent_id == root_id);
    CHECK(g_allocate_claiming == 2U);
    CHECK(g_send_claiming == 1U);
    CHECK(g_receive_claiming == 1U);

    zipc_message_t invalid_message;
    zipc_buffer_t failed_prepare;
    CHECK_OK(zipc_buffer_allocate(&pool, 1U, 0U, &failed_prepare));
    const zipc_slot_id_t failed_prepare_slot =
        zipc_handle_slot_id(zipc_buffer_handle(&failed_prepare));
    const uint32_t sequence_before =
        pool.controls[failed_prepare_slot].transfer_sequence;
    const zipc_component_id_t next_owner_before =
        pool.controls[failed_prepare_slot].next_owner_id;
    const uint32_t send_claiming_before = g_send_claiming;
    zipc_test_protection(failing_protection);
    CHECK(zipc_buffer_prepare_transfer(
              &pool, zipc_buffer_handle(&failed_prepare), 1U, 254U,
              &invalid_message) == ZIPC_ERR_PLATFORM);
    CHECK(atomic_load_explicit(&pool.controls[failed_prepare_slot].state,
                               memory_order_acquire) == ZIPC_SLOT_OWNED);
    CHECK(pool.controls[failed_prepare_slot].transfer_sequence ==
          sequence_before);
    CHECK(pool.controls[failed_prepare_slot].next_owner_id ==
          next_owner_before);
    CHECK(g_send_claiming == send_claiming_before);
    zipc_test_protection(NULL);
    CHECK_OK(zipc_pool_buffer_release(
        &pool, zipc_buffer_handle(&failed_prepare), 1U));

    g_trace_count = 0U;
    CHECK(zipc_buffer_prepare_transfer(
              &pool, ZIPC_INVALID_HANDLE, 1U, 254U, &invalid_message) ==
          ZIPC_ERR_INVALID_HANDLE);
    CHECK(g_trace_count == 1U && g_trace[0].event == ZIPC_TRACE_ERROR);
    CHECK(g_trace[0].buffer_id == 0U &&
          g_trace[0].handle == ZIPC_INVALID_HANDLE);

    uint32_t recovery_epoch = 0U;
    CHECK_OK(zipc_component_register(&pool, 10U, &recovery_epoch));
    zipc_buffer_t orphan;
    CHECK_OK(zipc_buffer_allocate(&pool, 10U, 0U, &orphan));
    const zipc_buffer_id_t orphan_id = zipc_buffer_id(&orphan);
    CHECK_OK(zipc_component_unregister(&pool, 10U, recovery_epoch));
    zipc_recovery_result_t recovery;
    CHECK_OK(zipc_pool_recover_owner(
        &pool, 10U, recovery_epoch, 0U, &recovery));
    CHECK(recovery.recovered_owned == 1U);
    CHECK(g_trace_count == 3U);
    CHECK(g_trace[1].event == ZIPC_TRACE_ALLOCATE);
    CHECK(g_trace[2].event == ZIPC_TRACE_RECOVER);
    CHECK(g_trace[2].buffer_id == orphan_id);

    CHECK(zipc_pool_recover_claiming(NULL, NULL) ==
          ZIPC_ERR_INVALID_ARGUMENT);
    zipc_slot_control_t *zero_claim = &pool.controls[0];
    memset((uint8_t *)zero_claim + offsetof(zipc_slot_control_t, generation), 0,
           sizeof(*zero_claim) - offsetof(zipc_slot_control_t, generation));
    atomic_store_explicit(&zero_claim->state, ZIPC_SLOT_CLAIMING,
                          memory_order_release);
    zipc_slot_control_t *partial_claim = &pool.controls[1];
    partial_claim->owner_id = 10U;
    partial_claim->next_owner_id = 254U;
    partial_claim->owner_epoch = recovery_epoch;
    partial_claim->transfer_sequence = 7U;
    partial_claim->acquired_ns = 0U;
    atomic_store_explicit(&partial_claim->state, ZIPC_SLOT_CLAIMING,
                          memory_order_release);
    const uint64_t component_recoveries = atomic_load_explicit(
        &pool.header->components[10U].recovered_slots, memory_order_relaxed);
    CHECK_OK(zipc_pool_recover_owner(
        &pool, 10U, recovery_epoch, 0U, &recovery));
    CHECK(atomic_load_explicit(&zero_claim->state,
                               memory_order_acquire) == ZIPC_SLOT_CLAIMING);
    CHECK(atomic_load_explicit(&partial_claim->state,
                               memory_order_acquire) == ZIPC_SLOT_CLAIMING);
    const uint64_t pool_recoveries = atomic_load_explicit(
        &pool.header->recovery_count, memory_order_relaxed);
    const uint32_t traces_before_failed_protection = g_trace_count;
    zipc_test_protection(failing_protection);
    uint32_t recovered_claiming = 0U;
    CHECK(zipc_pool_recover_claiming(&pool, &recovered_claiming) ==
          ZIPC_ERR_PLATFORM);
    CHECK(recovered_claiming == 0U);
    CHECK(atomic_load_explicit(&zero_claim->state,
                               memory_order_acquire) == ZIPC_SLOT_CLAIMING);
    CHECK(g_trace_count == traces_before_failed_protection);
    CHECK(atomic_load_explicit(&pool.header->recovery_count,
                               memory_order_relaxed) == pool_recoveries);
    zipc_test_protection(NULL);
    CHECK_OK(zipc_pool_recover_claiming(&pool, &recovered_claiming));
    CHECK(recovered_claiming == 2U);
    CHECK(atomic_load_explicit(&zero_claim->state,
                               memory_order_acquire) == ZIPC_SLOT_FREE);
    CHECK(atomic_load_explicit(&partial_claim->state,
                               memory_order_acquire) == ZIPC_SLOT_FREE);
    CHECK(atomic_load_explicit(&pool.header->recovery_count,
                               memory_order_relaxed) == pool_recoveries + 2U);
    CHECK(atomic_load_explicit(
              &pool.header->components[10U].recovered_slots,
              memory_order_relaxed) == component_recoveries);
    CHECK(g_trace[g_trace_count - 2U].component_id ==
          ZIPC_INVALID_COMPONENT_ID);
    CHECK(g_trace[g_trace_count - 1U].component_id ==
          ZIPC_INVALID_COMPONENT_ID);
    CHECK(g_trace[g_trace_count - 2U].buffer_id == 0U);
    CHECK(g_trace[g_trace_count - 1U].buffer_id == 0U);
    CHECK(g_trace[g_trace_count - 2U].handle == ZIPC_INVALID_HANDLE);
    CHECK(g_trace[g_trace_count - 1U].handle == ZIPC_INVALID_HANDLE);
    CHECK_OK(zipc_pool_recover_claiming(&pool, NULL));

    atomic_store_explicit(&ring->producer, depth, memory_order_relaxed);
    atomic_store_explicit(&ring->consumer, 0U, memory_order_relaxed);
    zipc_buffer_t unpublished_buffer;
    CHECK_OK(zipc_buffer_alloc(tx, 8U, &unpublished_buffer, NULL));
    const zipc_handle_t unpublished_handle =
        zipc_buffer_handle(&unpublished_buffer);
    CHECK(zipc_send(tx, &unpublished_buffer) == ZIPC_ERR_TRANSPORT);
    CHECK(zipc_buffer_is_valid(&unpublished_buffer));
    CHECK(atomic_load_explicit(
              &pool.controls[zipc_handle_slot_id(unpublished_handle)].state,
              memory_order_acquire) == ZIPC_SLOT_OWNED);
    CHECK_OK(zipc_buffer_release(&unpublished_buffer));
    atomic_store_explicit(&ring->producer, 0U, memory_order_relaxed);
    atomic_store_explicit(&ring->consumer, 0U, memory_order_relaxed);

    zipc_transport_spsc_ring_t *published_ring = calloc(
        1U, zipc_transport_spsc_ring_size(depth));
    CHECK(published_ring != NULL);
    zipc_platform_transport_config_t published_transport = {
        .type = ZIPC_TRANSPORT_PL_RING_IRQ,
        .platform_handle = published_ring,
        .ring_depth = depth,
        .create_endpoint = true,
        .signal_send = failing_signal_send,
        .signal_wait = successful_signal_wait,
    };
    zipc_link_config_t published_tx_config = tx_config;
    published_tx_config.local_component = 1U;
    published_tx_config.remote_component = 254U;
    published_tx_config.transport = published_transport;
    zipc_link_config_t published_rx_config = rx_config;
    published_rx_config.local_component = 254U;
    published_rx_config.remote_component = 1U;
    published_rx_config.transport = published_transport;
    published_rx_config.transport.create_endpoint = false;
    zipc_link_t *published_tx = NULL, *published_rx = NULL;
    CHECK_OK(zipc_link_create(&published_tx, &published_tx_config));
    CHECK_OK(zipc_link_create(&published_rx, &published_rx_config));
    zipc_buffer_t published_buffer;
    CHECK_OK(zipc_buffer_alloc(published_tx, 8U, &published_buffer, NULL));
    const zipc_handle_t published_handle =
        zipc_buffer_handle(&published_buffer);
    CHECK(zipc_send(published_tx, &published_buffer) ==
          ZIPC_ERR_TRANSPORT_PUBLISHED);
    CHECK(!zipc_buffer_is_valid(&published_buffer));
    CHECK(atomic_load_explicit(
              &pool.controls[zipc_handle_slot_id(published_handle)].state,
              memory_order_acquire) == ZIPC_SLOT_TRANSFER);
    CHECK(atomic_load_explicit(&published_ring->producer,
                               memory_order_acquire) == 1U);
    CHECK_OK(zipc_recv(published_rx, &published_buffer));
    CHECK_OK(zipc_buffer_release(&published_buffer));
    zipc_link_destroy(published_rx);
    zipc_link_destroy(published_tx);
    free(published_ring);
    zipc_trace_set_hook(NULL, NULL);
    g_trace_pool = NULL;

    const size_t other_control_size = zipc_pool_required_control_size(2U);
    const size_t other_payload_size = zipc_pool_required_payload_size(
        2U, capacity, 0U, 64U);
    void *other_control_storage = aligned_alloc(
        64U, (other_control_size + 63U) & ~(size_t)63U);
    void *other_payload_storage = aligned_alloc(64U, other_payload_size);
    CHECK(other_control_storage != NULL && other_payload_storage != NULL);
    zipc_platform_memory_t *other_control = NULL, *other_payload = NULL;
    control_config.size = other_control_size;
    control_config.backend.preallocated.address = other_control_storage;
    payload_config.size = other_payload_size;
    payload_config.backend.preallocated.address = other_payload_storage;
    CHECK_OK(zipc_platform_memory_open(&other_control, &control_config));
    CHECK_OK(zipc_platform_memory_open(&other_payload, &payload_config));
    zipc_pool_config_t other_pool_config = {
        .control_memory = other_control, .payload_memory = other_payload,
        .slot_count = 2U, .slot_capacity = capacity,
        .payload_alignment = 64U, .pool_id = 100U,
    };
    CHECK_OK(zipc_pool_format(&other_pool_config));
    zipc_pool_t other_pool;
    CHECK_OK(zipc_pool_attach(&other_pool, &other_pool_config));
    zipc_link_config_t other_link_config = {
        .pool = &other_pool, .local_component = 1U,
        .remote_component = 254U, .transport = transport,
    };
    zipc_link_t *other_link = NULL;
    CHECK_OK(zipc_link_create(&other_link, &other_link_config));
    zipc_buffer_t cross_parent, cross_child;
    CHECK_OK(zipc_buffer_alloc(tx, 8U, &cross_parent, NULL));
    const zipc_buffer_id_t cross_parent_id = zipc_buffer_id(&cross_parent);
    CHECK_OK(zipc_buffer_alloc(
        other_link, 8U, &cross_child, &cross_parent));
    CHECK(zipc_buffer_parent_id(&cross_child) == cross_parent_id);
    CHECK_OK(zipc_buffer_release(&cross_parent));
    CHECK(zipc_buffer_parent_id(&cross_child) == cross_parent_id);
    CHECK_OK(zipc_buffer_release(&cross_child));
    zipc_link_destroy(other_link);
    zipc_platform_memory_close(other_payload);
    zipc_platform_memory_close(other_control);
    free(other_payload_storage); free(other_control_storage);

    zipc_link_destroy(rx); zipc_link_destroy(tx);
    free(ring);
    zipc_platform_memory_close(payload);
    zipc_platform_memory_close(control);
    free(payload_storage); free(control_storage);
    puts("PASS: ABI-2 publication, identity gate, rollover, lineage, visited set and trace");
    return 0;
}
