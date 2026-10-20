#!/usr/bin/env bash
# Start the native Linux build (Steam Frame / aarch64 SteamOS) of the game: the counterpart of `Play SSBU.bat`.
#
#   scripts/play-linux.sh [--build <folder>] [--log <file>] [game options ...]
#
# Game options go to ssbu_native unchanged; it finds the player's own game data itself (runtime/datapaths.c), first found wins:
#   --data <dump folder>     the folder tools/extract/dump_folder.py made (exefs/ romfs/ sysdata/fonts), or its romfs/   (SSBU_DATA, SSBU_ROMFS)
#   --fonts <folder>         the 7 system fonts, when they are not in <dump>/sysdata/fonts                                (SSBU_FONTS)
#   --dlc <file or folder>   dlc.txt (tools/audit/dlc_list.py), or the folder of your DLC dumps (their names carry the ids) (SSBU_DLC_LIST)
#   --userdata <folder>      saves (default ~/.local/share/ssbu-recomp/userdata)                                         (SSBU_USERDATA)
#   --check-data             only check the game data, print where everything is, exit (0 = usable, 9 = not)
#   [main.elf [seconds]]     default: the build's own main.elf (state.json game_version), no time limit
# then the choice saved in ~/.config/ssbu-recomp/config (`data=<folder>` ...), then the build folder's own layout
# (build/linux-arm64/state.json "romfs" written by scripts/setup.py, ../romfs, ../sysdata/fonts). SSBU_* variables already in the
# environment (Steam launch options, a shell) are passed through. Exit code 9: the game data are missing or unusable (the reason
# is printed); other codes are the game's own (BUILDING.md).
#
# --build: the build folder holding work/ssbu_native (default: <repo>/build/linux-arm64, or the folder of this script when
#          ssbu_native sits beside it, or $SSBU_BUILD).
# --log:   also write the output to this file (default: ~/.local/state/ssbu-recomp/last_run.log; --log - = none).
# Nothing is downloaded and no game data ships with the port: the game runs only from your own dump.
set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# the game runs in its work folder: a relative path given here is made absolute first (from the folder this was started in)
abs() { case "$1" in ''|-|/*|'~'|'~/'*) printf '%s' "$1" ;; *) printf '%s/%s' "$PWD" "$1" ;; esac; }
build=${SSBU_BUILD:-}
log=
args=()
while [ $# -gt 0 ]; do
    case "$1" in
        --build)   [ $# -ge 2 ] || { echo "play-linux: --build needs a folder" >&2; exit 2; }; build=$2; shift 2 ;;
        --build=*) build=${1#--build=}; shift ;;
        --log)     [ $# -ge 2 ] || { echo "play-linux: --log needs a file" >&2; exit 2; }; log=$(abs "$2"); shift 2 ;;
        --log=*)   log=$(abs "${1#--log=}"); shift ;;
        --data|--fonts|--dlc|--userdata)
                   if [ $# -ge 2 ]; then args+=("$1" "$(abs "$2")"); shift 2; else args+=("$1"); shift; fi ;;
        --data=*|--fonts=*|--dlc=*|--userdata=*)
                   args+=("${1%%=*}=$(abs "${1#*=}")"); shift ;;
        -h|--help) sed -n '2,/^set -u/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *)         args+=("$1"); shift ;;
    esac
done

if [ -n "$build" ]; then work=$build/work
elif [ -x "$here/ssbu_native" ]; then work=$here                      # a copied work folder with this script inside
else work=$here/../build/linux-arm64/work
fi
exe=$work/ssbu_native
if [ ! -x "$exe" ]; then
    echo "play-linux: $exe not found. Build it first: python3 scripts/setup.py --target linux-arm64 --dump <your dump folder> (BUILDING.md)," >&2
    echo "            or name the build folder with --build <folder> / SSBU_BUILD." >&2
    exit 1
fi
work=$(cd "$work" && pwd)
exe=$work/ssbu_native                                                   # absolute: the game is started from inside $work

# the runtime keeps its caches in ../cache relative to the working folder (and creates the folders below it, not the folder itself)
mkdir -p "$work/../cache" 2>/dev/null

# started from ssh / a console: the desktop or Game Mode session of this user
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ] && [ -S "$XDG_RUNTIME_DIR/gamescope-0" ]; then
    export WAYLAND_DISPLAY=gamescope-0
fi

if [ -z "$log" ]; then
    state=${XDG_STATE_HOME:-}
    case "$state" in /*) ;; *) state=$HOME/.local/state ;; esac
    mkdir -p "$state/ssbu-recomp" 2>/dev/null && log=$state/ssbu-recomp/last_run.log || log=-
fi

cd "$work" || exit 1
if [ "$log" = - ]; then
    "$exe" "${args[@]}"
    code=$?
else
    "$exe" "${args[@]}" 2>&1 | tee "$log"
    code=${PIPESTATUS[0]}
fi
if [ "$code" = 9 ]; then
    echo "play-linux: the game data are missing or unusable (above). Give your dump folder with --data <folder>," >&2
    echo "            or check what is found with: $0 --check-data" >&2
fi
exit "$code"
