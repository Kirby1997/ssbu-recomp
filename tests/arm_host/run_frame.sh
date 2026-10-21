#!/bin/sh
# Lifter / runtime checks for an ARM host (PORT_AUDIT Phase 2), driven from the development machine.
#   tests/arm_host/run_frame.sh [ssh host = steamos@frame] [fp random pairs = 20000000] [litmus iterations = 1000000]
# 1. here: decoder test (python3), lifted litmus programs, x86 software-FP hash (cc / clang, -DSOFT_ONLY)
# 2. copies runtime/{runtime,fpexact,neon}.h and this directory to ~/ssbu-port/lifter/ on the host and runs
#    frame_tests.sh there (codegen of the ordering macros, litmus tests, FP vs. the instructions, neon.h native
#    vs. portable)
# No game files are used.
set -e
HOST=${1:-steamos@frame}
PAIRS=${2:-20000000}
ITERS=${3:-1000000}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

python3 "$HERE/test_lifter_atomics.py"
python3 "$HERE/test_lifter_atomics.py" --gen "$W/litmus_gen.h"
python3 "$HERE/test_lifter_atomics.py" --asm > "$W/litmus.s"
python3 "$HERE/test_lifter_atomics.py" --words > "$W/litmus.words"

CC=${CC:-$(command -v clang-15 || command -v clang || command -v cc)}
"$CC" -O1 -fno-strict-aliasing -ffp-contract=off -DSOFT_ONLY -w -I"$ROOT/runtime" "$HERE/fp_native_test.c" -o "$W/fp_soft" -lm -lpthread
"$W/fp_soft" "$PAIRS" | grep soft-hash > "$W/x86.hash"
echo "x86 ($CC): $(cat "$W/x86.hash")"

ssh -o BatchMode=yes "$HOST" 'mkdir -p ~/ssbu-port/lifter/runtime ~/ssbu-port/lifter/arm_host'
scp -q "$ROOT/runtime/runtime.h" "$ROOT/runtime/fpexact.h" "$ROOT/runtime/neon.h" "$HOST:ssbu-port/lifter/runtime/"
scp -q "$HERE/frame_tests.sh" "$HERE/litmus.c" "$HERE/codegen_check.c" "$HERE/fp_native_test.c" "$HERE/neon_native_test.c" \
    "$W/litmus_gen.h" "$W/litmus.s" "$W/litmus.words" "$W/x86.hash" "$HOST:ssbu-port/lifter/arm_host/"
# FRAME_CFLAGS / FRAME_CCS / FRAME_ARCHS (see frame_tests.sh) are passed on when set
ssh -o BatchMode=yes "$HOST" "FRAME_CFLAGS='${FRAME_CFLAGS:-}' FRAME_CCS='${FRAME_CCS:-}' FRAME_ARCHS='${FRAME_ARCHS:-}' sh ~/ssbu-port/lifter/arm_host/frame_tests.sh $PAIRS $ITERS x86.hash"
