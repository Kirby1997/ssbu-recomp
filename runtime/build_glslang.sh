#!/usr/bin/env bash
# Builds libssbu_glslang.so (vk_glslang_shim.c + glslang + SPIRV-Tools) for Linux. Run from the source folder
# (scripts/setup.py does). Counterpart of build_glslang.bat.
#
# Environment:  CC (default clang)   CFLAGS_TARGET (e.g. "--target=aarch64-linux-gnu --sysroot=... -march=armv8.2-a+lse -fuse-ld=lld")
#               GLSLANG_ROOT   prefix with include/glslang and lib/ (default /usr)
#               LIBDIR         where the libraries are (default $GLSLANG_ROOT/lib)
#               CXX            C++ driver used for the link, for libstdc++ (default: $CC with -lstdc++ -lm)
# glslang and SPIRV-Tools are linked statically when the .a files exist (the glslang C++ code is then inside the .so;
# libstdc++ stays dynamic, it is part of every SteamOS install), else shared. SPIRV-Tools may exist only as .so: then the
# .so is linked and must be shipped/found next to libssbu_glslang.so (rpath $ORIGIN).
set -euo pipefail
CC="${CC:-clang}"
GL="${GLSLANG_ROOT:-/usr}"
LIBDIR="${LIBDIR:-$GL/lib}"
OUT="${OUT:-libssbu_glslang.so}"
mkdir -p obj_host
inc=()
[ -d "$GL/include/glslang" ] && inc=(-I "$GL/include")
[ -d "$GL/include/glslang/Include" ] || { [ -d "$GL/include/glslang" ] || { echo "build_glslang: $GL/include/glslang not found (set GLSLANG_ROOT)" >&2; exit 1; }; }
# shellcheck disable=SC2086
$CC ${CFLAGS_TARGET:-} -O2 -fPIC -fvisibility=hidden -fno-semantic-interposition -w "${inc[@]}" -c vk_glslang_shim.c -o obj_host/vk_glslang_shim.o

pick() {   # pick <name>: static archive if present, else -l<name> shared
    if [ -f "$LIBDIR/lib$1.a" ]; then echo "$LIBDIR/lib$1.a"; else echo "-l$1"; fi
}
# glslang 14+ splits the static library into several archives; older/newer releases ship one or more of these
libs=()
for n in glslang-default-resource-limits glslang MachineIndependent GenericCodeGen OSDependent SPIRV SPIRV-Tools-opt SPIRV-Tools; do
    if [ -f "$LIBDIR/lib$n.a" ]; then libs+=("$LIBDIR/lib$n.a")
    elif ls "$LIBDIR"/lib$n.so >/dev/null 2>&1; then libs+=("-l$n"); fi
done
static_groups=()
for l in "${libs[@]}"; do case "$l" in *.a) static_groups+=("$l");; esac; done
shared=()
for l in "${libs[@]}"; do case "$l" in -l*) shared+=("$l");; esac; done
# shellcheck disable=SC2086
$CC ${CFLAGS_TARGET:-} -shared -o "$OUT" obj_host/vk_glslang_shim.o -L"$LIBDIR" \
    -Wl,--start-group "${static_groups[@]}" -Wl,--end-group "${shared[@]}" \
    -Wl,-rpath,'$ORIGIN' -Wl,-Bsymbolic -Wl,--exclude-libs,ALL -Wl,-z,defs -lstdc++ -lm -lpthread
echo "built $OUT: static: ${static_groups[*]##*/}   shared: ${shared[*]}"
