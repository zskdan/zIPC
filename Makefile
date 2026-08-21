CC ?= cc
AR ?= ar
SPHINXBUILD ?= sphinx-build
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
CPPFLAGS += -Iinclude -Iplatform/linux/common
LDLIBS += -pthread -lrt
DEPFLAGS = -MMD -MP -MF $@.d -MT $@

CORE := src/core/zipc.c src/core/zipc-backends.c
LINUX_COMMON := platform/linux/common/platform-linux-common.c
LINUX_USER := platform/linux/user/platform-linux-user.c platform/linux/user/zipc-topology-file.c
PUBLIC_HEADERS := $(wildcard include/zipc/*.h)
STUB_HEADERS := $(wildcard tests/stubs/*.h tests/stubs/openamp/*.h)

ZIPC_LIB := build/libs/libzipc.a
ZIPC_OBJS := build/libs/obj/zipc.o build/libs/obj/zipc-backends.o \
             build/libs/obj/platform-linux-common.o \
             build/libs/obj/platform-linux-user.o \
             build/libs/obj/zipc-topology-file.o

EXAMPLE_BINS := build/examples/zipc-basic \
                 build/examples/zipc-buffer-api \
                 build/examples/zipc-shared-buffer-chain \
                build/examples/zipc-ready-linux5
TEST_BINS := build/tests/zipc-integration-linux build/tests/zipc-resilience-linux \
             build/tests/zipc-guard-pages-linux build/tests/zipc-hardening-linux \
             build/tests/zipc-version-linux \
             build/tests/zipc-api-simplified-linux \
              build/tests/zipc-identity-linux \
              build/tests/zipc-recovery-chain-linux \
             build/tests/zipc-topology-config-linux \
             build/tests/zipc-strict-ownership-linux \
             build/tests/zipc-integration-freertos \
             build/tests/zipc-integration-baremetal
UTILITY_BINS := build/utilities/zipc-ping build/utilities/zipc-membench \
                build/utilities/zipc-packetrate build/utilities/zipc-stat
DEPS := $(ZIPC_OBJS:%=%.d) $(EXAMPLE_BINS:%=%.d) $(TEST_BINS:%=%.d) \
        $(UTILITY_BINS:%=%.d)

.PHONY: all clean test libs basic integration integration-targets integration-freertos \
        integration-baremetal guard-pages version-linux api-simplified topology-config \
		strict-ownership ready-linux5 ready-to-play utilities ping membench \
		transport-bench stat list-platforms doxygen docs shared-buffer-chain buffer-api

all: $(ZIPC_LIB) $(EXAMPLE_BINS) $(TEST_BINS) $(UTILITY_BINS)

libs: $(ZIPC_LIB)

build:
	mkdir -p $@
build/examples build/tests build/utilities build/libs build/libs/obj build/docs: build
	mkdir -p $@

build/libs/obj/%.o: src/core/%.c | build/libs/obj
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@
build/libs/obj/%.o: platform/linux/common/%.c | build/libs/obj
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@
build/libs/obj/%.o: platform/linux/user/%.c | build/libs/obj
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(ZIPC_LIB): $(ZIPC_OBJS) Makefile | build/libs
	$(RM) $@
	$(AR) rcs $@ $(ZIPC_OBJS)

build/examples/zipc-basic: $(ZIPC_LIB) examples/basic/basic.c | build/examples
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/examples/zipc-buffer-api: $(ZIPC_LIB) examples/buffer-api/main.c | build/examples
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/examples/zipc-shared-buffer-chain: $(ZIPC_LIB) examples/shared-buffer-chain/main.c | build/examples
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/examples/zipc-ready-linux5: $(ZIPC_LIB) examples/ready-to-play/linux-5-processes/main.c | build/examples
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)

build/tests/zipc-integration-linux: $(ZIPC_LIB) tests/integration-linux.c | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/tests/zipc-resilience-linux: $(ZIPC_LIB) tests/resilience-linux.c | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/tests/zipc-guard-pages-linux: $(ZIPC_LIB) tests/guard-pages-linux.c | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/tests/zipc-hardening-linux: $(ZIPC_LIB) tests/hardening-linux.c | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/tests/zipc-version-linux: $(ZIPC_LIB) tests/version-linux.c | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/tests/zipc-api-simplified-linux: $(ZIPC_LIB) tests/api-simplified-linux.c | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/tests/zipc-topology-config-linux: $(ZIPC_LIB) tests/topology-config-linux.c | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/tests/zipc-strict-ownership-linux: $(ZIPC_LIB) tests/strict-ownership-linux.c | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)
build/tests/zipc-identity-linux: $(CORE) $(LINUX_COMMON) $(LINUX_USER) tests/identity-linux.c tests/identity-test.h $(PUBLIC_HEADERS) | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) -DZIPC_TESTING $(filter %.c,$^) -o $@ $(LDLIBS)
build/tests/zipc-recovery-chain-linux: $(CORE) $(LINUX_COMMON) $(LINUX_USER) tests/recovery-chain-linux.c $(PUBLIC_HEADERS) | build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) -DZIPC_TESTING $(filter %.c,$^) -o $@ $(LDLIBS)

build/tests/zipc-integration-freertos: $(CORE) platform/freertos/platform-freertos.c tests/stubs/freertos-stubs.c tests/integration-freertos.c $(PUBLIC_HEADERS) $(STUB_HEADERS) | build/tests
	$(CC) -Itests/stubs $(CPPFLAGS) $(CFLAGS) -DZIPC_FREERTOS_RANDOM=zipc_test_random $(filter %.c,$^) -o $@
build/tests/zipc-integration-baremetal: $(CORE) platform/baremetal/platform-baremetal.c tests/stubs/baremetal-stubs.c tests/integration-baremetal.c $(PUBLIC_HEADERS) $(STUB_HEADERS) | build/tests
	$(CC) -Itests/stubs $(CPPFLAGS) $(CFLAGS) -DZIPC_BAREMETAL_RANDOM=zipc_test_random $(filter %.c,$^) -o $@

build/utilities/%: $(ZIPC_LIB) utilities/%.c | build/utilities
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) $(filter %.c,$^) $(ZIPC_LIB) -o $@ $(LDLIBS)

basic: build/examples/zipc-basic
buffer-api: build/examples/zipc-buffer-api
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

doxygen: build/docs
	@command -v doxygen >/dev/null 2>&1 || { echo "doxygen not installed"; exit 2; }
	doxygen Doxyfile

docs: doxygen
	@command -v $(SPHINXBUILD) >/dev/null 2>&1 || { \
		echo "sphinx-build not installed; run: python3 -m pip install -r docs/requirements.txt"; \
		exit 2; \
	}
	$(SPHINXBUILD) $(SPHINXOPTS) -b html docs build/docs/html

test: all
	./build/examples/zipc-basic
	./build/examples/zipc-buffer-api
	./build/tests/zipc-integration-linux
	./build/tests/zipc-resilience-linux
	./build/tests/zipc-guard-pages-linux
	./build/tests/zipc-hardening-linux
	./build/tests/zipc-version-linux
	./build/tests/zipc-api-simplified-linux
	./build/tests/zipc-identity-linux
	./build/tests/zipc-recovery-chain-linux --iterations 10 --seed 20260817 --quiet
	./build/tests/zipc-topology-config-linux
	./build/tests/zipc-strict-ownership-linux
	./build/examples/zipc-shared-buffer-chain
	./build/examples/zipc-shared-buffer-chain --config examples/shared-buffer-chain/zipc.conf
	./build/tests/zipc-integration-freertos
	./build/tests/zipc-integration-baremetal
	./build/examples/zipc-ready-linux5
	./build/utilities/zipc-ping --relays 2 --count 3 --interval 0 --timeout 1
	./build/utilities/zipc-membench --backend posix --size 4M --iterations 2
	./build/utilities/zipc-packetrate --transport ring-eventfd --packets 10000 --payload 8
	./build/utilities/zipc-packetrate --transport ring-eventfd --packets 32 --payload 1M --relays 2
	./build/utilities/zipc-packetrate --transport ring-eventfd --packets 100 --payload 1K --relays 2 --slots 2
	@if ./build/utilities/zipc-packetrate --ring-depth 2 >/dev/null 2>&1; then \
	  echo "zipc-packetrate accepted removed --ring-depth"; exit 1; \
	fi
	@for value in garbage 1junk nan inf -1 -0; do \
	  if ./build/utilities/zipc-ping --interval "$$value" >/dev/null 2>&1; then \
	    echo "zipc-ping accepted invalid interval: $$value"; exit 1; \
	  fi; \
	done
	@if ./build/utilities/zipc-ping --count -1 >/dev/null 2>&1; then \
	  echo "zipc-ping accepted a negative count"; exit 1; \
	fi
	@if ./build/utilities/zipc-ping --count " -1" >/dev/null 2>&1; then \
	  echo "zipc-ping accepted a whitespace-prefixed negative count"; exit 1; \
	fi
	@if ./build/utilities/zipc-stat --slots -1 >/dev/null 2>&1; then \
	  echo "zipc-stat accepted a negative slot count"; exit 1; \
	fi
	@if ./build/utilities/zipc-stat --slots 0 >/dev/null 2>&1; then \
	  echo "zipc-stat accepted a zero slot count"; exit 1; \
	fi
	@output="$$(./build/utilities/zipc-stat --name /zipc-stat-missing-$$$$ 2>&1)"; \
	status=$$?; \
	if [ $$status -ne 1 ]; then echo "zipc-stat missing-pool exit status: $$status"; exit 1; fi; \
	case "$$output" in *"only observes an already-running pool"*) ;; \
	  *) echo "zipc-stat missing-pool diagnostic absent"; exit 1;; esac

list-platforms:
	@printf '%s\n' \
	  linux-user linux-kernel linux-common freertos baremetal xen

clean:
	rm -rf build

-include $(DEPS)
