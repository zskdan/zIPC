#include <zipc/zipc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do {                                                     \
    zipc_status_t status__ = (call);                                          \
    if (status__ != ZIPC_OK) {                                                \
        fprintf(stderr, "%s failed: status=%d at %s:%d\n",                 \
                #call, (int)status__, __FILE__, __LINE__);                   \
        exit(EXIT_FAILURE);                                                  \
    }                                                                        \
} while (0)

enum {
    COMP_A = 0,
    COMP_B = 1,
    COMP_C = 2,
    COMP_D = 3
};

static void transfer(zipc_pool_t *pool,
                     zipc_platform_transport_t *link,
                     zipc_component_id_t source,
                     zipc_component_id_t destination,
                     zipc_buffer_t *buffer)
{
    zipc_message_t tx;
    zipc_message_t rx;
    CHECK(zipc_buffer_prepare_transfer(pool, zipc_buffer_handle(buffer),
                                      source, destination, &tx));
    CHECK(zipc_platform_transport_send(link, &tx));
    CHECK(zipc_platform_transport_receive(link, &rx));
    CHECK(zipc_buffer_claim(pool, &rx, destination, buffer));
}


static zipc_status_t simulated_pl_signal_send(void *context)
{
    uint32_t *doorbell_count = (uint32_t *)context;
    (*doorbell_count)++;
    return ZIPC_OK;
}

static zipc_status_t simulated_pl_signal_wait(void *context,
                                             uint32_t timeout_ticks)
{
    uint32_t *doorbell_count = (uint32_t *)context;
    (void)timeout_ticks;
    return *doorbell_count != 0U ? ZIPC_OK : ZIPC_ERR_TRANSPORT;
}

static void test_pl_ring_irq_transport(void)
{
    const uint32_t depth = 4U;
    const size_t bytes = zipc_transport_spsc_ring_size(depth);
    zipc_transport_spsc_ring_t *ring = calloc(1U, bytes);
    zipc_platform_transport_t *transport = NULL;
    uint32_t doorbell_count = 0U;

    if (ring == NULL) {
        fprintf(stderr, "PL ring allocation failed\n");
        exit(EXIT_FAILURE);
    }

    const zipc_platform_transport_config_t config = {
        .type = ZIPC_TRANSPORT_PL_RING_IRQ,
        .platform_handle = ring,
        .ring_depth = depth,
        .create_endpoint = true,
        .signal_send = simulated_pl_signal_send,
        .signal_wait = simulated_pl_signal_wait,
        .signal_context = &doorbell_count
    };

    const zipc_message_t tx = {
        .handle = zipc_handle_make(3U, 7U),
        .source_component = COMP_A,
        .destination_component = COMP_B,
        .transfer_sequence = 9U,
        .flags = 0x55U
    };
    zipc_message_t rx;

    CHECK(zipc_platform_transport_open(&transport, &config));
    CHECK(zipc_platform_transport_send(transport, &tx));
    CHECK(zipc_platform_transport_receive(transport, &rx));

    if (memcmp(&tx, &rx, sizeof(tx)) != 0 || doorbell_count != 1U) {
        fprintf(stderr, "PL ring/IRQ simulation failed\n");
        exit(EXIT_FAILURE);
    }

    zipc_platform_transport_close(transport);
    free(ring);
}


static zipc_status_t simulated_secure_call(
    void *context,
    uint32_t service_id,
    const zipc_message_t *request,
    zipc_message_t *response)
{
    uint32_t *call_count = context;
    if (request == NULL || response == NULL || service_id == 0U)
        return ZIPC_ERR_INVALID_ARGUMENT;
    (*call_count)++;
    *response = *request;
    response->flags ^= service_id;
    return ZIPC_OK;
}

static void test_secure_transports(void)
{
    const zipc_platform_transport_type_t types[] = {
        ZIPC_TRANSPORT_SMC, ZIPC_TRANSPORT_FFA
    };

    for (size_t i = 0U; i < sizeof(types) / sizeof(types[0]); ++i) {
        uint32_t call_count = 0U;
        zipc_platform_transport_t *transport = NULL;
        zipc_platform_transport_config_t config = {
            .type = types[i],
            .secure_call = simulated_secure_call,
            .secure_context = &call_count,
            .secure_service_id = UINT32_C(0x84000010) + (uint32_t)i
        };
        zipc_message_t tx = {
            .handle = zipc_handle_make(3U, 7U),
            .source_component = COMP_A,
            .destination_component = COMP_B,
            .transfer_sequence = 2U,
            .flags = UINT32_C(0x1234)
        };
        zipc_message_t rx;

        CHECK(zipc_platform_transport_open(&transport, &config));
        CHECK(zipc_platform_transport_send(transport, &tx));
        CHECK(zipc_platform_transport_receive(transport, &rx));
        if (call_count != 1U ||
            rx.flags != (tx.flags ^ config.secure_service_id) ||
            rx.handle != tx.handle) {
            fprintf(stderr, "secure transport simulation failed\n");
            exit(EXIT_FAILURE);
        }
        zipc_platform_transport_close(transport);
    }
}
int main(void)
{
    test_secure_transports();
    test_pl_ring_irq_transport();
    const uint32_t slot_count = 16U;
    const uint32_t slot_capacity = 4096U;
    const uint32_t alignment = 64U;
    const size_t control_size = zipc_pool_required_control_size(slot_count);
    const size_t payload_size = zipc_pool_required_payload_size(
        slot_count, slot_capacity, 0U, alignment);

    zipc_platform_memory_t *control_memory = NULL;
    zipc_platform_memory_t *payload_memory = NULL;

    zipc_platform_memory_config_t control_cfg = {
        .type = ZIPC_SHM_POSIX,
        .size = control_size,
        .backend.posix = {
            .name = "/chained_zipc_control",
            .create = true,
            .unlink_on_close = true
        }
    };
    zipc_platform_memory_config_t payload_cfg = {
        .type = ZIPC_SHM_POSIX,
        .size = payload_size,
        .backend.posix = {
            .name = "/chained_zipc_payload",
            .create = true,
            .unlink_on_close = true
        }
    };

    CHECK(zipc_platform_memory_open(&control_memory, &control_cfg));
    CHECK(zipc_platform_memory_open(&payload_memory, &payload_cfg));

    zipc_pool_config_t pool_cfg = {
        .control_memory = control_memory,
        .control_offset = 0U,
        .payload_memory = payload_memory, /* NULL would select single-memory mode. */
        .payload_offset = 0U,
        .slot_count = slot_count,
        .slot_capacity = slot_capacity,
        .slot_stride = 0U,
        .payload_alignment = alignment
    };

    CHECK(zipc_pool_format(&pool_cfg));

    zipc_pool_t pool;
    CHECK(zipc_pool_attach(&pool, &pool_cfg));

    zipc_platform_transport_t *link_ab = NULL;
    zipc_platform_transport_t *link_bc = NULL;
    zipc_platform_transport_t *link_bd = NULL;

    const zipc_platform_transport_config_t ab_cfg = {
        .type = ZIPC_TRANSPORT_FIFO,
        .endpoint_name = "/tmp/chained_zipc_ab.fifo",
        .create_endpoint = true
    };
    const zipc_platform_transport_config_t bc_cfg = {
        .type = ZIPC_TRANSPORT_UNIX_DGRAM,
        .endpoint_name = "/tmp/chained_zipc_bc.sock",
        .create_endpoint = true
    };
    const zipc_platform_transport_config_t bd_cfg = {
        .type = ZIPC_TRANSPORT_SHM_RING_EVENTFD,
        .ring_depth = 8U,
        .create_endpoint = true
    };

    CHECK(zipc_platform_transport_open(&link_ab, &ab_cfg));
    CHECK(zipc_platform_transport_open(&link_bc, &bc_cfg));
    CHECK(zipc_platform_transport_open(&link_bd, &bd_cfg));

    zipc_buffer_t buffer;
    CHECK(zipc_buffer_allocate(&pool, COMP_A, &buffer));
    CHECK(zipc_buffer_set_region(&buffer, 16U, 5U));
    memcpy(zipc_buffer_data(&buffer), "hello", 5U);

    transfer(&pool, link_ab, COMP_A, COMP_B, &buffer);
    CHECK(zipc_buffer_append(&buffer, "-B", 2U));

    transfer(&pool, link_bc, COMP_B, COMP_C, &buffer);
    CHECK(zipc_buffer_prepend(&buffer, "C:", 2U));

    transfer(&pool, link_bc, COMP_C, COMP_B, &buffer); /* loop */
    CHECK(zipc_buffer_append(&buffer, "-B2", 3U));
    CHECK(zipc_buffer_append(&buffer, "\0", 1U));

    transfer(&pool, link_bd, COMP_B, COMP_D, &buffer);

    printf("D consumed data='%s'\n", (char *)zipc_buffer_data(&buffer));
    printf("hop_count=%u distinct_components=%u visited_mask=0x%016llx\n",
           zipc_buffer_hop_count(&buffer),
           (unsigned)__builtin_popcountll(zipc_buffer_visited_mask(&buffer)),
           (unsigned long long)zipc_buffer_visited_mask(&buffer));
    printf("control_caps=0x%08x payload_caps=0x%08x\n",
           zipc_platform_memory_capabilities(control_memory),
           zipc_platform_memory_capabilities(payload_memory));

    if (strcmp((char *)zipc_buffer_data(&buffer), "C:hello-B-B2") != 0 ||
        zipc_buffer_hop_count(&buffer) != 5U ||
        __builtin_popcountll(zipc_buffer_visited_mask(&buffer)) != 4) {
        fprintf(stderr, "validation failed\n");
        return EXIT_FAILURE;
    }

    CHECK(zipc_buffer_release(&buffer));
    puts("PASS: SMC/FF-A, PL ring/IRQ, split memory, FIFO, Unix socket, ring/eventfd, loop, prepend and append");

    zipc_platform_transport_close(link_bd);
    zipc_platform_transport_close(link_bc);
    zipc_platform_transport_close(link_ab);
    zipc_platform_memory_close(payload_memory);
    zipc_platform_memory_close(control_memory);
    return EXIT_SUCCESS;
}
