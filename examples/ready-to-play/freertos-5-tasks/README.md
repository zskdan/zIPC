# Ready to play: five FreeRTOS tasks on R5_0

## Backend composition

```text
payload backend:    PREALLOCATED memory
descriptor backend: shared mailbox
event backend:      FreeRTOS task notification
```


The chain is:

```text
Task0 -> Task1 -> Task2 -> Task3 -> Task4
```

All tasks in one FreeRTOS image share the same address space. For task-to-task zIPC, the recommended memory backend is therefore:

```c
ZIPC_SHM_PREALLOCATED
```

`PREALLOCATED` is used instead of `EXTERNAL` or `GENERIC` because it describes the ownership precisely: the application, linker script, or BSP provides an already mapped region, and zIPC only wraps it. zIPC never allocates, maps, clears, or frees the supplied storage.

The region must remain valid for the lifetime of every pool and link that uses it. It should be aligned to at least the cache-line size, and its declared capability flags must match the actual MPU/cache configuration.

## Option 1: global static array

This is the simplest choice for tasks running in the same FreeRTOS image:

```c
#include <stdalign.h>
#include <stdint.h>
#include <zipc/zipc.h>

#define ZIPC_TASK_POOL_SIZE (256U * 1024U)

alignas(64)
static uint8_t zipc_task_pool_memory[ZIPC_TASK_POOL_SIZE];

static const zipc_platform_memory_config_t memory_cfg = {
    .type = ZIPC_SHM_PREALLOCATED,
    .size = sizeof(zipc_task_pool_memory),
    .backend.preallocated = {
        .address = zipc_task_pool_memory,
        .physical_address = ZIPC_PHYS_ADDR_INVALID,
        .capabilities = ZIPC_MEM_CAP_CPU_READ |
                        ZIPC_MEM_CAP_CPU_WRITE |
                        ZIPC_MEM_CAP_ATOMIC32 |
                        ZIPC_MEM_CAP_ATOMIC64 |
                        ZIPC_MEM_CAP_CACHEABLE,
    },
};
```

The array is normally placed in `.bss`. Call `zipc_pool_format()` before first use; an explicit `memset()` is not required if formatting initializes every field used by the pool.

## Option 2: dedicated linker-script section

Use a dedicated section when placement must be controlled precisely.

C declaration:

```c
#define ZIPC_TASK_POOL_SIZE (256U * 1024U)

__attribute__((section(".zipc_pool"), aligned(64)))
static uint8_t zipc_task_pool_memory[ZIPC_TASK_POOL_SIZE];
```

Linker script:

```ld
MEMORY
{
    R5_CODE  (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
    R5_DATA  (rwx) : ORIGIN = 0x00100000, LENGTH = 512K
    ZIPC_RAM (rw)  : ORIGIN = 0x00200000, LENGTH = 256K
}

SECTIONS
{
    .zipc_pool (NOLOAD) : ALIGN(64)
    {
        __zipc_pool_start = .;
        KEEP(*(.zipc_pool))
        KEEP(*(.zipc_pool.*))
        . = ALIGN(64);
        __zipc_pool_end = .;
    } > ZIPC_RAM
}
```

`NOLOAD` prevents the pool from increasing the initialized ELF image. The memory is still reserved in the final address map. `zipc_pool_format()` must initialize the region at startup.

The linker can also reserve the memory directly, without declaring a C array:

```ld
.zipc_pool (NOLOAD) : ALIGN(64)
{
    __zipc_pool_start = .;
    . += 256K;
    __zipc_pool_end = .;
} > R5_DATA
```

C code:

```c
extern uint8_t __zipc_pool_start[];
extern uint8_t __zipc_pool_end[];

static zipc_platform_memory_config_t memory_cfg = {
    .type = ZIPC_SHM_PREALLOCATED,
    .backend.preallocated = {
        .address = __zipc_pool_start,
        .physical_address = ZIPC_PHYS_ADDR_INVALID,
        .capabilities = ZIPC_MEM_CAP_CPU_READ |
                        ZIPC_MEM_CAP_CPU_WRITE |
                        ZIPC_MEM_CAP_ATOMIC32 |
                        ZIPC_MEM_CAP_ATOMIC64 |
                        ZIPC_MEM_CAP_CACHEABLE,
    },
};

void configure_zipc_size(void)
{
    memory_cfg.size = (size_t)(__zipc_pool_end - __zipc_pool_start);
}
```

## Option 3: OCRAM

OCRAM is useful when the pool must be placed in on-chip memory for deterministic access or shared between processors that can map the same physical OCRAM range.

Reserve the OCRAM range in the linker script and use the same `.zipc_pool` section pattern. Then declare the physical address and capabilities:

```c
static const zipc_platform_memory_config_t memory_cfg = {
    .type = ZIPC_SHM_PREALLOCATED,
    .size = sizeof(zipc_task_pool_memory),
    .backend.preallocated = {
        .address = zipc_task_pool_memory,
        .physical_address = (uint64_t)(uintptr_t)zipc_task_pool_memory,
        .capabilities = ZIPC_MEM_CAP_CPU_READ |
                        ZIPC_MEM_CAP_CPU_WRITE |
                        ZIPC_MEM_CAP_ATOMIC32 |
                        ZIPC_MEM_CAP_ATOMIC64 |
                        ZIPC_MEM_CAP_FIXED_PHYS |
                        ZIPC_MEM_CAP_REMOTE_ACCESS,
    },
};
```

For tasks on one R5 core, cache maintenance is normally unnecessary because they share one cache and address space. For R5_0/R5_1, A53/R5, or PS/PL sharing, both sides must map the same physical OCRAM range consistently. Cacheability, shareability, MPU attributes, and atomic support must be verified for the selected SoC configuration.

Do not mark ordinary shared metadata as Device memory merely because another processor can access it. The zIPC control region must support normal CPU reads/writes and 32-bit atomic operations.

## Existing reserved-DDR example

The supplied `main.c` still demonstrates a fixed physical reserved-DDR region using:

```c
ZIPC_SHM_DTREVMEM_UNCACHED
```

That remains appropriate for R5 communication with Linux or another processor when the physical region is reserved externally. For task-to-task communication within one FreeRTOS image, `ZIPC_SHM_PREALLOCATED` is simpler and does not depend on a device tree.

Integrate `main.c`, `src/core/zipc.c`, and `platform/freertos/platform-freertos.c` into the FreeRTOS application. Call:

```c
zipc_ready_freertos_start();
vTaskStartScheduler();
```

The depth-one task-notification transport assumes one outstanding descriptor per link. Replace it with a queue or shared ring when burst buffering is required.

## Console output

Each task prints the cumulative payload as soon as it has added its own stage,
before forwarding it to the next task. `printf` must be wired to a UART or
semihosting channel in the BSP. Expected output:

```text
T0: T0
T1: T0->T1
T2: T0->T1->T2
T3: T0->T1->T2->T3
T4: T0->T1->T2->T3->T4
final: T0->T1->T2->T3->T4
```
