#include <zipc/zipc.h>
#include "../shared-layout.h"
#include <string.h>

int main(void)
{
    /* Platform startup must expose /dev/rpmsg_zipc_r5_0 and the shared pool. */
    zipc_platform_memory_t *memory = NULL;
    zipc_pool_t pool;
    zipc_link_t *link = NULL;
    zipc_buffer_t buffer;

    const zipc_platform_memory_config_t mem_cfg = {
        .type = ZIPC_SHM_DTREVMEM_CACHED,
        .size = ZIPC_READY_SHM_SIZE,
        .backend.dtrevmem = {
            .device_path = "/dev/zipc-shm",
            .physical_address = ZIPC_READY_SHM_BASE,
            .supports_cpu_atomics = true,
            .remote_accessible = true,
        },
    };
    if (zipc_platform_memory_open(&memory, &mem_cfg) != ZIPC_OK) return 1;

    const zipc_pool_config_t pool_cfg = {
        .control_memory = memory, .payload_memory = NULL,
        .payload_offset = 64U * 1024U,
        .slot_count = ZIPC_READY_SLOT_COUNT,
        .slot_capacity = ZIPC_READY_SLOT_SIZE,
        .payload_alignment = 64U,
    };
    if (zipc_pool_format(&pool_cfg) != ZIPC_OK ||
        zipc_pool_attach(&pool, &pool_cfg) != ZIPC_OK) return 1;

    const zipc_link_config_t link_cfg = {
        .pool = &pool,
        .local_component = ZIPC_COMPONENT_LINUX_P1,
        .remote_component = ZIPC_COMPONENT_R5_0,
        .transport = {
            .type = ZIPC_TRANSPORT_RPMSG,
            .device_path = "/dev/rpmsg_zipc_r5_0",
        },
    };
    if (zipc_link_create(&link, &link_cfg) != ZIPC_OK) return 1;
    if (zipc_buffer_alloc_ex(link, 0U, 64U, 0U, &buffer) != ZIPC_OK) return 1;
    if (zipc_buffer_append(&buffer, "Linux-P1", 8U) != ZIPC_OK) return 1;
    return zipc_send(link, &buffer) == ZIPC_OK ? 0 : 1;
}
