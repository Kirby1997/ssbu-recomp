#!/bin/sh
# Copy the misc sources + tests to the Steam Frame (~/ssbu-port/misc) and run them with gcc and clang.
# Usage: tests/port/misc/run_frame.sh [host]   (default steamos@frame)
set -e
HOST=${1:-steamos@frame}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
tar -cf - runtime/plat.h runtime/plat_posix.c runtime/vmem.h runtime/vmem.c runtime/fiber.h runtime/fiber_aarch64.S runtime/runtime.h runtime/hostrt.h runtime/fpexact.h runtime/neon.h \
    runtime/nnmisc_posix.h runtime/nvn_vk_wd.c runtime/nvn_vk_posix.h tests/plat/tp.h tests/port/misc/Makefile tests/port/misc/*.c |
    ssh -o BatchMode=yes "$HOST" 'mkdir -p ~/ssbu-port/misc && cd ~/ssbu-port/misc && rm -rf tests/port/misc/build && tar -xf -'
ssh -o BatchMode=yes "$HOST" 'cd ~/ssbu-port/misc/tests/port/misc && set -e
  for cc in gcc clang; do echo "######## make CC=$cc"; make -s CC=$cc 2>&1; done'
