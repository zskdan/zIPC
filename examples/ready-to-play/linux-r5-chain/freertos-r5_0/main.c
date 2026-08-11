#include <zipc/zipc.h>
#include "../shared-layout.h"
#include "FreeRTOS.h"
#include "task.h"

extern struct rpmsg_endpoint g_linux_rpmsg_endpoint;
extern zipc_message_t g_rpmsg_rx_mailbox;
extern QueueHandle_t g_rpmsg_rx_queue;
extern zipc_message_t g_r5_ipi_mailbox;
extern zipc_status_t board_ipi_send_r5_1(void *context);

void zipc_r5_0_task(void *argument)
{
    (void)argument;
    zipc_platform_memory_t *memory = NULL;
    zipc_pool_t pool;
    zipc_link_t *from_linux = NULL;
    zipc_link_t *to_r5_1 = NULL;
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
    configASSERT(zipc_platform_memory_open(&memory, &mem_cfg) == ZIPC_OK);
    const zipc_pool_config_t pool_cfg = {
        .control_memory = memory, .payload_memory = NULL,
        .payload_offset = 64U * 1024U,
        .slot_count = ZIPC_READY_SLOT_COUNT,
        .slot_capacity = ZIPC_READY_SLOT_SIZE,
        .payload_alignment = 64U,
    };
    configASSERT(zipc_pool_attach(&pool, &pool_cfg) == ZIPC_OK);

    const zipc_link_config_t rx_cfg = {
        .pool = &pool,
        .local_component = ZIPC_COMPONENT_R5_0,
        .remote_component = ZIPC_COMPONENT_LINUX_P1,
        .transport = {
            .type = ZIPC_TRANSPORT_RPMSG,
            .platform_handle = &g_linux_rpmsg_endpoint,
            .receive_handle = g_rpmsg_rx_queue,
            .shared_mailbox = &g_rpmsg_rx_mailbox,
            .timeout_ticks = portMAX_DELAY,
        },
    };
    const zipc_link_config_t tx_cfg = {
        .pool = &pool,
        .local_component = ZIPC_COMPONENT_R5_0,
        .remote_component = ZIPC_COMPONENT_R5_1,
        .transport = {
            .type = ZIPC_TRANSPORT_IPI,
            .shared_mailbox = &g_r5_ipi_mailbox,
            .signal_send = board_ipi_send_r5_1,
        },
    };
    configASSERT(zipc_link_create(&from_linux, &rx_cfg) == ZIPC_OK);
    configASSERT(zipc_link_create(&to_r5_1, &tx_cfg) == ZIPC_OK);
    configASSERT(zipc_recv(from_linux, &buffer) == ZIPC_OK);
    configASSERT(zipc_buffer_append(&buffer, "->R5_0", 6U) == ZIPC_OK);
    configASSERT(zipc_send(to_r5_1, &buffer) == ZIPC_OK);
    vTaskDelete(NULL);
}
