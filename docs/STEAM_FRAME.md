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
