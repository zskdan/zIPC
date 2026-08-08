/* Reference integration fragment for the two normal Linux processes. */
#include "universal-chain.h"

int zipc_linux_user_process1(zipc_universal_links_t *links)
{
    zipc_message_t message;
    if (zipc_platform_transport_receive(links->r5_task2_to_linux_user1,
                                        &message) != ZIPC_OK)
        return -1;
    return zipc_platform_transport_send(links->linux_user1_to_user2,
                                        &message) == ZIPC_OK ? 0 : -1;
}

int zipc_linux_user_process2(zipc_universal_links_t *links)
{
    zipc_message_t message;
    if (zipc_platform_transport_receive(links->linux_user1_to_user2,
                                        &message) != ZIPC_OK)
        return -1;
    /* Forward to the kernel driver via a mapped ring + eventfd/ioctl kick. */
    return zipc_platform_transport_send(links->linux_user2_to_kernel_wq1,
                                        &message) == ZIPC_OK ? 0 : -1;
}
