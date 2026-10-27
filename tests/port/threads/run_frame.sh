#!/bin/sh
# C3b on the Steam Frame: copies the sources and these tests to the device (~/ssbu-port/threads) and runs there
#   - run.sh native with gcc and clang at -O1 and -O2 (thread / fiber stacks below 2^47, the switch, FLS, critical
#     sections, os_threads.c's guest fibers, then tests/cspike/gaps.c for comparison with the C-spike's run)
#   - run.sh bti with clang and gcc (-mbranch-protection=standard, PROT_BTI on the switch's page)
#   - the TLS hazard check: in os_threads.o (fiber_main, fib_switch) and hostrt.o (call_guest) at -O2, no direct read of
#     the thread pointer (tpidr_el0): their thread variables are reached through noinline accessors only
# Usage: tests/port/threads/run_frame.sh [ssh host = steamos@frame]. No game files are used.
set -e
HOST=${1:-steamos@frame}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
tar -cf - runtime/plat_posix.c runtime/plat runtime/os_threads.c runtime/hostrt.c runtime/*.h tests/cspike/gaps.c \
    tests/cspike/stubs/fake_whitelist.h tests/cspike/stubs/fake_openlist.h tests/port/threads |
    ssh -o BatchMode=yes "$HOST" 'rm -rf ~/ssbu-port/threads && mkdir -p ~/ssbu-port/threads && cd ~/ssbu-port/threads && tar -xf -'
ssh -o BatchMode=yes "$HOST" 'cd ~/ssbu-port/threads && rc=0
  uname -m; grep -m1 -i "^Features" /proc/cpuinfo || true
  for cc in gcc clang; do for opt in -O1 -O2; do
    echo "######## run.sh native CC=$cc OPT=$opt"
    CC=$cc OPT=$opt OUT=/tmp/ssbu-c3b-$cc$opt sh tests/port/threads/run.sh native 2>&1 || rc=1
  done; done
  for cc in clang gcc; do
    echo "######## run.sh bti CC=$cc"
    CC=$cc OUT=/tmp/ssbu-c3b-$cc-bti sh tests/port/threads/run.sh bti 2>&1 || rc=1
  done
  echo "######## TLS hazard: direct tpidr_el0 reads at -O2 (want 0 in each)"
  for cc in gcc clang; do
    F="-O2 -w -fno-strict-aliasing -fPIC -fvisibility=hidden -include runtime/plat/plat_pre.h -I runtime/plat -I runtime -I tests/port/threads/stub -I tests/cspike/stubs"
    $cc $F -c runtime/os_threads.c -o /tmp/c3b-os_threads-$cc.o && $cc $F -c runtime/hostrt.c -o /tmp/c3b-hostrt-$cc.o || { rc=1; continue; }
    for f in os_threads:fiber_main os_threads:fib_switch hostrt:call_guest; do
      o=/tmp/c3b-${f%%:*}-$cc.o; s=${f#*:}
      n=$(objdump -d --no-show-raw-insn --disassemble=$s $o | grep -c -i "tpidr_el0" || true)
      echo "  $cc $s: $n"; [ "$n" = 0 ] || rc=1
    done
  done
  exit $rc'
