#define _POSIX_C_SOURCE 200809L
#include <zipc/zipc.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SLOT_COUNT 4U
#define SLOT_CAPACITY (64U * 1024U)

static int expect_guard_fault(uint8_t *address)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        *address = UINT8_C(0x5a);
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return -1;

    if (!WIFSIGNALED(status))
        return -1;

    const int sig = WTERMSIG(status);
    return (sig == SIGSEGV || sig == SIGBUS) ? 0 : -1;
}

int main(void)
{
    const size_t page_size = zipc_platform_page_size();
    if (page_size == 0U || (SLOT_CAPACITY % page_size) != 0U) {
        fprintf(stderr, "guard-pages: unsupported page size\n");
        return 1;
    }

    const size_t control_size = zipc_pool_required_control_size(SLOT_COUNT);
    const size_t payload_size =
        zipc_pool_guarded_payload_size(SLOT_COUNT, SLOT_CAPACITY);
    const uint32_t guarded_stride =
        zipc_pool_guarded_slot_stride(SLOT_CAPACITY);
    if (control_size == 0U || payload_size == 0U || guarded_stride == 0U)
        return 1;

    char control_name[64];
    char payload_name[64];
    snprintf(control_name, sizeof(control_name), "/zipc-guard-ctrl-%ld", (long)getpid());
    snprintf(payload_name, sizeof(payload_name), "/zipc-guard-data-%ld", (long)getpid());

    zipc_platform_memory_t *control = NULL;
    zipc_platform_memory_t *payload = NULL;
    zipc_platform_memory_config_t control_cfg = {
        .type = ZIPC_SHM_POSIX,
        .size = control_size,
        .backend.posix = {
            .name = control_name,
            .create = true,
            .unlink_on_close = true,
        },
    };
    zipc_platform_memory_config_t payload_cfg = {
        .type = ZIPC_SHM_POSIX,
        .size = payload_size,
        .backend.posix = {
            .name = payload_name,
            .create = true,
            .unlink_on_close = true,
        },
    };

    if (zipc_platform_memory_open(&control, &control_cfg) != ZIPC_OK ||
        zipc_platform_memory_open(&payload, &payload_cfg) != ZIPC_OK) {
        fprintf(stderr, "guard-pages: memory open failed\n");
        zipc_platform_memory_close(payload);
        zipc_platform_memory_close(control);
        return 1;
    }

    zipc_pool_config_t pool_cfg = {
        .control_memory = control,
        .payload_memory = payload,
        .slot_count = SLOT_COUNT,
        .slot_capacity = SLOT_CAPACITY,
        .slot_stride = 0U,
        .payload_alignment = 64U,
        .flags = ZIPC_POOL_F_GUARD_PAGES,
    };

    if (zipc_pool_format(&pool_cfg) != ZIPC_OK) {
        fprintf(stderr, "guard-pages: pool format failed\n");
        goto fail;
    }

    zipc_pool_t pool;
    if (zipc_pool_attach(&pool, &pool_cfg) != ZIPC_OK) {
        fprintf(stderr, "guard-pages: pool attach failed\n");
        goto fail;
    }

    zipc_buffer_t buffer;
    if (zipc_buffer_allocate(&pool, 1U, 0U, &buffer) != ZIPC_OK) {
        fprintf(stderr, "guard-pages: allocation failed\n");
        goto fail;
    }

    uint8_t *slot_data = zipc_buffer_data(&buffer);
    memset(slot_data, 0xa5, SLOT_CAPACITY);

    if (expect_guard_fault(slot_data + SLOT_CAPACITY) != 0) {
        fprintf(stderr, "guard-pages: write past slot did not fault\n");
        goto fail;
    }

    /* Accessing another valid slot is still possible by address; the guard is
     * specifically an overflow/underrun detector, not an ownership MMU. */
    uint8_t *next_slot = slot_data + guarded_stride;
    *next_slot = UINT8_C(0x3c);
    if (*next_slot != UINT8_C(0x3c)) {
        fprintf(stderr, "guard-pages: next valid slot is unexpectedly protected\n");
        goto fail;
    }

    if (zipc_buffer_set_region(&buffer, 0U, SLOT_CAPACITY + 1U) !=
        ZIPC_ERR_REGION_OVERFLOW) {
        fprintf(stderr, "guard-pages: API bounds check failed\n");
        goto fail;
    }

    if (zipc_buffer_release(&buffer) != ZIPC_OK) {
        fprintf(stderr, "guard-pages: release failed\n");
        goto fail;
    }

    printf("guard-pages: PASS (slot=%u, page=%zu, stride=%u)\n",
           SLOT_CAPACITY, page_size, guarded_stride);
    zipc_platform_memory_close(payload);
    zipc_platform_memory_close(control);
    return 0;

fail:
    zipc_platform_memory_close(payload);
    zipc_platform_memory_close(control);
    return 1;
}
