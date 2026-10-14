#!/bin/sh
# Copy the sources tests/port/modules needs to the Steam Frame (~/ssbu-port/modules) and run `make` there with gcc and clang.
# Usage: tests/port/modules/run_frame.sh [host]   (default steamos@frame)
set -e
HOST=${1:-steamos@frame}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
tar -cf - runtime/*.h runtime/plat_posix.c runtime/vmem.c runtime/fiber_aarch64.S runtime/nnro.c runtime/nro_shim.c \
    runtime/nnlibc2.c runtime/nnlibc2_c5b.inc fighter_sdk/*_host.c fighter_sdk/win32_posix.h fighter_sdk/include \
    tests/port/modules/Makefile tests/port/modules/*.c tests/port/modules/*.h |
    ssh -o BatchMode=yes "$HOST" 'mkdir -p ~/ssbu-port/modules && cd ~/ssbu-port/modules && rm -rf tests/port/modules/build && tar -xf -'
ssh -o BatchMode=yes "$HOST" 'cd ~/ssbu-port/modules/tests/port/modules && st=0
  for cfg in "CC=gcc" "CC=clang"; do echo "######## make $cfg"; make -s $cfg 2>&1 || st=1; done; exit $st'
