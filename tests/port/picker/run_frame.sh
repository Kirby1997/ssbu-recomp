#!/bin/sh
# Copy the picker sources + tests to the Steam Frame (~/ssbu-port/picker) and run them:
#   test_picker (no display): gcc, clang, clang ASan+UBSan
#   test_picker_sdl: SDL's offscreen driver (gcc, clang), then on the running gamescope session's display - its Wayland socket,
#   and Xwayland :0 with the gamescope environment Steam gives a game in Game Mode - with each pictured screen held on screen
#   and photographed by the compositor (gamescopectl screenshot). The pictures are copied back to <shots> (default
#   tests/port/picker/build/frame_shots).
# Usage: tests/port/picker/run_frame.sh [host] [shots folder]     (default steamos@frame)
# Guarded like tests/port/boot: at most 300 tasks above what the user already has, a timeout on every run, no re-exec.
set -e
HOST=${1:-steamos@frame}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
SHOTS=${2:-$ROOT/tests/port/picker/build/frame_shots}
cd "$ROOT"
tar -cf - runtime/*.h runtime/plat_posix.c runtime/plat_crash.c runtime/vmem.c runtime/fiber_aarch64.S runtime/hostpath.c runtime/datapaths.c \
    runtime/datapicker.c runtime/datapicker_sdl.c tests/plat/tp.h tests/port/stubs tests/port/picker/Makefile tests/port/picker/*.c |
    ssh -o BatchMode=yes "$HOST" 'mkdir -p ~/ssbu-port/picker && cd ~/ssbu-port/picker && rm -rf tests/port/picker/build && tar -xf -'
ssh -o BatchMode=yes "$HOST" 'cd ~/ssbu-port/picker/tests/port/picker && set -e
  ulimit -u $(( $(ps -L -u $(id -u) --no-headers | wc -l) + 300 ))
  export SSBU_VMEM_NO_REEXEC=1
  for cfg in "CC=gcc" "CC=clang" "CC=clang SAN=1"; do echo "######## make $cfg"; timeout 600 make -s $cfg 2>&1 | grep -v "^  - "; done
  for cc in gcc clang; do timeout 600 make -s CC=$cc sdl; done
  S=$PWD/build/shots; rm -rf $S; mkdir -p $S/offscreen $S/wayland $S/xwayland
  run() { echo "######## $*"; env "$@" 2>&1 | grep -v "^  - " || true; }
  run SDL_VIDEODRIVER=offscreen timeout -s KILL 120 build/gcc/test_picker_sdl build/gcc/sdl $S/offscreen
  run SDL_VIDEODRIVER=offscreen timeout -s KILL 120 build/clang/test_picker_sdl build/clang/sdl $S/offscreen
  export XDG_RUNTIME_DIR=/run/user/$(id -u)
  WL=$(cd $XDG_RUNTIME_DIR && for s in gamescope-[0-9]*; do case $s in *-ei|*.lock) ;; *) [ -S $s ] && echo $s && break;; esac; done)
  X=:0; [ -S /tmp/.X11-unix/X0 ] || X=
  echo "######## display: WAYLAND_DISPLAY=${WL:-none} DISPLAY=${X:-none}"
  SHOT="env XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR WAYLAND_DISPLAY=${WL:-gamescope-0} gamescopectl screenshot"
  if [ -n "$WL" ]; then
    run WAYLAND_DISPLAY=$WL SDL_VIDEODRIVER=wayland PICKER_SHOT_CMD="$SHOT" timeout -s KILL 180 build/clang/test_picker_sdl build/clang/sdl $S/wayland 1500
  fi
  if [ -n "$X" ]; then      # as Steam starts a game in Game Mode: Xwayland, gamescope environment
    run DISPLAY=$X GAMESCOPE_WAYLAND_DISPLAY=${WL:-gamescope-0} XDG_CURRENT_DESKTOP=gamescope SDL_VIDEODRIVER=x11 PICKER_SHOT_CMD="$SHOT" \
        timeout -s KILL 180 build/gcc/test_picker_sdl build/gcc/sdl $S/xwayland 1500
  fi
  echo "######## left running: $(pgrep -c test_picker || true)"
  ls -R $S | head -60'
mkdir -p "$SHOTS"
ssh -o BatchMode=yes "$HOST" 'cd ~/ssbu-port/picker/tests/port/picker/build/shots && tar -cf - .' | tar -xf - -C "$SHOTS"
echo "pictures copied to $SHOTS"
