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

Written and tested only on other machines (macOS arm64) so far. Run these on the device before relying on them:

| Script | Checks |
|---|---|
| `tests/port/crash/run_frame.sh` | crash reports (gcc, clang, fortify, PAC) |
| `tests/port/picker/run_frame.sh` | the picker's logic and its SDL window with a virtual gamepad |
| `tests/port/boot`, `tests/port/fs` | game data lookup and file layer after the upstream merge |
| `tests/arm_host/run_frame.sh` | lifter memory ordering, FP and NEON |

## Playing against upstream builds

This fork's lifter matches the Switch more closely in a few edge cases (CLREX, FRECPS / FRSQRTS of 0 and infinity, some
LSE / LDAPR encodings that now trap instead of being skipped), on every host, Windows included. A fork build and an upstream
build are therefore not guaranteed to simulate identically, which matters for a rollback match between them.
