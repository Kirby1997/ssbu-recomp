# tests/port/boot - the player's game data on Linux (WP-U5a): runtime/datapaths.c (resolver order, config file, validation),
# boot.c's Linux launch_defaults / check_romfs / default main.elf (boot.c compiled with -Dmain=ssbu_boot_main) and nnmisc.c's
# add-on content list, on synthetic fixtures only. Native aarch64 Linux (Steam Frame): run_frame.sh runs
# `make CC=gcc`, `make CC=clang` and `make CC=clang SAN=1` (ASan + UBSan).
#   make CC=clang [SDL3_INC=<SDL3 include folder>]      (nnmisc.c includes SDL3/SDL.h; nothing of SDL is called)
REPO    ?= ../../..
RT      := $(REPO)/runtime
CC      ?= cc
SAN     ?=
SDL3_INC ?= $(HOME)/ssbu-port/deps/sdl3/include
BUILD   := build/$(notdir $(CC))$(if $(SAN),-san)
SCRATCH ?= $(CURDIR)/$(BUILD)
COMMA   := ,
SANFL   := $(if $(SAN),-fsanitize=address$(COMMA)undefined -fno-sanitize-recover=undefined)
WARN    := -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare -Wno-missing-field-initializers
CFLAGS  := -O1 -g -std=gnu11 -D_GNU_SOURCE -fno-pie -ffp-contract=off -fno-math-errno -fno-omit-frame-pointer -fno-strict-aliasing -pthread \
           -I$(RT) -I$(REPO) -I$(REPO)/tests/port/stubs -I$(REPO)/tests/plat -I$(SDL3_INC) -I. $(SANFL) \
           -Dfn_table='(*game_fn_table_p)' -Dfn_table_count='(*game_fn_table_count_p)'
LDFLAGS := -no-pie -pthread -rdynamic -ldl -lm -Wl,-z,lazy $(SANFL)
OBJS    := $(addprefix $(BUILD)/,plat_posix.o plat_crash.o vmem.o hostpath.o datapaths.o nnmisc.o boot.o)

all: run
build: $(BUILD)/test_boot
run: build
	@echo "== $(BUILD)/test_boot"; ./$(BUILD)/test_boot $(SCRATCH)

$(BUILD):
	mkdir -p $@
# the files of this work package are held to -Wall -Wextra; the rest is compiled as the game build does
$(BUILD)/datapaths.o: $(RT)/datapaths.c $(RT)/datapaths.h $(RT)/hostpath.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -Werror -c $< -o $@
$(BUILD)/boot.o: $(RT)/boot.c $(RT)/datapaths.h | $(BUILD)
	$(CC) $(CFLAGS) -w -Dmain=ssbu_boot_main -c $< -o $@
$(BUILD)/%.o: $(RT)/%.c $(wildcard $(RT)/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -w -c $< -o $@
# boot.c and nnmisc.c reach the whole runtime; only the paths run here are linked for real, the rest stays unresolved
$(BUILD)/test_boot: test_boot.c $(OBJS)
	$(CC) $(CFLAGS) $(WARN) $< $(OBJS) $(LDFLAGS) -Wl,--unresolved-symbols=ignore-all -o $@
clean:
	rm -rf build
.PHONY: all build run clean
