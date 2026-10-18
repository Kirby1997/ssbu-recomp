#!/bin/sh
# Copy display.c / nvn_vk.c + this test to the Steam Frame (~/ssbu-port/window), build with gcc and clang, and run the window
# test under the running gamescope session (Wayland socket gamescope-N and Xwayland :0 of /run/user/<uid>), then SDL offscreen.
# Usage: tests/port/window/run_frame.sh [host]   (default steamos@frame). Nothing is left running (each run: timeout -s KILL).
set -e
HOST=${1:-steamos@frame}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
tar -cf - runtime/plat.h runtime/plat_posix.c runtime/vmem.h runtime/vmem.c runtime/fiber.h runtime/fiber_aarch64.S runtime/plat_crash.h runtime/plat_crash.c \
    runtime/runtime.h runtime/hostrt.h runtime/fpexact.h runtime/neon.h runtime/gfx_options.h runtime/nvn_front.h runtime/mx_translate.h \
    runtime/display.c runtime/display_sdl.h runtime/nvn_vk.c tests/port/window/Makefile tests/port/window/*.c |
    ssh -o BatchMode=yes "$HOST" 'mkdir -p ~/ssbu-port/window && cd ~/ssbu-port/window && rm -rf tests/port/window/build && tar -xf -'
ssh -o BatchMode=yes "$HOST" 'cd ~/ssbu-port/window/tests/port/window && set -e
  for cc in gcc clang; do echo "######## make CC=$cc"; make -s CC=$cc 2>&1; done
  export XDG_RUNTIME_DIR=/run/user/$(id -u)
  WL=$(cd $XDG_RUNTIME_DIR && for s in gamescope-[0-9]*; do case $s in *-ei|*.lock) ;; *) [ -S $s ] && echo $s && break;; esac; done)
  X=:0; [ -S /tmp/.X11-unix/X0 ] || X=
  echo "######## display: XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR WAYLAND_DISPLAY=${WL:-none} DISPLAY=${X:-none}"
  T="timeout -s KILL 40"
  run() { echo "######## $*"; env "$@" 2>&1 | grep -v "^  ok" || true; }
  B=build/clang/test_window
  if [ -n "$WL" ]; then
    run WAYLAND_DISPLAY=$WL SDL_VIDEODRIVER=wayland $T $B frames
    run WAYLAND_DISPLAY=$WL SDL_VIDEODRIVER=wayland $T build/gcc/test_window close
    run WAYLAND_DISPLAY=$WL SDL_VIDEODRIVER=wayland SSBU_VK_PRESENT=fifo $T $B frames
    run WAYLAND_DISPLAY=$WL SDL_VIDEODRIVER=wayland SSBU_SHOW=0 $T $B frames
    run WAYLAND_DISPLAY=$WL SDL_VIDEODRIVER=wayland SSBU_VK_SELFTEST=120 $T $B frames
  fi
  if [ -n "$X" ]; then      # as Steam starts a game in game mode: Xwayland, gamescope WSI layer on
    S="DISPLAY=$X GAMESCOPE_WAYLAND_DISPLAY=${WL:-gamescope-0} XDG_CURRENT_DESKTOP=gamescope ENABLE_GAMESCOPE_WSI=1 SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS=0"
    run $S $T $B sigterm
    run $S SSBU_VK_PRESENT=fifo $T $B frames
    run $S ENABLE_GAMESCOPE_WSI=0 $T $B frames
    [ -n "$WL" ] && run $S WAYLAND_DISPLAY=$WL $T $B frames      # both sockets: SDL chooses
  fi
  run SDL_VIDEODRIVER=offscreen $T $B frames
  echo "######## left running: $(pgrep -c test_window || true)"'
