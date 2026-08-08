#ifndef ZIPC_UNIVERSAL_CHAIN_H
#define ZIPC_UNIVERSAL_CHAIN_H

#include <zipc/zipc.h>

enum zipc_universal_component {
    ZIPC_COMP_R5_0_TASK1 = 0,
    ZIPC_COMP_R5_0_TASK2,
    ZIPC_COMP_LINUX_USER1,
    ZIPC_COMP_LINUX_USER2,
    ZIPC_COMP_LINUX_WQ1,
    ZIPC_COMP_LINUX_WQ2,
    ZIPC_COMP_XEN_USER1,
    ZIPC_COMP_XEN_USER2,
    ZIPC_COMP_R5_1_BAREMETAL,
};

/* One logical link per hop; each link may choose a different backend. */
typedef struct {
    zipc_platform_transport_t *r5_task1_to_task2;
    zipc_platform_transport_t *r5_task2_to_linux_user1;
    zipc_platform_transport_t *linux_user1_to_user2;
    zipc_platform_transport_t *linux_user2_to_kernel_wq1;
    void *kernel_wq1_to_wq2;
    zipc_platform_transport_t *kernel_wq2_to_xen_user1;
    zipc_platform_transport_t *xen_user1_to_user2;
    zipc_platform_transport_t *xen_user2_to_r5_1;
} zipc_universal_links_t;

#endif
