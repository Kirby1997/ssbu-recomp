# tests/port/threads - runtime/os_threads.c (+ plat_heap.c), plat's fiber-local storage and nncxxrt.c's per-fiber exception
# state (test_fls, test_cxxrt) on aarch64 Linux (Steam Frame). Built and run on the device.
#   make CC=clang OPT=-O2            build + run into build/clang-O2 (also -O1: the fiber TLS hazard shows at -O2)
#   make CC=gcc SAN=tsan             ThreadSanitizer build of test_os_threads, test_fls, test_cxxrt (the heap test is skipped: TSan's allocator)
#   make CC=clang SAN=lsan           LeakSanitizer build of test_fls and test_cxxrt (lsan.supp: os_threads.c's known leaks;
#                                    ASan itself cannot run here: its shadow memory covers vmem.c's fixed guest window)
# run_frame.sh copies the files to the Frame and runs every configuration.
RT      ?= ../../../runtime
TP      ?= ../../plat
CC      ?= cc
OPT     ?= -O2
SAN     ?=
BUILD   := build/$(notdir $(CC))$(OPT)$(if $(SAN),-$(SAN))
CFLAGS  := $(OPT) -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable -Wno-missing-field-initializers \
           -std=gnu11 -D_GNU_SOURCE -pthread -fno-pie -fno-omit-frame-pointer -I$(RT) -I$(RT)/.. -I$(TP) -I.
LDFLAGS := -pthread -no-pie
TESTS   := test_os_threads test_fls test_cxxrt test_heap test_heap_nofix
ifeq ($(SAN),tsan)
CFLAGS  += -fsanitize=thread
LDFLAGS += -fsanitize=thread
TESTS   := test_os_threads test_fls test_cxxrt
endif
ifeq ($(SAN),lsan)
CFLAGS  += -fsanitize=leak
LDFLAGS += -fsanitize=leak
TESTS   := test_fls test_cxxrt
endif
# test_cxxrt: nncxxrt.c is #included by the test; the C++ runtime objects see the game tables through pointers (check_host.sh)
GAME    := -Dfn_table='(*game_fn_table_p)' -Dfn_table_count='(*game_fn_table_count_p)' -Ddyn_syms='(*game_dyn_syms_p)' \
           -Ddyn_sym_count='(*game_dyn_sym_count_p)' -I../stubs
CXXRT   := $(BUILD)/nncxxlib.o $(BUILD)/nnlibc2.o $(BUILD)/clib.o $(BUILD)/stubs_cxxrt.o
LIB := $(BUILD)/plat_posix.o $(BUILD)/vmem.o $(BUILD)/fiber_aarch64.o $(BUILD)/plat_crash.o $(BUILD)/os_threads.o $(BUILD)/stubs.o
HDR := $(RT)/plat.h $(RT)/vmem.h $(RT)/fiber.h $(RT)/plat_crash.h $(RT)/runtime.h $(RT)/hostrt.h $(TP)/tp.h

all: run
build: $(addprefix $(BUILD)/,$(TESTS))
# lsan.supp names functions: LSan needs a symbolizer (the Frame has addr2line, not llvm-symbolizer)
SYMBOLIZER := $(firstword $(shell command -v llvm-symbolizer addr2line 2>/dev/null))
run: build
	@set -e; for t in $(TESTS); do echo "== $(BUILD)/$$t"; TSAN_OPTIONS="suppressions=$(CURDIR)/tsan.supp $$TSAN_OPTIONS" LSAN_OPTIONS="suppressions=$(CURDIR)/lsan.supp print_suppressions=1 $(if $(SYMBOLIZER),external_symbolizer_path=$(SYMBOLIZER)) $$LSAN_OPTIONS" ./$(BUILD)/$$t; done

$(BUILD):
	mkdir -p $@
$(BUILD)/%.o: $(RT)/%.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: $(RT)/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/stubs.o: stubs.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/nncxxlib.o $(BUILD)/nnlibc2.o $(BUILD)/clib.o: $(BUILD)/%.o: $(RT)/%.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(GAME) -w -c $< -o $@
$(BUILD)/stubs_cxxrt.o: stubs_cxxrt.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(GAME) -c $< -o $@
$(BUILD)/test_cxxrt: test_cxxrt.c $(RT)/nncxxrt.c $(RT)/cxxrt_selftest.h $(CXXRT) $(LIB) $(HDR)
	$(CC) $(CFLAGS) $(GAME) -Wno-unused-function $(if $(findstring clang,$(CC)),-Wno-string-plus-int) $< $(CXXRT) $(LIB) $(LDFLAGS) -lm -o $@
$(BUILD)/test_heap: test_heap.c $(BUILD)/plat_heap.o $(LIB) $(HDR)
	$(CC) $(CFLAGS) $< $(BUILD)/plat_heap.o $(LIB) $(LDFLAGS) -o $@
$(BUILD)/test_heap_nofix: test_heap.c $(LIB) $(HDR)
	$(CC) $(CFLAGS) -DNOFIX $< $(LIB) $(LDFLAGS) -o $@
$(BUILD)/%: %.c $(LIB) $(HDR)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) -o $@

clean:
	rm -rf build
.PHONY: all build run clean
