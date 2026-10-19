#!/bin/sh
# Copy the sources tests/port/io needs to the Steam Frame (~/ssbu-port/io) and run `make` there with gcc and clang.
# Usage: tests/port/io/run_frame.sh [host]   (default steamos@frame)
# Fonts: the Makefile's defaults (system TrueType, CFF and .ttc files); FONT= / FONT_CFF= / FONT_TTC= in the environment
# override them. Audio: the user's PipeWire session (XDG_RUNTIME_DIR=/run/user/<uid> when ssh did not set it); with no
# PipeWire / PulseAudio socket there, the device run is skipped and the run says so (SDL's dummy driver does not consume
# in real time, so it cannot stand in for the device).
set -e
HOST=${1:-steamos@frame}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
tar -cf - runtime/*.h runtime/plat_posix.c runtime/vmem.c runtime/fiber_aarch64.S \
    runtime/nnaudio.c runtime/nnfontll.c runtime/nnhid.c runtime/nnpad_sdl.c runtime/r1_state.c \
    tests/port/stubs tests/port/io/Makefile tests/port/io/*.c |
    ssh -o BatchMode=yes "$HOST" 'mkdir -p ~/ssbu-port/io && cd ~/ssbu-port/io && rm -rf tests/port/io/build && tar -xf -'
ssh -o BatchMode=yes "$HOST" "export FONT='${FONT:-}' FONT_CFF='${FONT_CFF:-}' FONT_TTC='${FONT_TTC:-}';"' cd ~/ssbu-port/io/tests/port/io && st=0
  [ -n "$FONT" ] || unset FONT; [ -n "$FONT_CFF" ] || unset FONT_CFF; [ -n "$FONT_TTC" ] || unset FONT_TTC
  export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
  if [ ! -S "$XDG_RUNTIME_DIR/pipewire-0" ] && [ ! -S "$XDG_RUNTIME_DIR/pulse/native" ]; then
    echo "######## no PipeWire / PulseAudio socket in $XDG_RUNTIME_DIR: the audio device run is SKIPPED (only the clock-driven run)"; export AUDIO_DEVICE=0
  fi
  for cfg in "CC=gcc" "CC=clang"; do echo "######## make $cfg"; make -s $cfg 2>&1 || st=1; done
  exit $st'
