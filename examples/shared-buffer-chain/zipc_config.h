#ifndef ZIPC_SHARED_CHAIN_CONFIG_H
#define ZIPC_SHARED_CHAIN_CONFIG_H

#include <zipc/zipc.h>

/*
 * Compiled-in topology source.  Real processes normally keep only their local
 * endpoint entries, e.g. CompB would expose "ab" and "bc".  This single-process
 * demo carries all four endpoint views so it can simulate A, B and C together.
 */
static const zipc_topology_link_config_t zipc_chain_links[] = {
    {.name="ab-a", .link_id=0x1001, .pool_name="main", .transport_name="ab-ring", .local_component=1, .remote_component=2},
    {.name="ab-b", .link_id=0x1002, .pool_name="main", .transport_name="ab-ring", .local_component=2, .remote_component=1},
    {.name="bc-b", .link_id=0x1003, .pool_name="main", .transport_name="bc-ring", .local_component=2, .remote_component=3},
    {.name="bc-c", .link_id=0x1004, .pool_name="main", .transport_name="bc-ring", .local_component=3, .remote_component=2},
};

static const zipc_topology_config_t zipc_chain_topology = {
    .links = zipc_chain_links,
    .link_count = sizeof(zipc_chain_links) / sizeof(zipc_chain_links[0]),
};

#endif
