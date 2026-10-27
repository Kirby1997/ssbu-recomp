#!/bin/sh
# Copy the C4 crash test to the Steam Frame (~/ssbu-port/crash) and run it there in every configuration: gcc, clang, clang with
# -O2 _FORTIFY_SOURCE=2 (longjmp -> __longjmp_chk out of the handler), and PAC (-mbranch-protection=standard) with both.
# Usage: tests/port/crash/run_frame.sh [host]      Not run by the agent that wrote it: results come from the device.
set -e
HOST=${1:-steamos@frame}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
tar -cf - runtime/plat_crash.h runtime/plat_crash.c tests/port/crash/run.sh tests/port/crash/test_crash.c |
    ssh -o BatchMode=yes "$HOST" 'rm -rf ~/ssbu-port/crash && mkdir -p ~/ssbu-port/crash && cd ~/ssbu-port/crash && tar -xf -'
ssh -o BatchMode=yes "$HOST" 'cd ~/ssbu-port/crash && set -e
  for cfg in "CC=gcc" "CC=clang" "CC=clang CFG=fortify" "CC=clang CFG=pac" "CC=gcc CFG=pac"; do
    echo "######## $cfg"; env $cfg sh tests/port/crash/run.sh 2>&1
  done'
