#include <zipc/zipc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK_OK(x) do { zipc_status_t s_=(x); if (s_ != ZIPC_OK) { \
    fprintf(stderr, "%s failed: %d\n", #x, (int)s_); return 1; } } while (0)
#define CHECK_TRUE(x) do { if (!(x)) { fprintf(stderr, "check failed: %s\n", #x); return 1; } } while (0)

int main(void)
{
    const uint32_t slots = 8U, capacity = 4096U, depth = 8U;
    const size_t ctrl_size = zipc_pool_required_control_size(slots);
    const size_t data_size = zipc_pool_required_payload_size(slots, capacity, 0U, 64U);
    zipc_platform_memory_t *ctrl = NULL, *data = NULL;
    zipc_platform_memory_config_t cc = {.type=ZIPC_SHM_POSIX,.size=ctrl_size,
        .backend.posix={.name="/zipc-api-ctrl",.create=true,.unlink_on_close=true}};
    zipc_platform_memory_config_t dc = {.type=ZIPC_SHM_POSIX,.size=data_size,
        .backend.posix={.name="/zipc-api-data",.create=true,.unlink_on_close=true}};
    CHECK_OK(zipc_platform_memory_open(&ctrl, &cc));
    CHECK_OK(zipc_platform_memory_open(&data, &dc));
    zipc_pool_config_t pc = {.control_memory=ctrl,.payload_memory=data,.slot_count=slots,
        .slot_capacity=capacity,.payload_alignment=64U,.pool_id=7U};
    CHECK_OK(zipc_pool_format(&pc));
    zipc_pool_t pool; CHECK_OK(zipc_pool_attach(&pool, &pc));

    size_t ring_bytes = zipc_transport_spsc_ring_size(depth);
    zipc_transport_spsc_ring_t *ring = calloc(1U, ring_bytes);
    CHECK_TRUE(ring != NULL);
    CHECK_OK(zipc_transport_spsc_ring_initialize(ring, depth));
    zipc_platform_transport_config_t tc = {.type=ZIPC_TRANSPORT_SHM_RING_POLLING,
        .platform_handle=ring,.ring_depth=depth,.poll_timeout_ns=1000000U};
    zipc_topology_reset();
    CHECK_OK(zipc_topology_bind_pool("main", &pool));
    CHECK_OK(zipc_topology_bind_transport("ab-ring", &tc));
    static const zipc_topology_link_config_t links[2] = {
        {.name="ab-tx",.link_id=0x1001,.pool_name="main",.transport_name="ab-ring",.local_component=1U,.remote_component=2U},
        {.name="ab-rx",.link_id=0x1002,.pool_name="main",.transport_name="ab-ring",.local_component=2U,.remote_component=1U},
    };
    const zipc_topology_config_t topology = {.links=links,.link_count=2U};
    CHECK_OK(zipc_topology_register_config(&topology, ZIPC_TOPOLOGY_REJECT_DUPLICATES));
    zipc_link_t *tx=NULL,*rx=NULL; CHECK_OK(zipc_link_open(&tx,"ab-tx")); CHECK_OK(zipc_link_open(&rx,"ab-rx"));

    zipc_buffer_t b;
    CHECK_OK(zipc_buffer_alloc(tx, 32U, &b));
    CHECK_TRUE(zipc_buffer_is_valid(&b));
    CHECK_TRUE(zipc_buffer_size(&b) == 32U);
    memset(zipc_buffer_data(&b), 0, 32U);
    memcpy(zipc_buffer_data(&b), "abc", 4U);
    CHECK_TRUE(zipc_buffer_at(&b, 0U, 4U) != NULL);
    CHECK_TRUE(zipc_buffer_at(&b, capacity - 4U, 4U) != NULL);
    CHECK_TRUE(zipc_buffer_at(&b, capacity - 3U, 4U) == NULL);
    CHECK_TRUE(zipc_buffer_at(&b, capacity + 1U, 0U) == NULL);
    CHECK_OK(zipc_send(tx, &b));
    CHECK_TRUE(!zipc_buffer_is_valid(&b));
    CHECK_TRUE(zipc_buffer_data(&b) == NULL);
    CHECK_TRUE(zipc_buffer_at(&b, 0U, 1U) == NULL);
    CHECK_TRUE(zipc_send(tx, &b) == ZIPC_ERR_INVALID_BUFFER);
    CHECK_TRUE(zipc_buffer_release(&b) == ZIPC_ERR_INVALID_BUFFER);

    CHECK_OK(zipc_recv(rx, &b));
    CHECK_TRUE(strcmp((char *)zipc_buffer_data(&b), "abc") == 0);
    CHECK_TRUE(zipc_buffer_pool_id(&b) == 7U);
    CHECK_OK(zipc_buffer_trim_front(&b, 1U));
    CHECK_TRUE(zipc_buffer_size(&b) == 31U);
    CHECK_TRUE(zipc_buffer_headroom(&b) == 1U);
    CHECK_OK(zipc_buffer_trim_back(&b, 1U));
    CHECK_TRUE(zipc_buffer_size(&b) == 30U);
    CHECK_TRUE(zipc_buffer_tailroom(&b) == capacity - 31U);
    /* Absolute buffer offsets do not move when the logical data window moves. */
    CHECK_TRUE(zipc_buffer_at(&b, 0U, 1U) != NULL);
    CHECK_OK(zipc_buffer_release(&b));
    CHECK_TRUE(!zipc_buffer_is_valid(&b));
    CHECK_TRUE(zipc_buffer_release(&b) == ZIPC_ERR_INVALID_BUFFER);

    CHECK_OK(zipc_send_copy(tx, "copy", 5U));
    char out[16]; size_t actual=0U;
    CHECK_OK(zipc_recv_copy(rx, out, sizeof(out), &actual));
    CHECK_TRUE(actual == 5U && strcmp(out,"copy") == 0);

    zipc_link_destroy(rx); zipc_link_destroy(tx); zipc_topology_reset(); free(ring);
    zipc_platform_memory_close(data); zipc_platform_memory_close(ctrl);
    puts("PASS: simplified API, absolute buffer offsets, trim semantics, ownership invalidation and copy helpers");
    return 0;
}
