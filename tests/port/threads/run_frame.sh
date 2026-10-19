#!/bin/sh
# Copy the sources + tests to the Steam Frame (~/ssbu-port/threads) and run every configuration there:
# gcc/clang at -O1 and -O2, clang+TSan and gcc+TSan at -O2, clang+LSan and gcc+LSan (test_fls, test_cxxrt) at -O2.
# Usage: tests/port/threads/run_frame.sh [host]   (default steamos@frame)
set -e
HOST=${1:-steamos@frame}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
tar -cf - runtime/plat.h runtime/plat_posix.c runtime/vmem.h runtime/vmem.c runtime/fiber.h runtime/fiber_aarch64.S \
    runtime/runtime.h runtime/hostrt.h runtime/fpexact.h runtime/neon.h runtime/os_threads.c runtime/plat_heap.c runtime/plat_crash.h runtime/plat_crash.c \
    runtime/*.h runtime/*.inc runtime/nncxxrt.c runtime/nncxxlib.c runtime/nnlibc2.c runtime/clib.c fighter_sdk/win32_posix.h \
    tests/port/stubs/quad_tests.h tests/port/stubs/eh_tests.h \
    tests/plat/tp.h tests/port/threads/Makefile tests/port/threads/*.c tests/port/threads/tsan.supp tests/port/threads/lsan.supp |
    ssh -o BatchMode=yes "$HOST" 'mkdir -p ~/ssbu-port/threads && cd ~/ssbu-port/threads && rm -rf tests/port/threads/build && tar -xf -'
ssh -o BatchMode=yes "$HOST" 'cd ~/ssbu-port/threads/tests/port/threads && set -e
  for cfg in "CC=gcc OPT=-O1" "CC=gcc OPT=-O2" "CC=clang OPT=-O1" "CC=clang OPT=-O2" "CC=clang OPT=-O2 SAN=tsan" "CC=gcc OPT=-O2 SAN=tsan" \
             "CC=clang OPT=-O2 SAN=lsan" "CC=gcc OPT=-O2 SAN=lsan"; do
    echo "######## make $cfg"; make -s $cfg 2>&1
  done
  echo "######## fiber_main / fib_switch at -O2: TLS reached through the accessors only (no tpidr_el0 read in them)"
  for cc in gcc clang; do
    for f in fiber_main fib_switch; do
      d=$(objdump -dr --no-show-raw-insn --disassemble=$f build/$cc-O2/os_threads.o)
      n=$(echo "$d" | grep -c "tpidr_el0" || true)
      a=$(echo "$d" | grep -c "fiber_[gs]et" || true)
      echo "  $cc $f: tpidr_el0 reads $n, accessor calls $a"
    done
  done'
