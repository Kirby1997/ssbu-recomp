#!/bin/sh
# x86-64 code generation must not change (other than the intended CLREX fix): lift the litmus guest programs of
# test_lifter_atomics.py with the lifter of a base revision and with the working tree, compile each with its own
# runtime headers for x86-64 (-O1 -fno-strict-aliasing -ffp-contract=off) and compare the disassembly function by
# function. FP helpers that were moved behind #ifdef __aarch64__ are compared too.
#   tests/arm_host/x86_codegen_same.sh [base revision = ef71ec6] [compilers = "clang-15 gcc-12"]
# Reads git objects only (git show); no game files.
set -e
BASE=${1:-ef71ec6}
CCS=${2:-"clang-15 gcc-12"}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/old/tools/lifter" "$W/old/tools/nro" "$W/old/runtime" "$W/old/tests/arm_host"
# the old lifter reads main.elf at import: drop that line in the temporary copy
git -C "$ROOT" show "$BASE:tools/lifter/recomp.py" | sed "s/^IMG = open(ELF, 'rb').read()/IMG = None/" > "$W/old/tools/lifter/recomp.py"
git -C "$ROOT" show "$BASE:tools/nro/recomp_nro.py" > "$W/old/tools/nro/recomp_nro.py"
for f in runtime.h fpexact.h neon.h; do git -C "$ROOT" show "$BASE:runtime/$f" > "$W/old/runtime/$f"; done
cp "$HERE/test_lifter_atomics.py" "$W/old/tests/arm_host/"
python3 "$W/old/tests/arm_host/test_lifter_atomics.py" --gen "$W/old_gen.h" > /dev/null
python3 "$HERE/test_lifter_atomics.py" --gen "$W/new_gen.h" > /dev/null
sed -i 's/^static void/void/' "$W/old_gen.h" "$W/new_gen.h"
cat > "$W/cmp.c" <<'EOF'
#include "runtime.h"
#include GEN
float u_recpe(float x) { return recpe_f(x); }
double u_rsqrte(double x) { return rsqrte_d(x); }
uint16_t u_f2h(float x) { return f32_to_half(x); }
double u_h2d(uint16_t h) { return half_to_f64(h); }
float u_maxnm(float a, float b) { return fp_maxmin32(a, b, 0, 1); }
double u_min(double a, double b) { return fp_maxmin64(a, b, 1, 0); }
uint32_t u_f2s(double v) { return f2s32(v); }
uint64_t u_f2u(double v) { return f2u64(v); }
void u_fadd(V128 *d, const V128 *n, const V128 *m) { v_f3v(d, n, m, F_ADD, 0, 4); }
void u_fmla(V128 *d, const V128 *n, const V128 *m) { v_f3v(d, n, m, F_MLA, 1, 2); }
void u_fmax(V128 *d, const V128 *n, const V128 *m) { v_f3v(d, n, m, F_MAX, 0, 4); }
void u_sqrt(V128 *d, const V128 *n) { v_f1v(d, n, F1_SQRT, 0, 4); }
void u_recpe_v(V128 *d, const V128 *n) { v_f1v(d, n, F1_RECPE, 0, 4); }
float u_fnan(float a, float b) { return fnan32(a * b); }
void u_i3v(V128 *d, const V128 *n, const V128 *m) { v_i3v(d, n, m, I_CMGT, 1, 8); }
void u_tbl(Ctx *c) { v_tbl(c, 1, 30, 3, 4, 1, 0); }
void u_ldn(Ctx *c, uint64_t a) { v_ldn(c, a, 2, 3, 2, 1); }
void u_stn(Ctx *c, uint64_t a) { v_stn(c, a, 2, 4, 1, 0); }
void u_f1v(V128 *d, const V128 *n) { v_f1v(d, n, F1_CVTZS, 1, 2); }
EOF
status=0
for CC in $CCS; do
    for v in old new; do
        inc="$W/old/runtime"; [ $v = new ] && inc="$ROOT/runtime"
        "$CC" -O1 -fno-strict-aliasing -ffp-contract=off -w -c -I"$inc" -DGEN="\"$W/${v}_gen.h\"" "$W/cmp.c" -o "$W/$v.o"
        # one line per instruction, prefixed by its function; no addresses
        objdump -d --no-show-raw-insn "$W/$v.o" | grep -v 'file format' |
            sed -E 's/^ *[0-9a-f]+:\t//; s/[0-9a-f]+ <([a-z_0-9]+)\+0x[0-9a-f]+>/<\1+X>/; s/(call|jmp) +[0-9a-f]+ <[^>]*>/\1/' |
            awk '/^[0-9a-f]+ <.*>:$/ {f = $2; next} {print f " " $0}' > "$W/$v.dis"
    done
    changed=$(diff "$W/old.dis" "$W/new.dis" | grep -E '^[<>]' | awk '{print $2}' | sort -u | tr '\n' ' ')
    n=$(awk '{print $1}' "$W/old.dis" | sort -u | wc -l)
    if [ "$changed" = "<g_clrex_check>: " ]; then
        echo "ok   $CC: $n functions; identical x86 code except g_clrex_check (CLREX now clears the reservation)"
    else
        echo "FAIL $CC: functions with different x86 code: $changed"; status=1
    fi
done
exit $status
