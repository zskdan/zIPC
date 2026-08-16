#define _GNU_SOURCE
#include <zipc/zipc.h>

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>

#define SLOT_COUNT 256U
#define SLOT_SIZE 256U
#define DEFAULT_RING_DEPTH 1024U
#define WARMUP_PACKETS 1000U
#define MAX_RELAYS (ZIPC_COMPONENT_ID_MAX - 2U)

#define CHECK(expr) do { \
    zipc_status_t s_ = (expr); \
    if (s_ != ZIPC_OK) { \
        fprintf(stderr, "%s failed: %d\n", #expr, (int)s_); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

#define CHECK_SETUP(expr) do { \
    zipc_status_t s_ = (expr); \
    if (s_ != ZIPC_OK) { \
        fprintf(stderr, "%s failed: %d\n", #expr, (int)s_); \
        goto cleanup; \
    } \
} while (0)

typedef struct {
    zipc_transport_spsc_ring_t *ring;
    size_t ring_size;
    int event_fd;
    char endpoint[108];
    zipc_link_t *sender;
    zipc_link_t *receiver;
} edge_link_t;

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static bool parse_u64(const char *text, uint64_t maximum, uint64_t *value)
{
    if (text == NULL || !isdigit((unsigned char)text[0]))
        return false;

    char *end = NULL;
    errno = 0;
    const unsigned long long parsed = strtoull(text, &end, 0);
    if (errno == ERANGE || end == text || *end != '\0' || parsed > maximum)
        return false;

    *value = (uint64_t)parsed;
    return true;
}

static bool parse_u32(const char *text, uint32_t maximum, uint32_t *value)
{
    uint64_t parsed = 0U;
    if (!parse_u64(text, maximum, &parsed))
        return false;
    *value = (uint32_t)parsed;
    return true;
}


static void buffer_get_wait(zipc_link_t *sender, zipc_buffer_t *buffer)
{
    for (;;) {
        const zipc_status_t status = zipc_buffer_alloc_ex(sender, 0U, 0U, 0U, buffer, NULL);
        if (status == ZIPC_OK)
            return;
        if (status != ZIPC_ERR_NO_BUFFER) {
            fprintf(stderr, "zipc_buffer_alloc_ex failed: %d\n", (int)status);
            exit(EXIT_FAILURE);
        }
        sched_yield();
    }
}

static void buffer_send_wait(zipc_link_t *sender, zipc_buffer_t *buffer,
                             bool retry_transport_full)
{
    for (;;) {
        const zipc_status_t status = zipc_send(sender, buffer);
        if (status == ZIPC_OK)
            return;
        if (!retry_transport_full || status != ZIPC_ERR_TRANSPORT) {
            fprintf(stderr, "zipc_send failed: %d\n", (int)status);
            exit(EXIT_FAILURE);
        }
        sched_yield();
    }
}

static void edge_link_cleanup(edge_link_t *edge)
{
    if (edge == NULL)
        return;
    if (edge->sender != NULL)
        zipc_link_destroy(edge->sender);
    if (edge->receiver != NULL)
        zipc_link_destroy(edge->receiver);
    if (edge->event_fd >= 0)
        close(edge->event_fd);
    if (edge->ring != NULL && edge->ring != MAP_FAILED)
        munmap(edge->ring, edge->ring_size);
}

static void run_receiver(uint32_t index, uint32_t component_count,
                         uint64_t packet_count, edge_link_t *edges,
                         bool retry_transport_full, int ready_fd, int done_fd)
{
    const bool final_component = index + 1U == component_count;

    for (uint64_t i = 0U; i < WARMUP_PACKETS; ++i) {
        zipc_buffer_t buffer;
        CHECK(zipc_recv(edges[index - 1U].receiver, &buffer));
        if (final_component)
            CHECK(zipc_buffer_release(&buffer));
        else
            buffer_send_wait(edges[index].sender, &buffer,
                             retry_transport_full);
    }
    if (final_component) {
        const uint8_t ready = 1U;
        if (write(ready_fd, &ready, sizeof(ready)) != sizeof(ready))
            _exit(EXIT_FAILURE);
    }

    for (uint64_t i = 0U; i < packet_count; ++i) {
        zipc_buffer_t buffer;
        CHECK(zipc_recv(edges[index - 1U].receiver, &buffer));
        if (final_component)
            CHECK(zipc_buffer_release(&buffer));
        else
            buffer_send_wait(edges[index].sender, &buffer,
                             retry_transport_full);
    }
    if (final_component) {
        const uint8_t done = 1U;
        if (write(done_fd, &done, sizeof(done)) != sizeof(done))
            _exit(EXIT_FAILURE);
    }
    _exit(EXIT_SUCCESS);
}

static void run_sender(uint64_t packet_count, uint32_t payload_size,
                       uint32_t relay_count, edge_link_t *edges,
                       bool retry_transport_full, int ready_fd, int done_fd,
                       const char *transport_name)
{
    uint8_t payload[SLOT_SIZE];
    memset(payload, 0xA5, sizeof(payload));

    for (uint64_t i = 0U; i < WARMUP_PACKETS; ++i) {
        zipc_buffer_t buffer;
        buffer_get_wait(edges[0].sender, &buffer);
        CHECK(zipc_buffer_append(&buffer, payload, payload_size));
        buffer_send_wait(edges[0].sender, &buffer, retry_transport_full);
    }
    uint8_t marker;
    if (read(ready_fd, &marker, sizeof(marker)) != sizeof(marker))
        _exit(EXIT_FAILURE);

    const uint64_t start = now_ns();
    for (uint64_t i = 0U; i < packet_count; ++i) {
        zipc_buffer_t buffer;
        buffer_get_wait(edges[0].sender, &buffer);
        CHECK(zipc_buffer_append(&buffer, payload, payload_size));
        buffer_send_wait(edges[0].sender, &buffer, retry_transport_full);
    }
    if (read(done_fd, &marker, sizeof(marker)) != sizeof(marker))
        _exit(EXIT_FAILURE);
    const uint64_t end = now_ns();

    const uint32_t hop_count = relay_count + 1U;
    const double seconds = (double)(end - start) / 1e9;
    const double pps = (double)packet_count / seconds;
    const double transfers_per_second = pps * hop_count;
    const double mib_s = (double)packet_count * payload_size /
                         (1024.0 * 1024.0 * seconds);
    printf("transport=%s relays=%u hops=%u packets=%" PRIu64
           " payload=%u elapsed=%.6f s\n",
           transport_name, relay_count, hop_count, packet_count,
           payload_size, seconds);
    printf("rate=%.0f packets/s transfer-rate=%.0f transfers/s "
           "payload-throughput=%.3f MiB/s\n",
           pps, transfers_per_second, mib_s);
    fflush(stdout);
    _exit(EXIT_SUCCESS);
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--transport ring-eventfd|fifo|unix-dgram|mqueue] "
            "[--packets N] [--payload BYTES] [--relays N] [--slots N] "
            "[--ring-depth N]\n"
            "Defaults: --transport ring-eventfd --packets 1000000 "
            "--payload 8 --relays 0 --slots %u --ring-depth %u\n",
            program, (unsigned)SLOT_COUNT, (unsigned)DEFAULT_RING_DEPTH);
}

int main(int argc, char **argv)
{
    const char *transport_name = "ring-eventfd";
    uint64_t packet_count = UINT64_C(1000000);
    uint32_t payload_size = 8U;
    uint32_t relay_count = 0U;
    uint32_t slot_count = SLOT_COUNT;
    uint32_t ring_depth = DEFAULT_RING_DEPTH;
    bool transport_set = false, packets_set = false;
    bool payload_set = false, relays_set = false;
    bool slots_set = false, ring_set = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--transport") == 0 && i + 1 < argc) {
            transport_name = argv[++i];
            transport_set = true;
        } else if (strcmp(argv[i], "--packets") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], UINT64_MAX, &packet_count)) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            packets_set = true;
        } else if (strcmp(argv[i], "--payload") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], UINT32_MAX, &payload_size)) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            payload_set = true;
        } else if (strcmp(argv[i], "--relays") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], MAX_RELAYS, &relay_count)) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            relays_set = true;
        } else if (strcmp(argv[i], "--slots") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], UINT32_MAX, &slot_count)) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            slots_set = true;
        } else if (strcmp(argv[i], "--ring-depth") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], UINT32_MAX, &ring_depth)) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            ring_set = true;
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (packet_count == 0U || payload_size > SLOT_SIZE || slot_count < 2U ||
        ring_depth < 2U) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const uint32_t component_count = relay_count + 2U;
    const uint32_t edge_count = component_count - 1U;
    int result = EXIT_FAILURE;
    zipc_platform_memory_t *memory = NULL;
    edge_link_t *edges = NULL;
    pid_t *children = NULL;
    uint32_t spawned = 0U;
    int ready_pipe[2] = { -1, -1 };
    int done_pipe[2] = { -1, -1 };
    printf("config: transport=%s%s packets=%" PRIu64 "%s payload=%u%s "
           "relays=%u%s components=%u hops=%u slots=%u%s ring-depth=%u%s\n",
           transport_name, transport_set ? "" : " (default)",
           packet_count, packets_set ? "" : " (default)",
           payload_size, payload_set ? "" : " (default)",
           relay_count, relays_set ? "" : " (default)",
           component_count, edge_count,
           slot_count, slots_set ? "" : " (default)",
           ring_depth, ring_set ? "" : " (default)");

    const bool ring_transport = strcmp(transport_name, "ring-eventfd") == 0;
    if (!ring_transport && strcmp(transport_name, "fifo") != 0 &&
        strcmp(transport_name, "unix-dgram") != 0 &&
        strcmp(transport_name, "mqueue") != 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
#if SIZE_MAX == UINT32_MAX
    if (ring_transport && ring_depth >
            (SIZE_MAX - sizeof(zipc_transport_spsc_ring_t)) /
                sizeof(zipc_message_t)) {
        fprintf(stderr, "ring depth is too large for this platform\n");
        return EXIT_FAILURE;
    }
#endif

    const size_t control_size = zipc_pool_required_control_size(slot_count);
    if (control_size == 0U || control_size > SIZE_MAX - 63U) {
        fprintf(stderr, "invalid pool slot count\n");
        return EXIT_FAILURE;
    }
    const size_t payload_offset = (control_size + 63U) & ~(size_t)63U;
    if ((size_t)slot_count > (SIZE_MAX - payload_offset) / SLOT_SIZE) {
        fprintf(stderr, "pool size overflow\n");
        return EXIT_FAILURE;
    }
    const size_t shm_size = payload_offset + (size_t)slot_count * SLOT_SIZE;

    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "/zipc_tbench_%ld", (long)getpid());
    const zipc_platform_memory_config_t memory_cfg = {
        .type = ZIPC_SHM_POSIX,
        .size = shm_size,
        .backend.posix = {
            .name = shm_name,
            .create = true,
            .unlink_on_close = true,
        },
    };
    CHECK_SETUP(zipc_platform_memory_open(&memory, &memory_cfg));

    zipc_pool_t pool;
    const zipc_pool_config_t pool_cfg = {
        .control_memory = memory,
        .payload_memory = NULL,
        .control_offset = 0U,
        .payload_offset = payload_offset,
        .slot_count = slot_count,
        .slot_capacity = SLOT_SIZE,
        .slot_stride = SLOT_SIZE,
        .payload_alignment = 64U,
    };
    CHECK_SETUP(zipc_pool_format(&pool_cfg));
    CHECK_SETUP(zipc_pool_attach(&pool, &pool_cfg));

    edges = calloc(edge_count, sizeof(*edges));
    if (edges == NULL) {
        fprintf(stderr, "allocation failed\n");
        goto cleanup;
    }
    for (uint32_t edge = 0U; edge < edge_count; ++edge)
        edges[edge].event_fd = -1;
    children = calloc(component_count, sizeof(*children));
    if (children == NULL) {
        fprintf(stderr, "allocation failed\n");
        goto cleanup;
    }

    for (uint32_t edge = 0U; edge < edge_count; ++edge) {
        edge_link_t *link = &edges[edge];
        zipc_platform_transport_config_t sender_transport;
        zipc_platform_transport_config_t receiver_transport;
        memset(&sender_transport, 0, sizeof(sender_transport));
        memset(&receiver_transport, 0, sizeof(receiver_transport));

        if (ring_transport) {
            link->ring_size = zipc_transport_spsc_ring_size(ring_depth);
            link->ring = mmap(NULL, link->ring_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
            if (link->ring == MAP_FAILED) {
                perror("mmap");
                goto cleanup;
            }
            CHECK_SETUP(zipc_transport_spsc_ring_initialize(link->ring,
                                                             ring_depth));
            link->event_fd = eventfd(0U, EFD_CLOEXEC);
            if (link->event_fd < 0) {
                perror("eventfd");
                goto cleanup;
            }
            sender_transport.type = ZIPC_TRANSPORT_SHM_RING_EVENTFD;
            sender_transport.platform_handle = link->ring;
            sender_transport.receive_handle =
                (void *)(intptr_t)link->event_fd;
            sender_transport.ring_depth = ring_depth;
            receiver_transport = sender_transport;
        } else {
            if (strcmp(transport_name, "fifo") == 0) {
                snprintf(link->endpoint, sizeof(link->endpoint),
                         "/tmp/zipc_tbench_%ld_%u.fifo", (long)getpid(), edge);
                sender_transport.type = ZIPC_TRANSPORT_FIFO;
            } else if (strcmp(transport_name, "unix-dgram") == 0) {
                snprintf(link->endpoint, sizeof(link->endpoint),
                         "/tmp/zipc_tbench_%ld_%u.sock", (long)getpid(), edge);
                sender_transport.type = ZIPC_TRANSPORT_UNIX_DGRAM;
            } else {
                snprintf(link->endpoint, sizeof(link->endpoint),
                         "/zipc_tbench_%ld_%u", (long)getpid(), edge);
                sender_transport.type = ZIPC_TRANSPORT_POSIX_MQUEUE;
            }
            sender_transport.endpoint_name = link->endpoint;
            receiver_transport = sender_transport;
            receiver_transport.create_endpoint = true;
        }

        const zipc_component_id_t source =
            (zipc_component_id_t)(edge + ZIPC_COMPONENT_ID_MIN);
        const zipc_component_id_t destination =
            (zipc_component_id_t)(source + 1U);
        const zipc_link_config_t receiver_cfg = {
            .pool = &pool,
            .local_component = destination,
            .remote_component = source,
            .transport = receiver_transport,
        };
        const zipc_link_config_t sender_cfg = {
            .pool = &pool,
            .local_component = source,
            .remote_component = destination,
            .transport = sender_transport,
        };
        CHECK_SETUP(zipc_link_create(&link->receiver, &receiver_cfg));
        CHECK_SETUP(zipc_link_create(&link->sender, &sender_cfg));
    }

    if (pipe(ready_pipe) != 0 || pipe(done_pipe) != 0) {
        perror("pipe");
        goto cleanup;
    }

    fflush(NULL);
    result = EXIT_SUCCESS;
    for (uint32_t index = 0U; index < component_count; ++index) {
        children[index] = fork();
        if (children[index] < 0) {
            perror("fork");
            result = EXIT_FAILURE;
            for (uint32_t i = 0U; i < spawned; ++i)
                (void)kill(children[i], SIGTERM);
            break;
        }
        if (children[index] == 0) {
            if (index == 0U) {
                close(ready_pipe[1]);
                close(done_pipe[1]);
                run_sender(packet_count, payload_size, relay_count, edges,
                           ring_transport, ready_pipe[0], done_pipe[0],
                           transport_name);
            }
            close(ready_pipe[0]);
            close(done_pipe[0]);
            if (index + 1U != component_count) {
                close(ready_pipe[1]);
                close(done_pipe[1]);
            }
            run_receiver(index, component_count, packet_count, edges,
                         ring_transport, ready_pipe[1], done_pipe[1]);
        }
        spawned++;
    }

    close(ready_pipe[0]);
    ready_pipe[0] = -1;
    close(ready_pipe[1]);
    ready_pipe[1] = -1;
    close(done_pipe[0]);
    done_pipe[0] = -1;
    close(done_pipe[1]);
    done_pipe[1] = -1;

    uint32_t remaining = spawned;
    while (remaining > 0U) {
        int child_status = 0;
        const pid_t finished = waitpid(-1, &child_status, 0);
        if (finished < 0) {
            if (errno == EINTR)
                continue;
            perror("waitpid");
            result = EXIT_FAILURE;
            break;
        }
        remaining--;
        for (uint32_t i = 0U; i < spawned; ++i) {
            if (children[i] == finished) {
                children[i] = 0;
                break;
            }
        }
        if (!WIFEXITED(child_status) ||
            WEXITSTATUS(child_status) != EXIT_SUCCESS) {
            result = EXIT_FAILURE;
            for (uint32_t i = 0U; i < spawned; ++i) {
                if (children[i] > 0)
                    (void)kill(children[i], SIGTERM);
            }
        }
    }

cleanup:
    if (children != NULL) {
        for (uint32_t i = 0U; i < spawned; ++i) {
            if (children[i] <= 0)
                continue;
            (void)kill(children[i], SIGTERM);
            while (waitpid(children[i], NULL, 0) < 0 && errno == EINTR) {
            }
        }
    }
    for (size_t i = 0U; i < 2U; ++i) {
        if (ready_pipe[i] >= 0)
            close(ready_pipe[i]);
        if (done_pipe[i] >= 0)
            close(done_pipe[i]);
    }
    for (uint32_t edge = 0U; edge < edge_count; ++edge)
        edge_link_cleanup(edges == NULL ? NULL : &edges[edge]);
    free(children);
    free(edges);
    zipc_platform_memory_close(memory);
    return result;
}
