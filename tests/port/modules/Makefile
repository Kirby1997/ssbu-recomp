# tests/port/modules - nn::ro (nnro.c + nro_shim.c modules), runtime/sha256.h, nnlibc2's SetFileSize. Native aarch64 Linux
# (Steam Frame). run_frame.sh copies the sources there and runs `make CC=gcc` and `make CC=clang`.
#   make CC=clang      build + run into build/clang
REPO    ?= ../../..
RT      := $(REPO)/runtime
CC      ?= cc
BUILD   := build/$(notdir $(CC))
GAME    := -Dfn_table='(*game_fn_table_p)' -Dfn_table_count='(*game_fn_table_count_p)' -Ddyn_syms='(*game_dyn_syms_p)' -Ddyn_sym_count='(*game_dyn_sym_count_p)'
# the executable: as build.py on linux-arm64 (position dependent, -no-pie), plus -rdynamic so that it exports the same names as
# the modules (stricter than the real exe: a module that is not bound to itself would reach these)
CFLAGS  := -O1 -g -std=gnu11 -D_GNU_SOURCE -fno-pie -ffp-contract=off -fno-math-errno -fno-omit-frame-pointer -fno-strict-aliasing -pthread -I$(RT) -I$(REPO) -I.
LDFLAGS := -no-pie -pthread -rdynamic -ldl
# a module: buildplat.codegen_flags(shared=True) + link_shared_flags(module=True)
MODC    := -O1 -std=gnu11 -ffp-contract=off -fno-math-errno -fno-omit-frame-pointer -fno-strict-aliasing -fPIC -fno-semantic-interposition -I$(RT)
MODSRC  := fake_mod.c fake_who.c $(RT)/nro_shim.c
LIB     := $(BUILD)/plat_posix.o $(BUILD)/vmem.o $(BUILD)/fiber_aarch64.o
TESTS   := test_sha256 test_libc2 test_libc2_nofs test_nnro
MODS    := $(BUILD)/nro_bin_test/fake_a.so $(BUILD)/nro_bin_test/fake_b.so $(BUILD)/extra/fake_c.so $(BUILD)/extra/fake_def.so $(BUILD)/extra/fake_int.so

all: run
build: $(addprefix $(BUILD)/,$(TESTS)) $(MODS)
run: build
	@set -e; echo "== exported dynamic symbols of a module (nm -D --defined-only $(BUILD)/nro_bin_test/fake_a.so):"; \
	  nm -D --defined-only $(BUILD)/nro_bin_test/fake_a.so | sed 's/^/     /'; \
	  n=$$(nm -D --defined-only $(BUILD)/nro_bin_test/fake_a.so | grep -vc ' nro_bind$$' || true); \
	  if [ "$$n" = 0 ]; then echo "exports          PASS (only nro_bind)"; else echo "exports          FAIL ($$n other symbols)"; exit 1; fi; \
	  r=$$(readelf -d $(BUILD)/nro_bin_test/fake_a.so | grep -c 'SYMBOLIC' || true); \
	  if [ "$$r" != 0 ]; then echo "DT_SYMBOLIC      PASS (-Bsymbolic)"; else echo "DT_SYMBOLIC      FAIL"; exit 1; fi
	@set -e; for t in test_sha256 test_libc2 test_libc2_nofs; do echo "== $(BUILD)/$$t"; ./$(BUILD)/$$t; done
	@echo "== $(BUILD)/test_nnro"; SSBU_NRO_BIN=nro_bin_test ./$(BUILD)/test_nnro $(abspath $(BUILD)/extra)

$(BUILD) $(BUILD)/nro_bin_test $(BUILD)/extra:
	mkdir -p $@
$(BUILD)/%.o: $(RT)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: $(RT)/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/test_sha256: test_sha256.c $(RT)/sha256.h mt.h | $(BUILD)
	$(CC) $(CFLAGS) -Wall -Wextra $< -no-pie -o $@
$(BUILD)/test_libc2: test_libc2.c $(RT)/nnlibc2.c $(RT)/nnlibc2_c5b.inc $(LIB) mt.h
	$(CC) $(CFLAGS) $(GAME) -DWITH_NNFS_FD $< $(LIB) $(LDFLAGS) -o $@
$(BUILD)/test_libc2_nofs: test_libc2.c $(RT)/nnlibc2.c $(RT)/nnlibc2_c5b.inc $(LIB) mt.h
	$(CC) $(CFLAGS) $(GAME) $< $(LIB) $(LDFLAGS) -o $@
$(BUILD)/test_nnro: test_nnro.c $(RT)/nnro.c $(RT)/sha256.h $(REPO)/fighter_sdk/win32_posix.h $(LIB) mt.h
	$(CC) $(CFLAGS) $(GAME) $< $(LIB) $(LDFLAGS) -o $@

# modules: the real configuration (hidden + -Bsymbolic) for fake_a / fake_b / fake_c; two controls
$(BUILD)/nro_bin_test/fake_%.so: $(MODSRC) | $(BUILD)/nro_bin_test
	$(CC) $(MODC) -fvisibility=hidden -DNRO_MODULE=fake_$* -DWHO="'$(shell echo $* | tr a-z A-Z)'" -DMOD_BASE=$(if $(filter a,$*),0x7200000000ULL,0x7200200000ULL) -shared -Wl,-Bsymbolic $(MODSRC) -o $@
$(BUILD)/extra/fake_c.so: $(MODSRC) | $(BUILD)/extra
	$(CC) $(MODC) -fvisibility=hidden -DNRO_MODULE=fake_c -DWHO="'C'" -DMOD_BASE=0x7200400000ULL -shared -Wl,-Bsymbolic $(MODSRC) -o $@
$(BUILD)/extra/fake_def.so: $(MODSRC) | $(BUILD)/extra
	$(CC) $(MODC) -DNRO_MODULE=fake_def -DWHO="'D'" -DMOD_BASE=0x7200600000ULL -shared -Wl,-Bsymbolic $(MODSRC) -o $@
$(BUILD)/extra/fake_int.so: $(MODSRC) | $(BUILD)/extra
	$(CC) $(MODC) -DNRO_MODULE=fake_int -DWHO="'I'" -DMOD_BASE=0x7200800000ULL -shared $(MODSRC) -o $@

clean:
	rm -rf build
.PHONY: all build run clean
