#!/bin/sh
# Copy plat sources + tests to the Steam Frame (~/ssbu-port/plat) and run every configuration there:
# gcc, clang, clang+TSan, gcc+TSan, clang+BTI/PAC, gcc+BTI/PAC. Usage: tests/plat/run_frame.sh [host]   (default steamos@frame)
set -e
HOST=${1:-steamos@frame}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
tar -cf - runtime/plat.h runtime/plat_posix.c runtime/vmem.h runtime/vmem.c runtime/fiber.h runtime/fiber_aarch64.S \
    runtime/runtime.h runtime/fpexact.h runtime/neon.h \
    tests/plat/Makefile tests/plat/tp.h tests/plat/*.c tests/plat/*.S |
    ssh -o BatchMode=yes "$HOST" 'mkdir -p ~/ssbu-port/plat && cd ~/ssbu-port/plat && rm -rf tests/plat/build && tar -xf -'
ssh -o BatchMode=yes "$HOST" 'cd ~/ssbu-port/plat/tests/plat && set -e
  for cfg in "CC=gcc" "CC=clang" "CC=clang SAN=tsan" "CC=gcc SAN=tsan" "CC=clang SAN=bti" "CC=gcc SAN=bti"; do
    echo "######## make $cfg"; make -s $cfg 2>&1
  done
  echo "######## GNU property of the BTI build objects:"; for o in plat_posix vmem fiber_aarch64; do printf "%s: " $o; readelf -n build/clang-bti/$o.o | grep -i "aarch64 feature"; done'
