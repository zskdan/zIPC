/* Reference integration fragment; build inside the R5_1 bare-metal BSP. */
#include "universal-chain.h"

int zipc_r5_1_consume(zipc_universal_links_t *links)
{
    zipc_message_t message;
    if (zipc_platform_transport_receive(links->xen_user2_to_r5_1,
                                        &message) != ZIPC_OK)
        return -1;
    /* Claim, consume, and release the final slot here. */
    return 0;
}
