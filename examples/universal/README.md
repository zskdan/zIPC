# Universal chain reference example

## Backend composition

```text
payload backend:    per-hop shared payload backend
descriptor backend: per-hop descriptor backend
event backend:      per-hop event backend
```


This is an integration blueprint, not a single-host executable. It shows one
zIPC handle traversing the requested heterogeneous chain:

```
R5_0 FreeRTOS task1
  -> R5_0 FreeRTOS task2              TASK_NOTIFICATION
  -> Linux userspace process1         RPMSG or IPI
  -> Linux userspace process2         SHM_RING_EVENTFD
  -> Linux kernel workqueue1           mapped driver ring + eventfd/ioctl kick
  -> Linux kernel workqueue2           kernel ring + workqueue scheduling
  -> Xen guest userspace process1      XEN_RING_EVTCHN
  -> Xen guest userspace process2      XEN_RING_EVTCHN or guest-local ring
  -> R5_1 bare metal                   IPI or RPMSG
```

The slot pool uses DT-reserved DDR when the handle must remain valid across
A53, R5, PL, kernel, and guest boundaries. Control metadata must live in a
memory region supporting CPU atomics. Payload memory may independently be
reserved DDR or uncached PL BRAM.

Each source file is compiled in its native environment. Board/hypervisor setup
must provide physical mappings, cache maintenance, IPI channels, RPMsg
endpoints, Xen static shared memory, and event-channel bindings.
