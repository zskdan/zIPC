#include <zipc/zipc.h>
#include "zipc_config.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK_OK(x) do { zipc_status_t s_ = (x); if (s_ != ZIPC_OK) { \
    fprintf(stderr, "%s failed: %d\n", #x, (int)s_); return 1; } } while (0)
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "validation failed: %s\n", #x); return 1; } } while (0)

int main(int argc, char **argv)
{
    const uint32_t slots = 8U, capacity = 320U, depth = 8U;
    const size_t control_size = zipc_pool_required_control_size(slots);
    const size_t payload_size = zipc_pool_required_payload_size(
        slots, capacity, 0U, 64U);
    zipc_platform_memory_t *control = NULL, *payload = NULL;
    zipc_platform_memory_config_t control_config = {
        .type = ZIPC_SHM_POSIX, .size = control_size,
        .backend.posix = {.name = "/zipc-chain-ctrl", .create = true,
                          .unlink_on_close = true},
    };
    zipc_platform_memory_config_t payload_config = {
        .type = ZIPC_SHM_POSIX, .size = payload_size,
        .backend.posix = {.name = "/zipc-chain-data", .create = true,
                          .unlink_on_close = true},
    };
    CHECK_OK(zipc_platform_memory_open(&control, &control_config));
    CHECK_OK(zipc_platform_memory_open(&payload, &payload_config));
    zipc_pool_config_t pool_config = {
        .control_memory = control, .payload_memory = payload,
        .slot_count = slots, .slot_capacity = capacity,
        .payload_alignment = 64U, .pool_id = 1U,
    };
    CHECK_OK(zipc_pool_format(&pool_config));
    zipc_pool_t pool;
    CHECK_OK(zipc_pool_attach(&pool, &pool_config));

    const size_t ring_bytes = zipc_transport_spsc_ring_size(depth);
    zipc_transport_spsc_ring_t *ab = calloc(1U, ring_bytes);
    zipc_transport_spsc_ring_t *bc = calloc(1U, ring_bytes);
    zipc_transport_spsc_ring_t *cd = calloc(1U, ring_bytes);
    if (ab == NULL || bc == NULL || cd == NULL)
        return 1;
    CHECK_OK(zipc_transport_spsc_ring_initialize(ab, depth));
    CHECK_OK(zipc_transport_spsc_ring_initialize(bc, depth));
    CHECK_OK(zipc_transport_spsc_ring_initialize(cd, depth));
    zipc_platform_transport_config_t ab_transport = {
        .type = ZIPC_TRANSPORT_SHM_RING_POLLING, .platform_handle = ab,
        .ring_depth = depth, .poll_timeout_ns = 1000000U,
    };
    zipc_platform_transport_config_t bc_transport = {
        .type = ZIPC_TRANSPORT_SHM_RING_POLLING, .platform_handle = bc,
        .ring_depth = depth, .poll_timeout_ns = 1000000U,
    };
    zipc_platform_transport_config_t cd_transport = {
        .type = ZIPC_TRANSPORT_SHM_RING_POLLING, .platform_handle = cd,
        .ring_depth = depth, .poll_timeout_ns = 1000000U,
    };

    zipc_topology_reset();
    CHECK_OK(zipc_topology_bind_pool("main", &pool));
    CHECK_OK(zipc_topology_bind_transport("ab-ring", &ab_transport));
    CHECK_OK(zipc_topology_bind_transport("bc-ring", &bc_transport));
    CHECK_OK(zipc_topology_bind_transport("cd-ring", &cd_transport));
    if (argc == 3 && strcmp(argv[1], "--config") == 0)
        CHECK_OK(zipc_topology_load_file(
            argv[2], ZIPC_TOPOLOGY_REJECT_DUPLICATES));
    else
        CHECK_OK(zipc_topology_register_config(
            &zipc_chain_topology, ZIPC_TOPOLOGY_REJECT_DUPLICATES));

    zipc_link_t *ab_a = NULL, *ab_b = NULL, *bc_b = NULL, *bc_c = NULL;
    zipc_link_t *cd_c = NULL, *cd_d = NULL;
    CHECK_OK(zipc_link_open(&ab_a, "ab-a"));
    CHECK_OK(zipc_link_open(&ab_b, "ab-b"));
    CHECK_OK(zipc_link_open(&bc_b, "bc-b"));
    CHECK_OK(zipc_link_open(&bc_c, "bc-c"));
    CHECK_OK(zipc_link_open(&cd_c, "cd-c"));
    CHECK_OK(zipc_link_open(&cd_d, "cd-d"));

    zipc_buffer_t root;
    CHECK_OK(zipc_buffer_alloc(ab_a, 320U, &root, NULL));
    for (uint32_t i = 0U; i < 320U; ++i)
        ((uint8_t *)zipc_buffer_data(&root))[i] = (uint8_t)i;
    const zipc_buffer_id_t root_id = zipc_buffer_id(&root);
    const zipc_buffer_id_t root_parent = zipc_buffer_parent_id(&root);
    const size_t root_size = zipc_buffer_size(&root);
    printf("A: created root buffer id=0x%016" PRIx64
           " parent=0x%016" PRIx64 " size=%zu\n",
           root_id, root_parent, root_size);
    CHECK_OK(zipc_send(ab_a, &root));
    printf("A: sent root buffer id=0x%016" PRIx64
           " parent=0x%016" PRIx64 " size=%zu\n",
           root_id, root_parent, root_size);

    CHECK_OK(zipc_recv(ab_b, &root));
    const zipc_buffer_id_t relayed_id = zipc_buffer_id(&root);
    const zipc_buffer_id_t relayed_parent = zipc_buffer_parent_id(&root);
    const size_t relayed_size = zipc_buffer_size(&root);
    CHECK_OK(zipc_send(bc_b, &root));
    printf("B: relayed root buffer id=0x%016" PRIx64
           " parent=0x%016" PRIx64 " size=%zu\n",
           relayed_id, relayed_parent, relayed_size);

    CHECK_OK(zipc_recv(bc_c, &root));
    printf("C: received root buffer id=0x%016" PRIx64
           " parent=0x%016" PRIx64 " size=%zu\n",
           zipc_buffer_id(&root), zipc_buffer_parent_id(&root),
           zipc_buffer_size(&root));

    zipc_buffer_t children[5];
    zipc_buffer_id_t child_ids[5];
    zipc_buffer_id_t child_parents[5];
    size_t child_sizes[5];
    uint8_t child_payloads[5][64];
    for (uint32_t i = 0U; i < 5U; ++i) {
        CHECK_OK(zipc_buffer_alloc(cd_c, 64U, &children[i], &root));
        memcpy(child_payloads[i],
               (const uint8_t *)zipc_buffer_data(&root) + i * 64U, 64U);
        memcpy(zipc_buffer_data(&children[i]), child_payloads[i], 64U);
        child_ids[i] = zipc_buffer_id(&children[i]);
        child_parents[i] = zipc_buffer_parent_id(&children[i]);
        child_sizes[i] = zipc_buffer_size(&children[i]);
        printf("C: created child[%u] buffer id=0x%016" PRIx64
               " parent=0x%016" PRIx64 " size=%zu\n",
               i, child_ids[i], child_parents[i], child_sizes[i]);
    }
    CHECK_OK(zipc_buffer_release(&root));
    printf("C: released root buffer id=0x%016" PRIx64
           " parent=0x%016" PRIx64 " size=%zu\n",
           root_id, root_parent, root_size);

    for (uint32_t i = 0U; i < 5U; ++i) {
        CHECK_OK(zipc_send(cd_c, &children[i]));
        printf("C: sent child[%u] buffer id=0x%016" PRIx64
               " parent=0x%016" PRIx64 " size=%zu\n",
               i, child_ids[i], child_parents[i], child_sizes[i]);
    }
    for (uint32_t i = 0U; i < 5U; ++i) {
        CHECK_OK(zipc_recv(cd_d, &children[i]));
        const zipc_buffer_id_t received_id = zipc_buffer_id(&children[i]);
        const zipc_buffer_id_t received_parent =
            zipc_buffer_parent_id(&children[i]);
        const size_t received_size = zipc_buffer_size(&children[i]);
        CHECK(received_id == child_ids[i]);
        CHECK(received_parent == root_id &&
              received_parent == child_parents[i]);
        CHECK(received_size == 64U && received_size == child_sizes[i]);
        CHECK(memcmp(zipc_buffer_const_data(&children[i]),
                     child_payloads[i], 64U) == 0);
        printf("D: received child[%u] buffer id=0x%016" PRIx64
               " parent=0x%016" PRIx64 " size=%zu\n",
               i, received_id, received_parent, received_size);
        CHECK_OK(zipc_buffer_release(&children[i]));
        printf("D: released child[%u] buffer id=0x%016" PRIx64
               " parent=0x%016" PRIx64 " size=%zu\n",
               i, received_id, received_parent, received_size);
    }
    puts("PASS: root lineage and all five child payloads validated");

    zipc_link_destroy(cd_d); zipc_link_destroy(cd_c);
    zipc_link_destroy(bc_c); zipc_link_destroy(bc_b);
    zipc_link_destroy(ab_b); zipc_link_destroy(ab_a);
    zipc_topology_reset();
    free(cd); free(bc); free(ab);
    zipc_platform_memory_close(payload);
    zipc_platform_memory_close(control);
    return 0;
}
