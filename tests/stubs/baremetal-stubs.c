#include <openamp/open_amp.h>
int rpmsg_send(struct rpmsg_endpoint *endpoint, const void *data, size_t length) {
    (void)endpoint; (void)data; return (int)length;
}
