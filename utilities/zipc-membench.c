#define _GNU_SOURCE
#include <zipc/zipc.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static uint64_t parse_size(const char *text)
{
    char *end = NULL;
    uint64_t value = strtoull(text, &end, 0);
    if (end == text)
        return 0U;
    if (*end == 'K' || *end == 'k')
        value *= 1024U;
    else if (*end == 'M' || *end == 'm')
        value *= 1024U * 1024U;
    else if (*end == 'G' || *end == 'g')
        value *= UINT64_C(1024) * 1024U * 1024U;
    else if (*end != '\0')
        return 0U;
    return value;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s --backend posix|hugepages|dtrevmem-cached|dtrevmem-uncached "
            "[--size 64M] [--iterations 20] [--path PATH] [--phys ADDRESS]\n",
            program);
}

int main(int argc, char **argv)
{
    const char *backend = NULL;
    const char *path = NULL;
    uint64_t physical_address = 0U;
    size_t size = 64U * 1024U * 1024U;
    unsigned int iterations = 20U;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc)
            backend = argv[++i];
        else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc)
            size = (size_t)parse_size(argv[++i]);
        else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc)
            iterations = (unsigned int)strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc)
            path = argv[++i];
        else if (strcmp(argv[i], "--phys") == 0 && i + 1 < argc)
            physical_address = strtoull(argv[++i], NULL, 0);
        else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (backend == NULL || size == 0U || iterations == 0U) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    char generated_path[128];
    char generated_name[64];
    zipc_platform_memory_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.size = size;

    if (strcmp(backend, "posix") == 0) {
        snprintf(generated_name, sizeof(generated_name),
                 "/zipc_membench_%ld", (long)getpid());
        cfg.type = ZIPC_SHM_POSIX;
        cfg.backend.posix.name = generated_name;
        cfg.backend.posix.create = true;
        cfg.backend.posix.unlink_on_close = true;
    } else if (strcmp(backend, "hugepages") == 0) {
        if (path == NULL) {
            snprintf(generated_path, sizeof(generated_path),
                     "/dev/hugepages/zipc_membench_%ld", (long)getpid());
            path = generated_path;
        }
        cfg.type = ZIPC_SHM_HUGEPAGES;
        cfg.backend.hugepages.path = path;
        cfg.backend.hugepages.create = true;
        cfg.backend.hugepages.unlink_on_close = true;
    } else if (strcmp(backend, "dtrevmem-cached") == 0 ||
               strcmp(backend, "dtrevmem-uncached") == 0) {
        if (physical_address == 0U) {
            fprintf(stderr, "--phys is required for DT reserved memory\n");
            return EXIT_FAILURE;
        }
        cfg.type = strcmp(backend, "dtrevmem-cached") == 0
                       ? ZIPC_SHM_DTREVMEM_CACHED
                       : ZIPC_SHM_DTREVMEM_UNCACHED;
        cfg.backend.dtrevmem.device_path = path != NULL ? path : "/dev/mem";
        cfg.backend.dtrevmem.physical_address = physical_address;
        cfg.backend.dtrevmem.supports_cpu_atomics = true;
        cfg.backend.dtrevmem.remote_accessible = true;
        cfg.backend.dtrevmem.device_memory =
            cfg.type == ZIPC_SHM_DTREVMEM_UNCACHED;
    } else {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    zipc_platform_memory_t *memory = NULL;
    const zipc_status_t status = zipc_platform_memory_open(&memory, &cfg);
    if (status != ZIPC_OK) {
        fprintf(stderr, "memory open failed: %d\n", (int)status);
        return EXIT_FAILURE;
    }

    uint8_t *base = zipc_platform_memory_base(memory);
    memset(base, 0, size); /* pre-fault */

    uint64_t start = now_ns();
    for (unsigned int i = 0U; i < iterations; ++i)
        memset(base, (int)(i + 1U), size);
    uint64_t end = now_ns();
    const double write_seconds = (double)(end - start) / 1e9;
    const double write_gib = (double)size * iterations /
                             (1024.0 * 1024.0 * 1024.0);

    volatile uint64_t checksum = 0U;
    start = now_ns();
    for (unsigned int i = 0U; i < iterations; ++i) {
        uint64_t local = 0U;
        for (size_t offset = 0U; offset < size; offset += 64U)
            local += base[offset];
        checksum += local;
    }
    end = now_ns();
    const double read_seconds = (double)(end - start) / 1e9;
    const double read_gib = (double)size * iterations /
                            (1024.0 * 1024.0 * 1024.0);

    printf("backend=%s size=%zu iterations=%u caps=0x%08x\n",
           backend, size, iterations,
           zipc_platform_memory_capabilities(memory));
    printf("write=%.3f GiB/s read-scan=%.3f GiB/s checksum=%" PRIu64 "\n",
           write_gib / write_seconds, read_gib / read_seconds,
           (uint64_t)checksum);

    zipc_platform_memory_close(memory);
    return EXIT_SUCCESS;
}
