# tests/port/fs - the file services on native aarch64 Linux (Steam Frame): hostpath.c, nnfs.c, nnfsdir.c, nnpl.c, gfx_options.c and
# nnlibc2.c's SetFileSize, compiled from runtime/ and driven through their import tables (test_fs.c). run_frame.sh copies the sources
# to the device and runs `make CC=gcc`, `make CC=clang` and `make CC=clang SAN=1` (ASan + UBSan).
# The scratch folder is made under $(SCRATCH) (default: the build folder, i.e. the same disk as the tree, so fsync is real) and
# removed when every check passes.
REPO    ?= ../../..
RT      := $(REPO)/runtime
CC      ?= cc
SAN     ?=
BUILD   := build/$(notdir $(CC))$(if $(SAN),-san)
SCRATCH ?= $(CURDIR)/$(BUILD)
COMMA   := ,
SANFL   := $(if $(SAN),-fsanitize=address$(COMMA)undefined -fno-sanitize-recover=undefined)
CFLAGS  := -O1 -g -std=gnu11 -D_GNU_SOURCE -fno-pie -ffp-contract=off -fno-math-errno -fno-omit-frame-pointer -fno-strict-aliasing -pthread \
           -I$(RT) -I$(REPO) -I$(REPO)/tests/port/stubs -I. -Wno-everything $(SANFL)
LDFLAGS := -no-pie -pthread -rdynamic -ldl $(SANFL)
# r1_state: its Linux section (R1 off), for nnfs.c read_at's r1_wrote, as linked in the game
UNITS   := plat_posix hostpath nnfs nnfsdir nnpl gfx_options r1_state
OBJS    := $(addprefix $(BUILD)/,$(addsuffix .o,$(UNITS)) libc2_tu.o)

all: run
build: $(BUILD)/test_fs
run: build
	@echo "== $(BUILD)/test_fs"; ./$(BUILD)/test_fs $(SCRATCH)

$(BUILD):
	mkdir -p $@
$(BUILD)/%.o: $(RT)/%.c $(wildcard $(RT)/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/libc2_tu.o: libc2_tu.c $(RT)/nnlibc2.c $(RT)/nnlibc2_c5b.inc | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# nnlibc2.c and nnpl.c reach the SDK lift layer and the trace file; only the paths used here run, the rest is left unresolved
$(BUILD)/test_fs: test_fs.c $(OBJS)
	$(CC) $(CFLAGS) $< $(OBJS) $(LDFLAGS) -Wl,--unresolved-symbols=ignore-all -o $@
clean:
	rm -rf build
.PHONY: all build run clean
