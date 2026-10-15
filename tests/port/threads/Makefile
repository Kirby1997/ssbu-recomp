# tests/port/threads - runtime/os_threads.c (+ plat_heap.c) on aarch64 Linux (Steam Frame). Built and run on the device.
#   make CC=clang OPT=-O2            build + run into build/clang-O2 (also -O1: the fiber TLS hazard shows at -O2)
#   make CC=gcc SAN=tsan             ThreadSanitizer build of test_os_threads (the heap test is skipped: TSan's allocator)
# run_frame.sh copies the files to the Frame and runs every configuration.
RT      ?= ../../../runtime
TP      ?= ../../plat
CC      ?= cc
OPT     ?= -O2
SAN     ?=
BUILD   := build/$(notdir $(CC))$(OPT)$(if $(SAN),-$(SAN))
CFLAGS  := $(OPT) -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable -Wno-missing-field-initializers \
           -std=gnu11 -D_GNU_SOURCE -pthread -fno-pie -fno-omit-frame-pointer -I$(RT) -I$(TP) -I.
LDFLAGS := -pthread -no-pie
TESTS   := test_os_threads test_heap test_heap_nofix
ifeq ($(SAN),tsan)
CFLAGS  += -fsanitize=thread
LDFLAGS += -fsanitize=thread
TESTS   := test_os_threads
endif
LIB := $(BUILD)/plat_posix.o $(BUILD)/vmem.o $(BUILD)/fiber_aarch64.o $(BUILD)/plat_crash.o $(BUILD)/os_threads.o $(BUILD)/stubs.o
HDR := $(RT)/plat.h $(RT)/vmem.h $(RT)/fiber.h $(RT)/plat_crash.h $(RT)/runtime.h $(RT)/hostrt.h $(TP)/tp.h

all: run
build: $(addprefix $(BUILD)/,$(TESTS))
run: build
	@set -e; for t in $(TESTS); do echo "== $(BUILD)/$$t"; TSAN_OPTIONS="suppressions=$(CURDIR)/tsan.supp $$TSAN_OPTIONS" ./$(BUILD)/$$t; done

$(BUILD):
	mkdir -p $@
$(BUILD)/%.o: $(RT)/%.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: $(RT)/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/stubs.o: stubs.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_heap: test_heap.c $(BUILD)/plat_heap.o $(LIB) $(HDR)
	$(CC) $(CFLAGS) $< $(BUILD)/plat_heap.o $(LIB) $(LDFLAGS) -o $@
$(BUILD)/test_heap_nofix: test_heap.c $(LIB) $(HDR)
	$(CC) $(CFLAGS) -DNOFIX $< $(LIB) $(LDFLAGS) -o $@
$(BUILD)/%: %.c $(LIB) $(HDR)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) -o $@

clean:
	rm -rf build
.PHONY: all build run clean
