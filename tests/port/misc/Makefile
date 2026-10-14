# tests/port/misc - nnmisc_posix.h (hashes, getrandom, locale) and the Linux diagnostics. Native aarch64 Linux (Steam Frame).
#   make CC=clang | make CC=gcc
RT    ?= ../../../runtime
PLAT  ?= ../../plat
CC    ?= cc
BUILD := build/$(notdir $(CC))
CFLAGS  := -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -std=gnu11 -D_GNU_SOURCE -pthread -I$(RT) -I$(PLAT) -I. \
           -Dfn_table='(*game_fn_table_p)' -Dfn_table_count='(*game_fn_table_count_p)'
LDFLAGS := -pthread -rdynamic
LIB := $(BUILD)/plat_posix.o $(BUILD)/vmem.o $(BUILD)/fiber_aarch64.o
all: run
$(BUILD):
	mkdir -p $@
$(BUILD)/%.o: $(RT)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_misc: test_misc.c $(RT)/nnmisc_posix.h | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@
$(BUILD)/test_diag: test_diag.c $(LIB) $(RT)/nvn_vk_wd.c | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) -o $@
$(BUILD)/%.o: $(RT)/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
run: $(BUILD)/test_misc $(BUILD)/test_diag
	./$(BUILD)/test_misc
	./$(BUILD)/test_diag
