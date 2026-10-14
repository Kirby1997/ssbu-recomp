# Steam Frame (linux-arm64) toolchain

Target: aarch64 SteamOS (Arch-style: flat `/usr/lib`, `/lib -> usr/lib`, glibc 2.39, Mesa Turnip).
Two ways to build, both driven by `python3 scripts/setup.py --target linux-arm64`:

| mode | where | compiler | headers/libs |
|---|---|---|---|
| cross | x86-64 WSL/Linux | `~/.local/llvm-<ver>/bin/clang` + `ld.lld` (official LLVM release tarball) | sysroot `~/sysroots/steam-frame` |
| native | on the Frame | `/usr/bin/clang` (19.x) + `/usr/bin/ld.lld` | `/usr`, SDL3 in `~/ssbu-port/deps/sdl3` |

## One-time setup (no sudo)

1. SDL3 is not on SteamOS. On the Frame: `toolchain/build_deps_on_device.sh` (SDL3 3.4.18, shared, Wayland+X11+KMSDRM video,
   PipeWire/Pulse/ALSA audio, all dlopen'ed at run time) -> `~/ssbu-port/deps/sdl3`.
2. Cross only: LLVM. `curl -LO https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/LLVM-19.1.7-Linux-X64.tar.xz`,
   unpack to `~/.local/llvm-19.1.7`.
3. Cross only: `toolchain/sync_sysroot.sh` (tar over ssh; `SSBU_DEVICE`/`-H` = ssh host, `-d` = sysroot dir; about 1.2 GB, 20 s).
   Re-run it after the device's packages or the SDL3 build change.

`. toolchain/env.sh` exports `CC`, `SSBU_CFLAGS` etc. for ad-hoc builds; `toolchain/aarch64-frame.cmake` is the CMake toolchain file.

## Flags

`--target=aarch64-linux-gnu --sysroot=... -fuse-ld=lld -O1 -w -fno-strict-aliasing -ffp-contract=off -fno-math-errno
-fno-omit-frame-pointer -march=armv8.2-a+lse`. Shared objects add `-fPIC -fvisibility=hidden -fno-semantic-interposition`;
NRO modules link with `-Wl,-Bsymbolic`. `-ffp-contract=off` is mandatory (clang fuses a*b+c into `fmadd` on aarch64 even at -O0).
The flags live in `tools/lifter/buildplat.py` (used by build.py/build_opt.py/build_nro.py).

## LLVM 9.0.0 on Debian 12

The official `clang+llvm-9.0.0-x86_64-linux-gnu-ubuntu-18.04.tar.xz` (releases.llvm.org) does NOT start on Debian 12
(`libtinfo.so.5` missing). It runs, and cross-compiles/links aarch64 against the sysroot, with a stub library:
`~/.local/llvm-9-compat/libtinfo.so.5` (5 functions, built from a 4-line C file with a `NCURSES_TINFO_5.0.19991023` version
script) on `LD_LIBRARY_PATH`. `scripts/setup.py --llvm ~/.local/llvm-9` and `LLVM_ROOT=~/.local/llvm-9 . toolchain/env.sh`
add that path automatically. Default is the newest LLVM found (19.1.7, same major as the Frame's own clang 19.1.7).

## Device runtime notes (measured)

- From ssh there is no desktop session: set `XDG_RUNTIME_DIR=/run/user/1000`; with `WAYLAND_DISPLAY=gamescope-0` SDL3 picks Wayland
  (present modes: mailbox, fifo), with `DISPLAY=:0`/`:1` X11 (immediate, mailbox, fifo, fifo_relaxed),
  with `SDL_VIDEO_DRIVER=offscreen` the headless surface (mailbox, fifo). Without any of these SDL picks `kmsdrm`.
- `SDL_VIDEO_DRIVER=dummy` has no Vulkan support.
