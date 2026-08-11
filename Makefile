CC ?= cc
AR ?= ar
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
CPPFLAGS += -Iinclude -Iplatform/linux/common
LDLIBS += -pthread -lrt

CORE := src/core/zipc.c src/core/zipc-backends.c
LINUX_COMMON := platform/linux/common/platform-linux-common.c
LINUX_USER := platform/linux/user/platform-linux-user.c platform/linux/user/zipc-topology-file.c

ZIPC_LIB := build/libs/libzipc.a
ZIPC_OBJS := build/libs/obj/zipc.o build/libs/obj/zipc-backends.o \
             build/libs/obj/platform-linux-common.o \
             build/libs/obj/platform-linux-user.o \
             build/libs/obj/zipc-topology-file.o

EXAMPLE_BINS := build/examples/zipc-basic \
                build/examples/zipc-shared-buffer-chain \
                build/examples/zipc-ready-linux5
TEST_BINS := build/tests/zipc-integration-linux build/tests/zipc-resilience-linux \
             build/tests/zipc-guard-pages-linux build/tests/zipc-version-linux \
             build/tests/zipc-api-simplified-linux \
             build/tests/zipc-topology-config-linux \
             build/tests/zipc-strict-ownership-linux \
             build/tests/zipc-integration-freertos \
             build/tests/zipc-integration-baremetal
UTILITY_BINS := build/utilities/zipc-ping build/utilities/zipc-membench \
                build/utilities/zipc-packetrate build/utilities/zipc-stat

.PHONY: all clean test libs basic integration integration-targets integration-freertos \
        integration-baremetal guard-pages version-linux api-simplified topology-config \
        strict-ownership ready-linux5 ready-to-play utilities ping membench \
        transport-bench stat list-platforms docs shared-buffer-chain

all: libs basic integration build/tests/zipc-resilience-linux integration-targets \
     guard-pages version-linux api-simplified topology-config strict-ownership \
     shared-buffer-chain ready-linux5 utilities

libs: $(ZIPC_LIB)

build:
	mkdir -p $@
build/examples build/tests build/utilities build/libs build/libs/obj: build
	mkdir -p $@

build/libs/obj/%.o: src/core/%.c | build/libs/obj
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
build/libs/obj/%.o: platform/linux/common/%.c | build/libs/obj
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
build/libs/obj/%.o: platform/linux/user/%.c | build/libs/obj
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(ZIPC_LIB): $(ZIPC_OBJS) | build/libs
	$(AR) rcs $@ $^

build/examples/zipc-basic: $(ZIPC_LIB) examples/basic/basic.c | build/examples
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/examples/zipc-shared-buffer-chain: $(ZIPC_LIB) examples/shared-buffer-chain/main.c | build/examples
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/examples/zipc-ready-linux5: $(ZIPC_LIB) examples/ready-to-play/linux-5-processes/main.c | build/examples
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)

build/tests/zipc-integration-linux: $(ZIPC_LIB) tests/integration-linux.c | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/tests/zipc-resilience-linux: $(ZIPC_LIB) tests/resilience-linux.c | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/tests/zipc-guard-pages-linux: $(ZIPC_LIB) tests/guard-pages-linux.c | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/tests/zipc-version-linux: $(ZIPC_LIB) tests/version-linux.c | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/tests/zipc-api-simplified-linux: $(ZIPC_LIB) tests/api-simplified-linux.c | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/tests/zipc-topology-config-linux: $(ZIPC_LIB) tests/topology-config-linux.c | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/tests/zipc-strict-ownership-linux: $(ZIPC_LIB) tests/strict-ownership-linux.c | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)

build/tests/zipc-integration-freertos: $(CORE) platform/freertos/platform-freertos.c tests/stubs/freertos-stubs.c tests/integration-freertos.c | build/tests
	$(CC) -Itests/stubs $(CPPFLAGS) $(CFLAGS) $^ -o $@
build/tests/zipc-integration-baremetal: $(CORE) platform/baremetal/platform-baremetal.c tests/stubs/baremetal-stubs.c tests/integration-baremetal.c | build/tests
	$(CC) -Itests/stubs $(CPPFLAGS) $(CFLAGS) $^ -o $@

build/utilities/%: $(ZIPC_LIB) utilities/%.c | build/utilities
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)

basic: build/examples/zipc-basic
integration: build/tests/zipc-integration-linux
integration-targets: integration-freertos integration-baremetal
integration-freertos: build/tests/zipc-integration-freertos
integration-baremetal: build/tests/zipc-integration-baremetal
guard-pages: build/tests/zipc-guard-pages-linux
version-linux: build/tests/zipc-version-linux
api-simplified: build/tests/zipc-api-simplified-linux
topology-config: build/tests/zipc-topology-config-linux
strict-ownership: build/tests/zipc-strict-ownership-linux
shared-buffer-chain: build/examples/zipc-shared-buffer-chain
ready-linux5: build/examples/zipc-ready-linux5
ready-to-play: ready-linux5

utilities: ping membench transport-bench stat
ping: build/utilities/zipc-ping
membench: build/utilities/zipc-membench
transport-bench: build/utilities/zipc-packetrate
stat: build/utilities/zipc-stat

docs:
	@command -v doxygen >/dev/null 2>&1 || { echo "doxygen not installed"; exit 2; }
	doxygen Doxyfile

test: all
	./build/examples/zipc-basic
	./build/tests/zipc-integration-linux
	./build/tests/zipc-resilience-linux
	./build/tests/zipc-guard-pages-linux
	./build/tests/zipc-version-linux
	./build/tests/zipc-api-simplified-linux
	./build/tests/zipc-topology-config-linux
	./build/tests/zipc-strict-ownership-linux
	./build/examples/zipc-shared-buffer-chain
	./build/tests/zipc-integration-freertos
	./build/tests/zipc-integration-baremetal
	./build/examples/zipc-ready-linux5
	./build/utilities/zipc-ping --relays 2 --count 3 --interval 0
	./build/utilities/zipc-membench --backend posix --size 4M --iterations 2
	./build/utilities/zipc-packetrate --transport ring-eventfd --packets 10000 --payload 8

list-platforms:
	@printf '%s\n' \
	  linux-user linux-kernel linux-common freertos baremetal xen

clean:
	rm -rf build
