# tests/port/io - runtime/nnaudio.c (SDL3 AudioOut + the dlopen'ed libopus), runtime/nnfontll.c (the FreeType outline /
# glyph-map path) and runtime/nnhid.c + nnpad_sdl.c (SDL3 pads) on native aarch64 Linux (Steam Frame). run_frame.sh copies
# the sources to the device and runs `make CC=gcc` and `make CC=clang`. test_audio.c, test_font.c and test_pad.c #include
# the runtime file they test (nnaudio.c, nnfontll.c, nnhid.c); stubs.c supplies the few host functions they call.
#   make CC=gcc|clang [FONT=<ttf> FONT_CFF=<otf> FONT_TTC=<ttc>]   (a font that is not there is skipped and said so)
REPO    ?= ../../..
RT      := $(REPO)/runtime
SDL     ?= $(HOME)/ssbu-port/deps/sdl3
FT      ?= /usr/include/freetype2
CC      ?= cc
BUILD   := build/$(notdir $(CC))
GAME    := -Dfn_table='(*game_fn_table_p)' -Dfn_table_count='(*game_fn_table_count_p)' -Ddyn_syms='(*game_dyn_syms_p)' -Ddyn_sym_count='(*game_dyn_sym_count_p)'
CFLAGS  := -O1 -g -std=gnu11 -D_GNU_SOURCE -fno-pie -ffp-contract=off -fno-math-errno -fno-omit-frame-pointer -fno-strict-aliasing -pthread -I$(RT) -I$(REPO) -I. -I$(REPO)/tests/port/stubs -I$(SDL)/include -I$(FT) -Wno-everything
LDFLAGS := -no-pie -pthread -rdynamic -ldl -lm
SDLLIB  := -L$(SDL)/lib -lSDL3 -Wl,-rpath,$(SDL)/lib
# r1_state.c: its Linux section (R1 off: the r1_* hooks nnhid.c / nnaudio.c call), as linked in the game
LIB     := $(BUILD)/plat_posix.o $(BUILD)/vmem.o $(BUILD)/fiber_aarch64.o $(BUILD)/r1_state.o $(BUILD)/stubs.o
TESTS   := test_audio test_font test_pad
# TrueType (quadratic: the conic -> cubic raise), CFF (FreeType's own cubics) and a collection (face index 2)
FONT     ?= $(firstword $(wildcard /usr/share/fonts/TTF/Hack-Regular.ttf /usr/share/fonts/TTF/DejaVuSans.ttf /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf))
FONT_CFF ?= $(firstword $(wildcard /usr/share/fonts/adobe-source-code-pro/SourceCodePro-Regular.otf))
FONT_TTC ?= $(firstword $(wildcard /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc))
# the audio device: PipeWire / PulseAudio need the user's runtime dir (an ssh session may not have it). AUDIO_DEVICE=0 skips
# the device run (SDL's dummy driver is no substitute: it does not consume in real time, so the clock checks fail)
AUDIO_DEVICE ?= 1
XDG_RUNTIME_DIR ?= /run/user/$(shell id -u)
export XDG_RUNTIME_DIR

all: run
build: $(addprefix $(BUILD)/,$(TESTS))
run: build
	@set -e; if [ "$(AUDIO_DEVICE)" = 0 ]; then echo "== test_audio with the SDL device: skipped (AUDIO_DEVICE=0)"; \
	  else echo "== $(BUILD)/test_audio (SDL device)"; ./$(BUILD)/test_audio; fi
	@set -e; echo "== $(BUILD)/test_audio SSBU_AUDIO_MUTE=1 (no device, clock-driven)"; SSBU_AUDIO_MUTE=1 ./$(BUILD)/test_audio
	@set -e; for f in "$(FONT) 0" "$(FONT_CFF) 0" "$(FONT_TTC) 2"; do set -- $$f; \
	  if [ $$# -lt 2 ]; then echo "== test_font: a font file (FONT / FONT_CFF / FONT_TTC) is missing, skipped"; continue; fi; \
	  echo "== $(BUILD)/test_font $$1 face $$2"; ./$(BUILD)/test_font "$$1" $$2 32; done
	@set -e; echo "== $(BUILD)/test_pad pad (SDL virtual joysticks)"; ./$(BUILD)/test_pad pad
	@set -e; echo "== $(BUILD)/test_pad hid desktop"; env -u GAMESCOPE_WAYLAND_DISPLAY -u WAYLAND_DISPLAY XDG_CURRENT_DESKTOP=KDE ./$(BUILD)/test_pad hid desktop
	@set -e; echo "== $(BUILD)/test_pad hid gamescope"; env -u WAYLAND_DISPLAY -u XDG_CURRENT_DESKTOP GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 ./$(BUILD)/test_pad hid gamescope

$(BUILD):
	mkdir -p $@
$(BUILD)/%.o: $(RT)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: $(RT)/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/stubs.o: stubs.c $(RT)/plat.h $(RT)/runtime.h $(RT)/hostrt.h | $(BUILD)
	$(CC) $(CFLAGS) $(GAME) -c stubs.c -o $@
$(BUILD)/nnpad_sdl.o: $(RT)/nnpad.h
# test_audio.c #includes nnaudio.c: the SDL3 stream is the output device, libopus.so.0 is dlopen'ed at run time
$(BUILD)/test_audio: test_audio.c $(RT)/nnaudio.c $(RT)/plat_crash.h $(LIB)
	$(CC) $(CFLAGS) $(GAME) test_audio.c $(LIB) $(LDFLAGS) $(SDLLIB) -o $@
# test_font.c #includes nnfontll.c and links FreeType itself, which the test compares the outlines against
$(BUILD)/test_font: test_font.c $(RT)/nnfontll.c $(LIB)
	$(CC) $(CFLAGS) $(GAME) test_font.c $(LIB) $(LDFLAGS) -lfreetype -o $@
# test_pad.c #includes nnhid.c; nnpad_sdl.c is linked as a separate object, as in the game
$(BUILD)/test_pad: test_pad.c $(RT)/nnhid.c $(RT)/nnpad.h $(BUILD)/nnpad_sdl.o $(LIB)
	$(CC) $(CFLAGS) $(GAME) test_pad.c $(BUILD)/nnpad_sdl.o $(LIB) $(LDFLAGS) $(SDLLIB) -o $@
clean:
	rm -rf build
.PHONY: all build run clean
