# tests/port/window - display.c (SDL3 window, pacing) + the top of nvn_vk.c (instance/device/surface/swapchain/present) on the
# Steam Frame. nvn_vk.c is compiled from a copy in the build folder next to the stand-in nvn_vk_draw.c of this folder.
#   make CC=clang | make CC=gcc        (SDL3 from $(SDL); Vulkan headers and libvulkan.so.1 from the system)
RT    ?= ../../../runtime
SDL   ?= $(HOME)/ssbu-port/deps/sdl3
CC    ?= cc
BUILD := build/$(notdir $(CC))
CFLAGS  := -O1 -g -Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -Wno-switch -std=gnu11 -D_GNU_SOURCE -pthread -fno-omit-frame-pointer \
           -I$(RT) -I$(SDL)/include -Dfn_table='(*game_fn_table_p)' -Dfn_table_count='(*game_fn_table_count_p)'
LDFLAGS := -pthread -rdynamic -L$(SDL)/lib -lSDL3 -Wl,-rpath,$(SDL)/lib -ldl -lm
OBJS := $(BUILD)/display.o $(BUILD)/nvn_vk.o $(BUILD)/stubs.o $(BUILD)/plat_posix.o $(BUILD)/vmem.o $(BUILD)/plat_crash.o $(BUILD)/fiber_aarch64.o
all: $(BUILD)/test_window
$(BUILD):
	mkdir -p $@
$(BUILD)/nvn_vk.c: $(RT)/nvn_vk.c nvn_vk_draw.c | $(BUILD)
	cp $(RT)/nvn_vk.c $@ && cp nvn_vk_draw.c $(BUILD)/nvn_vk_draw.c
$(BUILD)/nvn_vk.o: $(BUILD)/nvn_vk.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/stubs.o: stubs.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: $(RT)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: $(RT)/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/test_window: test_window.c $(OBJS)
	$(CC) $(CFLAGS) $< $(OBJS) $(LDFLAGS) -o $@
