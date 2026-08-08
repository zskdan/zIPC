#ifndef ZIPC_PLATFORM_LINUX_COMMON_H
#define ZIPC_PLATFORM_LINUX_COMMON_H

#include <zipc/zipc.h>

/* Shared by Linux userspace, Linux kernel adapters, and Xen guests. */
zipc_status_t zipc_linux_common_ring_push(
    zipc_transport_spsc_ring_t *ring,
    const zipc_message_t *message);

zipc_status_t zipc_linux_common_ring_pop(
    zipc_transport_spsc_ring_t *ring,
    zipc_message_t *message);

#endif
