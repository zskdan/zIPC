#define _POSIX_C_SOURCE 200809L
#include <zipc/zipc.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define CAP (64U * 1024U)
#define CHECK_OK(x) do { zipc_status_t s_=(x); if (s_ != ZIPC_OK) { fprintf(stderr,"%s failed: %d\n",#x,(int)s_); return 1; } } while(0)

static int expect_fault(uint8_t *p)
{
    pid_t pid=fork(); if(pid<0) return -1;
    if(pid==0){ *p ^= 1U; _exit(0); }
    int st=0; if(waitpid(pid,&st,0)!=pid) return -1;
    return WIFSIGNALED(st) && (WTERMSIG(st)==SIGSEGV || WTERMSIG(st)==SIGBUS) ? 0 : -1;
}

int main(void)
{
    const uint32_t slots=4U, depth=8U;
    size_t cs=zipc_pool_required_control_size(slots);
    size_t ds=zipc_pool_required_payload_size(slots,CAP,0U,4096U);
    zipc_platform_memory_t *cm=NULL,*dm=NULL;
    zipc_platform_memory_config_t cc={.type=ZIPC_SHM_POSIX,.size=cs,.backend.posix={.name="/zipc-strict-c",.create=true,.unlink_on_close=true}};
    zipc_platform_memory_config_t dc={.type=ZIPC_SHM_POSIX,.size=ds,.backend.posix={.name="/zipc-strict-d",.create=true,.unlink_on_close=true}};
    CHECK_OK(zipc_platform_memory_open(&cm,&cc)); CHECK_OK(zipc_platform_memory_open(&dm,&dc));
    zipc_pool_config_t pc={.control_memory=cm,.payload_memory=dm,.slot_count=slots,.slot_capacity=CAP,.payload_alignment=4096U,.flags=ZIPC_POOL_F_STRICT_OWNERSHIP,.pool_id=9U};
    CHECK_OK(zipc_pool_format(&pc)); zipc_pool_t pool; CHECK_OK(zipc_pool_attach(&pool,&pc));
    size_t rb=zipc_transport_spsc_ring_size(depth); zipc_transport_spsc_ring_t *ring=calloc(1U,rb); if(!ring) return 1;
    CHECK_OK(zipc_transport_spsc_ring_initialize(ring,depth));
    zipc_platform_transport_config_t tc={.type=ZIPC_TRANSPORT_SHM_RING_POLLING,.platform_handle=ring,.ring_depth=depth,.poll_timeout_ns=1000000U};
    zipc_link_config_t txc={.pool=&pool,.local_component=1U,.remote_component=2U,.transport=tc};
    zipc_link_config_t rxc={.pool=&pool,.local_component=2U,.remote_component=1U,.transport=tc};
    zipc_link_t *tx=NULL,*rx=NULL; CHECK_OK(zipc_link_create(&tx,&txc)); CHECK_OK(zipc_link_create(&rx,&rxc));
    zipc_buffer_t b; CHECK_OK(zipc_buffer_alloc(tx,16U,&b));
    uint8_t *retained=zipc_buffer_data(&b); retained[0]=0x5a;
    CHECK_OK(zipc_send(tx,&b));
    if(expect_fault(retained)!=0){ fprintf(stderr,"retained pointer remained writable after send\n"); return 1; }
    CHECK_OK(zipc_recv(rx,&b));
    if(((uint8_t *)zipc_buffer_data(&b))[0] != 0x5a){ fprintf(stderr,"receiver could not read payload\n"); return 1; }
    uint8_t *released=zipc_buffer_data(&b); CHECK_OK(zipc_buffer_release(&b));
    if(expect_fault(released)!=0){ fprintf(stderr,"released pointer remained writable\n"); return 1; }
    zipc_link_destroy(rx); zipc_link_destroy(tx); free(ring); zipc_platform_memory_close(dm); zipc_platform_memory_close(cm);
    puts("PASS: strict ownership revokes retained payload pointers"); return 0;
}
