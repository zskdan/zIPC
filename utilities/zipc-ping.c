#define _GNU_SOURCE
#include <zipc/zipc.h>

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
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

static void edge_link_cleanup(edge_link_t *link, size_t ring_size)
{
    if (link == NULL)
        return;
    if (link->receiver != NULL)
        zipc_link_destroy(link->receiver);
    if (link->sender != NULL)
        zipc_link_destroy(link->sender);
    if (link->event_fd >= 0)
        close(link->event_fd);
    if (link->ring != NULL && link->ring != MAP_FAILED)
        munmap(link->ring, ring_size);
}

static int parse_unsigned(const char *text, unsigned int *value)
{
    if (text[0] == '-' || isspace((unsigned char)text[0]))
        return -1;
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno == ERANGE || end == text || *end != '\0' || parsed > UINT_MAX)
        return -1;
    *value = (unsigned int)parsed;
    return 0;
}

static int parse_seconds(const char *text, double maximum, double *value)
{
    if (text[0] == '-' || isspace((unsigned char)text[0]))
        return -1;
    char *end = NULL;
    errno = 0;
    const double parsed = strtod(text, &end);
    if (errno == ERANGE || end == text || *end != '\0' ||
        parsed != parsed || parsed < 0.0 || parsed > maximum) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int sleep_seconds(double seconds)
{
    const time_t whole = (time_t)seconds;
    long nanoseconds = (long)((seconds - (double)whole) * 1000000000.0);
    if (nanoseconds >= 1000000000L)
        nanoseconds = 999999999L;
    struct timespec remaining = { .tv_sec = whole, .tv_nsec = nanoseconds };
    while (nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

static int wait_for_reply(int event_fd, double timeout_s)
{
    struct pollfd descriptor = {
        .fd = event_fd,
        .events = POLLIN,
    };
    const double timeout_ms_exact = timeout_s * 1000.0;
    int timeout_ms = (int)timeout_ms_exact;
    if ((double)timeout_ms < timeout_ms_exact)
        timeout_ms++;
    if (timeout_s > 0.0 && timeout_ms == 0)
        timeout_ms = 1;

    int result;
    do {
        result = poll(&descriptor, 1U, timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        return -1;
    if (result == 0)
        return 0;
    return (descriptor.revents & POLLIN) != 0 ? 1 : -1;
}

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
                          double interval_s,
                          double timeout_s,
                          edge_link_t *forward,
                          edge_link_t *reverse)
{
    if (index == 0U) {
        printf("PING zIPC chain: %u components (%u relays), %u probes, %.3f s interval\n",
               component_count, component_count - 2U, iterations, interval_s);
        fflush(stdout);
        uint64_t min_ns = UINT64_MAX;
        uint64_t max_ns = 0U;
        uint64_t sum_ns = 0U;
        unsigned int transmitted = 0U;
        unsigned int received = 0U;
        bool failed = false;
        for (unsigned int it = 0; it < iterations; ++it) {
            zipc_buffer_t buffer;
            CHECK(zipc_buffer_alloc_ex(forward[0].sender, 0U, 0U, 0U, &buffer, NULL));
            ping_payload_t payload;
            memset(&payload, 0, sizeof(payload));
            payload.sequence = it;
            payload.component_count = component_count;
            payload.depart_ns[0] = now_ns();
            CHECK(zipc_buffer_append(&buffer, &payload, sizeof(payload)));
            CHECK(zipc_send(forward[0].sender, &buffer));
            transmitted++;

            const int ready = wait_for_reply(reverse[0].event_fd, timeout_s);
            if (ready < 0) {
                perror("poll");
                failed = true;
                break;
            }
            if (ready == 0) {
                printf("timeout seq=%u after %.3f s\n", it, timeout_s);
                failed = true;
                break;
            }
            CHECK(zipc_recv(reverse[0].receiver, &buffer));
            received++;
            const uint64_t done = now_ns();
            const ping_payload_t *reply = zipc_buffer_data(&buffer);
            const uint64_t rtt_ns = done - reply->depart_ns[0];
            if (rtt_ns < min_ns)
                min_ns = rtt_ns;
            if (rtt_ns > max_ns)
                max_ns = rtt_ns;
            sum_ns += rtt_ns;
            printf("reply seq=%" PRIu64 " time=%" PRIu64 ".%03" PRIu64 " us",
                   reply->sequence, rtt_ns / 1000U, rtt_ns % 1000U);
            {
                const uint64_t one_way_ns =
                    reply->arrival_ns[component_count - 1U] -
                    reply->depart_ns[0];
                printf(" forward=%" PRIu64 ".%03" PRIu64 " us hops=[",
                       one_way_ns / 1000U, one_way_ns % 1000U);
                for (unsigned int hop = 1U; hop < component_count; ++hop) {
                    const uint64_t hop_ns =
                        reply->arrival_ns[hop] - reply->depart_ns[hop - 1U];
                    printf("%s%" PRIu64 ".%03" PRIu64, hop == 1U ? "" : ",",
                           hop_ns / 1000U, hop_ns % 1000U);
                }
                printf("]");
            }
            printf(" hop_count=%u\n", zipc_buffer_hop_count(&buffer));
            fflush(stdout);
            CHECK(zipc_buffer_release(&buffer));

            if (interval_s > 0.0 && it + 1U < iterations &&
                sleep_seconds(interval_s) != 0) {
                perror("nanosleep");
                failed = true;
                break;
            }
        }
        printf("--- zIPC chain ping statistics ---\n");
        const double loss = transmitted == 0U ? 0.0 :
            100.0 * (double)(transmitted - received) / (double)transmitted;
        printf("%u packets transmitted, %u received, %.1f%% packet loss\n",
               transmitted, received, loss);
        if (received > 0U) {
            const uint64_t average_ns = sum_ns / received;
            printf("rtt min/avg/max = %" PRIu64 ".%03" PRIu64 "/%" PRIu64 ".%03"
                   PRIu64 "/%" PRIu64 ".%03" PRIu64 " us\n",
                   min_ns / 1000U, min_ns % 1000U,
                   average_ns / 1000U, average_ns % 1000U,
                   max_ns / 1000U, max_ns % 1000U);
        }
        fflush(stdout);
        _exit(failed ? EXIT_FAILURE : EXIT_SUCCESS);
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
    fprintf(stderr,
            "Usage: %s [--relays N] [--count N] [--interval SECONDS] [--timeout SECONDS]\n"
            "  --relays N        number of relay components (default 0)\n"
            "  --count N         number of probes (default 10)\n"
            "  --interval SECONDS  delay between probes (default 1; 0 disables)\n"
            "  --timeout SECONDS   reply timeout (default 5; 0 polls once)\n",
            program);
}

int main(int argc, char **argv)
{
    unsigned int relay_count = 0U;
    unsigned int iterations = 10U;
    double interval_s = 1.0;
    double timeout_s = 5.0;
    bool relays_set = false;
    bool count_set = false;
    bool interval_set = false;
    bool timeout_set = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--relays") == 0 && i + 1 < argc) {
            if (parse_unsigned(argv[++i], &relay_count) != 0) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            relays_set = true;
        } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            if (parse_unsigned(argv[++i], &iterations) != 0) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            count_set = true;
        } else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            if (parse_seconds(argv[++i], (double)INT_MAX, &interval_s) != 0) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            interval_set = true;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            if (parse_seconds(argv[++i], (double)INT_MAX / 1000.0,
                              &timeout_s) != 0) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            timeout_set = true;
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (relay_count > MAX_COMPONENTS - 2U || iterations == 0U) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const unsigned int component_count = relay_count + 2U;
    printf("config: relays=%u%s count=%u%s interval=%.3f%s timeout=%.3f%s\n",
           relay_count, relays_set ? "" : " (default)",
           iterations, count_set ? "" : " (default)",
           interval_s, interval_set ? "" : " (default)",
           timeout_s, timeout_set ? "" : " (default)");
    const unsigned int edge_count = component_count - 1U;

    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "/zipc_ping_%ld", (long)getpid());
    zipc_platform_memory_t *memory = NULL;
    edge_link_t *forward = NULL;
    edge_link_t *reverse = NULL;
    pid_t *children = NULL;
    const size_t ring_size = zipc_transport_spsc_ring_size(RING_DEPTH);
    int result = EXIT_FAILURE;
    const zipc_platform_memory_config_t memory_cfg = {
        .type = ZIPC_SHM_POSIX,
        .size = SHM_SIZE,
        .backend.posix = {
            .name = shm_name,
            .create = true,
            .unlink_on_close = true,
        },
    };
    zipc_status_t setup_status = zipc_platform_memory_open(&memory, &memory_cfg);
    if (setup_status != ZIPC_OK) {
        fprintf(stderr, "zipc_platform_memory_open failed: %d\n",
                (int)setup_status);
        goto cleanup;
    }

    zipc_pool_t pool;
    const size_t payload_offset =
        (zipc_pool_required_control_size(SLOT_COUNT) + 63U) & ~(size_t)63U;
    const zipc_pool_config_t pool_cfg = {
        .control_memory = memory,
        .payload_memory = NULL,
        .control_offset = 0U,
        .payload_offset = payload_offset,
        .slot_count = SLOT_COUNT,
        .slot_capacity = SLOT_SIZE,
        .slot_stride = SLOT_SIZE,
        .payload_alignment = 64U,
    };
    setup_status = zipc_pool_format(&pool_cfg);
    if (setup_status != ZIPC_OK) {
        fprintf(stderr, "zipc_pool_format failed: %d\n", (int)setup_status);
        goto cleanup;
    }
    setup_status = zipc_pool_attach(&pool, &pool_cfg);
    if (setup_status != ZIPC_OK) {
        fprintf(stderr, "zipc_pool_attach failed: %d\n", (int)setup_status);
        goto cleanup;
    }

    forward = calloc(edge_count, sizeof(*forward));
    reverse = calloc(edge_count, sizeof(*reverse));
    children = calloc(component_count, sizeof(*children));
    for (unsigned int edge = 0U; edge < edge_count; ++edge) {
        if (forward != NULL)
            forward[edge].event_fd = -1;
        if (reverse != NULL)
            reverse[edge].event_fd = -1;
    }
    if (forward == NULL || reverse == NULL || children == NULL) {
        fprintf(stderr, "allocation failed\n");
        goto cleanup;
    }

    for (unsigned int edge = 0U; edge < edge_count; ++edge) {
        edge_link_t *directions[2] = { &forward[edge], &reverse[edge] };
        for (unsigned int d = 0U; d < 2U; ++d) {
            edge_link_t *link = directions[d];
            link->ring = mmap(NULL, ring_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
            if (link->ring == MAP_FAILED) {
                perror("mmap");
                goto cleanup;
            }
            setup_status =
                zipc_transport_spsc_ring_initialize(link->ring, RING_DEPTH);
            if (setup_status != ZIPC_OK) {
                fprintf(stderr, "ring initialization failed: %d\n",
                        (int)setup_status);
                goto cleanup;
            }
            link->event_fd = eventfd(0U, EFD_CLOEXEC);
            if (link->event_fd < 0) {
                perror("eventfd");
                goto cleanup;
            }
            const zipc_platform_transport_config_t transport = {
                .type = ZIPC_TRANSPORT_SHM_RING_EVENTFD,
                .platform_handle = link->ring,
                .receive_handle = (void *)(intptr_t)link->event_fd,
                .ring_depth = RING_DEPTH,
            };
            const unsigned int src = d == 0U ? edge + 1U : edge + 2U;
            const unsigned int dst = d == 0U ? edge + 2U : edge + 1U;
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
            setup_status = zipc_link_create(&link->sender, &sender_cfg);
            if (setup_status != ZIPC_OK) {
                fprintf(stderr, "sender link creation failed: %d\n",
                        (int)setup_status);
                goto cleanup;
            }
            setup_status = zipc_link_create(&link->receiver, &receiver_cfg);
            if (setup_status != ZIPC_OK) {
                fprintf(stderr, "receiver link creation failed: %d\n",
                        (int)setup_status);
                goto cleanup;
            }
        }
    }

    result = EXIT_SUCCESS;
    unsigned int spawned = 0U;
    fflush(NULL);
    for (unsigned int i = 0U; i < component_count; ++i) {
        children[i] = fork();
        if (children[i] < 0) {
            perror("fork");
            result = EXIT_FAILURE;
            for (unsigned int child = 0U; child < spawned; ++child)
                (void)kill(children[child], SIGTERM);
            break;
        }
        if (children[i] == 0)
            run_component(i, component_count, iterations, interval_s, timeout_s,
                          forward, reverse);
        spawned++;
    }

    unsigned int remaining = spawned;
    while (remaining > 0U) {
        int status = 0;
        const pid_t finished = waitpid(-1, &status, 0);
        if (finished < 0) {
            if (errno == EINTR)
                continue;
            result = EXIT_FAILURE;
            break;
        }
        remaining--;
        for (unsigned int i = 0U; i < component_count; ++i) {
            if (children[i] == finished) {
                children[i] = 0;
                break;
            }
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
            result = EXIT_FAILURE;
            for (unsigned int i = 0U; i < component_count; ++i) {
                if (children[i] > 0)
                    (void)kill(children[i], SIGTERM);
            }
        }
    }

cleanup:
    for (unsigned int edge = 0U; edge < edge_count; ++edge) {
        edge_link_cleanup(forward == NULL ? NULL : &forward[edge], ring_size);
        edge_link_cleanup(reverse == NULL ? NULL : &reverse[edge], ring_size);
    }
    free(children);
    free(reverse);
    free(forward);
    zipc_platform_memory_close(memory);
    return result;
}
