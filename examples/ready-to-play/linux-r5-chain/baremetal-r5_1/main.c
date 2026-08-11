#include <zipc/zipc.h>
#include "../shared-layout.h"

extern zipc_message_t g_r5_ipi_mailbox;
extern zipc_status_t board_ipi_wait_r5_0(void *context, uint32_t timeout);
extern struct rpmsg_endpoint g_linux_rpmsg_endpoint;
extern zipc_message_t g_rpmsg_rx_mailbox;
extern zipc_status_t board_openamp_poll(void *context, uint32_t timeout);

int main(void)
{
    zipc_platform_memory_t *memory = NULL;
    zipc_pool_t pool;
    zipc_link_t *from_r5_0 = NULL;
    zipc_link_t *to_linux = NULL;
    zipc_buffer_t buffer;

    const zipc_platform_memory_config_t mem_cfg = {
        .type = ZIPC_SHM_DTREVMEM_UNCACHED,
        .size = ZIPC_READY_SHM_SIZE,
        .backend.dtrevmem = {
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
    if (zipc_pool_attach(&pool, &pool_cfg) != ZIPC_OK) return 1;

    const zipc_link_config_t rx_cfg = {
        .pool = &pool,
        .local_component = ZIPC_COMPONENT_R5_1,
        .remote_component = ZIPC_COMPONENT_R5_0,
        .transport = {
            .type = ZIPC_TRANSPORT_IPI,
            .shared_mailbox = &g_r5_ipi_mailbox,
            .signal_wait = board_ipi_wait_r5_0,
            .timeout_ticks = UINT32_MAX,
        },
    };
    const zipc_link_config_t tx_cfg = {
        .pool = &pool,
        .local_component = ZIPC_COMPONENT_R5_1,
        .remote_component = ZIPC_COMPONENT_LINUX_P2,
        .transport = {
            .type = ZIPC_TRANSPORT_RPMSG,
            .platform_handle = &g_linux_rpmsg_endpoint,
            .shared_mailbox = &g_rpmsg_rx_mailbox,
            .signal_wait = board_openamp_poll,
            .timeout_ticks = UINT32_MAX,
        },
    };
    if (zipc_link_create(&from_r5_0, &rx_cfg) != ZIPC_OK ||
        zipc_link_create(&to_linux, &tx_cfg) != ZIPC_OK) return 1;
    if (zipc_recv(from_r5_0, &buffer) != ZIPC_OK) return 1;
    if (zipc_buffer_append(&buffer, "->R5_1", 6U) != ZIPC_OK) return 1;
    return zipc_send(to_linux, &buffer) == ZIPC_OK ? 0 : 1;
}
