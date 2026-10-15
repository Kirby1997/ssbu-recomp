#!/bin/sh
# Copy the crash tests + the runtime files they need to the Steam Frame (~/ssbu-port/crash) and run every configuration:
# gcc, clang, clang -O2 _FORTIFY_SOURCE, clang PAC (-mbranch-protection=standard). Usage: tests/port/crash/run_frame.sh [host]
set -e
HOST=${1:-steamos@frame}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
tar -cf - runtime/plat.h runtime/plat_posix.c runtime/vmem.h runtime/vmem.c runtime/fiber.h runtime/fiber_aarch64.S \
    runtime/plat_crash.h runtime/plat_crash.c tests/plat/tp.h tests/port/crash/Makefile tests/port/crash/*.c |
    ssh -o BatchMode=yes "$HOST" 'mkdir -p ~/ssbu-port/crash && cd ~/ssbu-port/crash && rm -rf tests/port/crash/build && tar -xf -'
ssh -o BatchMode=yes "$HOST" 'cd ~/ssbu-port/crash/tests/port/crash && set -e
  for cfg in "CC=gcc" "CC=clang" "CC=clang CFG=fortify" "CC=clang CFG=pac" "CC=gcc CFG=pac"; do
    echo "######## make $cfg"; make -s $cfg 2>&1
  done'
