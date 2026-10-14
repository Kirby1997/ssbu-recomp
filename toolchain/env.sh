# Source this to cross-compile for the Steam Frame (aarch64 Linux) from x86-64 WSL/Linux.
#   . toolchain/env.sh          then:  $CC hello.c -o hello $SSBU_CFLAGS
# Override before sourcing: LLVM_ROOT (default ~/.local/llvm-19.1.7), SYSROOT (default ~/sysroots/steam-frame).
# For the LLVM 9.0.0 tarball on Debian 12 (needs the libtinfo.so.5 stub in ~/.local/llvm-9-compat, see README.md):
#   LLVM_ROOT=~/.local/llvm-9 . toolchain/env.sh
export LLVM_ROOT="${LLVM_ROOT:-$HOME/.local/llvm-19.1.7}"
export SYSROOT="${SYSROOT:-$HOME/sysroots/steam-frame}"
export SDL3_ROOT="${SDL3_ROOT:-$SYSROOT/opt/ssbu-deps/sdl3}"
export VULKAN_HEADERS="${VULKAN_HEADERS:-$SYSROOT/usr}"      # include/vulkan/vulkan.h
export GLSLANG_ROOT="${GLSLANG_ROOT:-$SYSROOT/usr}"          # include/glslang, lib/libglslang.a
case "$LLVM_ROOT" in *llvm-9*) export LD_LIBRARY_PATH="$HOME/.local/llvm-9-compat${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}";; esac
export PATH="$LLVM_ROOT/bin:$PATH"
export SSBU_TARGET=linux-arm64
export SSBU_TRIPLE=aarch64-linux-gnu
# flags of PORT_AUDIT Phase 1: never fast-math / -ffinite-math-only / -fno-signed-zeros / -mstrict-align
export SSBU_ARCH_FLAGS="--target=aarch64-linux-gnu --sysroot=$SYSROOT -march=armv8.2-a+lse"
export SSBU_CFLAGS="$SSBU_ARCH_FLAGS -fuse-ld=lld -O1 -w -fno-strict-aliasing -ffp-contract=off -fno-math-errno -fno-omit-frame-pointer"
export CC="$LLVM_ROOT/bin/clang" CXX="$LLVM_ROOT/bin/clang++"
export RECOMP_CLANG="$CC"
