# tests/port/services - nntime.c / nnmii.c / nnnet.c / nnapplet.c / nnalbum.c on native aarch64 Linux (Steam Frame). run_frame.sh copies the
# sources to the device and runs `make CC=gcc` and `make CC=clang`.
REPO    ?= ../../..
RT      := $(REPO)/runtime
CC      ?= cc
BUILD   := build/$(notdir $(CC))
GAME    := -Dfn_table='(*game_fn_table_p)' -Dfn_table_count='(*game_fn_table_count_p)' -Ddyn_syms='(*game_dyn_syms_p)' -Ddyn_sym_count='(*game_dyn_sym_count_p)'
CFLAGS  := -O1 -g -std=gnu11 -D_GNU_SOURCE -fno-pie -ffp-contract=off -fno-math-errno -fno-omit-frame-pointer -fno-strict-aliasing -pthread -I$(RT) -I$(REPO) -I. -I$(REPO)/tests/port/stubs -Wno-everything
LDFLAGS := -no-pie -pthread -rdynamic -ldl
LIB     := $(BUILD)/plat_posix.o $(BUILD)/vmem.o $(BUILD)/fiber_aarch64.o
TESTS   := test_time test_mii test_net test_applet

all: run
build: $(addprefix $(BUILD)/,$(TESTS))
run: build
	@set -e; for t in $(TESTS); do echo "== $(BUILD)/$$t"; ./$(BUILD)/$$t; done
	@for z in America/New_York Europe/London Australia/Sydney Asia/Kolkata; do echo "== TZ=$$z test_time"; TZ=$$z ./$(BUILD)/test_time > $(BUILD)/tz.log || { cat $(BUILD)/tz.log; exit 1; }; tail -3 $(BUILD)/tz.log; done
	@# slim zone files stop at the last rule change, so the POSIX footer continuation (tzrule_build_linux) is exercised only here
	@if command -v zic >/dev/null 2>&1 && [ -r /usr/share/zoneinfo/tzdata.zi ]; then \
	  rm -rf $(BUILD)/slim && zic -b slim -d $(CURDIR)/$(BUILD)/slim /usr/share/zoneinfo/tzdata.zi; \
	  echo "== slim zone files (footer continuation) test_time"; \
	  SSBU_TZDIR=$(CURDIR)/$(BUILD)/slim TZDIR=$(CURDIR)/$(BUILD)/slim ./$(BUILD)/test_time > $(BUILD)/tz.log || { cat $(BUILD)/tz.log; exit 1; }; tail -3 $(BUILD)/tz.log; \
	else echo "== slim zone files: no zic or /usr/share/zoneinfo/tzdata.zi, skipped"; fi

$(BUILD):
	mkdir -p $@
$(BUILD)/%.o: $(RT)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: $(RT)/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_time: test_time.c stubs.h $(RT)/nntime.c $(LIB)
	$(CC) $(CFLAGS) $(GAME) $< $(LIB) $(LDFLAGS) -o $@
# nnmii.c reaches the whole SDK lift layer; only host_region_code is exercised here, so unresolved symbols are left to fail lazily
$(BUILD)/test_mii: test_mii.c stubs.h $(RT)/nnmii.c $(LIB)
	$(CC) $(CFLAGS) $(GAME) $< $(LIB) $(LDFLAGS) -Wl,--unresolved-symbols=ignore-all -o $@
$(BUILD)/test_net: test_net.c stubs.h $(RT)/nnnet.c $(RT)/nnnet_c4a.inc $(LIB)
	$(CC) $(CFLAGS) $(GAME) $< $(LIB) $(LDFLAGS) -Wl,--unresolved-symbols=ignore-all -o $@
$(BUILD)/test_applet: test_applet.c stubs.h $(RT)/nnapplet.c $(LIB)
	$(CC) $(CFLAGS) $(GAME) $< $(LIB) $(LDFLAGS) -Wl,--unresolved-symbols=ignore-all -o $@
clean:
	rm -rf build
.PHONY: all build run clean
