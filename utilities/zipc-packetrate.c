#define _GNU_SOURCE
#include <zipc/zipc.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>

#define SLOT_COUNT 256U
#define SLOT_SIZE 256U
#define SHM_SIZE (256U * 1024U)
#define DEFAULT_RING_DEPTH 1024U
#define WARMUP_PACKETS 1000U

#define CHECK(expr) do { \
    zipc_status_t s_ = (expr); \
    if (s_ != ZIPC_OK) { \
        fprintf(stderr, "%s failed: %d\n", #expr, (int)s_); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}


static void buffer_get_wait(zipc_link_t *sender, zipc_buffer_t *buffer)
{
    for (;;) {
        const zipc_status_t status = zipc_buffer_alloc_ex(sender, 0U, 0U, 0U, buffer);
        if (status == ZIPC_OK)
            return;
        if (status != ZIPC_ERR_NO_BUFFER) {
            fprintf(stderr, "zipc_buffer_alloc_ex failed: %d\n", (int)status);
            exit(EXIT_FAILURE);
        }
        sched_yield();
    }
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--transport ring-eventfd|fifo|unix-dgram|mqueue] "
            "[--packets N] [--payload BYTES] [--ring-depth N]\n"
            "Defaults: --transport ring-eventfd --packets 1000000 "
            "--payload 8 --ring-depth %u\n",
            program, (unsigned)DEFAULT_RING_DEPTH);
}

int main(int argc, char **argv)
{
    const char *transport_name = "ring-eventfd";
    uint64_t packet_count = UINT64_C(1000000);
    uint32_t payload_size = 8U;
    uint32_t ring_depth = DEFAULT_RING_DEPTH;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--transport") == 0 && i + 1 < argc)
            transport_name = argv[++i];
        else if (strcmp(argv[i], "--packets") == 0 && i + 1 < argc)
            packet_count = strtoull(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--payload") == 0 && i + 1 < argc)
            payload_size = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--ring-depth") == 0 && i + 1 < argc)
            ring_depth = (uint32_t)strtoul(argv[++i], NULL, 0);
        else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (packet_count == 0U || payload_size > SLOT_SIZE || ring_depth < 2U) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "/zipc_tbench_%ld", (long)getpid());
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
        .payload_offset = 64U * 1024U,
        .slot_count = SLOT_COUNT,
        .slot_capacity = SLOT_SIZE,
        .slot_stride = SLOT_SIZE,
        .payload_alignment = 64U,
    };
    CHECK(zipc_pool_format(&pool_cfg));
    CHECK(zipc_pool_attach(&pool, &pool_cfg));

    zipc_platform_transport_config_t sender_transport;
    zipc_platform_transport_config_t receiver_transport;
    memset(&sender_transport, 0, sizeof(sender_transport));
    memset(&receiver_transport, 0, sizeof(receiver_transport));

    zipc_transport_spsc_ring_t *ring = NULL;
    size_t ring_size = 0U;
    int event_fd = -1;
    char endpoint[108];

    if (strcmp(transport_name, "ring-eventfd") == 0) {
        ring_size = zipc_transport_spsc_ring_size(ring_depth);
        ring = mmap(NULL, ring_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (ring == MAP_FAILED) {
            perror("mmap");
            return EXIT_FAILURE;
        }
        CHECK(zipc_transport_spsc_ring_initialize(ring, ring_depth));
        event_fd = eventfd(0U, EFD_CLOEXEC);
        if (event_fd < 0) {
            perror("eventfd");
            return EXIT_FAILURE;
        }
        sender_transport.type = ZIPC_TRANSPORT_SHM_RING_EVENTFD;
        sender_transport.platform_handle = ring;
        sender_transport.receive_handle = (void *)(intptr_t)event_fd;
        sender_transport.ring_depth = ring_depth;
        receiver_transport = sender_transport;
    } else if (strcmp(transport_name, "fifo") == 0) {
        snprintf(endpoint, sizeof(endpoint), "/tmp/zipc_tbench_%ld.fifo",
                 (long)getpid());
        sender_transport.type = ZIPC_TRANSPORT_FIFO;
        sender_transport.endpoint_name = endpoint;
        receiver_transport = sender_transport;
        receiver_transport.create_endpoint = true;
    } else if (strcmp(transport_name, "unix-dgram") == 0) {
        snprintf(endpoint, sizeof(endpoint), "/tmp/zipc_tbench_%ld.sock",
                 (long)getpid());
        sender_transport.type = ZIPC_TRANSPORT_UNIX_DGRAM;
        sender_transport.endpoint_name = endpoint;
        receiver_transport = sender_transport;
        receiver_transport.create_endpoint = true;
    } else if (strcmp(transport_name, "mqueue") == 0) {
        snprintf(endpoint, sizeof(endpoint), "/zipc_tbench_%ld", (long)getpid());
        sender_transport.type = ZIPC_TRANSPORT_POSIX_MQUEUE;
        sender_transport.endpoint_name = endpoint;
        receiver_transport = sender_transport;
        receiver_transport.create_endpoint = true;
    } else {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    zipc_link_t *receiver = NULL;
    zipc_link_t *sender = NULL;
    const zipc_link_config_t receiver_cfg = {
        .pool = &pool,
        .local_component = 1U,
        .remote_component = 0U,
        .transport = receiver_transport,
    };
    const zipc_link_config_t sender_cfg = {
        .pool = &pool,
        .local_component = 0U,
        .remote_component = 1U,
        .transport = sender_transport,
    };
    CHECK(zipc_link_create(&receiver, &receiver_cfg));
    CHECK(zipc_link_create(&sender, &sender_cfg));

    int ready_pipe[2];
    int done_pipe[2];
    if (pipe(ready_pipe) != 0 || pipe(done_pipe) != 0) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    const pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return EXIT_FAILURE;
    }
    if (child == 0) {
        close(ready_pipe[0]);
        close(done_pipe[0]);
        for (uint64_t i = 0U; i < WARMUP_PACKETS; ++i) {
            zipc_buffer_t buffer;
            CHECK(zipc_recv(receiver, &buffer));
            CHECK(zipc_buffer_release(&buffer));
        }
        const uint8_t ready = 1U;
        if (write(ready_pipe[1], &ready, sizeof(ready)) != sizeof(ready))
            _exit(EXIT_FAILURE);
        for (uint64_t i = 0U; i < packet_count; ++i) {
            zipc_buffer_t buffer;
            CHECK(zipc_recv(receiver, &buffer));
            CHECK(zipc_buffer_release(&buffer));
        }
        const uint8_t done = 1U;
        if (write(done_pipe[1], &done, sizeof(done)) != sizeof(done))
            _exit(EXIT_FAILURE);
        _exit(EXIT_SUCCESS);
    }

    close(ready_pipe[1]);
    close(done_pipe[1]);
    uint8_t payload[SLOT_SIZE];
    memset(payload, 0xA5, sizeof(payload));

    for (uint64_t i = 0U; i < WARMUP_PACKETS; ++i) {
        zipc_buffer_t buffer;
        buffer_get_wait(sender, &buffer);
        CHECK(zipc_buffer_append(&buffer, payload, payload_size));
        CHECK(zipc_send(sender, &buffer));
    }
    uint8_t marker;
    if (read(ready_pipe[0], &marker, sizeof(marker)) != sizeof(marker))
        return EXIT_FAILURE;

    const uint64_t start = now_ns();
    for (uint64_t i = 0U; i < packet_count; ++i) {
        zipc_buffer_t buffer;
        buffer_get_wait(sender, &buffer);
        CHECK(zipc_buffer_append(&buffer, payload, payload_size));
        CHECK(zipc_send(sender, &buffer));
    }
    if (read(done_pipe[0], &marker, sizeof(marker)) != sizeof(marker))
        return EXIT_FAILURE;
    const uint64_t end = now_ns();

    int child_status = 0;
    if (waitpid(child, &child_status, 0) < 0 ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    const double seconds = (double)(end - start) / 1e9;
    const double pps = (double)packet_count / seconds;
    const double mib_s = (double)packet_count * payload_size /
                         (1024.0 * 1024.0 * seconds);
    printf("transport=%s packets=%" PRIu64 " payload=%u elapsed=%.6f s\n",
           transport_name, packet_count, payload_size, seconds);
    printf("rate=%.0f packets/s payload-throughput=%.3f MiB/s\n", pps, mib_s);

    close(done_pipe[0]);
    close(ready_pipe[0]);
    zipc_link_destroy(sender);
    zipc_link_destroy(receiver);
    if (event_fd >= 0)
        close(event_fd);
    if (ring != NULL)
        munmap(ring, ring_size);
    zipc_platform_memory_close(memory);
    return EXIT_SUCCESS;
}
