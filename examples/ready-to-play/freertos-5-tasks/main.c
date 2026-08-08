#include <zipc/zipc.h>

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <string.h>

#define TASK_COUNT 5U
#define SHM_BASE   UINT64_C(0x70000000)
#define SHM_SIZE   (256U * 1024U)

static zipc_platform_memory_t *g_memory;
static zipc_pool_t g_pool;
static zipc_link_t *g_tx[TASK_COUNT - 1U];
static zipc_link_t *g_rx[TASK_COUNT - 1U];
static TaskHandle_t g_tasks[TASK_COUNT];
static zipc_message_t g_mailboxes[TASK_COUNT - 1U];

static void check(zipc_status_t status)
{
    configASSERT(status == ZIPC_OK);
}

static void stage_task(void *argument)
{
    const uint32_t index = (uint32_t)(uintptr_t)argument;
    zipc_buffer_t buffer;
    char suffix[16];

    if (index == 0U) {
        check(zipc_buffer_get(g_tx[0], 32U, &buffer));
        check(zipc_buffer_append(&buffer, "T0", 2U));
    } else {
        check(zipc_receive(g_rx[index - 1U], &buffer));
        const int n = snprintf(suffix, sizeof(suffix), "->T%lu",
                               (unsigned long)index);
        configASSERT(n > 0 && (size_t)n < sizeof(suffix));
        check(zipc_buffer_append(&buffer, suffix, (uint32_t)n));
    }

    if (index + 1U < TASK_COUNT) {
        check(zipc_send(g_tx[index], &buffer));
    } else {
        printf("final: %.*s\n", (int)zipc_buffer_length(&buffer),
               (const char *)zipc_buffer_const_data(&buffer));
        check(zipc_buffer_put(g_rx[index - 1U], &buffer));
    }

    vTaskDelete(NULL);
}

void zipc_ready_freertos_start(void)
{
    const zipc_platform_memory_config_t memory_cfg = {
        .type = ZIPC_SHM_DTREVMEM_UNCACHED,
        .size = SHM_SIZE,
        .backend.dtrevmem = {
            .physical_address = SHM_BASE,
            .supports_cpu_atomics = true,
            .remote_accessible = false,
            .device_memory = false,
        },
    };
    check(zipc_platform_memory_open(&g_memory, &memory_cfg));

    const zipc_pool_config_t pool_cfg = {
        .control_memory = g_memory,
        .payload_memory = NULL,
        .payload_offset = 16U * 1024U,
        .slot_count = 16U,
        .slot_capacity = 2048U,
        .payload_alignment = 64U,
    };
    check(zipc_pool_format(&pool_cfg));
    check(zipc_pool_attach(&g_pool, &pool_cfg));

    /* Create tasks suspended so all destination handles are known first. */
    for (uint32_t i = 0U; i < TASK_COUNT; ++i) {
        BaseType_t rc = xTaskCreate(stage_task, "zipc-stage", 1024U,
                                    (void *)(uintptr_t)i,
                                    tskIDLE_PRIORITY + 2U, &g_tasks[i]);
        configASSERT(rc == pdPASS);
        vTaskSuspend(g_tasks[i]);
    }

    for (uint32_t i = 0U; i + 1U < TASK_COUNT; ++i) {
        const zipc_platform_transport_config_t transport_cfg = {
            .type = ZIPC_TRANSPORT_TASK_NOTIFICATION,
            .platform_handle = g_tasks[i + 1U],
            .shared_mailbox = &g_mailboxes[i],
            .notification_index = 0U,
            .timeout_ticks = portMAX_DELAY,
        };
        const zipc_link_config_t tx_cfg = {
            .pool = &g_pool,
            .local_component = (zipc_component_id_t)i,
            .remote_component = (zipc_component_id_t)(i + 1U),
            .transport = transport_cfg,
        };
        const zipc_link_config_t rx_cfg = {
            .pool = &g_pool,
            .local_component = (zipc_component_id_t)(i + 1U),
            .remote_component = (zipc_component_id_t)i,
            .transport = transport_cfg,
        };
        check(zipc_link_create(&g_tx[i], &tx_cfg));
        check(zipc_link_create(&g_rx[i], &rx_cfg));
    }

    for (uint32_t i = 0U; i < TASK_COUNT; ++i)
        vTaskResume(g_tasks[i]);
}
