#!/bin/sh
# Copy the sources tests/port/services needs to the Steam Frame (~/ssbu-port/services) and run `make` there with gcc and clang.
# Usage: tests/port/services/run_frame.sh [host]   (default steamos@frame)
set -e
HOST=${1:-steamos@frame}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
tar -cf - runtime/*.h runtime/*.inc runtime/plat_posix.c runtime/vmem.c runtime/fiber_aarch64.S runtime/nntime.c runtime/nnmii.c runtime/nnnet.c runtime/nnapplet.c runtime/nnalbum.c \
    tests/port/modules/mt.h tests/port/stubs tests/port/services/Makefile tests/port/services/*.c tests/port/services/*.h |
    ssh -o BatchMode=yes "$HOST" 'mkdir -p ~/ssbu-port/services && cd ~/ssbu-port/services && rm -rf tests/port/services/build && tar -xf -'
ssh -o BatchMode=yes "$HOST" 'cd ~/ssbu-port/services/tests/port/services && st=0
  for cfg in "CC=gcc" "CC=clang"; do echo "######## make $cfg"; make -s $cfg 2>&1 || st=1; done; exit $st'
