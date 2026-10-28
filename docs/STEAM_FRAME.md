# Steam Frame

What is specific to the native aarch64 Linux build on the Valve Steam Frame (`setup.py --target linux-arm64`).
Everything general about Linux is in [BUILDING_LINUX.md](../BUILDING_LINUX.md).

**Status: in progress, the game does not boot on the Frame yet.**

## Building

From an x86-64 PC: `source toolchain/env.sh`, with the Frame's sysroot copied by `toolchain/sync_sysroot.sh`.
Natively on the Frame: `toolchain/build_deps_on_device.sh` builds SDL3 into `~/ssbu-port/deps/sdl3`, then
`setup.py --target linux-arm64` (`--target linux` on an aarch64 host means the same).

## Game data

The runtime finds the dump itself (`runtime/datapaths.c`), in this order: `--data`, `SSBU_DATA` / `SSBU_ROMFS`, `data=` in
`~/.config/ssbu-recomp/config`, then the build's own layout.

When the data are missing or unusable (no `data.arc` or `.nrr`, packages that still need `tools/extract/dump_folder.py`,
the other game build, missing system fonts) and a display is present, the game opens its own picker before the game
window. It names the problem and lets you choose the folder:

| Button | Does |
|---|---|
| A | opens the folder |
| B | goes up |
| X | uses the folder shown |
| Y | lists places such as the SD card and USB drives |

The keyboard and mouse work too. In Desktop Mode the system folder dialog is offered first (never under Game Mode's
gamescope). The folder is checked and saved as `data=`.

To change it later, start once with the picker: in Steam's launch options `%command% --pick-data` or
`SSBU_PICK_DATA=1 %command%`. Scripted runs, `SSBU_SHOW=0` and runs without a display print the reason and exit with code 9
instead.

## Crash reports

On a crash the game writes `<exe>_crash.txt` to the user data folder (`SSBU_REPORT_DIR` for another one): the signal,
host and guest call stacks as module+offset, registers and the stack. It is written without malloc or stdio, on a stack of
its own, so a stack overflow is reported too. The process then dies of the original signal (exit status and core dump as
usual); `SSBU_CRASH_EXIT=1` exits with code 4 instead, as on Windows.

## Waiting for a Frame run

Several checks were written and tested only on other machines (macOS arm64) and still have to run on the device. One command
runs all of them, natively on the Frame, with no PC and no ssh. It needs no root and changes nothing outside the checkout and
its work folder `build-frame-tests/` (ignored by the repository).

On the Frame, in a terminal (Desktop Mode, or ssh into the running session):

```sh
git clone -b frame-pending <this repository's URL> ssbu-recomp    # or, in an existing clone: fetch, check out and pull frame-pending
cd ssbu-recomp
sh tests/frame/run_all.sh
```

It first checks what it needs and prints a one-line fix for anything missing (SteamOS's root filesystem is read-only, so the
fixes avoid pacman where they can: SDL3 comes from `toolchain/build_deps_on_device.sh`, kernel headers can come from a folder
passed with `--kheaders`). A missing tool skips only the suites that need it, and the report says which. Then it runs every
suite under a timeout, carries on after a failure, and prints a table. A full run is expected to take 10 to 30 minutes.

| Option | Does |
|---|---|
| `--quick` | a short subset of each suite (first compiler only, smaller counts): a few minutes |
| `--suite NAME` | one suite (repeat it or use `NAME,NAME`); `--list` names them |
| `--dry-run` | the preflight and the plan only; nothing is built or run |
| `--work DIR` | another work folder (default `build-frame-tests/` in the checkout) |
| `--sdl3 DIR`, `--kheaders DIR` | SDL3 prefix (default `~/ssbu-port/deps/sdl3`); a folder holding `linux/userfaultfd.h` |
| `--with-shots` | put the picker's screenshots in the `.tar.gz` (off by default: a screenshot can show what else is on screen) |
| `--ref-hash FILE` | the `soft-hash` line of another host, to compare the lifter's software-FP hash with |

For the picker's display runs, start it from the running Game Mode session (for example over ssh while Steam's gamescope
session is up); elsewhere those runs are skipped with a note.

**What to send back:** the two files it prints at the end, `build-frame-tests/ssbu-frame-<stamp>.txt` (the report) and
`.tar.gz` (the report and every log). The exit status is 0 when everything that ran passed. The report holds the summary
table (result, check count and seconds per suite), the preflight, the device facts and the key lines of each suite. Device
facts: `uname`, CPU features (BTI, PAC, MTE, SVE2, LSE), glibc, compiler versions, address-space settings
(`vm.mmap_rnd_bits`, VA bits, the top of `/proc/self/maps`), gamescope state, the SDL3 version and `vulkaninfo --summary`.
`$HOME` is written as `~`, user and host names are replaced, and no environment is dumped.

| Suite | Covers | Script |
|---|---|---|
| `crash` | crash reports: gcc, clang, FORTIFY, PAC | `tests/port/crash/run_frame.sh` |
| `fs` | file layer: case-insensitive paths, write-through rename | `tests/port/fs/run.sh native` |
| `boot` | game data lookup and launch defaults (synthetic fixtures) | `tests/port/boot/run.sh native` |
| `plat` | `VirtualAlloc` below 2^47, write watch, `plat_selftest`, Vulkan loader | `tests/cspike/c3_build.sh native` |
| `threads` | stacks and `__thread` below 2^47, fibers, FLS, critical sections, BTI, TLS hazard | `tests/port/threads/run_frame.sh` |
| `picker` | the data-folder picker, in an SDL window offscreen and on gamescope | `tests/port/picker/run_frame.sh` |
| `lifter` | memory-ordering codegen, litmus tests, FP vs the instructions, NEON | `tests/arm_host/run_frame.sh` |

The `run_frame.sh` scripts also work alone: on the Frame they run in place, with `--local` on any host, and with an ssh host
as the first argument they push from a PC as before. The lifter's software-FP hash is printed, and compared with another
host only when given `--ref-hash`; from a PC, `tests/arm_host/run_frame.sh <host>` still does the x86 comparison. Not in the kit:
`tests/cspike/xbuild.sh` and `xcheck.sh` are cross-build checks of the whole runtime (they need the cross toolchain and
sysroot; `setup.py --target linux-arm64` is the native build), and `gaps.c` is already built and run by `threads`.

## Playing against upstream builds

This fork's lifter matches the Switch more closely in a few edge cases (CLREX, FRECPS / FRSQRTS of 0 and infinity, some
LSE / LDAPR encodings that now trap instead of being skipped), on every host, Windows included. A fork build and an upstream
build are therefore not guaranteed to simulate identically, which matters for a rollback match between them.
