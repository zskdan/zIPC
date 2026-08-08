/* Reference integration fragment for two Xen Linux guest processes. */
#include "universal-chain.h"

int zipc_xen_guest_process1(zipc_universal_links_t *links)
{
    zipc_message_t message;
    if (zipc_platform_transport_receive(links->kernel_wq2_to_xen_user1,
                                        &message) != ZIPC_OK)
        return -1;
    return zipc_platform_transport_send(links->xen_user1_to_user2,
                                        &message) == ZIPC_OK ? 0 : -1;
}

int zipc_xen_guest_process2(zipc_universal_links_t *links)
{
    zipc_message_t message;
    if (zipc_platform_transport_receive(links->xen_user1_to_user2,
                                        &message) != ZIPC_OK)
        return -1;
    return zipc_platform_transport_send(links->xen_user2_to_r5_1,
                                        &message) == ZIPC_OK ? 0 : -1;
}
