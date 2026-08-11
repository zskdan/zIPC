CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
CPPFLAGS += -Iinclude -Iplatform/linux/common
LDLIBS += -pthread -lrt

CORE := src/core/zipc.c src/core/zipc-backends.c
LINUX_COMMON := platform/linux/common/platform-linux-common.c
LINUX_USER := platform/linux/user/platform-linux-user.c platform/linux/user/zipc-topology-file.c

.PHONY: all clean test basic integration integration-targets integration-freertos integration-baremetal guard-pages api-simplified topology-config strict-ownership ready-linux5 ready-to-play utilities ping membench transport-bench stat list-platforms docs shared-buffer-chain

all: basic integration build/zipc-resilience-linux integration-targets guard-pages api-simplified topology-config strict-ownership shared-buffer-chain ready-linux5 utilities

basic: build/zipc-basic
integration: build/zipc-integration-linux

integration-targets: integration-freertos integration-baremetal
integration-freertos: build/zipc-integration-freertos
integration-baremetal: build/zipc-integration-baremetal
guard-pages: build/zipc-guard-pages-linux
api-simplified: build/zipc-api-simplified-linux
topology-config: build/zipc-topology-config-linux
strict-ownership: build/zipc-strict-ownership-linux
shared-buffer-chain: build/zipc-shared-buffer-chain
ready-linux5: build/zipc-ready-linux5
ready-to-play: ready-linux5

utilities: ping membench transport-bench stat
ping: build/zipc-ping
membench: build/zipc-membench
transport-bench: build/zipc-packetrate
stat: build/zipc-stat

build:
	mkdir -p $@

build/zipc-basic: $(CORE) $(LINUX_COMMON) $(LINUX_USER) examples/basic/basic.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/zipc-integration-linux: $(CORE) $(LINUX_COMMON) $(LINUX_USER) tests/integration-linux.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/zipc-resilience-linux: $(CORE) $(LINUX_COMMON) $(LINUX_USER) tests/resilience-linux.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)



build/zipc-shared-buffer-chain: $(CORE) $(LINUX_COMMON) $(LINUX_USER) examples/shared-buffer-chain/main.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)

docs:
	@command -v doxygen >/dev/null 2>&1 || { echo "doxygen not installed"; exit 2; }
	doxygen Doxyfile

build/zipc-api-simplified-linux: $(CORE) $(LINUX_COMMON) $(LINUX_USER) tests/api-simplified-linux.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/zipc-topology-config-linux: $(CORE) $(LINUX_COMMON) $(LINUX_USER) tests/topology-config-linux.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/zipc-strict-ownership-linux: $(CORE) $(LINUX_COMMON) $(LINUX_USER) tests/strict-ownership-linux.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/zipc-guard-pages-linux: $(CORE) $(LINUX_COMMON) $(LINUX_USER) tests/guard-pages-linux.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)


build/zipc-integration-freertos: $(CORE) platform/freertos/platform-freertos.c tests/stubs/freertos-stubs.c tests/integration-freertos.c | build
	$(CC) -Itests/stubs $(CPPFLAGS) $(CFLAGS) $^ -o $@

build/zipc-integration-baremetal: $(CORE) platform/baremetal/platform-baremetal.c tests/stubs/baremetal-stubs.c tests/integration-baremetal.c | build
	$(CC) -Itests/stubs $(CPPFLAGS) $(CFLAGS) $^ -o $@

build/zipc-ready-linux5: $(CORE) $(LINUX_COMMON) $(LINUX_USER) examples/ready-to-play/linux-5-processes/main.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/zipc-ping: $(CORE) $(LINUX_COMMON) $(LINUX_USER) utilities/zipc-ping.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/zipc-membench: $(CORE) $(LINUX_COMMON) $(LINUX_USER) utilities/zipc-membench.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/zipc-packetrate: $(CORE) $(LINUX_COMMON) $(LINUX_USER) utilities/zipc-packetrate.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/zipc-stat: $(CORE) $(LINUX_COMMON) $(LINUX_USER) utilities/zipc-stat.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)

test: all guard-pages api-simplified topology-config strict-ownership shared-buffer-chain ready-linux5 utilities
	./build/zipc-basic
	./build/zipc-integration-linux
	./build/zipc-resilience-linux
	./build/zipc-guard-pages-linux
	./build/zipc-api-simplified-linux
	./build/zipc-topology-config-linux
	./build/zipc-strict-ownership-linux
	./build/zipc-shared-buffer-chain
	./build/zipc-integration-freertos
	./build/zipc-integration-baremetal
	./build/zipc-ready-linux5
	./build/zipc-ping --relays 2 --count 3
	./build/zipc-membench --backend posix --size 4M --iterations 2
	./build/zipc-packetrate --transport ring-eventfd --packets 10000 --payload 8

list-platforms:
	@printf '%s\n' \
	  linux-user linux-kernel linux-common freertos baremetal xen

clean:
	rm -rf build
