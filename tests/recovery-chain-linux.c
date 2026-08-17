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
    REPORT_CHECKPOINT = 1,
    REPORT_RECOVERY,
    REPORT_SINK,
    REPORT_ERROR
} report_type_t;

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
    int32_t status;
    uint64_t timestamp_ns;
    uint64_t buffer_id;
    uint64_t protocol_ns;
    uint32_t recovered_transfers;
    uint32_t reserved;
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
} timing_t;

static uint64_t monotonic_raw_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0)
        return 0U;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
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
                       crash_phase_t phase, zipc_status_t status)
{
    const report_t report = {
        .type = REPORT_ERROR,
        .component = component,
        .phase = phase,
        .status = status,
        .timestamp_ns = monotonic_raw_ns(),
    };
    (void)write_report(fd, &report);
    _exit(1);
}

static void stop_at_checkpoint(int fd, zipc_component_id_t component,
                               crash_phase_t phase)
{
    const report_t report = {
        .type = REPORT_CHECKPOINT,
        .component = component,
        .phase = phase,
        .status = ZIPC_OK,
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
        child_fail(report_fd, 1U, 0, status);
    zipc_buffer_t buffer;
    status = zipc_buffer_alloc(tx, sizeof(test_payload_t), &buffer, NULL);
    if (status != ZIPC_OK)
        child_fail(report_fd, 1U, 0, status);
    test_payload_t *payload = zipc_buffer_data(&buffer);
    if (payload == NULL)
        child_fail(report_fd, 1U, 0, ZIPC_ERR_INVALID_BUFFER);
    *payload = (test_payload_t){
        .magic = PAYLOAD_MAGIC,
        .iteration = iteration,
    };
    status = zipc_send(tx, &buffer);
    if (status != ZIPC_OK)
        child_fail(report_fd, 1U, 0, status);
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
        child_fail(report_fd, 5U, 0, status);
    zipc_buffer_t buffer;
    status = zipc_recv(rx, &buffer);
    if (status != ZIPC_OK)
        child_fail(report_fd, 5U, 0, status);
    const test_payload_t *payload = zipc_buffer_const_data(&buffer);
    if (payload == NULL || payload->magic != PAYLOAD_MAGIC ||
        payload->iteration != iteration || payload->relay_mask != 7U)
        child_fail(report_fd, 5U, 0, ZIPC_ERR_INVALID_BUFFER);
    const report_t report = {
        .type = REPORT_SINK,
        .component = 5U,
        .status = ZIPC_OK,
        .timestamp_ns = monotonic_raw_ns(),
        .buffer_id = zipc_buffer_id(&buffer),
    };
    if (write_report(report_fd, &report) != 0)
        _exit(1);
    status = zipc_buffer_release(&buffer);
    if (status != ZIPC_OK)
        child_fail(report_fd, 5U, 0, status);
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
        child_fail(report_fd, component, phase, status);

    zipc_buffer_t buffer;
    status = zipc_recv(rx, &buffer);
    if (status != ZIPC_OK)
        child_fail(report_fd, component, phase, status);
    if (component == victim && phase == CRASH_OWNED)
        stop_at_checkpoint(report_fd, component, phase);
    process_relay(&buffer, component);
    if (component == victim && phase == CRASH_AFTER_PROCESS)
        stop_at_checkpoint(report_fd, component, phase);
    status = zipc_send(tx, &buffer);
    if (status != ZIPC_OK)
        child_fail(report_fd, component, phase, status);
    zipc_link_destroy(tx);
    zipc_link_destroy(rx);
    _exit(0);
}

static void run_restart(fixture_t *fixture, zipc_component_id_t component,
                        crash_phase_t phase, bool eventfd_backend, int report_fd)
{
    zipc_test_recovery_set_hook(NULL, NULL);
    const uint64_t begin_ns = monotonic_raw_ns();
    zipc_restart_t restart;
    zipc_status_t status = zipc_component_restart_begin(
        &fixture->pool, component, 1U, &restart);
    if (status != ZIPC_OK)
        child_fail(report_fd, component, phase, status);
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
        child_fail(report_fd, component, phase, status);

    uint32_t incoming = 0U;
    uint32_t outgoing = 0U;
    status = zipc_link_reconcile(rx, &restart, &incoming);
    if (status == ZIPC_OK)
        status = zipc_link_reconcile(tx, &restart, &outgoing);
    if (status != ZIPC_OK)
        child_fail(report_fd, component, phase, status);

    zipc_buffer_t buffer;
    bool adopted = false;
    for (;;) {
        status = zipc_component_restart_next(&restart, &buffer);
        if (status == ZIPC_ERR_NO_BUFFER) {
            status = ZIPC_OK;
            break;
        }
        if (status != ZIPC_OK)
            break;
        adopted = true;
        process_relay(&buffer, component);
        status = zipc_send(tx, &buffer);
        if (status != ZIPC_OK)
            break;
    }
    if (status == ZIPC_OK && !adopted &&
        phase != CRASH_SEND_PRE_PUBLISH &&
        phase != CRASH_SEND_POST_PUBLISH) {
        status = zipc_recv(rx, &buffer);
        if (status == ZIPC_OK) {
            process_relay(&buffer, component);
            status = zipc_send(tx, &buffer);
        }
    }
    if (status != ZIPC_OK)
        child_fail(report_fd, component, phase, status);
    status = zipc_component_restart_finish(&restart);
    if (status != ZIPC_OK)
        child_fail(report_fd, component, phase, status);
    const uint64_t end_ns = monotonic_raw_ns();
    const report_t report = {
        .type = REPORT_RECOVERY,
        .component = component,
        .phase = phase,
        .status = ZIPC_OK,
        .timestamp_ns = end_ns,
        .protocol_ns = end_ns - begin_ns,
        .recovered_transfers = outgoing,
    };
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

static int run_iteration(uint32_t iteration, zipc_component_id_t victim,
                         crash_phase_t phase, bool eventfd_backend,
                         uint32_t timeout_ms, timing_t *timing)
{
    fixture_t fixture;
    if (fixture_setup(&fixture, eventfd_backend) != 0)
        return -1;
    int reports[2];
    if (pipe(reports) != 0) {
        fixture_destroy(&fixture);
        return -1;
    }

    pid_t pids[NODE_COUNT] = {0};
    for (int index = (int)NODE_COUNT - 1; index >= 0; --index) {
        const pid_t pid = fork();
        if (pid < 0)
            return -1;
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
        if (read_report_timeout(reports[0], &report, timeout_ms) != 0)
            goto fail;
        if (report.type == REPORT_ERROR)
            goto fail;
        if (report.type == REPORT_SINK) {
            sink_seen = true;
            sink_report = report;
        } else if (report.type == REPORT_CHECKPOINT &&
                   report.component == victim && report.phase == (uint32_t)phase) {
            checkpoint_seen = true;
        }
    }

    const uint64_t kill_ns = monotonic_raw_ns();
    const pid_t victim_pid = pids[victim - 1U];
    if (kill(victim_pid, SIGKILL) != 0)
        goto fail;
    int victim_status = 0;
    if (waitpid(victim_pid, &victim_status, 0) != victim_pid ||
        !WIFSIGNALED(victim_status) || WTERMSIG(victim_status) != SIGKILL)
        goto fail;
    pids[victim - 1U] = 0;

    const pid_t restart_pid = fork();
    if (restart_pid < 0)
        goto fail;
    if (restart_pid == 0) {
        (void)close(reports[0]);
        run_restart(&fixture, victim, phase, eventfd_backend, reports[1]);
    }

    bool recovery_seen = false;
    report_t recovery_report = {0};
    while (!recovery_seen || !sink_seen) {
        report_t report;
        if (read_report_timeout(reports[0], &report, timeout_ms) != 0)
            goto fail_restart;
        if (report.type == REPORT_ERROR)
            goto fail_restart;
        if (report.type == REPORT_RECOVERY) {
            recovery_seen = true;
            recovery_report = report;
        } else if (report.type == REPORT_SINK) {
            if (sink_seen)
                goto fail_restart;
            sink_seen = true;
            sink_report = report;
        }
    }

    if (wait_child(restart_pid) != 0)
        goto fail;
    for (uint32_t i = 0U; i < NODE_COUNT; ++i)
        if (wait_child(pids[i]) != 0)
            goto fail;
    for (uint32_t i = 0U; i < EDGE_COUNT; ++i)
        if (atomic_load_explicit(&fixture.rings[i]->producer,
                                 memory_order_acquire) !=
            atomic_load_explicit(&fixture.rings[i]->consumer,
                                 memory_order_acquire))
            goto fail;
    for (uint32_t i = 0U; i < SLOT_COUNT; ++i)
        if (atomic_load_explicit(&fixture.pool.controls[i].state,
                                 memory_order_acquire) != ZIPC_SLOT_FREE)
            goto fail;

    timing->protocol_ns = recovery_report.protocol_ns;
    timing->service_ns = sink_report.timestamp_ns > kill_ns
                       ? sink_report.timestamp_ns - kill_ns : 0U;
    (void)close(reports[0]);
    (void)close(reports[1]);
    fixture_destroy(&fixture);
    return 0;

fail_restart:
    (void)kill(restart_pid, SIGKILL);
    (void)waitpid(restart_pid, NULL, 0);
fail:
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

static void print_summary(const char *backend, timing_t *timings,
                          uint32_t count)
{
    uint64_t *protocol = calloc(count, sizeof(*protocol));
    uint64_t *service = calloc(count, sizeof(*service));
    if (protocol == NULL || service == NULL)
        exit(1);
    uint64_t protocol_sum = 0U;
    uint64_t service_sum = 0U;
    for (uint32_t i = 0U; i < count; ++i) {
        protocol[i] = timings[i].protocol_ns;
        service[i] = timings[i].service_ns;
        protocol_sum += protocol[i];
        service_sum += service[i];
    }
    qsort(protocol, count, sizeof(*protocol), compare_u64);
    qsort(service, count, sizeof(*service), compare_u64);
    const uint32_t p50 = (count - 1U) / 2U;
    const uint32_t p95 = ((count * 95U + 99U) / 100U) - 1U;
    printf("summary backend=%s iterations=%u "
           "protocol_us[min/mean/p50/p95/max]=%.3f/%.3f/%.3f/%.3f/%.3f "
           "service_us[min/mean/p50/p95/max]=%.3f/%.3f/%.3f/%.3f/%.3f\n",
           backend, count,
           protocol[0] / 1000.0, protocol_sum / (double)count / 1000.0,
           protocol[p50] / 1000.0, protocol[p95] / 1000.0,
           protocol[count - 1U] / 1000.0,
           service[0] / 1000.0, service_sum / (double)count / 1000.0,
           service[p50] / 1000.0, service[p95] / 1000.0,
           service[count - 1U] / 1000.0);
    free(service);
    free(protocol);
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
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--iterations") == 0 && ++i < argc)
            iterations = (uint32_t)parse_u64(argv[i]);
        else if (strcmp(argv[i], "--seed") == 0 && ++i < argc)
            seed = parse_u64(argv[i]);
        else if (strcmp(argv[i], "--max-recovery-ms") == 0 && ++i < argc)
            timeout_ms = (uint32_t)parse_u64(argv[i]);
        else if (strcmp(argv[i], "--recovery-budget-us") == 0 && ++i < argc)
            budget_us = parse_u64(argv[i]);
        else {
            fprintf(stderr, "usage: %s [--iterations N] [--seed N] "
                    "[--max-recovery-ms N] [--recovery-budget-us N]\n",
                    argv[0]);
            return 2;
        }
    }
    if (iterations == 0U || seed == 0U || timeout_ms == 0U) {
        fprintf(stderr, "iterations, seed and timeout must be nonzero\n");
        return 2;
    }

    printf("config: nodes=5 relays=3 iterations=%u seed=%" PRIu64
           " timeout_ms=%u\n", iterations, seed, timeout_ms);
    uint64_t random_state = seed;
    for (uint32_t backend_index = 0U; backend_index < 2U; ++backend_index) {
        const bool eventfd_backend = backend_index == 0U;
        const char *backend = eventfd_backend ? "ring-eventfd" : "ring-polling";
        timing_t *timings = calloc(iterations, sizeof(*timings));
        if (timings == NULL)
            return 1;
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
                              timeout_ms, &timings[iteration]) != 0) {
                fprintf(stderr,
                        "FAIL iteration=%u seed=%" PRIu64
                        " backend=%s relay=%u phase=%u\n",
                        iteration, seed, backend, victim, phase);
                free(timings);
                return 1;
            }
            printf("iteration=%u seed=%" PRIu64
                   " backend=%s relay=%u phase=%u protocol_recovery_us=%.3f "
                   "service_recovery_us=%.3f result=PASS\n",
                   iteration, seed, backend, victim, phase,
                   timings[iteration].protocol_ns / 1000.0,
                   timings[iteration].service_ns / 1000.0);
            if (budget_us != 0U &&
                (timings[iteration].protocol_ns > budget_us * 1000U ||
                 timings[iteration].service_ns > budget_us * 1000U)) {
                fprintf(stderr, "recovery budget exceeded\n");
                free(timings);
                return 1;
            }
        }
        print_summary(backend, timings, iterations);
        free(timings);
    }
    puts("PASS: randomized five-process relay restart recovery");
    return 0;
}
