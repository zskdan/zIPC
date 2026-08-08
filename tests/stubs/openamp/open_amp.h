#ifndef TEST_OPENAMP_H
#define TEST_OPENAMP_H
#include <stddef.h>
struct rpmsg_endpoint { int dummy; };
int rpmsg_send(struct rpmsg_endpoint *endpoint, const void *data, size_t length);
#endif
