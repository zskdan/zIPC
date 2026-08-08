#ifndef ZIPC_KERNEL_H
#define ZIPC_KERNEL_H

#ifdef __KERNEL__
#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/kfifo.h>

#define ZIPC_KERNEL_ABI_VERSION 1U

typedef u64 zipc_handle_t;
typedef u16 zipc_component_id_t;

typedef struct {
    zipc_handle_t handle;
    zipc_component_id_t source_component;
    zipc_component_id_t destination_component;
    u32 transfer_sequence;
    u32 flags;
} zipc_message_t;

typedef enum {
    ZIPC_OK = 0,
    ZIPC_ERR_INVALID_ARGUMENT,
    ZIPC_ERR_INVALID_POOL,
    ZIPC_ERR_NO_BUFFER,
    ZIPC_ERR_INVALID_HANDLE,
    ZIPC_ERR_STALE_HANDLE,
    ZIPC_ERR_INVALID_STATE,
    ZIPC_ERR_NOT_OWNER,
    ZIPC_ERR_INVALID_RECEIVER,
    ZIPC_ERR_SEQUENCE_MISMATCH,
    ZIPC_ERR_REGION_OVERFLOW,
    ZIPC_ERR_UNSUPPORTED_MEMORY,
    ZIPC_ERR_TRANSPORT,
    ZIPC_ERR_PLATFORM
} zipc_status_t;

typedef enum {
    ZIPC_KERNEL_TRANSPORT_KFIFO = 0,
    ZIPC_KERNEL_TRANSPORT_RING_WAITQUEUE,
    ZIPC_KERNEL_TRANSPORT_RPMSG,
    ZIPC_KERNEL_TRANSPORT_IPI,
    ZIPC_KERNEL_TRANSPORT_PL_RING_IRQ,
    ZIPC_KERNEL_TRANSPORT_SMC,
    ZIPC_KERNEL_TRANSPORT_FFA
} zipc_kernel_transport_type_t;

typedef struct {
    atomic_t producer;
    atomic_t consumer;
    u32 depth;
    u32 reserved;
    zipc_message_t entries[];
} zipc_kernel_spsc_ring_t;

typedef struct zipc_kernel_transport zipc_kernel_transport_t;

typedef struct {
    zipc_kernel_transport_type_t type;
    zipc_kernel_spsc_ring_t *ring;
    u32 ring_depth;
    wait_queue_head_t *waitq;
    struct kfifo *fifo;
    void *platform_handle;
    int (*notify)(void *context);
    int (*wait)(void *context, unsigned long timeout_jiffies);
    void *context;
} zipc_kernel_transport_config_t;

zipc_status_t zipc_kernel_transport_open(
    zipc_kernel_transport_t **transport,
    const zipc_kernel_transport_config_t *config);
zipc_status_t zipc_kernel_transport_send(
    zipc_kernel_transport_t *transport,
    const zipc_message_t *message);
zipc_status_t zipc_kernel_transport_receive(
    zipc_kernel_transport_t *transport,
    zipc_message_t *message,
    unsigned long timeout_jiffies);
void zipc_kernel_transport_close(zipc_kernel_transport_t *transport);

#endif /* __KERNEL__ */
#endif /* ZIPC_KERNEL_H */
