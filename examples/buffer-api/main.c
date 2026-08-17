#include <zipc/zipc.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SLOT_COUNT 4U
#define SLOT_CAPACITY 256U
#define PAYLOAD_ALIGNMENT 64U
#define INITIAL_HEADROOM 32U
#define REQUIRED_TAILROOM 32U
#define METADATA_OFFSET 128U

#define CHECK_STATUS(expr) do { \
    const zipc_status_t status_ = (expr); \
    if (status_ != ZIPC_OK) { \
        fprintf(stderr, "%s failed: %d\n", #expr, (int)status_); \
        goto cleanup; \
    } \
} while (0)

#define CHECK_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "validation failed: %s\n", #expr); \
        goto cleanup; \
    } \
} while (0)

enum { PRODUCER = 1, CONSUMER = 2 };

typedef struct {
    uint32_t magic;
    uint16_t writer;
    uint16_t flags;
} fixed_metadata_t;

static void print_state(const char *stage, const zipc_buffer_t *buffer)
{
    printf("%-22s size=%zu headroom=%" PRIu32 " tailroom=%" PRIu32 "\n",
           stage, zipc_buffer_size(buffer), zipc_buffer_headroom(buffer),
           zipc_buffer_tailroom(buffer));
}

static bool append_stays_before_metadata(const zipc_buffer_t *buffer,
                                         size_t length)
{
    const size_t data_offset = zipc_buffer_headroom(buffer);
    const size_t data_size = zipc_buffer_size(buffer);
    return data_offset <= METADATA_OFFSET &&
           data_size <= METADATA_OFFSET - data_offset &&
           length <= METADATA_OFFSET - data_offset - data_size;
}

int main(void)
{
    static const char header[] = "[hdr]";
    static const char body[] = "hello";
    static const char trailer[] = "[tail]";
    static const char complete[] = "[hdr]hello[tail]";
    const size_t header_size = sizeof(header) - 1U;
    const size_t body_size = sizeof(body) - 1U;
    const size_t trailer_size = sizeof(trailer) - 1U;
    int result = 1;
    zipc_platform_memory_t *memory = NULL;
    zipc_link_t *producer = NULL;
    zipc_link_t *consumer = NULL;
    zipc_pool_t pool;
    zipc_buffer_t buffer = {0};

    char memory_name[64];
    char endpoint_name[64];
    snprintf(memory_name, sizeof(memory_name), "/zipc_buffer_api_%ld",
             (long)getpid());
    snprintf(endpoint_name, sizeof(endpoint_name), "/zipc_buffer_api_link_%ld",
             (long)getpid());

    const size_t control_size = zipc_pool_required_control_size(SLOT_COUNT);
    const size_t payload_offset =
        (control_size + PAYLOAD_ALIGNMENT - 1U) &
        ~(size_t)(PAYLOAD_ALIGNMENT - 1U);
    const size_t payload_size = zipc_pool_required_payload_size(
        SLOT_COUNT, SLOT_CAPACITY, 0U, PAYLOAD_ALIGNMENT);
    CHECK_TRUE(control_size != 0U && payload_size != 0U);

    const zipc_platform_memory_config_t memory_cfg = {
        .type = ZIPC_SHM_POSIX,
        .size = payload_offset + payload_size,
        .backend.posix = {
            .name = memory_name,
            .create = true,
            .unlink_on_close = true,
        },
    };
    CHECK_STATUS(zipc_platform_memory_open(&memory, &memory_cfg));

    const zipc_pool_config_t pool_cfg = {
        .control_memory = memory,
        .payload_memory = NULL,
        .payload_offset = payload_offset,
        .slot_count = SLOT_COUNT,
        .slot_capacity = SLOT_CAPACITY,
        .slot_stride = 0U,
        .payload_alignment = PAYLOAD_ALIGNMENT,
    };
    CHECK_STATUS(zipc_pool_format(&pool_cfg));
    CHECK_STATUS(zipc_pool_attach(&pool, &pool_cfg));

    const zipc_link_config_t producer_cfg = {
        .pool = &pool,
        .local_component = PRODUCER,
        .remote_component = CONSUMER,
        .transport = {
            .type = ZIPC_TRANSPORT_MESSAGE_QUEUE,
            .endpoint_name = endpoint_name,
            .create_endpoint = true,
            .queue_length = 8U,
        },
    };
    const zipc_link_config_t consumer_cfg = {
        .pool = &pool,
        .local_component = CONSUMER,
        .remote_component = PRODUCER,
        .transport = {
            .type = ZIPC_TRANSPORT_MESSAGE_QUEUE,
            .endpoint_name = endpoint_name,
        },
    };
    CHECK_STATUS(zipc_link_create(&producer, &producer_cfg));
    CHECK_STATUS(zipc_link_create(&consumer, &consumer_cfg));

    CHECK_STATUS(zipc_buffer_alloc_ex(
        producer, 0U, INITIAL_HEADROOM, REQUIRED_TAILROOM, &buffer, NULL));
    print_state("allocated", &buffer);
    CHECK_TRUE(zipc_buffer_size(&buffer) == 0U);
    CHECK_TRUE(zipc_buffer_headroom(&buffer) == INITIAL_HEADROOM);
    CHECK_TRUE(zipc_buffer_tailroom(&buffer) ==
               SLOT_CAPACITY - INITIAL_HEADROOM);

    fixed_metadata_t *metadata = zipc_buffer_at(
        &buffer, METADATA_OFFSET, sizeof(*metadata));
    CHECK_TRUE(metadata != NULL);
    *metadata = (fixed_metadata_t) {
        .magic = UINT32_C(0x5A495043),
        .writer = PRODUCER,
        .flags = UINT16_C(0x0001),
    };
    printf("absolute metadata      offset=%" PRIu32 " magic=0x%08" PRIx32
           "\n", (uint32_t)METADATA_OFFSET, metadata->magic);

    CHECK_TRUE(append_stays_before_metadata(&buffer, body_size));
    CHECK_STATUS(zipc_buffer_append(&buffer, body, (uint32_t)body_size));
    print_state("after body append", &buffer);
    CHECK_TRUE(zipc_buffer_size(&buffer) == body_size);
    CHECK_TRUE(zipc_buffer_headroom(&buffer) == INITIAL_HEADROOM);
    CHECK_TRUE(zipc_buffer_tailroom(&buffer) ==
               SLOT_CAPACITY - INITIAL_HEADROOM - body_size);
    CHECK_TRUE(append_stays_before_metadata(&buffer, trailer_size));
    CHECK_STATUS(zipc_buffer_append(
        &buffer, trailer, (uint32_t)trailer_size));
    print_state("after trailer append", &buffer);
    CHECK_TRUE(zipc_buffer_size(&buffer) == body_size + trailer_size);
    CHECK_TRUE(zipc_buffer_headroom(&buffer) == INITIAL_HEADROOM);
    CHECK_TRUE(zipc_buffer_tailroom(&buffer) ==
               SLOT_CAPACITY - INITIAL_HEADROOM - body_size - trailer_size);
    CHECK_STATUS(zipc_buffer_prepend(
        &buffer, header, (uint32_t)header_size));
    print_state("after header prepend", &buffer);
    CHECK_TRUE(zipc_buffer_size(&buffer) == sizeof(complete) - 1U);
    CHECK_TRUE(zipc_buffer_headroom(&buffer) ==
               INITIAL_HEADROOM - header_size);
    CHECK_TRUE(zipc_buffer_tailroom(&buffer) ==
               SLOT_CAPACITY - INITIAL_HEADROOM - body_size - trailer_size);
    CHECK_TRUE(append_stays_before_metadata(&buffer, 0U));
    CHECK_TRUE(memcmp(zipc_buffer_data(&buffer), complete,
                      sizeof(complete) - 1U) == 0);
    printf("producer logical data  %.*s\n", (int)zipc_buffer_size(&buffer),
           (const char *)zipc_buffer_data(&buffer));

    CHECK_STATUS(zipc_send(producer, &buffer));
    CHECK_TRUE(!zipc_buffer_is_valid(&buffer));
    puts("send consumed producer ownership");

    CHECK_STATUS(zipc_recv(consumer, &buffer));
    print_state("consumer received", &buffer);
    CHECK_TRUE(zipc_buffer_size(&buffer) == sizeof(complete) - 1U);
    CHECK_TRUE(zipc_buffer_headroom(&buffer) ==
               INITIAL_HEADROOM - header_size);
    CHECK_TRUE(zipc_buffer_tailroom(&buffer) ==
               SLOT_CAPACITY - INITIAL_HEADROOM - body_size - trailer_size);
    CHECK_TRUE(memcmp(zipc_buffer_data(&buffer), complete,
                      sizeof(complete) - 1U) == 0);
    metadata = zipc_buffer_at(&buffer, METADATA_OFFSET, sizeof(*metadata));
    CHECK_TRUE(metadata != NULL);
    CHECK_TRUE(metadata->magic == UINT32_C(0x5A495043));
    CHECK_TRUE(metadata->writer == PRODUCER);
    CHECK_TRUE(metadata->flags == UINT16_C(0x0001));
    CHECK_TRUE(zipc_buffer_at(
        &buffer, SLOT_CAPACITY - 2U, sizeof(uint32_t)) == NULL);
    CHECK_TRUE(zipc_buffer_at(&buffer, SIZE_MAX, 1U) == NULL);
    CHECK_TRUE(zipc_buffer_at(&buffer, SIZE_MAX - 1U, 4U) == NULL);
    printf("consumer metadata      offset=%" PRIu32 " writer=%" PRIu16
           " flags=0x%04" PRIx16 "\n", (uint32_t)METADATA_OFFSET,
           metadata->writer, metadata->flags);

    CHECK_STATUS(zipc_buffer_trim_front(&buffer, (uint32_t)header_size));
    print_state("after front trim", &buffer);
    CHECK_TRUE(zipc_buffer_size(&buffer) == body_size + trailer_size);
    CHECK_TRUE(zipc_buffer_headroom(&buffer) == INITIAL_HEADROOM);
    CHECK_TRUE(zipc_buffer_tailroom(&buffer) ==
               SLOT_CAPACITY - INITIAL_HEADROOM - body_size - trailer_size);
    CHECK_STATUS(zipc_buffer_trim_back(&buffer, (uint32_t)trailer_size));
    print_state("after back trim", &buffer);
    CHECK_TRUE(zipc_buffer_size(&buffer) == body_size);
    CHECK_TRUE(zipc_buffer_headroom(&buffer) == INITIAL_HEADROOM);
    CHECK_TRUE(zipc_buffer_tailroom(&buffer) ==
               SLOT_CAPACITY - INITIAL_HEADROOM - body_size);
    CHECK_TRUE(memcmp(zipc_buffer_data(&buffer), body, body_size) == 0);
    metadata = zipc_buffer_at(&buffer, METADATA_OFFSET, sizeof(*metadata));
    CHECK_TRUE(metadata != NULL && metadata->magic == UINT32_C(0x5A495043));
    printf("trimmed logical data   %.*s\n", (int)zipc_buffer_size(&buffer),
           (const char *)zipc_buffer_data(&buffer));

    CHECK_STATUS(zipc_buffer_release(&buffer));
    CHECK_TRUE(!zipc_buffer_is_valid(&buffer));
    puts("PASS: append, prepend, absolute offset, trim and ownership");
    result = 0;

cleanup:
    if (zipc_buffer_is_valid(&buffer))
        (void)zipc_buffer_release(&buffer);
    if (consumer != NULL)
        zipc_link_destroy(consumer);
    if (producer != NULL)
        zipc_link_destroy(producer);
    zipc_platform_memory_close(memory);
    return result;
}
