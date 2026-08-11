#define _POSIX_C_SOURCE 200809L
#include <zipc/zipc.h>

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *state_name(uint32_t state)
{
    switch (state) {
    case ZIPC_SLOT_FREE: return "FREE";
    case ZIPC_SLOT_OWNED: return "OWNED";
    case ZIPC_SLOT_TRANSFER: return "TRANSFER";
    case ZIPC_SLOT_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}

static const char *event_name(uint16_t event)
{
    switch (event) {
    case ZIPC_TRACE_ALLOCATE: return "ALLOC";
    case ZIPC_TRACE_SEND: return "SEND";
    case ZIPC_TRACE_RECEIVE: return "RECV";
    case ZIPC_TRACE_RELEASE: return "FREE";
    case ZIPC_TRACE_RECOVER: return "RECOVER";
    case ZIPC_TRACE_ERROR: return "ERROR";
    default: return "?";
    }
}

static uint32_t parse_u32(const char *s)
{
    if (s[0] == '-' || isspace((unsigned char)s[0])) {
        fprintf(stderr, "invalid number: %s\n", s);
        exit(2);
    }
    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(s, &end, 0);
    if (errno == ERANGE || end == s || *end != '\0' ||
        value == 0U || value > UINT32_MAX) {
        fprintf(stderr, "invalid number: %s\n", s);
        exit(2);
    }
    return (uint32_t)value;
}

int main(int argc, char **argv)
{
    const char *name = "/zipc_pool";
    uint32_t slots = 64U;
    uint32_t capacity = 4096U;
    bool show_free = false;
    bool name_set = false, slots_set = false, capacity_set = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--name") == 0 && ++i < argc) { name = argv[i]; name_set = true; }
        else if (strcmp(argv[i], "--slots") == 0 && ++i < argc) { slots = parse_u32(argv[i]); slots_set = true; }
        else if (strcmp(argv[i], "--capacity") == 0 && ++i < argc) { capacity = parse_u32(argv[i]); capacity_set = true; }
        else if (strcmp(argv[i], "--all") == 0) show_free = true;
        else {
            fprintf(stderr, "usage: %s [--name /shm] [--slots N] [--capacity N] [--all]\n", argv[0]);
            return 2;
        }
    }
    printf("config: name=%s%s slots=%u%s capacity=%u%s\n",
           name, name_set ? "" : " (default)",
           slots, slots_set ? "" : " (default)",
           capacity, capacity_set ? "" : " (default)");

    const size_t control_size = zipc_pool_required_control_size(slots);
    const size_t payload_size = zipc_pool_required_payload_size(slots, capacity, 0U, 64U);
    zipc_platform_memory_t *memory = NULL;
    zipc_platform_memory_config_t memory_cfg = {
        .type = ZIPC_SHM_POSIX,
        .size = control_size + payload_size + 64U,
        .backend.posix = {.name = name, .create = false, .unlink_on_close = false}
    };
    const zipc_status_t open_status =
        zipc_platform_memory_open(&memory, &memory_cfg);
    if (open_status != ZIPC_OK) {
        fprintf(stderr,
                "cannot open zIPC pool %s (status=%d)\n"
                "  zipc-stat only observes an already-running pool; it never creates one.\n"
                "  Start the application first, then run:\n"
                "    %s --name %s --slots %u --capacity %u\n"
                "  Pass --slots/--capacity exactly as the pool was created, and check\n"
                "  the POSIX shared-memory object and permissions under /dev/shm.\n",
                name, (int)open_status, argv[0], name, slots, capacity);
        return 1;
    }

    const size_t payload_offset = (control_size + 63U) & ~(size_t)63U;
    zipc_pool_config_t pool_cfg = {
        .control_memory = memory,
        .control_offset = 0U,
        .payload_memory = memory,
        .payload_offset = payload_offset,
        .slot_count = slots,
        .slot_capacity = capacity,
        .slot_stride = 0U,
        .payload_alignment = 64U
    };
    zipc_pool_t pool;
    if (zipc_pool_attach(&pool, &pool_cfg) != ZIPC_OK) {
        fprintf(stderr,
                "pool geometry/ABI mismatch for %s\n"
                "  The slot count, capacity, stride, alignment, flags, or ABI do not\n"
                "  match the live pool. zipc-stat currently supports conventional\n"
                "  contiguous pools with derived stride and 64-byte alignment.\n"
                "  Verify the layout against the pool creator's configuration.\n",
                name);
        zipc_platform_memory_close(memory);
        return 1;
    }

    printf("zIPC ABI=%u slots=%u capacity=%u allocations=%" PRIu64
           " releases=%" PRIu64 " failures=%" PRIu64 " recoveries=%" PRIu64 "\n",
           pool.header->abi_version, pool.header->slot_count, pool.header->slot_capacity,
           atomic_load(&pool.header->allocation_count),
           atomic_load(&pool.header->release_count),
           atomic_load(&pool.header->allocation_failure_count),
           atomic_load(&pool.header->recovery_count));

    const uint64_t now = zipc_platform_time_ns();
    for (uint32_t i = 0; i < slots; ++i) {
        zipc_slot_control_t *slot = &pool.controls[i];
        const uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
        if (!show_free && state == ZIPC_SLOT_FREE) continue;
        const uint64_t age = now >= slot->acquired_ns ? now - slot->acquired_ns : 0U;
        printf("slot=%u state=%s owner=%u epoch=%u gen=%u hops=%u/%u len=%u age_ns=%" PRIu64 " recoveries=%u\n",
               i, state_name(state), slot->owner_id, slot->owner_epoch,
               slot->generation, slot->hop_count, slot->hop_limit,
               slot->region.length, age, slot->recovery_count);
        zipc_trace_entry_t trace[ZIPC_TRACE_DEPTH];
        const uint32_t count = zipc_slot_trace_copy(slot, trace, ZIPC_TRACE_DEPTH);
        for (uint32_t t = 0; t < count; ++t)
            printf("  trace ts=%" PRIu64 " seq=%u component=%u event=%s\n",
                   trace[t].timestamp_ns, trace[t].sequence,
                   trace[t].component_id, event_name(trace[t].event));
    }

    puts("components:");
    for (uint32_t i = 0; i < ZIPC_MAX_COMPONENTS; ++i) {
        zipc_component_snapshot_t snapshot;
        (void)zipc_component_snapshot(&pool, (zipc_component_id_t)i, &snapshot);
        if (snapshot.epoch == 0U && !snapshot.active && snapshot.recovered_slots == 0U) continue;
        printf("  id=%u epoch=%u active=%u heartbeat_ns=%" PRIu64 " recovered=%" PRIu64 "\n",
               i, snapshot.epoch, snapshot.active ? 1U : 0U,
               snapshot.last_heartbeat_ns, snapshot.recovered_slots);
    }

    zipc_platform_memory_close(memory);
    return 0;
}
