#define _GNU_SOURCE
#include <zipc/zipc.h>

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_COMPONENTS 16U
#define RING_DEPTH 64U
#define SLOT_COUNT 32U
#define SLOT_SIZE 4096U
#define SHM_SIZE (256U * 1024U)

#define CHECK(expr) do { \
    zipc_status_t s_ = (expr); \
    if (s_ != ZIPC_OK) { \
        fprintf(stderr, "%s failed: %d\n", #expr, (int)s_); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

typedef struct {
    uint64_t sequence;
    uint32_t component_count;
    uint32_t reserved;
    uint64_t depart_ns[MAX_COMPONENTS];
    uint64_t arrival_ns[MAX_COMPONENTS];
} ping_payload_t;

typedef struct {
    zipc_transport_spsc_ring_t *ring;
    int event_fd;
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

static void run_component(unsigned int index,
                          unsigned int component_count,
                          unsigned int iterations,
                          edge_link_t *forward,
                          edge_link_t *reverse)
{
    if (index == 0U) {
        for (unsigned int it = 0; it < iterations; ++it) {
            zipc_buffer_t buffer;
            CHECK(zipc_buffer_alloc_ex(forward[0].sender, 0U, 0U, 0U, &buffer));
            ping_payload_t payload;
            memset(&payload, 0, sizeof(payload));
            payload.sequence = it;
            payload.component_count = component_count;
            payload.depart_ns[0] = now_ns();
            CHECK(zipc_buffer_append(&buffer, &payload, sizeof(payload)));
            CHECK(zipc_send(forward[0].sender, &buffer));

            CHECK(zipc_recv(reverse[0].receiver, &buffer));
            const uint64_t done = now_ns();
            const ping_payload_t *reply = zipc_buffer_data(&buffer);
            const double rtt_us = (double)(done - reply->depart_ns[0]) / 1000.0;
            const double one_way_us =
                (double)(reply->arrival_ns[component_count - 1U] -
                         reply->depart_ns[0]) / 1000.0;
            printf("seq=%" PRIu64 " rtt=%.3f us forward=%.3f us hops=",
                   reply->sequence, rtt_us, one_way_us);
            for (unsigned int hop = 1U; hop < component_count; ++hop) {
                const double hop_us =
                    (double)(reply->arrival_ns[hop] -
                             reply->depart_ns[hop - 1U]) / 1000.0;
                printf("%s%.3f", hop == 1U ? "" : ",", hop_us);
            }
            printf(" us hop_count=%u\n", zipc_buffer_hop_count(&buffer));
            fflush(stdout);
            CHECK(zipc_buffer_release(&buffer));
        }
        _exit(EXIT_SUCCESS);
    }

    const bool final_component = index + 1U == component_count;
    for (unsigned int it = 0; it < iterations; ++it) {
        zipc_buffer_t buffer;
        CHECK(zipc_recv(forward[index - 1U].receiver, &buffer));
        ping_payload_t *payload = zipc_buffer_data(&buffer);
        payload->arrival_ns[index] = now_ns();
        payload->depart_ns[index] = now_ns();

        if (final_component) {
            CHECK(zipc_send(reverse[index - 1U].sender, &buffer));
            continue;
        }

        CHECK(zipc_send(forward[index].sender, &buffer));
        CHECK(zipc_recv(reverse[index].receiver, &buffer));
        CHECK(zipc_send(reverse[index - 1U].sender, &buffer));
    }
    _exit(EXIT_SUCCESS);
}

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s [--relays N] [--count N]\n", program);
}

int main(int argc, char **argv)
{
    unsigned int relay_count = 0U;
    unsigned int iterations = 10U;
    bool relays_set = false;
    bool count_set = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--relays") == 0 && i + 1 < argc) {
            relay_count = (unsigned int)strtoul(argv[++i], NULL, 0);
            relays_set = true;
        } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            iterations = (unsigned int)strtoul(argv[++i], NULL, 0);
            count_set = true;
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    const unsigned int component_count = relay_count + 2U;
    if (component_count > MAX_COMPONENTS || iterations == 0U) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    printf("config: relays=%u%s count=%u%s\n",
           relay_count, relays_set ? "" : " (default)",
           iterations, count_set ? "" : " (default)");
    const unsigned int edge_count = component_count - 1U;

    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "/zipc_ping_%ld", (long)getpid());
    zipc_platform_memory_t *memory = NULL;
    const zipc_platform_memory_config_t memory_cfg = {
        .type = ZIPC_SHM_POSIX,
        .size = SHM_SIZE,
        .backend.posix = {
            .name = shm_name,
            .create = true,
            .unlink_on_close = true,
        },
    };
    CHECK(zipc_platform_memory_open(&memory, &memory_cfg));

    zipc_pool_t pool;
    const zipc_pool_config_t pool_cfg = {
        .control_memory = memory,
        .payload_memory = NULL,
        .control_offset = 0U,
        .payload_offset = 16U * 1024U,
        .slot_count = SLOT_COUNT,
        .slot_capacity = SLOT_SIZE,
        .slot_stride = SLOT_SIZE,
        .payload_alignment = 64U,
    };
    CHECK(zipc_pool_format(&pool_cfg));
    CHECK(zipc_pool_attach(&pool, &pool_cfg));

    edge_link_t *forward = calloc(edge_count, sizeof(*forward));
    edge_link_t *reverse = calloc(edge_count, sizeof(*reverse));
    pid_t *children = calloc(component_count, sizeof(*children));
    if (forward == NULL || reverse == NULL || children == NULL)
        return EXIT_FAILURE;

    const size_t ring_size = zipc_transport_spsc_ring_size(RING_DEPTH);
    for (unsigned int edge = 0U; edge < edge_count; ++edge) {
        edge_link_t *directions[2] = { &forward[edge], &reverse[edge] };
        for (unsigned int d = 0U; d < 2U; ++d) {
            edge_link_t *link = directions[d];
            link->ring = mmap(NULL, ring_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
            if (link->ring == MAP_FAILED) {
                perror("mmap");
                return EXIT_FAILURE;
            }
            CHECK(zipc_transport_spsc_ring_initialize(link->ring, RING_DEPTH));
            link->event_fd = eventfd(0U, EFD_CLOEXEC);
            if (link->event_fd < 0) {
                perror("eventfd");
                return EXIT_FAILURE;
            }
            const zipc_platform_transport_config_t transport = {
                .type = ZIPC_TRANSPORT_SHM_RING_EVENTFD,
                .platform_handle = link->ring,
                .receive_handle = (void *)(intptr_t)link->event_fd,
                .ring_depth = RING_DEPTH,
            };
            const unsigned int src = d == 0U ? edge : edge + 1U;
            const unsigned int dst = d == 0U ? edge + 1U : edge;
            const zipc_link_config_t sender_cfg = {
                .pool = &pool,
                .local_component = (zipc_component_id_t)src,
                .remote_component = (zipc_component_id_t)dst,
                .transport = transport,
            };
            const zipc_link_config_t receiver_cfg = {
                .pool = &pool,
                .local_component = (zipc_component_id_t)dst,
                .remote_component = (zipc_component_id_t)src,
                .transport = transport,
            };
            CHECK(zipc_link_create(&link->sender, &sender_cfg));
            CHECK(zipc_link_create(&link->receiver, &receiver_cfg));
        }
    }

    for (unsigned int i = 0U; i < component_count; ++i) {
        children[i] = fork();
        if (children[i] < 0) {
            perror("fork");
            return EXIT_FAILURE;
        }
        if (children[i] == 0)
            run_component(i, component_count, iterations, forward, reverse);
    }

    int result = EXIT_SUCCESS;
    for (unsigned int i = 0U; i < component_count; ++i) {
        int status = 0;
        if (waitpid(children[i], &status, 0) < 0 ||
            !WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS)
            result = EXIT_FAILURE;
    }

    for (unsigned int edge = 0U; edge < edge_count; ++edge) {
        edge_link_t *directions[2] = { &forward[edge], &reverse[edge] };
        for (unsigned int d = 0U; d < 2U; ++d) {
            zipc_link_destroy(directions[d]->receiver);
            zipc_link_destroy(directions[d]->sender);
            close(directions[d]->event_fd);
            munmap(directions[d]->ring, ring_size);
        }
    }
    free(children);
    free(reverse);
    free(forward);
    zipc_platform_memory_close(memory);
    return result;
}
