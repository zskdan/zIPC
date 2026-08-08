/* Reference integration fragment; build inside the R5_0 FreeRTOS BSP. */
#include "universal-chain.h"
#include "FreeRTOS.h"
#include "task.h"

void zipc_r5_0_task1(void *arg)
{
    zipc_universal_links_t *links = arg;
    zipc_message_t message;
    /* Allocate/fill a slot, prepare transfer to TASK2, then: */
    (void)zipc_platform_transport_send(links->r5_task1_to_task2, &message);
    vTaskDelete(NULL);
}

void zipc_r5_0_task2(void *arg)
{
    zipc_universal_links_t *links = arg;
    zipc_message_t message;
    (void)zipc_platform_transport_receive(links->r5_task1_to_task2, &message);
    /* Claim/transform, then notify Linux through RPMsg or IPI. */
    (void)zipc_platform_transport_send(links->r5_task2_to_linux_user1, &message);
    vTaskDelete(NULL);
}
