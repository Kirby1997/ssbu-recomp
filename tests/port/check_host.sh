#!/bin/sh
# Compile every hand-written runtime source for linux-arm64 (cross, from WSL) and report errors per file.
#   tests/port/check_host.sh [file.c ...]      default: all host sources build.py links on Linux
# Uses toolchain/env.sh (LLVM 19 + Steam Frame sysroot). Objects go to $OUT (default /tmp/ssbu-check-host).
# Generated sources (sdkdata.c, nro_registry.c, sdklift_gen.c) need game data and are not checked.
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO/toolchain/env.sh"
RT="$REPO/runtime"
OUT=${OUT:-/tmp/ssbu-check-host}
mkdir -p "$OUT"
FLAGS="$SSBU_ARCH_FLAGS -D_GNU_SOURCE -fno-pie -O1 -fno-strict-aliasing -ffp-contract=off -fno-math-errno -fno-omit-frame-pointer \
 -ferror-limit=0 -Wno-everything -Werror=implicit-function-declaration -Werror=int-conversion -Werror=incompatible-pointer-types \
 -I$RT -I$REPO -I$REPO/tests/port/stubs -Dfn_table=(*game_fn_table_p) -Dfn_table_count=(*game_fn_table_count_p) -Ddyn_syms=(*game_dyn_syms_p) \
 -Ddyn_sym_count=(*game_dyn_sym_count_p) -I$SDL3_ROOT/include -I$VULKAN_HEADERS/include -I$SYSROOT/usr/include/freetype2 \
 -I$SYSROOT/usr/include/opus"
if [ $# -gt 0 ]; then FILES="$*"; else
FILES=$(python3 - "$REPO" <<'PY'
import sys, os
sys.path.insert(0, os.path.join(sys.argv[1], 'tools', 'lifter'))
os.environ['RECOMP_TARGET'] = 'linux-arm64'
import importlib.util
src = open(os.path.join(sys.argv[1], 'tools', 'lifter', 'build.py')).read()
ns = {}
start = src.index('HOST_SOURCES = ['); end = src.index('# A handful of files')
exec('import os\nimport buildplat as plat\n' + src[start:end], ns)
gen = {'sdkdata.c', 'nro_registry.c', 'sdklift_gen.c'}
print(' '.join([s for s in ns['HOST_SOURCES'] if s not in gen] + ['gamedll.c', 'game_shim_eh.c', 'nro_shim.c']))
PY
)
fi
ok=0; bad=0; summary=""
for f in $FILES; do
  log="$OUT/${f%.*}.log"
  case "$f" in      # the shims are compiled with the game library / an NRO module, not with the host flags
    game_shim*.c|nro_shim.c) F="$SSBU_ARCH_FLAGS -D_GNU_SOURCE -O1 -fPIC -fvisibility=hidden -ferror-limit=0 -Wno-everything -Werror=implicit-function-declaration -I$RT -DNRO_MODULE=check -I$REPO/tests/port/stubs";;
    *) F="$FLAGS";;
  esac
  if "$CC" -c $F "$RT/$f" -o "$OUT/${f%.*}.o" >"$log" 2>&1; then ok=$((ok+1)); summary="$summary\n  ok    $f"
  else n=$(grep -c "error:" "$log"); bad=$((bad+1)); summary="$summary\n  FAIL  $f ($n errors)"; fi
done
printf "$summary\n"
echo "compiled: $ok ok, $bad failed (logs in $OUT)"
[ $bad -eq 0 ]
