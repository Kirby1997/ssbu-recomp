# tests/plat - plat.h / vmem / fiber tests. Native aarch64 Linux (Steam Frame), or any aarch64 Linux.
#   make CC=clang            build + run with clang into build/clang
#   make CC=gcc              same with gcc
#   make CC=clang SAN=tsan   ThreadSanitizer build of sync/threads/fiber (vmem is skipped: TSan owns the VA layout)
#   make CC=clang SAN=bti    -mbranch-protection=standard (PAC on), test_fiber turns on PROT_BTI for the fiber pages
# run_frame.sh copies the files to the Frame and runs all of these.
RT      ?= ../../runtime
CC      ?= cc
SAN     ?=
BUILD   := build/$(notdir $(CC))$(if $(SAN),-$(SAN))
CFLAGS  := -O1 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -D_GNU_SOURCE -pthread -I$(RT) -I.
LDFLAGS := -pthread -rdynamic
TESTS   := test_sync test_threads test_vmem test_fiber test_headers
ifeq ($(SAN),tsan)
CFLAGS  += -fsanitize=thread
LDFLAGS += -fsanitize=thread
TESTS   := test_sync test_threads test_fiber
endif
ifeq ($(SAN),bti)
CFLAGS  += -mbranch-protection=standard -DTP_BTI
LDFLAGS += -mbranch-protection=standard
endif
LIB := $(BUILD)/plat_posix.o $(BUILD)/vmem.o $(BUILD)/fiber_aarch64.o
HDR := $(RT)/plat.h $(RT)/vmem.h $(RT)/fiber.h tp.h

all: run
build: $(addprefix $(BUILD)/,$(TESTS))
run: build
	@set -e; for t in $(TESTS); do echo "== $(BUILD)/$$t"; ./$(BUILD)/$$t; done

$(BUILD):
	mkdir -p $@
$(BUILD)/%.o: $(RT)/%.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: $(RT)/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/regcheck_aarch64.o: regcheck_aarch64.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_fiber: test_fiber.c $(BUILD)/regcheck_aarch64.o $(LIB) $(HDR)
	$(CC) $(CFLAGS) $< $(BUILD)/regcheck_aarch64.o $(LIB) $(LDFLAGS) -o $@
$(BUILD)/test_headers: test_headers.c $(LIB) $(HDR)
	$(CC) $(CFLAGS) -Werror -DWITH_RUNTIME_H -DPLAT_FIRST -c $< -o $(BUILD)/hdr_first.o
	$(CC) $(filter-out -D_GNU_SOURCE,$(CFLAGS)) -Werror -DWITH_RUNTIME_H -c $< -o $(BUILD)/hdr_last.o
	$(CC) $(BUILD)/hdr_first.o $(BUILD)/hdr_last.o $(LIB) $(LDFLAGS) -o $@
$(BUILD)/%: %.c $(LIB) $(HDR)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) -o $@

clean:
	rm -rf build
.PHONY: all build run clean
