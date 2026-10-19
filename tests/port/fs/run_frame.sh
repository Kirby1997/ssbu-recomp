#!/bin/sh
# Copy the sources tests/port/fs needs to the Steam Frame and run `make` there with gcc, clang and clang + ASan/UBSan.
# Usage: tests/port/fs/run_frame.sh [host] [remote dir]   (default steamos@frame, ~/ssbu-port/fs)
set -e
HOST=${1:-steamos@frame}
DIR=${2:-ssbu-port/fs}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
tar -cf - runtime/*.h runtime/*.inc runtime/plat_posix.c runtime/hostpath.c runtime/nnfs.c runtime/nnfsdir.c runtime/nnpl.c \
    runtime/gfx_options.c runtime/nnlibc2.c runtime/r1_state.c tests/port/modules/mt.h tests/port/stubs tests/port/fs/Makefile tests/port/fs/*.c |
    ssh -o BatchMode=yes "$HOST" "mkdir -p $DIR && cd $DIR && rm -rf tests/port/fs/build && tar -xf -"
ssh -o BatchMode=yes "$HOST" "cd $DIR/tests/port/fs && st=0
  for cfg in 'CC=gcc' 'CC=clang' 'CC=clang SAN=1'; do echo \"######## make \$cfg\"; make -s \$cfg 2>&1 || st=1; done; exit \$st"
