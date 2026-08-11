#include <zipc/zipc.h>
#include "zipc_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK_OK(x) do { zipc_status_t s_ = (x); if (s_ != ZIPC_OK) { \
    fprintf(stderr, "%s failed: %d\n", #x, (int)s_); return 1; } } while (0)

static int put_u32(zipc_buffer_t *buffer, size_t offset, uint32_t value)
{
    uint32_t *p = zipc_buffer_at(buffer, offset, sizeof(*p));
    if (p == NULL) return -1;
    memcpy(p, &value, sizeof(value));
    return 0;
}

static int get_u32(zipc_buffer_t *buffer, size_t offset, uint32_t *value)
{
    void *p = zipc_buffer_at(buffer, offset, sizeof(*value));
    if (p == NULL || value == NULL) return -1;
    memcpy(value, p, sizeof(*value));
    return 0;
}

int main(int argc, char **argv)
{
    enum { COMP_A = 1, COMP_B = 2, COMP_C = 3 };
    const uint32_t slots = 8U, capacity = 4096U, depth = 8U;
    const size_t ctrl_size = zipc_pool_required_control_size(slots);
    const size_t data_size = zipc_pool_required_payload_size(slots, capacity, 0U, 64U);
    zipc_platform_memory_t *ctrl = NULL, *data = NULL;
    zipc_platform_memory_config_t cc = {.type=ZIPC_SHM_POSIX,.size=ctrl_size,
        .backend.posix={.name="/zipc-chain-ctrl",.create=true,.unlink_on_close=true}};
    zipc_platform_memory_config_t dc = {.type=ZIPC_SHM_POSIX,.size=data_size,
        .backend.posix={.name="/zipc-chain-data",.create=true,.unlink_on_close=true}};
    CHECK_OK(zipc_platform_memory_open(&ctrl, &cc));
    CHECK_OK(zipc_platform_memory_open(&data, &dc));
    zipc_pool_config_t pc = {.control_memory=ctrl,.payload_memory=data,.slot_count=slots,
        .slot_capacity=capacity,.payload_alignment=64U,.pool_id=1U};
    CHECK_OK(zipc_pool_format(&pc));
    zipc_pool_t pool; CHECK_OK(zipc_pool_attach(&pool, &pc));

    size_t ring_bytes = zipc_transport_spsc_ring_size(depth);
    zipc_transport_spsc_ring_t *ab = calloc(1U, ring_bytes);
    zipc_transport_spsc_ring_t *bc = calloc(1U, ring_bytes);
    if (ab == NULL || bc == NULL) return 1;
    CHECK_OK(zipc_transport_spsc_ring_initialize(ab, depth));
    CHECK_OK(zipc_transport_spsc_ring_initialize(bc, depth));

    zipc_platform_transport_config_t ab_t = {.type=ZIPC_TRANSPORT_SHM_RING_POLLING,
        .platform_handle=ab,.ring_depth=depth,.poll_timeout_ns=1000000U};
    zipc_platform_transport_config_t bc_t = {.type=ZIPC_TRANSPORT_SHM_RING_POLLING,
        .platform_handle=bc,.ring_depth=depth,.poll_timeout_ns=1000000U};
    /* Runtime resources are bound once; topology may then come from C or a file. */
    zipc_topology_reset();
    CHECK_OK(zipc_topology_bind_pool("main", &pool));
    CHECK_OK(zipc_topology_bind_transport("ab-ring", &ab_t));
    CHECK_OK(zipc_topology_bind_transport("bc-ring", &bc_t));
    if (argc == 3 && strcmp(argv[1], "--config") == 0)
        CHECK_OK(zipc_topology_load_file(argv[2], ZIPC_TOPOLOGY_REJECT_DUPLICATES));
    else
        CHECK_OK(zipc_topology_register_config(&zipc_chain_topology,
                                               ZIPC_TOPOLOGY_REJECT_DUPLICATES));

    zipc_link_t *ab_a=NULL,*ab_b=NULL,*bc_b=NULL,*bc_c=NULL;
    CHECK_OK(zipc_link_open(&ab_a,"ab-a")); CHECK_OK(zipc_link_open(&ab_b,"ab-b"));
    CHECK_OK(zipc_link_open(&bc_b,"bc-b")); CHECK_OK(zipc_link_open(&bc_c,"bc-c"));

    printf("shared-buffer chain: one buffer flows A -> B -> C, never copied\n");
    printf("offsets: A@0x00 B@0x40 C@0x80 via zipc_buffer_at(); handle must be identical at every stage\n");

    zipc_buffer_t buffer;
    CHECK_OK(zipc_buffer_alloc(ab_a, 256U, &buffer));
    memset(zipc_buffer_data(&buffer), 0, zipc_buffer_size(&buffer));
    if (put_u32(&buffer, 0x00U, UINT32_C(0xaaaaaaaa)) != 0) return 1;
    zipc_handle_t original = zipc_buffer_handle(&buffer);
    uint32_t a_value;
    if (get_u32(&buffer, 0x00U, &a_value) != 0) return 1;
    printf("A: send handle=0x%016llx A=%08x\n",
           (unsigned long long)original, a_value);
    CHECK_OK(zipc_send(ab_a, &buffer));

    CHECK_OK(zipc_recv(ab_b, &buffer));
    if (zipc_buffer_handle(&buffer) != original) return 1;
    if (put_u32(&buffer, 0x40U, UINT32_C(0xbbbbbbbb)) != 0) return 1;
    uint32_t b_value;
    if (get_u32(&buffer, 0x40U, &b_value) != 0) return 1;
    printf("B: forward handle=0x%016llx A=%08x B=%08x\n",
           (unsigned long long)original, a_value, b_value);
    CHECK_OK(zipc_send(bc_b, &buffer));

    CHECK_OK(zipc_recv(bc_c, &buffer));
    if (zipc_buffer_handle(&buffer) != original) return 1;
    if (put_u32(&buffer, 0x80U, UINT32_C(0xcccccccc)) != 0) return 1;

    uint32_t c_value;
    if (get_u32(&buffer, 0x00U, &a_value) != 0 ||
        get_u32(&buffer, 0x40U, &b_value) != 0 ||
        get_u32(&buffer, 0x80U, &c_value) != 0) return 1;
    printf("C: recv handle=0x%016llx A=%08x B=%08x C=%08x\n",
           (unsigned long long)original, a_value, b_value, c_value);
    printf("PASS: same handle 0x%016llx across all stages; fixed offsets 0x00/0x40/0x80 "
           "read consistently\n",
           (unsigned long long)original);
    CHECK_OK(zipc_buffer_release(&buffer));

    zipc_link_destroy(bc_c); zipc_link_destroy(bc_b);
    zipc_topology_reset();
    zipc_link_destroy(ab_b); zipc_link_destroy(ab_a);
    free(bc); free(ab); zipc_platform_memory_close(data); zipc_platform_memory_close(ctrl);
    return 0;
}
