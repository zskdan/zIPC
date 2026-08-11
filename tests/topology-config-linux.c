#include <zipc/zipc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK_OK(x) do { zipc_status_t s_=(x); if (s_ != ZIPC_OK) { \
    fprintf(stderr, "%s failed: %d line=%d\n", #x, (int)s_, __LINE__); return 1; } } while (0)
#define CHECK_TRUE(x) do { if (!(x)) { fprintf(stderr, "check failed: %s line=%d\n", #x, __LINE__); return 1; } } while (0)

static int exercise_pair(const char *tx_name, const char *rx_name)
{
    zipc_link_t *tx = NULL, *rx = NULL;
    CHECK_OK(zipc_link_open(&tx, tx_name));
    CHECK_OK(zipc_link_open(&rx, rx_name));
    CHECK_OK(zipc_send_copy(tx, "topology", 9U));
    char out[32] = {0}; size_t actual = 0U;
    CHECK_OK(zipc_recv_copy(rx, out, sizeof(out), &actual));
    CHECK_TRUE(actual == 9U && strcmp(out, "topology") == 0);
    zipc_link_destroy(rx); zipc_link_destroy(tx);
    return 0;
}

int main(void)
{
    const uint32_t slots=8U, capacity=4096U, depth=8U;
    size_t cs=zipc_pool_required_control_size(slots);
    size_t ps=zipc_pool_required_payload_size(slots,capacity,0U,64U);
    zipc_platform_memory_t *cm=NULL,*pm=NULL;
    zipc_platform_memory_config_t cc={.type=ZIPC_SHM_POSIX,.size=cs,
        .backend.posix={.name="/zipc-topo-ctrl",.create=true,.unlink_on_close=true}};
    zipc_platform_memory_config_t pcfg={.type=ZIPC_SHM_POSIX,.size=ps,
        .backend.posix={.name="/zipc-topo-data",.create=true,.unlink_on_close=true}};
    CHECK_OK(zipc_platform_memory_open(&cm,&cc)); CHECK_OK(zipc_platform_memory_open(&pm,&pcfg));
    zipc_pool_config_t pool_cfg={.control_memory=cm,.payload_memory=pm,.slot_count=slots,
        .slot_capacity=capacity,.payload_alignment=64U,.pool_id=11U};
    CHECK_OK(zipc_pool_format(&pool_cfg)); zipc_pool_t pool; CHECK_OK(zipc_pool_attach(&pool,&pool_cfg));
    size_t rb=zipc_transport_spsc_ring_size(depth); zipc_transport_spsc_ring_t *ring=calloc(1U,rb);
    CHECK_TRUE(ring != NULL); CHECK_OK(zipc_transport_spsc_ring_initialize(ring,depth));
    zipc_platform_transport_config_t transport={.type=ZIPC_TRANSPORT_SHM_RING_POLLING,
        .platform_handle=ring,.ring_depth=depth,.poll_timeout_ns=1000000U};

    /* Common runtime resource binding used by either topology source. */
    zipc_topology_reset();
    CHECK_OK(zipc_topology_bind_pool("main", &pool));
    CHECK_OK(zipc_topology_bind_transport("ab-ring", &transport));

    /* Source 1: compiled-in/header style configuration. */
    static const zipc_topology_link_config_t static_links[] = {
        {.name="ab-tx",.link_id=0x1001,.pool_name="main",.transport_name="ab-ring",.local_component=1,.remote_component=2},
        {.name="ab-rx",.link_id=0x1002,.pool_name="main",.transport_name="ab-ring",.local_component=2,.remote_component=1},
    };
    const zipc_topology_config_t static_cfg={.links=static_links,.link_count=2U};
    CHECK_OK(zipc_topology_register_config(&static_cfg, ZIPC_TOPOLOGY_REJECT_DUPLICATES));
    if (exercise_pair("ab-tx","ab-rx") != 0) return 1;

    /* Source 2: identical canonical model parsed from a string. */
    zipc_topology_reset(); CHECK_OK(zipc_topology_bind_pool("main",&pool));
    CHECK_OK(zipc_topology_bind_transport("ab-ring",&transport));
    const char text[] =
        "[link.ab-tx]\n"
        "id=0x2001\n"
        "pool=main\ntransport=ab-ring\nlocal=1\nremote=2\n"
        "[link.ab-rx]\n"
        "id=0x2002\n"
        "pool=main\ntransport=ab-ring\nlocal=2\nremote=1\n";
    CHECK_OK(zipc_topology_load_string(text, sizeof(text)-1U, ZIPC_TOPOLOGY_REJECT_DUPLICATES));
    if (exercise_pair("ab-tx","ab-rx") != 0) return 1;
    CHECK_TRUE(zipc_topology_load_string(text,sizeof(text)-1U,ZIPC_TOPOLOGY_REJECT_DUPLICATES) != ZIPC_OK);
    CHECK_OK(zipc_topology_load_string(text,sizeof(text)-1U,ZIPC_TOPOLOGY_OVERRIDE));

    /* Source 3: hosted/Linux file loader. */
    char filename[128]; snprintf(filename,sizeof(filename),"/tmp/zipc-topology-%ld.conf",(long)getpid());
    FILE *f=fopen(filename,"wb"); CHECK_TRUE(f != NULL);
    const char file_text[] =
        "# zIPC topology\n"
        "[link.ab-tx]\nid=0x3001\npool=main\ntransport=ab-ring\nlocal=1\nremote=2\n"
        "[link.ab-rx]\nid=0x3002\npool=main\ntransport=ab-ring\nlocal=2\nremote=1\n";
    CHECK_TRUE(fwrite(file_text,1U,sizeof(file_text)-1U,f) == sizeof(file_text)-1U); fclose(f);
    zipc_topology_reset(); CHECK_OK(zipc_topology_bind_pool("main",&pool));
    CHECK_OK(zipc_topology_bind_transport("ab-ring",&transport));
    CHECK_OK(zipc_topology_load_file(filename, ZIPC_TOPOLOGY_REJECT_DUPLICATES)); unlink(filename);
    if (exercise_pair("ab-tx","ab-rx") != 0) return 1;

    zipc_topology_reset(); free(ring); zipc_platform_memory_close(pm); zipc_platform_memory_close(cm);
    puts("PASS: static, string and file topology sources share one canonical registry");
    return 0;
}
