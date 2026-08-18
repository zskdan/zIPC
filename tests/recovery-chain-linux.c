#define _GNU_SOURCE
#include <zipc/zipc.h>

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NODE_COUNT 5U
#define RELAY_COUNT 3U
#define EDGE_COUNT 4U
#define SLOT_COUNT 16U
#define SLOT_CAPACITY 128U
#define RING_DEPTH 8U
#define PAYLOAD_MAGIC UINT32_C(0x52454333)

typedef enum {
    CRASH_OWNED = 1,
    CRASH_AFTER_PROCESS,
    CRASH_SEND_CLAIM,
    CRASH_SEND_PRE_PUBLISH,
    CRASH_SEND_POST_PUBLISH,
    CRASH_RECEIVE_CLAIM,
    CRASH_RECEIVE_POST_CLAIM,
    CRASH_PHASE_COUNT
} crash_phase_t;

typedef enum {
    REPORT_SOURCE_SENT = 1,
    REPORT_CHECKPOINT,
    REPORT_KILL_SENT,
    REPORT_KILL_CONFIRMED,
    REPORT_RESTART_BEGIN,
    REPORT_RESTART_REGISTERED,
    REPORT_LINKS_OPENED,
    REPORT_INPUT_RECONCILED,
    REPORT_OUTPUT_RECONCILED,
    REPORT_BUFFER_ADOPTED,
    REPORT_PENDING_RECEIVED,
    REPORT_HANDLER_COMPLETED,
    REPORT_BUFFER_SENT,
    REPORT_RECOVERY_ACTIVE,
    REPORT_SINK_RECEIVED,
    REPORT_VERIFIED,
    REPORT_ERROR
} report_type_t;

typedef enum {
    OP_NONE = 0,
    OP_LINK_OPEN,
    OP_BUFFER_ALLOCATE,
    OP_BUFFER_RECEIVE,
    OP_BUFFER_SEND,
    OP_BUFFER_RELEASE,
    OP_PAYLOAD_VALIDATE,
    OP_RESTART_BEGIN,
    OP_INPUT_RECONCILE,
    OP_OUTPUT_RECONCILE,
    OP_BUFFER_ADOPT,
    OP_RESTART_FINISH
} operation_t;

typedef enum {
    VERBOSITY_QUIET = 0,
    VERBOSITY_NORMAL,
    VERBOSITY_VERBOSE
} verbosity_t;

typedef struct {
    uint32_t magic;
    uint32_t iteration;
    uint32_t relay_mask;
    uint32_t reserved;
} test_payload_t;

typedef struct {
    uint32_t type;
    uint32_t component;
    uint32_t phase;
    uint32_t operation;
    int32_t status;
    int32_t pid;
    uint32_t old_epoch;
    uint32_t new_epoch;
    uint64_t timestamp_ns;
    uint64_t buffer_id;
    uint64_t handle;
    uint64_t protocol_ns;
    uint32_t value;
    uint32_t auxiliary;
} report_t;

typedef struct {
    int report_fd;
    zipc_component_id_t victim;
    crash_phase_t phase;
} hook_context_t;

typedef struct {
    void *mapping;
    size_t mapping_size;
    zipc_platform_memory_t *memory;
    zipc_pool_t pool;
    zipc_transport_spsc_ring_t *rings[EDGE_COUNT];
    size_t ring_size;
    int event_fds[EDGE_COUNT];
} fixture_t;

typedef struct {
    uint64_t protocol_ns;
    uint64_t service_ns;
    uint64_t termination_ns;
    uint64_t respawn_ns;
    uint64_t restart_begin_ns;
    uint64_t links_ns;
    uint64_t input_reconcile_ns;
    uint64_t output_reconcile_ns;
    uint64_t adoption_ns;
    uint64_t handler_ns;
    uint64_t send_ns;
    uint64_t activation_ns;
} timing_t;

#define MAX_TIMELINE_EVENTS 32U

typedef struct {
    timing_t timing;
    report_t timeline[MAX_TIMELINE_EVENTS];
    uint32_t timeline_count;
    uint32_t adopted;
    uint32_t processed;
    uint32_t replayed;
    uint32_t republished;
    uint64_t buffer_id;
} iteration_result_t;

static uint64_t monotonic_raw_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0)
        return 0U;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static const char *node_name(zipc_component_id_t component)
{
    static const char *const names[] = {"?", "A", "B", "C", "D", "E"};
    return component <= NODE_COUNT ? names[component] : "?";
}

static const char *phase_name(crash_phase_t phase)
{
    switch (phase) {
    case CRASH_OWNED: return "OWNED_AFTER_RECEIVE";
    case CRASH_AFTER_PROCESS: return "AFTER_HANDLER";
    case CRASH_SEND_CLAIM: return "SEND_CLAIM";
    case CRASH_SEND_PRE_PUBLISH: return "SEND_PRE_PUBLISH";
    case CRASH_SEND_POST_PUBLISH: return "SEND_POST_PUBLISH";
    case CRASH_RECEIVE_CLAIM: return "RECEIVE_RESERVED";
    case CRASH_RECEIVE_POST_CLAIM: return "RECEIVE_POST_CLAIM";
    default: return "UNKNOWN";
    }
}

static const char *phase_description(crash_phase_t phase)
{
    switch (phase) {
    case CRASH_OWNED:
        return "relay owns the buffer; handler has not started";
    case CRASH_AFTER_PROCESS:
        return "handler completed; send has not started";
    case CRASH_SEND_CLAIM:
        return "send intent exists; slot is CLAIMING";
    case CRASH_SEND_PRE_PUBLISH:
        return "slot is TRANSFER; descriptor is not queued";
    case CRASH_SEND_POST_PUBLISH:
        return "descriptor is queued; notification is not sent";
    case CRASH_RECEIVE_CLAIM:
        return "receive intent exists; descriptor remains queued";
    case CRASH_RECEIVE_POST_CLAIM:
        return "relay owns the buffer; ring consumption is not committed";
    default:
        return "unknown checkpoint";
    }
}

static const char *report_name(report_type_t type)
{
    switch (type) {
    case REPORT_SOURCE_SENT: return "source sent buffer";
    case REPORT_CHECKPOINT: return "fault checkpoint reached";
    case REPORT_KILL_SENT: return "SIGKILL sent";
    case REPORT_KILL_CONFIRMED: return "old relay termination confirmed";
    case REPORT_RESTART_BEGIN: return "restart began";
    case REPORT_RESTART_REGISTERED: return "new recovery epoch registered";
    case REPORT_LINKS_OPENED: return "replacement links opened";
    case REPORT_INPUT_RECONCILED: return "input link reconciled";
    case REPORT_OUTPUT_RECONCILED: return "output link reconciled";
    case REPORT_BUFFER_ADOPTED: return "buffer adopted";
    case REPORT_PENDING_RECEIVED: return "pending input claimed";
    case REPORT_HANDLER_COMPLETED: return "handler completed";
    case REPORT_BUFFER_SENT: return "buffer transferred downstream";
    case REPORT_RECOVERY_ACTIVE: return "replacement became ACTIVE";
    case REPORT_SINK_RECEIVED: return "sink received and validated buffer";
    case REPORT_VERIFIED: return "rings drained and slots released";
    case REPORT_ERROR: return "child operation failed";
    default: return "unknown event";
    }
}

static const char *operation_name(operation_t operation)
{
    switch (operation) {
    case OP_NONE: return "none";
    case OP_LINK_OPEN: return "link-open";
    case OP_BUFFER_ALLOCATE: return "buffer-allocate";
    case OP_BUFFER_RECEIVE: return "buffer-receive";
    case OP_BUFFER_SEND: return "buffer-send";
    case OP_BUFFER_RELEASE: return "buffer-release";
    case OP_PAYLOAD_VALIDATE: return "payload-validate";
    case OP_RESTART_BEGIN: return "restart-begin";
    case OP_INPUT_RECONCILE: return "input-reconcile";
    case OP_OUTPUT_RECONCILE: return "output-reconcile";
    case OP_BUFFER_ADOPT: return "buffer-adopt";
    case OP_RESTART_FINISH: return "restart-finish";
    default: return "unknown";
    }
}

static const char *status_name(zipc_status_t status)
{
    switch (status) {
    case ZIPC_OK: return "ZIPC_OK";
    case ZIPC_ERR_INVALID_ARGUMENT: return "ZIPC_ERR_INVALID_ARGUMENT";
    case ZIPC_ERR_INVALID_POOL: return "ZIPC_ERR_INVALID_POOL";
    case ZIPC_ERR_NO_BUFFER: return "ZIPC_ERR_NO_BUFFER";
    case ZIPC_ERR_INVALID_HANDLE: return "ZIPC_ERR_INVALID_HANDLE";
    case ZIPC_ERR_STALE_HANDLE: return "ZIPC_ERR_STALE_HANDLE";
    case ZIPC_ERR_INVALID_STATE: return "ZIPC_ERR_INVALID_STATE";
    case ZIPC_ERR_NOT_OWNER: return "ZIPC_ERR_NOT_OWNER";
    case ZIPC_ERR_INVALID_RECEIVER: return "ZIPC_ERR_INVALID_RECEIVER";
    case ZIPC_ERR_SEQUENCE_MISMATCH: return "ZIPC_ERR_SEQUENCE_MISMATCH";
    case ZIPC_ERR_REGION_OVERFLOW: return "ZIPC_ERR_REGION_OVERFLOW";
    case ZIPC_ERR_UNSUPPORTED_MEMORY: return "ZIPC_ERR_UNSUPPORTED_MEMORY";
    case ZIPC_ERR_TRANSPORT: return "ZIPC_ERR_TRANSPORT";
    case ZIPC_ERR_PLATFORM: return "ZIPC_ERR_PLATFORM";
    case ZIPC_ERR_TIMEOUT: return "ZIPC_ERR_TIMEOUT";
    case ZIPC_ERR_HOP_LIMIT: return "ZIPC_ERR_HOP_LIMIT";
    case ZIPC_ERR_DEADLINE: return "ZIPC_ERR_DEADLINE";
    case ZIPC_ERR_COMPONENT_STALE: return "ZIPC_ERR_COMPONENT_STALE";
    case ZIPC_ERR_RECOVERY_REQUIRED: return "ZIPC_ERR_RECOVERY_REQUIRED";
    case ZIPC_ERR_INVALID_BUFFER: return "ZIPC_ERR_INVALID_BUFFER";
    case ZIPC_ERR_BUFFER_TOO_SMALL: return "ZIPC_ERR_BUFFER_TOO_SMALL";
    case ZIPC_ERR_ENTROPY_UNAVAILABLE: return "ZIPC_ERR_ENTROPY_UNAVAILABLE";
    case ZIPC_ERR_TRANSPORT_PUBLISHED: return "ZIPC_ERR_TRANSPORT_PUBLISHED";
    case ZIPC_ERR_RECOVERY_UNSUPPORTED:
        return "ZIPC_ERR_RECOVERY_UNSUPPORTED";
    default: return "ZIPC_ERR_UNKNOWN";
    }
}

static const char *slot_state_name(uint32_t state)
{
    switch ((zipc_slot_state_t)state) {
    case ZIPC_SLOT_FREE: return "FREE";
    case ZIPC_SLOT_CLAIMING: return "CLAIMING";
    case ZIPC_SLOT_OWNED: return "OWNED";
    case ZIPC_SLOT_TRANSFER: return "TRANSFER";
    case ZIPC_SLOT_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}

static const char *claim_name(uint64_t token)
{
    switch ((zipc_claim_kind_t)(uint8_t)token) {
    case ZIPC_CLAIM_NONE: return "NONE";
    case ZIPC_CLAIM_ALLOCATE: return "ALLOCATE";
    case ZIPC_CLAIM_SEND: return "SEND";
    case ZIPC_CLAIM_RECEIVE: return "RECEIVE";
    case ZIPC_CLAIM_RELEASE: return "RELEASE";
    case ZIPC_CLAIM_ROLLBACK: return "ROLLBACK";
    default: return "UNKNOWN";
    }
}

static void timeline_add(iteration_result_t *result, const report_t *report)
{
    if (result != NULL && report != NULL &&
        result->timeline_count < MAX_TIMELINE_EVENTS)
        result->timeline[result->timeline_count++] = *report;
}

static const report_t *timeline_find(const iteration_result_t *result,
                                     report_type_t type)
{
    for (uint32_t i = 0U; i < result->timeline_count; ++i)
        if (result->timeline[i].type == (uint32_t)type)
            return &result->timeline[i];
    return NULL;
}

static uint64_t elapsed_between(const report_t *start, const report_t *end)
{
    return start != NULL && end != NULL &&
           end->timestamp_ns >= start->timestamp_ns
         ? end->timestamp_ns - start->timestamp_ns : 0U;
}

static void calculate_breakdown(iteration_result_t *result)
{
    const report_t *killed = timeline_find(result, REPORT_KILL_CONFIRMED);
    const report_t *began = timeline_find(result, REPORT_RESTART_BEGIN);
    const report_t *registered = timeline_find(result,
                                                REPORT_RESTART_REGISTERED);
    const report_t *links = timeline_find(result, REPORT_LINKS_OPENED);
    const report_t *input = timeline_find(result, REPORT_INPUT_RECONCILED);
    const report_t *output = timeline_find(result, REPORT_OUTPUT_RECONCILED);
    const report_t *acquired = timeline_find(result, REPORT_BUFFER_ADOPTED);
    if (acquired == NULL)
        acquired = timeline_find(result, REPORT_PENDING_RECEIVED);
    const report_t *handled = timeline_find(result, REPORT_HANDLER_COMPLETED);
    const report_t *sent = timeline_find(result, REPORT_BUFFER_SENT);
    const report_t *active = timeline_find(result, REPORT_RECOVERY_ACTIVE);

    result->timing.respawn_ns = elapsed_between(killed, began);
    result->timing.restart_begin_ns = elapsed_between(began, registered);
    result->timing.links_ns = elapsed_between(registered, links);
    result->timing.input_reconcile_ns = elapsed_between(links, input);
    result->timing.output_reconcile_ns = elapsed_between(input, output);
    result->timing.adoption_ns = elapsed_between(output, acquired);
    result->timing.handler_ns = elapsed_between(acquired, handled);
    result->timing.send_ns = elapsed_between(handled, sent);
    result->timing.activation_ns = elapsed_between(
        sent != NULL ? sent : output, active);
}

static int write_report(int fd, const report_t *report)
{
    const uint8_t *bytes = (const uint8_t *)(const void *)report;
    size_t remaining = sizeof(*report);
    while (remaining != 0U) {
        const ssize_t count = write(fd, bytes, remaining);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        bytes += (size_t)count;
        remaining -= (size_t)count;
    }
    return 0;
}

static void child_fail(int fd, zipc_component_id_t component,
                       crash_phase_t phase, operation_t operation,
                       zipc_status_t status)
{
    const report_t report = {
        .type = REPORT_ERROR,
        .component = component,
        .phase = phase,
        .operation = operation,
        .status = status,
        .pid = (int32_t)getpid(),
        .timestamp_ns = monotonic_raw_ns(),
    };
    (void)write_report(fd, &report);
    _exit(1);
}

static void restart_fail(int fd, const report_t *stages, size_t stage_count,
                         zipc_component_id_t component, crash_phase_t phase,
                         operation_t operation, zipc_status_t status)
{
    for (size_t i = 0U; i < stage_count; ++i)
        (void)write_report(fd, &stages[i]);
    child_fail(fd, component, phase, operation, status);
}

static void stop_at_checkpoint(int fd, zipc_component_id_t component,
                               crash_phase_t phase)
{
    const report_t report = {
        .type = REPORT_CHECKPOINT,
        .component = component,
        .phase = phase,
        .status = ZIPC_OK,
        .pid = (int32_t)getpid(),
        .timestamp_ns = monotonic_raw_ns(),
    };
    if (write_report(fd, &report) != 0)
        _exit(1);
    (void)raise(SIGSTOP);
    _exit(1);
}

static crash_phase_t hook_phase(zipc_test_recovery_phase_t phase)
{
    switch (phase) {
    case ZIPC_TEST_RECOVERY_SEND_CLAIM: return CRASH_SEND_CLAIM;
    case ZIPC_TEST_RECOVERY_RECEIVE_CLAIM: return CRASH_RECEIVE_CLAIM;
    case ZIPC_TEST_RECOVERY_SEND_PRE_PUBLISH: return CRASH_SEND_PRE_PUBLISH;
    case ZIPC_TEST_RECOVERY_SEND_POST_PUBLISH: return CRASH_SEND_POST_PUBLISH;
    case ZIPC_TEST_RECOVERY_RECEIVE_POST_CLAIM:
        return CRASH_RECEIVE_POST_CLAIM;
    default: return 0;
    }
}

static void recovery_hook(zipc_test_recovery_phase_t phase,
                          zipc_component_id_t component, void *context)
{
    hook_context_t *hook = context;
    const crash_phase_t selected = hook_phase(phase);
    if (hook != NULL && component == hook->victim && selected == hook->phase)
        stop_at_checkpoint(hook->report_fd, component, selected);
}

static size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static int fixture_setup(fixture_t *fixture, bool eventfd_backend)
{
    memset(fixture, 0, sizeof(*fixture));
    for (uint32_t i = 0U; i < EDGE_COUNT; ++i)
        fixture->event_fds[i] = -1;

    const size_t control_size = zipc_pool_required_control_size(SLOT_COUNT);
    const size_t payload_offset = align_up(control_size, 64U);
    const size_t payload_size = zipc_pool_required_payload_size(
        SLOT_COUNT, SLOT_CAPACITY, 0U, 64U);
    fixture->mapping_size = payload_offset + payload_size;
    fixture->mapping = mmap(NULL, fixture->mapping_size,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (fixture->mapping == MAP_FAILED)
        return -1;

    zipc_platform_memory_config_t memory_config = {
        .type = ZIPC_SHM_PREALLOCATED,
        .size = fixture->mapping_size,
        .backend.preallocated = {
            .address = fixture->mapping,
            .physical_address = ZIPC_PHYS_ADDR_INVALID,
            .capabilities = ZIPC_MEM_CAP_CPU_READ | ZIPC_MEM_CAP_CPU_WRITE |
                            ZIPC_MEM_CAP_ATOMIC32 | ZIPC_MEM_CAP_ATOMIC64,
        },
    };
    if (zipc_platform_memory_open(&fixture->memory, &memory_config) != ZIPC_OK)
        return -1;
    zipc_pool_config_t pool_config = {
        .control_memory = fixture->memory,
        .payload_memory = fixture->memory,
        .payload_offset = payload_offset,
        .slot_count = SLOT_COUNT,
        .slot_capacity = SLOT_CAPACITY,
        .payload_alignment = 64U,
        .pool_id = 300U,
    };
    if (zipc_pool_format(&pool_config) != ZIPC_OK ||
        zipc_pool_attach(&fixture->pool, &pool_config) != ZIPC_OK)
        return -1;
    for (zipc_component_id_t component = 1U; component <= NODE_COUNT;
         ++component) {
        uint32_t epoch = 0U;
        if (zipc_component_register(&fixture->pool, component, &epoch) !=
                ZIPC_OK || epoch != 1U)
            return -1;
    }

    fixture->ring_size = zipc_transport_spsc_ring_size(RING_DEPTH);
    for (uint32_t i = 0U; i < EDGE_COUNT; ++i) {
        fixture->rings[i] = mmap(NULL, fixture->ring_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (fixture->rings[i] == MAP_FAILED ||
            zipc_transport_spsc_ring_initialize(fixture->rings[i],
                                                 RING_DEPTH) != ZIPC_OK)
            return -1;
        if (eventfd_backend) {
            fixture->event_fds[i] = eventfd(0, EFD_CLOEXEC);
            if (fixture->event_fds[i] < 0)
                return -1;
        }
    }
    return 0;
}

static void fixture_destroy(fixture_t *fixture)
{
    for (uint32_t i = 0U; i < EDGE_COUNT; ++i) {
        if (fixture->event_fds[i] >= 0)
            (void)close(fixture->event_fds[i]);
        if (fixture->rings[i] != NULL && fixture->rings[i] != MAP_FAILED)
            (void)munmap(fixture->rings[i], fixture->ring_size);
    }
    if (fixture->memory != NULL)
        zipc_platform_memory_close(fixture->memory);
    if (fixture->mapping != NULL && fixture->mapping != MAP_FAILED)
        (void)munmap(fixture->mapping, fixture->mapping_size);
}

static zipc_status_t create_link(fixture_t *fixture, uint32_t edge,
                                 zipc_component_id_t local,
                                 zipc_component_id_t remote, uint32_t epoch,
                                 zipc_link_role_t role, bool eventfd_backend,
                                 zipc_link_t **link)
{
    zipc_platform_transport_config_t transport = {
        .type = eventfd_backend ? ZIPC_TRANSPORT_SHM_RING_EVENTFD
                                : ZIPC_TRANSPORT_SHM_RING_POLLING,
        .platform_handle = fixture->rings[edge],
        .receive_handle = eventfd_backend
            ? (void *)(intptr_t)fixture->event_fds[edge] : NULL,
        .ring_depth = RING_DEPTH,
        .poll_timeout_ns = UINT64_C(5000000000),
    };
    const zipc_link_config_t config = {
        .pool = &fixture->pool,
        .local_component = local,
        .remote_component = remote,
        .link_id = UINT64_C(0x3000) + edge,
        .local_epoch = epoch,
        .role = role,
        .hop_limit = 16U,
        .transport = transport,
    };
    return zipc_link_create(link, &config);
}

static void process_relay(zipc_buffer_t *buffer,
                          zipc_component_id_t component)
{
    test_payload_t *payload = zipc_buffer_data(buffer);
    if (payload != NULL)
        payload->relay_mask |= UINT32_C(1) << (component - 2U);
}

static void run_source(fixture_t *fixture, uint32_t iteration,
                       bool eventfd_backend, int report_fd)
{
    zipc_link_t *tx = NULL;
    zipc_status_t status = create_link(fixture, 0U, 1U, 2U, 1U,
                                       ZIPC_LINK_ROLE_PRODUCER,
                                       eventfd_backend, &tx);
    if (status != ZIPC_OK)
        child_fail(report_fd, 1U, 0, OP_LINK_OPEN, status);
    zipc_buffer_t buffer;
    status = zipc_buffer_alloc(tx, sizeof(test_payload_t), &buffer, NULL);
    if (status != ZIPC_OK)
        child_fail(report_fd, 1U, 0, OP_BUFFER_ALLOCATE, status);
    test_payload_t *payload = zipc_buffer_data(&buffer);
    if (payload == NULL)
        child_fail(report_fd, 1U, 0, OP_PAYLOAD_VALIDATE,
                   ZIPC_ERR_INVALID_BUFFER);
    *payload = (test_payload_t){
        .magic = PAYLOAD_MAGIC,
        .iteration = iteration,
    };
    const zipc_buffer_id_t buffer_id = zipc_buffer_id(&buffer);
    const zipc_handle_t handle = zipc_buffer_handle(&buffer);
    status = zipc_send(tx, &buffer);
    if (status != ZIPC_OK)
        child_fail(report_fd, 1U, 0, OP_BUFFER_SEND, status);
    const report_t report = {
        .type = REPORT_SOURCE_SENT,
        .component = 1U,
        .status = ZIPC_OK,
        .pid = (int32_t)getpid(),
        .timestamp_ns = monotonic_raw_ns(),
        .buffer_id = buffer_id,
        .handle = handle,
    };
    if (write_report(report_fd, &report) != 0)
        _exit(1);
    zipc_link_destroy(tx);
    _exit(0);
}

static void run_sink(fixture_t *fixture, uint32_t iteration,
                     bool eventfd_backend, int report_fd)
{
    zipc_link_t *rx = NULL;
    zipc_status_t status = create_link(fixture, 3U, 5U, 4U, 1U,
                                       ZIPC_LINK_ROLE_CONSUMER,
                                       eventfd_backend, &rx);
    if (status != ZIPC_OK)
        child_fail(report_fd, 5U, 0, OP_LINK_OPEN, status);
    zipc_buffer_t buffer;
    status = zipc_recv(rx, &buffer);
    if (status != ZIPC_OK)
        child_fail(report_fd, 5U, 0, OP_BUFFER_RECEIVE, status);
    const test_payload_t *payload = zipc_buffer_const_data(&buffer);
    if (payload == NULL || payload->magic != PAYLOAD_MAGIC ||
        payload->iteration != iteration || payload->relay_mask != 7U)
        child_fail(report_fd, 5U, 0, OP_PAYLOAD_VALIDATE,
                   ZIPC_ERR_INVALID_BUFFER);
    const report_t report = {
        .type = REPORT_SINK_RECEIVED,
        .component = 5U,
        .status = ZIPC_OK,
        .pid = (int32_t)getpid(),
        .timestamp_ns = monotonic_raw_ns(),
        .buffer_id = zipc_buffer_id(&buffer),
        .handle = zipc_buffer_handle(&buffer),
    };
    status = zipc_buffer_release(&buffer);
    if (status != ZIPC_OK)
        child_fail(report_fd, 5U, 0, OP_BUFFER_RELEASE, status);
    if (write_report(report_fd, &report) != 0)
        _exit(1);
    zipc_link_destroy(rx);
    _exit(0);
}

static void run_relay(fixture_t *fixture, zipc_component_id_t component,
                      zipc_component_id_t victim, crash_phase_t phase,
                      bool eventfd_backend, int report_fd)
{
    hook_context_t hook = {
        .report_fd = report_fd,
        .victim = victim,
        .phase = phase,
    };
    zipc_test_recovery_set_hook(recovery_hook, &hook);
    zipc_link_t *rx = NULL;
    zipc_link_t *tx = NULL;
    zipc_status_t status = create_link(
        fixture, component - 2U, component, component - 1U, 1U,
        ZIPC_LINK_ROLE_CONSUMER, eventfd_backend, &rx);
    if (status == ZIPC_OK)
        status = create_link(
            fixture, component - 1U, component, component + 1U, 1U,
            ZIPC_LINK_ROLE_PRODUCER, eventfd_backend, &tx);
    if (status != ZIPC_OK)
        child_fail(report_fd, component, phase, OP_LINK_OPEN, status);

    zipc_buffer_t buffer;
    status = zipc_recv(rx, &buffer);
    if (status != ZIPC_OK)
        child_fail(report_fd, component, phase, OP_BUFFER_RECEIVE, status);
    if (component == victim && phase == CRASH_OWNED)
        stop_at_checkpoint(report_fd, component, phase);
    process_relay(&buffer, component);
    if (component == victim && phase == CRASH_AFTER_PROCESS)
        stop_at_checkpoint(report_fd, component, phase);
    status = zipc_send(tx, &buffer);
    if (status != ZIPC_OK)
        child_fail(report_fd, component, phase, OP_BUFFER_SEND, status);
    zipc_link_destroy(tx);
    zipc_link_destroy(rx);
    _exit(0);
}

static void run_restart(fixture_t *fixture, zipc_component_id_t component,
                        crash_phase_t phase, bool eventfd_backend, int report_fd)
{
    enum { MAX_RESTART_STAGES = 12 };
    zipc_test_recovery_set_hook(NULL, NULL);
    const uint64_t begin_ns = monotonic_raw_ns();
    zipc_restart_t restart;
    zipc_status_t status = zipc_component_restart_begin(
        &fixture->pool, component, 1U, &restart);
    if (status != ZIPC_OK)
        child_fail(report_fd, component, phase, OP_RESTART_BEGIN, status);
    report_t stages[MAX_RESTART_STAGES];
    size_t stage_count = 0U;
    report_t stage = {
        .type = REPORT_RESTART_BEGIN,
        .component = component,
        .phase = phase,
        .status = ZIPC_OK,
        .pid = (int32_t)getpid(),
        .old_epoch = 1U,
        .new_epoch = restart.epoch,
        .timestamp_ns = begin_ns,
    };
    stages[stage_count++] = stage;
    stage = (report_t){
        .type = REPORT_RESTART_REGISTERED,
        .component = component,
        .phase = phase,
        .status = ZIPC_OK,
        .pid = (int32_t)getpid(),
        .old_epoch = 1U,
        .new_epoch = restart.epoch,
        .timestamp_ns = monotonic_raw_ns(),
    };
    stages[stage_count++] = stage;
    zipc_link_t *rx = NULL;
    zipc_link_t *tx = NULL;
    status = create_link(fixture, component - 2U, component, component - 1U,
                         restart.epoch, ZIPC_LINK_ROLE_CONSUMER,
                         eventfd_backend, &rx);
    if (status == ZIPC_OK)
        status = create_link(fixture, component - 1U, component, component + 1U,
                             restart.epoch, ZIPC_LINK_ROLE_PRODUCER,
                             eventfd_backend, &tx);
    if (status != ZIPC_OK)
        restart_fail(report_fd, stages, stage_count, component, phase,
                     OP_LINK_OPEN, status);
    stage = (report_t){
        .type = REPORT_LINKS_OPENED,
        .component = component,
        .phase = phase,
        .status = ZIPC_OK,
        .pid = (int32_t)getpid(),
        .old_epoch = 1U,
        .new_epoch = restart.epoch,
        .timestamp_ns = monotonic_raw_ns(),
    };
    stages[stage_count++] = stage;

    uint32_t incoming = 0U;
    uint32_t outgoing = 0U;
    status = zipc_link_reconcile(rx, &restart, &incoming);
    if (status != ZIPC_OK)
        restart_fail(report_fd, stages, stage_count, component, phase,
                     OP_INPUT_RECONCILE, status);
    stage = (report_t){
        .type = REPORT_INPUT_RECONCILED,
        .component = component,
        .phase = phase,
        .status = ZIPC_OK,
        .pid = (int32_t)getpid(),
        .old_epoch = 1U,
        .new_epoch = restart.epoch,
        .timestamp_ns = monotonic_raw_ns(),
        .value = incoming,
    };
    stages[stage_count++] = stage;
    status = zipc_link_reconcile(tx, &restart, &outgoing);
    if (status != ZIPC_OK)
        restart_fail(report_fd, stages, stage_count, component, phase,
                     OP_OUTPUT_RECONCILE, status);
    stage = (report_t){
        .type = REPORT_OUTPUT_RECONCILED,
        .component = component,
        .phase = phase,
        .status = ZIPC_OK,
        .pid = (int32_t)getpid(),
        .old_epoch = 1U,
        .new_epoch = restart.epoch,
        .timestamp_ns = monotonic_raw_ns(),
        .value = outgoing,
    };
    stages[stage_count++] = stage;

    zipc_buffer_t buffer;
    bool adopted = false;
    for (;;) {
        status = zipc_component_restart_next(&restart, &buffer);
        if (status == ZIPC_ERR_NO_BUFFER) {
            status = ZIPC_OK;
            break;
        }
        if (status != ZIPC_OK)
            restart_fail(report_fd, stages, stage_count, component, phase,
                         OP_BUFFER_ADOPT, status);
        adopted = true;
        stage = (report_t){
            .type = REPORT_BUFFER_ADOPTED,
            .component = component,
            .phase = phase,
            .status = ZIPC_OK,
            .pid = (int32_t)getpid(),
            .old_epoch = 1U,
            .new_epoch = restart.epoch,
            .timestamp_ns = monotonic_raw_ns(),
            .buffer_id = zipc_buffer_id(&buffer),
            .handle = zipc_buffer_handle(&buffer),
        };
        stages[stage_count++] = stage;
        process_relay(&buffer, component);
        stage = (report_t){
            .type = REPORT_HANDLER_COMPLETED,
            .component = component,
            .phase = phase,
            .status = ZIPC_OK,
            .pid = (int32_t)getpid(),
            .old_epoch = 1U,
            .new_epoch = restart.epoch,
            .timestamp_ns = monotonic_raw_ns(),
            .buffer_id = zipc_buffer_id(&buffer),
            .handle = zipc_buffer_handle(&buffer),
            .value = phase != CRASH_OWNED &&
                     phase != CRASH_RECEIVE_POST_CLAIM ? 1U : 0U,
        };
        stages[stage_count++] = stage;
        status = zipc_send(tx, &buffer);
        if (status != ZIPC_OK)
            restart_fail(report_fd, stages, stage_count, component, phase,
                         OP_BUFFER_SEND, status);
        stage.type = REPORT_BUFFER_SENT;
        stage.timestamp_ns = monotonic_raw_ns();
        stages[stage_count++] = stage;
    }
    if (status == ZIPC_OK && !adopted &&
        phase != CRASH_SEND_PRE_PUBLISH &&
        phase != CRASH_SEND_POST_PUBLISH) {
        status = zipc_recv(rx, &buffer);
        if (status != ZIPC_OK)
            restart_fail(report_fd, stages, stage_count, component, phase,
                         OP_BUFFER_RECEIVE, status);
        stage = (report_t){
            .type = REPORT_PENDING_RECEIVED,
            .component = component,
            .phase = phase,
            .status = ZIPC_OK,
            .pid = (int32_t)getpid(),
            .old_epoch = 1U,
            .new_epoch = restart.epoch,
            .timestamp_ns = monotonic_raw_ns(),
            .buffer_id = zipc_buffer_id(&buffer),
            .handle = zipc_buffer_handle(&buffer),
        };
        stages[stage_count++] = stage;
        process_relay(&buffer, component);
        stage.type = REPORT_HANDLER_COMPLETED;
        stage.timestamp_ns = monotonic_raw_ns();
        stages[stage_count++] = stage;
        status = zipc_send(tx, &buffer);
        if (status != ZIPC_OK)
            restart_fail(report_fd, stages, stage_count, component, phase,
                         OP_BUFFER_SEND, status);
        stage.type = REPORT_BUFFER_SENT;
        stage.timestamp_ns = monotonic_raw_ns();
        stages[stage_count++] = stage;
    }
    status = zipc_component_restart_finish(&restart);
    if (status != ZIPC_OK)
        restart_fail(report_fd, stages, stage_count, component, phase,
                     OP_RESTART_FINISH, status);
    const uint64_t end_ns = monotonic_raw_ns();
    const report_t report = {
        .type = REPORT_RECOVERY_ACTIVE,
        .component = component,
        .phase = phase,
        .status = ZIPC_OK,
        .pid = (int32_t)getpid(),
        .old_epoch = 1U,
        .new_epoch = 2U,
        .timestamp_ns = end_ns,
        .protocol_ns = end_ns - begin_ns,
        .value = outgoing,
    };
    for (size_t i = 0U; i < stage_count; ++i) {
        if (write_report(report_fd, &stages[i]) != 0)
            _exit(1);
    }
    if (write_report(report_fd, &report) != 0)
        _exit(1);
    zipc_link_destroy(tx);
    zipc_link_destroy(rx);
    _exit(0);
}

static int read_report_timeout(int fd, report_t *report, uint32_t timeout_ms)
{
    struct pollfd descriptor = {.fd = fd, .events = POLLIN};
    int ready;
    do {
        ready = poll(&descriptor, 1U, (int)timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0)
        return -1;
    uint8_t *bytes = (uint8_t *)(void *)report;
    size_t remaining = sizeof(*report);
    while (remaining != 0U) {
        const ssize_t count = read(fd, bytes, remaining);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        bytes += (size_t)count;
        remaining -= (size_t)count;
    }
    return 0;
}

static int wait_child(pid_t pid)
{
    int status = 0;
    if (pid <= 0)
        return 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

typedef enum {
    CHILD_STOPPED,
    CHILD_REAPED,
    CHILD_STOP_FAILED,
} child_stop_result_t;

static child_stop_result_t stop_child(pid_t pid)
{
    if (pid <= 0)
        return CHILD_REAPED;
    if (kill(pid, SIGSTOP) != 0) {
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(pid, &status, WNOHANG);
        } while (waited < 0 && errno == EINTR);
        return waited == pid || (waited < 0 && errno == ECHILD)
             ? CHILD_REAPED : CHILD_STOP_FAILED;
    }
    int status = 0;
    while (waitpid(pid, &status, WUNTRACED) < 0) {
        if (errno != EINTR)
            return errno == ECHILD ? CHILD_REAPED : CHILD_STOP_FAILED;
    }
    return WIFSTOPPED(status) ? CHILD_STOPPED : CHILD_REAPED;
}

static void drain_reports(int fd, iteration_result_t *result)
{
    report_t report;
    while (read_report_timeout(fd, &report, 0U) == 0)
        timeline_add(result, &report);
}

static int compare_reports(const void *left, const void *right)
{
    const report_t *a = left;
    const report_t *b = right;
    return a->timestamp_ns < b->timestamp_ns
         ? -1 : a->timestamp_ns > b->timestamp_ns ? 1 : 0;
}

static void print_timeline(FILE *stream, const iteration_result_t *result)
{
    if (result->timeline_count == 0U) {
        fputs("    (no events reported)\n", stream);
        return;
    }
    report_t ordered[MAX_TIMELINE_EVENTS];
    memcpy(ordered, result->timeline,
           result->timeline_count * sizeof(ordered[0]));
    qsort(ordered, result->timeline_count, sizeof(ordered[0]),
          compare_reports);
    const report_t *checkpoint = timeline_find(result, REPORT_CHECKPOINT);
    const uint64_t baseline = checkpoint != NULL
                            ? checkpoint->timestamp_ns
                            : ordered[0].timestamp_ns;

    for (uint32_t i = 0U; i < result->timeline_count; ++i) {
        const report_t *event = &ordered[i];
        const double relative_us = event->timestamp_ns >= baseline
            ? (double)(event->timestamp_ns - baseline) / 1000.0
            : -(double)(baseline - event->timestamp_ns) / 1000.0;
        fprintf(stream, "    %c%9.3f us  %-2s  %s",
                relative_us >= 0.0 ? '+' : '-',
                relative_us >= 0.0 ? relative_us : -relative_us,
                event->type == REPORT_KILL_SENT ||
                event->type == REPORT_KILL_CONFIRMED ||
                event->type == REPORT_VERIFIED
                    ? "TEST" : node_name((zipc_component_id_t)event->component),
                report_name((report_type_t)event->type));
        switch ((report_type_t)event->type) {
        case REPORT_SOURCE_SENT:
        case REPORT_BUFFER_ADOPTED:
        case REPORT_PENDING_RECEIVED:
        case REPORT_BUFFER_SENT:
        case REPORT_SINK_RECEIVED:
            fprintf(stream, ": buffer=0x%016" PRIx64 " handle=0x%016" PRIx64,
                    event->buffer_id, event->handle);
            break;
        case REPORT_CHECKPOINT:
            fprintf(stream, ": %s", phase_name((crash_phase_t)event->phase));
            break;
        case REPORT_KILL_SENT:
        case REPORT_KILL_CONFIRMED:
            fprintf(stream, ": relay=%s pid=%d",
                    node_name((zipc_component_id_t)event->component), event->pid);
            break;
        case REPORT_RESTART_BEGIN:
        case REPORT_RESTART_REGISTERED:
            fprintf(stream, ": epoch=%u->%u pid=%d",
                     event->old_epoch, event->new_epoch, event->pid);
            break;
        case REPORT_INPUT_RECONCILED:
            fputs(": complete", stream);
            break;
        case REPORT_OUTPUT_RECONCILED:
            fprintf(stream, ": republished=%u", event->value);
            break;
        case REPORT_RECOVERY_ACTIVE:
            fprintf(stream, ": epoch=%u protocol=%.3f us",
                    event->new_epoch, event->protocol_ns / 1000.0);
            break;
        case REPORT_HANDLER_COMPLETED:
            fprintf(stream, ": replay=%s", event->value != 0U ? "yes" : "no");
            break;
        case REPORT_ERROR:
            fprintf(stream, ": operation=%s status=%s(%d)",
                    operation_name((operation_t)event->operation),
                    status_name((zipc_status_t)event->status), event->status);
            break;
        default:
            break;
        }
        fputc('\n', stream);
    }
}

static int run_iteration(uint32_t iteration, zipc_component_id_t victim,
                         crash_phase_t phase, bool eventfd_backend,
                         uint32_t timeout_ms, iteration_result_t *result,
                         uint64_t seed, uint32_t total_iterations)
{
    memset(result, 0, sizeof(*result));
    const char *failure_reason = "unknown failure";
    report_t failure_report = {0};
    bool has_failure_report = false;
    fixture_t fixture;
    if (fixture_setup(&fixture, eventfd_backend) != 0) {
        fprintf(stderr, "FAIL: fixture setup failed\n");
        return -1;
    }
    int reports[2];
    if (pipe(reports) != 0) {
        fixture_destroy(&fixture);
        return -1;
    }

    pid_t pids[NODE_COUNT] = {0};
    pid_t restart_pid = 0;
    for (int index = (int)NODE_COUNT - 1; index >= 0; --index) {
        const pid_t pid = fork();
        if (pid < 0) {
            failure_reason = "failed to fork chain node";
            goto fail;
        }
        if (pid == 0) {
            (void)close(reports[0]);
            const zipc_component_id_t component =
                (zipc_component_id_t)(index + 1);
            if (component == 1U)
                run_source(&fixture, iteration, eventfd_backend, reports[1]);
            if (component == 5U)
                run_sink(&fixture, iteration, eventfd_backend, reports[1]);
            run_relay(&fixture, component, victim, phase, eventfd_backend,
                      reports[1]);
        }
        pids[index] = pid;
    }

    bool checkpoint_seen = false;
    bool sink_seen = false;
    report_t sink_report = {0};
    while (!checkpoint_seen) {
        report_t report;
        if (read_report_timeout(reports[0], &report, timeout_ms) != 0) {
            failure_reason = "timed out waiting for fault checkpoint";
            goto fail;
        }
        timeline_add(result, &report);
        if (report.type == REPORT_ERROR) {
            failure_reason = "chain child reported an operation failure";
            failure_report = report;
            has_failure_report = true;
            goto fail;
        }
        if (report.type == REPORT_SINK_RECEIVED) {
            sink_seen = true;
            sink_report = report;
        } else if (report.type == REPORT_CHECKPOINT &&
                   report.component == victim && report.phase == (uint32_t)phase) {
            checkpoint_seen = true;
        }
    }

    const uint64_t kill_ns = monotonic_raw_ns();
    const pid_t victim_pid = pids[victim - 1U];
    report_t local = {
        .type = REPORT_KILL_SENT,
        .component = victim,
        .phase = phase,
        .pid = (int32_t)victim_pid,
        .status = ZIPC_OK,
        .timestamp_ns = kill_ns,
    };
    if (kill(victim_pid, SIGKILL) != 0) {
        failure_reason = "failed to send SIGKILL to selected relay";
        goto fail;
    }
    timeline_add(result, &local);
    int victim_status = 0;
    const pid_t victim_waited = waitpid(victim_pid, &victim_status, 0);
    if (victim_waited == victim_pid)
        pids[victim - 1U] = 0;
    if (victim_waited != victim_pid || !WIFSIGNALED(victim_status) ||
        WTERMSIG(victim_status) != SIGKILL) {
        failure_reason = "selected relay did not terminate with SIGKILL";
        goto fail;
    }
    const uint64_t kill_confirmed_ns = monotonic_raw_ns();
    local.type = REPORT_KILL_CONFIRMED;
    local.timestamp_ns = kill_confirmed_ns;
    timeline_add(result, &local);

    restart_pid = fork();
    if (restart_pid < 0) {
        failure_reason = "failed to fork replacement relay";
        goto fail;
    }
    if (restart_pid == 0) {
        (void)close(reports[0]);
        run_restart(&fixture, victim, phase, eventfd_backend, reports[1]);
    }

    bool recovery_seen = false;
    report_t recovery_report = {0};
    while (!recovery_seen || !sink_seen) {
        report_t report;
        if (read_report_timeout(reports[0], &report, timeout_ms) != 0) {
            failure_reason = "timed out waiting for recovery or sink delivery";
            goto fail_restart;
        }
        timeline_add(result, &report);
        if (report.type == REPORT_ERROR) {
            failure_reason = "replacement or sink reported an operation failure";
            failure_report = report;
            has_failure_report = true;
            goto fail_restart;
        }
        if (report.type == REPORT_RECOVERY_ACTIVE) {
            recovery_seen = true;
            recovery_report = report;
        } else if (report.type == REPORT_SINK_RECEIVED) {
            if (sink_seen) {
                failure_reason = "sink reported duplicate delivery";
                goto fail_restart;
            }
            sink_seen = true;
            sink_report = report;
        }
    }

    const int restart_result = wait_child(restart_pid);
    restart_pid = 0;
    if (restart_result != 0) {
        failure_reason = "replacement relay exited unsuccessfully";
        goto fail;
    }
    for (uint32_t i = 0U; i < NODE_COUNT; ++i) {
        const int child_result = wait_child(pids[i]);
        pids[i] = 0;
        if (child_result != 0) {
            failure_reason = "chain node exited unsuccessfully";
            goto fail;
        }
    }
    drain_reports(reports[0], result);
    const report_t *late_error = timeline_find(result, REPORT_ERROR);
    if (late_error != NULL) {
        failure_reason = "chain child reported a late operation failure";
        failure_report = *late_error;
        has_failure_report = true;
        goto fail;
    }
    for (uint32_t i = 0U; i < EDGE_COUNT; ++i)
        if (atomic_load_explicit(&fixture.rings[i]->producer,
                                 memory_order_acquire) !=
            atomic_load_explicit(&fixture.rings[i]->consumer,
                                 memory_order_acquire)) {
            failure_reason = "descriptor ring was not drained";
            goto fail;
        }
    for (uint32_t i = 0U; i < SLOT_COUNT; ++i)
        if (atomic_load_explicit(&fixture.pool.controls[i].state,
                                 memory_order_acquire) != ZIPC_SLOT_FREE) {
            failure_reason = "pool contains a stranded non-FREE slot";
            goto fail;
        }

    local = (report_t){
        .type = REPORT_VERIFIED,
        .component = victim,
        .phase = phase,
        .status = ZIPC_OK,
        .pid = (int32_t)getpid(),
        .timestamp_ns = monotonic_raw_ns(),
    };
    timeline_add(result, &local);
    result->timing.protocol_ns = recovery_report.protocol_ns;
    result->timing.service_ns = sink_report.timestamp_ns > kill_ns
                              ? sink_report.timestamp_ns - kill_ns : 0U;
    result->timing.termination_ns = kill_confirmed_ns - kill_ns;
    result->buffer_id = sink_report.buffer_id;
    result->republished = recovery_report.value;
    for (uint32_t i = 0U; i < result->timeline_count; ++i) {
        if (result->timeline[i].type == REPORT_BUFFER_ADOPTED)
            result->adopted++;
        if (result->timeline[i].type == REPORT_HANDLER_COMPLETED) {
            result->processed++;
            result->replayed += result->timeline[i].value != 0U ? 1U : 0U;
        }
    }
    calculate_breakdown(result);
    (void)close(reports[0]);
    (void)close(reports[1]);
    fixture_destroy(&fixture);
    return 0;

fail_restart:
fail:
    if (restart_pid > 0 && stop_child(restart_pid) == CHILD_REAPED)
        restart_pid = 0;
    for (uint32_t i = 0U; i < NODE_COUNT; ++i) {
        if (pids[i] > 0 && stop_child(pids[i]) == CHILD_REAPED)
            pids[i] = 0;
    }
    drain_reports(reports[0], result);
    fprintf(stderr,
            "FAIL: %s\n"
            "  backend=%s iteration=%u relay=%s(component=%u) checkpoint=%s\n"
            "  meaning: %s\n"
            "  replay: ./build/tests/zipc-recovery-chain-linux "
            "--iterations %u --seed %" PRIu64
            " --max-recovery-ms %u --verbose\n",
            failure_reason, eventfd_backend ? "ring-eventfd" : "ring-polling",
            iteration, node_name(victim), (unsigned)victim, phase_name(phase),
            phase_description(phase), total_iterations, seed, timeout_ms);
    if (has_failure_report) {
        fprintf(stderr,
                "  child_error: node=%s pid=%d operation=%s status=%s(%d)\n",
                node_name((zipc_component_id_t)failure_report.component),
                failure_report.pid,
                operation_name((operation_t)failure_report.operation),
                status_name((zipc_status_t)failure_report.status),
                failure_report.status);
    }
    fputs("  timeline:\n", stderr);
    print_timeline(stderr, result);
    fputs("  components:\n", stderr);
    for (zipc_component_id_t component = 1U; component <= NODE_COUNT;
         ++component) {
        zipc_component_snapshot_t snapshot;
        if (zipc_component_snapshot(&fixture.pool, component, &snapshot) ==
            ZIPC_OK) {
            const char *state = snapshot.state == ZIPC_COMPONENT_INACTIVE
                              ? "INACTIVE"
                              : snapshot.state == ZIPC_COMPONENT_RECOVERING
                              ? "RECOVERING" : "ACTIVE";
            fprintf(stderr, "    %s: component=%u epoch=%u state=%s\n",
                    node_name(component), (unsigned)component, snapshot.epoch,
                    state);
        }
    }
    fputs("  rings:\n", stderr);
    for (uint32_t i = 0U; i < EDGE_COUNT; ++i) {
        const uint32_t producer = atomic_load_explicit(
            &fixture.rings[i]->producer, memory_order_acquire);
        const uint32_t consumer = atomic_load_explicit(
            &fixture.rings[i]->consumer, memory_order_acquire);
        fprintf(stderr, "    %s%s: producer=%u consumer=%u occupancy=%u\n",
                node_name((zipc_component_id_t)(i + 1U)),
                node_name((zipc_component_id_t)(i + 2U)), producer, consumer,
                producer - consumer);
    }
    fputs("  active slots:\n", stderr);
    for (uint32_t i = 0U; i < SLOT_COUNT; ++i) {
        zipc_slot_control_t *slot = &fixture.pool.controls[i];
        const uint32_t state = atomic_load_explicit(&slot->state,
                                                    memory_order_acquire);
        if (state != ZIPC_SLOT_FREE) {
            fprintf(stderr,
                    "    slot=%u state=%s owner=%s/%u epoch=%u generation=%u "
                    "next=%s/%u sequence=%u claim=%s link=0x%" PRIx64 "\n",
                    i, slot_state_name(state), node_name(slot->owner_id),
                    (unsigned)slot->owner_id,
                    slot->owner_epoch,
                    atomic_load_explicit(&slot->generation,
                                         memory_order_relaxed),
                    node_name(slot->next_owner_id),
                    (unsigned)slot->next_owner_id,
                    slot->transfer_sequence,
                    claim_name(atomic_load_explicit(&slot->claim_token,
                                                    memory_order_acquire)),
                    slot->transfer_link_id);
        }
    }
    if (restart_pid > 0) {
        (void)kill(restart_pid, SIGKILL);
        (void)waitpid(restart_pid, NULL, 0);
    }
    for (uint32_t i = 0U; i < NODE_COUNT; ++i) {
        if (pids[i] > 0) {
            (void)kill(pids[i], SIGKILL);
            (void)waitpid(pids[i], NULL, 0);
        }
    }
    (void)close(reports[0]);
    (void)close(reports[1]);
    fixture_destroy(&fixture);
    return -1;
}

static uint64_t random_next(uint64_t *state)
{
    uint64_t value = *state;
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    *state = value;
    return value;
}

static int compare_u64(const void *left, const void *right)
{
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static void print_summary(const char *backend, iteration_result_t *results,
                          uint32_t count)
{
    uint64_t *protocol = calloc(count, sizeof(*protocol));
    uint64_t *service = calloc(count, sizeof(*service));
    if (protocol == NULL || service == NULL)
        exit(1);
    uint64_t protocol_sum = 0U;
    uint64_t service_sum = 0U;
    for (uint32_t i = 0U; i < count; ++i) {
        protocol[i] = results[i].timing.protocol_ns;
        service[i] = results[i].timing.service_ns;
        protocol_sum += protocol[i];
        service_sum += service[i];
    }
    qsort(protocol, count, sizeof(*protocol), compare_u64);
    qsort(service, count, sizeof(*service), compare_u64);
    const uint32_t p50 = (count - 1U) / 2U;
    const uint32_t p95 = ((count * 95U + 99U) / 100U) - 1U;
    printf("\n%s: %u/%u iterations passed\n", backend, count, count);
    printf("  protocol recovery: min=%.3f mean=%.3f p50=%.3f p95=%.3f "
           "max=%.3f us\n",
           protocol[0] / 1000.0, protocol_sum / (double)count / 1000.0,
           protocol[p50] / 1000.0, protocol[p95] / 1000.0,
           protocol[count - 1U] / 1000.0);
    printf("  service recovery:  min=%.3f mean=%.3f p50=%.3f p95=%.3f "
           "max=%.3f us\n",
           service[0] / 1000.0, service_sum / (double)count / 1000.0,
           service[p50] / 1000.0, service[p95] / 1000.0,
           service[count - 1U] / 1000.0);
    free(service);
    free(protocol);
}

static void print_iteration(const char *backend, uint32_t iteration,
                            uint32_t total, zipc_component_id_t victim,
                            crash_phase_t phase,
                            const iteration_result_t *result,
                            verbosity_t verbosity)
{
    if (verbosity == VERBOSITY_QUIET)
        return;
    printf("\n[%02u/%02u %s] relay=%s component=%u\n",
           iteration + 1U, total, backend, node_name(victim),
           (unsigned)victim);
    printf("  fault: %s\n", phase_name(phase));
    printf("  meaning: %s\n", phase_description(phase));
    printf("  recovery: epoch=1->2 adopted=%u processed=%u replayed=%u "
           "republished=%u\n", result->adopted, result->processed,
           result->replayed, result->republished);
    printf("  verification: delivered_once=yes payload=yes rings_empty=yes "
           "slots_free=yes buffer=0x%016" PRIx64 "\n",
           result->buffer_id);
    printf("  latency: protocol=%.3f us service=%.3f us termination=%.3f us\n",
           result->timing.protocol_ns / 1000.0,
           result->timing.service_ns / 1000.0,
           result->timing.termination_ns / 1000.0);
    if (verbosity == VERBOSITY_VERBOSE) {
        puts("  timeline (relative to fault checkpoint):");
        print_timeline(stdout, result);
        printf("  breakdown: respawn=%.3f restart_begin=%.3f links=%.3f "
               "input_reconcile=%.3f output_reconcile=%.3f acquire=%.3f "
               "handler=%.3f send=%.3f activation=%.3f us\n",
               result->timing.respawn_ns / 1000.0,
               result->timing.restart_begin_ns / 1000.0,
               result->timing.links_ns / 1000.0,
               result->timing.input_reconcile_ns / 1000.0,
               result->timing.output_reconcile_ns / 1000.0,
               result->timing.adoption_ns / 1000.0,
               result->timing.handler_ns / 1000.0,
               result->timing.send_ns / 1000.0,
               result->timing.activation_ns / 1000.0);
    }
    puts("  result: PASS");
}

static uint64_t parse_u64(const char *text)
{
    char *end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        fprintf(stderr, "invalid number: %s\n", text);
        exit(2);
    }
    return (uint64_t)value;
}

int main(int argc, char **argv)
{
    uint32_t iterations = 10U;
    uint64_t seed = monotonic_raw_ns() ^ (uint64_t)getpid();
    uint32_t timeout_ms = 5000U;
    uint64_t budget_us = 0U;
    verbosity_t verbosity = VERBOSITY_NORMAL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--iterations") == 0 && ++i < argc)
            iterations = (uint32_t)parse_u64(argv[i]);
        else if (strcmp(argv[i], "--seed") == 0 && ++i < argc)
            seed = parse_u64(argv[i]);
        else if (strcmp(argv[i], "--max-recovery-ms") == 0 && ++i < argc)
            timeout_ms = (uint32_t)parse_u64(argv[i]);
        else if (strcmp(argv[i], "--recovery-budget-us") == 0 && ++i < argc)
            budget_us = parse_u64(argv[i]);
        else if (strcmp(argv[i], "--verbose") == 0)
            verbosity = VERBOSITY_VERBOSE;
        else if (strcmp(argv[i], "--quiet") == 0)
            verbosity = VERBOSITY_QUIET;
        else {
            fprintf(stderr, "usage: %s [--iterations N] [--seed N] "
                    "[--max-recovery-ms N] [--recovery-budget-us N] "
                    "[--verbose|--quiet]\n",
                    argv[0]);
            return 2;
        }
    }
    if (iterations == 0U || seed == 0U || timeout_ms == 0U) {
        fprintf(stderr, "iterations, seed and timeout must be nonzero\n");
        return 2;
    }

    if (verbosity != VERBOSITY_QUIET) {
        puts("zIPC relay restart recovery");
        puts("  topology: A(source) -> B(relay) -> C(relay) -> D(relay) -> E(sink)");
        puts("  policy: kill one relay, restart epoch 1 -> 2, resume delivery from a safe stage");
        puts("  schedule: checkpoints 1-7 provide coverage; later cases are seeded random");
        printf("  iterations: %u per backend\n", iterations);
        printf("  seed: %" PRIu64 "\n", seed);
        printf("  timeout: %u ms\n", timeout_ms);
    }
    uint64_t random_state = seed;
    for (uint32_t backend_index = 0U; backend_index < 2U; ++backend_index) {
        const bool eventfd_backend = backend_index == 0U;
        const char *backend = eventfd_backend ? "ring-eventfd" : "ring-polling";
        iteration_result_t *results = calloc(iterations, sizeof(*results));
        if (results == NULL)
            return 1;
        if (verbosity != VERBOSITY_QUIET)
            printf("\nBackend: %s\n", backend);
        for (uint32_t iteration = 0U; iteration < iterations; ++iteration) {
            const zipc_component_id_t victim =
                (zipc_component_id_t)(2U + random_next(&random_state) % 3U);
            crash_phase_t phase = iteration < CRASH_PHASE_COUNT - 1U
                ? (crash_phase_t)(iteration + 1U)
                : (crash_phase_t)(1U + random_next(&random_state) %
                    (CRASH_PHASE_COUNT - 1U));
            if (!eventfd_backend && phase == CRASH_SEND_POST_PUBLISH)
                phase = CRASH_SEND_PRE_PUBLISH;
            if (run_iteration(iteration, victim, phase, eventfd_backend,
                              timeout_ms, &results[iteration], seed,
                              iterations) != 0) {
                free(results);
                return 1;
            }
            if (budget_us != 0U &&
                (results[iteration].timing.protocol_ns > budget_us * 1000U ||
                 results[iteration].timing.service_ns > budget_us * 1000U)) {
                fprintf(stderr,
                        "FAIL: recovery budget exceeded: backend=%s "
                        "iteration=%u relay=%s checkpoint=%s "
                        "protocol=%.3f us service=%.3f us budget=%" PRIu64
                        " us\n"
                        "  replay: ./build/tests/zipc-recovery-chain-linux "
                        "--iterations %u --seed %" PRIu64
                        " --max-recovery-ms %u --recovery-budget-us %" PRIu64
                        " --verbose\n",
                        backend, iteration, node_name(victim), phase_name(phase),
                        results[iteration].timing.protocol_ns / 1000.0,
                        results[iteration].timing.service_ns / 1000.0,
                        budget_us, iterations, seed, timeout_ms, budget_us);
                fputs("  timeline:\n", stderr);
                print_timeline(stderr, &results[iteration]);
                free(results);
                return 1;
            }
            print_iteration(backend, iteration, iterations, victim, phase,
                            &results[iteration], verbosity);
        }
        print_summary(backend, results, iterations);
        free(results);
    }
    puts("PASS: randomized five-process relay restart recovery");
    return 0;
}
