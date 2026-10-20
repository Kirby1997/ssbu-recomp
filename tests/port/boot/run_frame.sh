#!/bin/sh
# Copy the boot-path sources + tests to the Steam Frame (~/ssbu-port/boot) and run them: gcc, clang, clang ASan+UBSan.
# Usage: tests/port/boot/run_frame.sh [host]   (default steamos@frame). Synthetic fixtures only; the scratch folder is
# removed when every check passes.
set -e
HOST=${1:-steamos@frame}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
tar -cf - runtime/*.h runtime/plat_posix.c runtime/plat_crash.c runtime/vmem.c runtime/hostpath.c runtime/datapaths.c runtime/nnmisc.c runtime/boot.c \
    tests/plat/tp.h tests/port/stubs tests/port/boot/Makefile tests/port/boot/*.c |
    ssh -o BatchMode=yes "$HOST" 'mkdir -p ~/ssbu-port/boot && cd ~/ssbu-port/boot && rm -rf tests/port/boot/build && tar -xf -'
# The boot cases fork children that run boot.c; a re-exec loop there once fork-bombed the device. Guard every run: at
# most 300 tasks above what the user already has, and 10 minutes per configuration.
ssh -o BatchMode=yes "$HOST" 'cd ~/ssbu-port/boot/tests/port/boot && set -e
  ulimit -u $(( $(ps -L -u $(id -u) --no-headers | wc -l) + 300 ))
  for cfg in "CC=gcc" "CC=clang" "CC=clang SAN=1"; do echo "######## make $cfg"; timeout 600 make -s $cfg 2>&1; done'
