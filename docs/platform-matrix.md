# zIPC v0.1 platform matrix

| Platform | Shared memory | Transports |
|---|---|---|
| Linux userspace | POSIX SHM, hugetlbfs, DT reserved cached/uncached | mqueue, Unix datagram, FIFO, ring/eventfd, RPMsg device, IPI callback, PL ring/IRQ, SMC, FF-A |
| Linux kernel | reserved/DMA/device mappings supplied by driver | kfifo, ring/waitqueue, RPMsg, IPI, PL IRQ, SMC, FF-A hooks |
| FreeRTOS | DT reserved cached/uncached | queue, task notification, RPMsg, IPI, PL ring/IRQ, SMC, FF-A |
| Bare metal | DT reserved cached/uncached | RPMsg, IPI, PL ring/IRQ, SMC, FF-A |
| Xen Linux guest | Xen static shared memory | shared ring + Xen event channel |

The same slot-handle ABI is used at every boundary. Native object handles,
interrupt setup, cache maintenance, and endpoint lifecycle remain platform
responsibilities.


## Backend terminology

Use the canonical three-role model: payload backend, descriptor backend, and event backend. See `docs/BACKENDS.md`.
