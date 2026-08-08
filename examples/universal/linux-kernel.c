/* Reference integration fragment; build as part of a Linux kernel module. */
#include <zipc/zipc-kernel.h>
#ifdef __KERNEL__
#include <linux/workqueue.h>

struct zipc_wq_stage {
    struct work_struct work;
    zipc_kernel_transport_t *input;
    zipc_kernel_transport_t *output;
};

static void zipc_workqueue_stage(struct work_struct *work)
{
    struct zipc_wq_stage *stage = container_of(work, struct zipc_wq_stage, work);
    zipc_message_t message;
    if (zipc_kernel_transport_receive(stage->input, &message, 1) == ZIPC_OK)
        (void)zipc_kernel_transport_send(stage->output, &message);
}

/* Instantiate two stages: WQ1 output is WQ2 input. WQ2 forwards to the
 * Xen-facing shared ring/event-channel driver endpoint. */
#endif
