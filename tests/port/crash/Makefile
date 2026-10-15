# tests/port/crash - plat_crash.c (signal crash reports, frame-pointer walks, A64 trivial-handler check), the readable
# check behind nncxxrt.c, and the setjmp/longjmp patterns of nnunwind.c / sdklift_rt.c. Native aarch64 Linux.
#   make CC=clang              build + run into build/clang
#   make CC=gcc                same with gcc
#   make CC=clang CFG=fortify  -O2 -D_FORTIFY_SOURCE=2 (longjmp -> __longjmp_chk)
#   make CC=clang CFG=pac      -mbranch-protection=standard (return addresses in frame records are PAC-signed)
# run_frame.sh copies the files to the Frame and runs every configuration.
RT      ?= ../../../runtime
PLAT    ?= ../../plat
CC      ?= cc
CFG     ?=
BUILD   := build/$(notdir $(CC))$(if $(CFG),-$(CFG))
OPT     := -O1
ifeq ($(CFG),fortify)
OPT     := -O2 -D_FORTIFY_SOURCE=2
endif
CFLAGS  := $(OPT) -g -Wall -Wextra -Wno-unused-parameter -Wno-infinite-recursion -std=gnu11 -D_GNU_SOURCE -pthread -fno-omit-frame-pointer -I$(RT) -I$(PLAT)
ifeq ($(CFG),pac)
CFLAGS  += -mbranch-protection=standard
endif
LDFLAGS := -pthread -rdynamic
TESTS   := test_a64 test_readable test_setjmp test_dlbind test_crash
LIB := $(BUILD)/plat_posix.o $(BUILD)/vmem.o $(BUILD)/fiber_aarch64.o $(BUILD)/plat_crash.o
HDR := $(RT)/plat.h $(RT)/vmem.h $(RT)/fiber.h $(RT)/plat_crash.h $(PLAT)/tp.h

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
$(BUILD)/a64_funcs.o: a64_funcs.c | $(BUILD)
	$(CC) $(CFLAGS) -O1 -c $< -o $@
$(BUILD)/test_a64: test_a64.c $(BUILD)/a64_funcs.o $(LIB) $(HDR)
	$(CC) $(CFLAGS) $< $(BUILD)/a64_funcs.o $(LIB) $(LDFLAGS) -o $@
$(BUILD)/libdlbind.so: dlbind_lib.c | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -shared $< -o $@
$(BUILD)/test_dlbind: test_dlbind.c $(BUILD)/libdlbind.so $(LIB) $(HDR)
	$(CC) $(CFLAGS) $< $(LIB) -L$(BUILD) -ldlbind -Wl,-rpath,'$$ORIGIN' $(LDFLAGS) -ldl -o $@
$(BUILD)/%: %.c $(LIB) $(HDR)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) -o $@

clean:
	rm -rf build
.PHONY: all build run clean
