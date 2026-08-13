#include <zipc/zipc.h>
#include <openamp/open_amp.h>
zipc_status_t zipc_test_random(void *buffer, size_t length) {
    static uint32_t value = UINT32_C(0x2468ace1); uint8_t *bytes = buffer;
    for (size_t i = 0U; i < length; ++i) {
        value = value * UINT32_C(1103515245) + UINT32_C(12345);
        bytes[i] = (uint8_t)(value >> 24);
    }
    return ZIPC_OK;
}
int rpmsg_send(struct rpmsg_endpoint *endpoint, const void *data, size_t length) {
    (void)endpoint; (void)data; return (int)length;
}
