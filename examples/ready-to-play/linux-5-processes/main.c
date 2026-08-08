#define _GNU_SOURCE
#include <zipc/zipc.h>

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PROCESS_COUNT 5U
#define LINK_COUNT    (PROCESS_COUNT - 1U)
#define RING_DEPTH    64U
#define SLOT_COUNT    32U
#define SLOT_SIZE     4096U
#define SHM_SIZE      (256U * 1024U)

#define CHECK_STATUS(expr)                                                     \
    do {                                                                       \
        zipc_status_t status_ = (expr);                                        \
        if (status_ != ZIPC_OK) {                                              \
            fprintf(stderr, "%s failed: %d\n", #expr, (int)status_);         \
            exit(EXIT_FAILURE);                                                \
        }                                                                      \
    } while (0)

typedef struct {
    zipc_transport_spsc_ring_t *ring;
    int event_fd;
    zipc_link_t *sender;
    zipc_link_t *receiver;
} process_link_t;

static void child_run(unsigned int index,
                      process_link_t links[LINK_COUNT])
{
    zipc_buffer_t buffer;
    char stage[32];

    if (index == 0U) {
        CHECK_STATUS(zipc_buffer_get(links[0].sender, 64U, &buffer));
        CHECK_STATUS(zipc_buffer_append(&buffer, "P0", 2U));
        CHECK_STATUS(zipc_send(links[0].sender, &buffer));
        _exit(EXIT_SUCCESS);
    }

    CHECK_STATUS(zipc_receive(links[index - 1U].receiver, &buffer));
    const int count = snprintf(stage, sizeof(stage), "->P%u", index);
    if (count < 0 || (size_t)count >= sizeof(stage))
        _exit(EXIT_FAILURE);
    CHECK_STATUS(zipc_buffer_append(&buffer, stage, (uint32_t)count));

    if (index + 1U < PROCESS_COUNT) {
        CHECK_STATUS(zipc_send(links[index].sender, &buffer));
    } else {
        printf("final: %.*s\n", (int)zipc_buffer_length(&buffer),
               (const char *)zipc_buffer_const_data(&buffer));
        printf("hop_count=%u distinct=%u\n",
               buffer.control->hop_count,
               (unsigned int)__builtin_popcountll(buffer.control->visited_mask));
        CHECK_STATUS(zipc_buffer_put(links[index - 1U].receiver, &buffer));
        fflush(stdout);
    }
    _exit(EXIT_SUCCESS);
}

int main(void)
{
    zipc_platform_memory_t *memory = NULL;
    zipc_pool_t pool;
    process_link_t links[LINK_COUNT] = {0};
    pid_t children[PROCESS_COUNT] = {0};

    const zipc_platform_memory_config_t memory_cfg = {
        .type = ZIPC_SHM_POSIX,
        .size = SHM_SIZE,
        .backend.posix = {
            .name = "/zipc_rtp_linux5",
            .create = true,
            .unlink_on_close = true,
        },
    };
    CHECK_STATUS(zipc_platform_memory_open(&memory, &memory_cfg));

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
    CHECK_STATUS(zipc_pool_format(&pool_cfg));
    CHECK_STATUS(zipc_pool_attach(&pool, &pool_cfg));

    for (unsigned int i = 0U; i < LINK_COUNT; ++i) {
        const size_t ring_size = zipc_transport_spsc_ring_size(RING_DEPTH);
        links[i].ring = mmap(NULL, ring_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (links[i].ring == MAP_FAILED) {
            perror("mmap ring");
            return EXIT_FAILURE;
        }
        CHECK_STATUS(zipc_transport_spsc_ring_initialize(links[i].ring,
                                                         RING_DEPTH));
        links[i].event_fd = eventfd(0U, EFD_CLOEXEC);
        if (links[i].event_fd < 0) {
            perror("eventfd");
            return EXIT_FAILURE;
        }

        const zipc_platform_transport_config_t transport_cfg = {
            .type = ZIPC_TRANSPORT_SHM_RING_EVENTFD,
            .platform_handle = links[i].ring,
            .receive_handle = (void *)(intptr_t)links[i].event_fd,
            .ring_depth = RING_DEPTH,
        };
        const zipc_link_config_t sender_cfg = {
            .pool = &pool,
            .local_component = (zipc_component_id_t)i,
            .remote_component = (zipc_component_id_t)(i + 1U),
            .transport = transport_cfg,
        };
        const zipc_link_config_t receiver_cfg = {
            .pool = &pool,
            .local_component = (zipc_component_id_t)(i + 1U),
            .remote_component = (zipc_component_id_t)i,
            .transport = transport_cfg,
        };
        CHECK_STATUS(zipc_link_create(&links[i].sender, &sender_cfg));
        CHECK_STATUS(zipc_link_create(&links[i].receiver, &receiver_cfg));
    }

    for (unsigned int i = 0U; i < PROCESS_COUNT; ++i) {
        children[i] = fork();
        if (children[i] < 0) {
            perror("fork");
            return EXIT_FAILURE;
        }
        if (children[i] == 0)
            child_run(i, links);
    }

    int result = EXIT_SUCCESS;
    for (unsigned int i = 0U; i < PROCESS_COUNT; ++i) {
        int status = 0;
        if (waitpid(children[i], &status, 0) < 0 ||
            !WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS)
            result = EXIT_FAILURE;
    }

    for (unsigned int i = 0U; i < LINK_COUNT; ++i) {
        const size_t ring_size = zipc_transport_spsc_ring_size(RING_DEPTH);
        zipc_link_destroy(links[i].receiver);
        zipc_link_destroy(links[i].sender);
        close(links[i].event_fd);
        munmap(links[i].ring, ring_size);
    }
    zipc_platform_memory_close(memory);
    return result;
}
