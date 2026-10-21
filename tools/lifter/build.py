"""Parallel build of the lifted game.

    python build.py compile [jobs] [--force]    gen/*.c -> obj/*.obj (only what changed)
    python build.py link                        obj/*.obj + host sources -> ssbu_native.exe
    python build.py host                        host sources -> the exe alone (needs no game DLL; scripts/make_release.py)
    python build.py gamedll                     obj/*.obj + the game-data sources (game_data.h) -> the game DLL alone
    python build.py nro <module> [...]          lift + compile + link one DLL per run-time module (see build_nro.py)
"""
import concurrent.futures
import os
import shlex
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# P3 (2026-10-09): the OPTIMISED set is the default: gen_eh_o2 (lifter with the fcvt-half and FMAX/FMIN fixes) -> obj_eh_o1
# (clang 9 -O1 through build_opt.py) -> ssbu_game_eh_o1.dll. The unoptimised set stays buildable under its own names:
#     RECOMP_GEN=gen_eh RECOMP_OBJ=obj_eh RECOMP_DLL=ssbu_game_eh.dll python build.py compile --opt=-O0   (and: ... python build.py link)
GEN = os.path.join(HERE, os.environ.get('RECOMP_GEN', 'gen_eh_o2'))      # side-by-side builds: RECOMP_GEN / RECOMP_OBJ / RECOMP_DLL / RECOMP_SHIM
OBJ = os.path.join(HERE, os.environ.get('RECOMP_OBJ', 'obj_eh_o1'))
CLANG = os.environ.get('RECOMP_CLANG', r'C:\llvm-8.0.0\bin\clang.exe')      # scripts\setup.py sets RECOMP_CLANG (LLVM/clang 9.0.0)
# RECOMP_TARGET=linux: a native Linux build (ELF objects, the game and the modules as .so files, the exe linked with the
# system's SDL3 / FreeType / OpenSSL). The Win32 calls of the host sources come from plat/windows.h + plat_posix.c.
# RECOMP_TARGET=linux-arm64: the same ELF build for the Steam Frame (aarch64): cross from an x86-64 host against a
# sysroot copied from the device, or native on the Frame itself. C2 adds the branches only: the Windows and the x86-64
# Linux flag lists stay exactly as they were (TARGET_FLAGS and CODEGEN_FLAGS are empty there).
TARGET = os.environ.get('RECOMP_TARGET', '')
TARGET_LINUX = TARGET in ('linux', 'linux-arm64')
TARGET_ARM64 = TARGET == 'linux-arm64'

def arm_target_flags():
    """Driver flags of the aarch64 target, used for compiling AND linking. RECOMP_TARGET_FLAGS (scripts/setup.py
    --target linux-arm64, exported by toolchain/env.sh) wins; otherwise --sysroot from SYSROOT when cross-compiling and
    the Frame's own core: Snapdragon 8 Gen 3 = 1 Cortex-X4, 5 Cortex-A720, 2 Cortex-A520 (armv9.2-a, LSE, SVE2 on every core).
    A clang that has no -mcpu for those cores falls back to -march=armv8.2-a+lse (the set PORT_AUDIT Phase 1 validated)
    and says so - the fallback is never silent."""
    flags = ['--target=aarch64-linux-gnu', '-fuse-ld=lld']
    sysroot = os.environ.get('SYSROOT') or os.environ.get('PKG_CONFIG_SYSROOT_DIR') or ''
    if sysroot:
        flags.append('--sysroot=' + os.path.abspath(os.path.expanduser(sysroot)))
    for cpu in ('cortex-x4', 'cortex-a720'):
        if subprocess.run([CLANG, '--target=aarch64-linux-gnu', '-mcpu=' + cpu, '-fsyntax-only', '-x', 'c', '/dev/null'],
                          capture_output=True, text=True).returncode == 0:
            return flags + ['-mcpu=' + cpu]
    print('note: %s has no -mcpu for the Frame (Cortex-X4): building with -march=armv8.2-a+lse' % CLANG, flush=True)
    return flags + ['-march=armv8.2-a+lse']

TARGET_FLAGS = shlex.split(os.environ.get('RECOMP_TARGET_FLAGS', '')) if TARGET_ARM64 else []
if TARGET_ARM64 and not any(f.startswith('-mcpu') or f.startswith('-march') for f in TARGET_FLAGS):
    TARGET_FLAGS = arm_target_flags() + TARGET_FLAGS
# clang on aarch64 fuses a*b+c into fmadd even at -O0; the lifted code has to stay bit-exact against the game, so the
# contraction is off for every translation unit of the arm64 build (also native; x86-64 -O1 never fused one).
CODEGEN_FLAGS = TARGET_FLAGS + ['-ffp-contract=off', '-fno-math-errno', '-fno-omit-frame-pointer'] if TARGET_ARM64 else []
# The whole tree defaulted to -O1; on the Frame the lifted code is meant to start at -O2. OPT_BASE is the '-O1' the
# code below substitutes for the per-file --opt=, so windows and x86-64 keep their -O1 lines character for character.
OPT_BASE = '-O2' if TARGET_ARM64 else '-O1'
# The dozen largest lifted files: on x86-64 clang 8/9 needed -O0 for them; clang 19 on aarch64 does not, so the floor
# there is -O1 (RECOMP_SLOW_OPT overrides it; a file that lands on it is reported, see compile_one).
SLOW_OPT = os.environ.get('RECOMP_SLOW_OPT', '-O1' if TARGET_ARM64 else '-O0')
PLAT_FLAGS = ['-fPIC', '-fvisibility=hidden', '-include', os.path.join(HERE, 'plat', 'plat_pre.h')] if TARGET_LINUX else []
CFLAGS = ['-c', OPT_BASE, '-w', '-fno-strict-aliasing', '-I', HERE, '-I', GEN] + PLAT_FLAGS + CODEGEN_FLAGS
HOST_SOURCES = ['hostrt.c', 'boot.c', 'nnsdk.c', 'libcxx.c', 'os_threads.c', 'nvn_stub.c', 'nvn_gl.c', 'nvn_vk.c', 'mx_translate.c', 'tex_decode.c', 'nnfs.c', 'clib.c', 'nntime.c', 'nnhid.c', 'nnpad_sdl.c', 'nnpad_gca.c', 'display.c', 'nnpl.c', 'nnfontll.c', 'nnmovie.c', 'nnmisc.c', 'nnaudio.c', 'nnro.c', 'nnmovie_mf.c', 'nniostream.c', 'nncxxrt.c', 'nncxxlib.c', 'nnlibc2.c',
                'sdklift_rt.c', 'nnsdk2.c', 'nnmii.c', 'nnapplet.c', 'nnalbum.c', 'nnnet.c', 'nnfsdir.c', 'nnunwind.c', 'nvn_sdk.c', 'nvn_c4.c', 'nvn_c5.c', 'hitch.c', 'gfx_options.c', 'debug_mode.c', 'r1_state.c',
                'loose_host.c', 'osk.c', 'nnldn.c', 'net_relay.c', 'rb_net.c']    # the new fighters' file layer (loose_files.c): off unless SSBU_CHARACTERS / SSBU_LOOSE_FILES is set
# nnldn.c / net_relay.c: online play over the game's Local Wireless (plan/phases/N3_local_wireless_online.md), off unless SSBU_ONLINE
# (taken only when present: a tree without the online files, e.g. a public one before they are published, still links)
HOST_SOURCES[:] = [n for n in HOST_SOURCES if n not in ('nnldn.c', 'net_relay.c', 'rb_net.c') or os.path.exists(os.path.join(HERE, n))]   # rb_net.c: online rollback (R3), off unless SSBU_RB_NET
# Everything of the host that is derived from the dump is linked into the game DLL instead (game_data.h): the exe is made
# of the runtime's own sources only and can be built without a dump (scripts/make_release.py).
GAME_DATA_SOURCES = ['game_data.c', 'sdkdata.c', 'nro_registry.c', 'sdklift_gen.c']
# RECOMP_NOCRT=1: the game DLL and the module DLLs are compiled and linked with LLVM alone, without the MSVC Build Tools or
# the Windows SDK. The few C-library headers the lifted code includes come from nocrt\ (the same declarations as the
# Universal CRT's), the few functions it calls from ucrtbase.dll and ntdll.dll, which every Windows 10 / 11 has (import
# libraries made by lld-link from the .def files in nocrt\), and the DLLs have no C runtime and no entry point (nothing in
# them needs one). The code is the same: objects compiled both ways are byte-identical but for the COFF time stamp.
NOCRT = os.environ.get('RECOMP_NOCRT', '') not in ('', '0')
NOCRT_DIR = os.path.join(HERE, 'nocrt')
NOCRT_FLAGS = ['-nostdlibinc', '-isystem', NOCRT_DIR, '-DGAME_NOCRT'] if NOCRT else []
LLD_LINK = os.path.join(os.path.dirname(CLANG), 'lld-link.exe')
# A handful of files hold functions so large that clang -O1 needs more than an hour on them;
# pass --opt=-O0 to compile whatever is still missing without optimization.
OPT = next((a.split('=', 1)[1] for a in sys.argv if a.startswith('--opt=')), OPT_BASE)
GEN_HEADERS = ['runtime.h', 'neon.h', 'fpexact.h']
SLOW_FILES = {'c_019e.c', 'c_00a3.c', 'c_013a.c', 'c_0099.c', 'c_0367.c', 'c_03f7.c', 'c_00b3.c', 'c_03d9.c', 'c_012f.c', 'c_00cf.c', 'c_040e.c', 'c_0180.c', 'c_04a5.c'}


def compile_one(name):
    src = os.path.join(GEN, name)
    obj = os.path.join(OBJ, name[:-2] + '.obj')
    t = time.time()
    # the dozen largest files hold functions clang 8 cannot optimise in reasonable time: keep them at -O0
    # (on aarch64 the floor is -O1; anything below the build's -O2 is named in the output)
    opt = SLOW_OPT if name in SLOW_FILES else OPT
    if TARGET_ARM64 and opt < OPT_BASE:      # never silent: every file under -O2 is named with the reason
        print('  %s: compiled at %s, below the %s target of the Frame build (%s)' % (name, opt, OPT_BASE,
              'SLOW_FILES floor, RECOMP_SLOW_OPT' if name in SLOW_FILES else '--opt= on the command line'), flush=True)
    flags = [opt if f == OPT_BASE else f for f in CFLAGS] + NOCRT_FLAGS
    r = subprocess.run([CLANG] + flags + [src, '-o', obj], capture_output=True, text=True)
    return name, r.returncode, r.stderr[-2000:], time.time() - t


def cmd_compile(jobs, force):
    os.makedirs(OBJ, exist_ok=True)
    sources = sorted(n for n in os.listdir(GEN) if n.endswith('.c'))
    # only the headers the generated code includes; hostrt.h belongs to the host sources, which cmd_link rebuilds when they are stale
    newest_header = max(os.path.getmtime(os.path.join(HERE, h)) for h in GEN_HEADERS)
    todo = []
    for n in sources:
        obj = os.path.join(OBJ, n[:-2] + '.obj')
        if force or not os.path.exists(obj) or os.path.getmtime(obj) < max(os.path.getmtime(os.path.join(GEN, n)), newest_header):
            todo.append(n)
    wanted = set(n[:-2] + '.obj' for n in sources)
    for n in os.listdir(OBJ):
        if n.endswith('.obj') and n not in wanted:
            os.remove(os.path.join(OBJ, n))
    # longest first (source size: the compile time follows it): the biggest files no longer start last and hold up the end
    todo.sort(key=lambda n: -os.path.getsize(os.path.join(GEN, n)))
    print('sources: %d   to compile: %d   jobs: %d' % (len(sources), len(todo), jobs), flush=True)
    t0 = time.time()
    failed, done, cpu = [], 0, 0.0
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for name, code, err, secs in pool.map(compile_one, todo):
            done += 1
            cpu += secs
            if code:
                failed.append(name)
                print('FAILED %s\n%s' % (name, err), flush=True)
            if done % 50 == 0 or done == len(todo):
                print('  %d/%d   %.0fs elapsed   %.0fs compiler time' % (done, len(todo), time.time() - t0, cpu), flush=True)
    print('compiled: %d   failed: %d   elapsed: %.0fs' % (len(todo) - len(failed), len(failed), time.time() - t0), flush=True)
    return 1 if failed else 0


# The lifted game code lives in ssbu_game.dll, linked only when obj\ changes; the exe holds just the
# host runtime, so the edit-link-run loop costs a couple of seconds (see game_shim.c, gamedll.c).
GAME_DLL = os.environ.get('RECOMP_DLL', 'ssbu_game_eh_o1.dll')
HOST_FLAGS = [OPT_BASE, '-g', '-w', '-fno-strict-aliasing', '-I', HERE, '-I', GEN, '-Dfn_table=(*game_fn_table_p)', '-Dfn_table_count=(*game_fn_table_count_p)',
              '-Ddyn_syms=(*game_dyn_syms_p)', '-Ddyn_sym_count=(*game_dyn_sym_count_p)',
              '-I', os.path.join(os.environ.get('SDL3_ROOT', os.path.join(HERE, '..', 'reference', 'upstream', 'SDL3-3.4.18-VC', 'SDL3-3.4.18')), 'include'),
              '-I', os.path.join(os.environ.get('VULKAN_HEADERS', os.path.join(HERE, '..', 'reference', 'upstream', 'Vulkan-Headers')), 'include')] + CODEGEN_FLAGS + (['-fno-pie'] if TARGET_ARM64 else [])      # Khronos headers for nvn_vk.c (track V); arm64: -fno-pie so the exe's brk heap sits low (see the link below)
SDL_DIR = os.path.join(os.environ.get('SDL3_ROOT', os.path.join(HERE, '..', 'reference', 'upstream', 'SDL3-3.4.18-VC', 'SDL3-3.4.18')), 'lib', 'x64')      # SDL3 for nnpad_sdl.c; SDL3.dll is copied beside the exe at link (SDL3_ROOT / VULKAN_HEADERS: scripts\setup.py)
# U1 (plan\phases\U1_settings_menu.md): the settings menu (F1) is C++ on Dear ImGui v1.91.9b (MIT; the pinned archive and
# its SHA-256 are in the public tree's scripts\install.py, TOOLS 'imgui'). IMGUI_ROOT = its folder (scripts\setup.py --imgui
# sets it); default: reference\upstream\imgui-1.91.9b beside the study tree. Compiled by the same clang as C++14 without
# exceptions or RTTI, objects beside the host's. Without imgui.h there, menu_stub.c is built instead: no menu (F1 does
# nothing; the log says why), everything else the same.
IMGUI = os.environ.get('IMGUI_ROOT') or os.path.join(HERE, '..', 'reference', 'upstream', 'imgui-1.91.9b')
HAVE_IMGUI = os.path.isfile(os.path.join(IMGUI, 'imgui.h')) and os.path.isfile(os.path.join(IMGUI, 'imgui.cpp'))
CXX_SOURCES = ([os.path.join(HERE, 'menu.cpp')] + [os.path.join(IMGUI, n) for n in ('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp')]
               if HAVE_IMGUI else [])
CXX_FLAGS = ['-x', 'c++', '-std=c++14', OPT_BASE, '-g', '-w', '-fno-exceptions', '-fno-rtti', '-D_CRT_SECURE_NO_WARNINGS', '-DIMGUI_USER_CONFIG="menu_imconfig.h"', '-I', HERE, '-I', IMGUI]
LIBS = ['-luser32', '-lgdi32', '-lwinmm', '-lmfplat', '-lmfreadwrite', '-lmfuuid', '-lole32', '-lshlwapi', '-lshell32', '-lsetupapi']
if TARGET_LINUX:
    PKGS = ['sdl3', 'freetype2', 'openssl', 'libavformat', 'libavcodec', 'libswscale', 'libswresample', 'libavutil']
    SYSROOT_DIR = next((f.split('=', 1)[1] for f in TARGET_FLAGS if f.startswith('--sysroot=')), '')
    PKG_ENV = dict(os.environ)
    if TARGET_ARM64 and SYSROOT_DIR:
        # cross build: pkg-config reads the sysroot's .pc files only (LIBDIR replaces its default search path, which on
        # an x86-64 host would find the host's libraries) and prefixes every path it returns with the sysroot, so the
        # -I/-L of the Frame's libraries reach clang without a hand-written list. An existing PKG_CONFIG_LIBDIR wins.
        PKG_ENV['PKG_CONFIG_SYSROOT_DIR'] = SYSROOT_DIR
        PKG_ENV['PKG_CONFIG_LIBDIR'] = PKG_ENV.get('PKG_CONFIG_LIBDIR') or ':'.join(
            os.path.join(SYSROOT_DIR, p) for p in ('usr/lib/pkgconfig', 'usr/share/pkgconfig', 'opt/ssbu-deps/sdl3/lib/pkgconfig'))
    def pkg(*args):
        """pkg-config, with PKG_CONFIG_SYSROOT_DIR for the arm64 target. No pkg-config on a cross host, or no .pc files
        in the sysroot: fall back to the same libraries spelled out (SDL3_ROOT / the sysroot's /usr/include), which is
        what toolchain/env.sh exports - and say so, so nobody links against the host's x86 libraries unnoticed."""
        try:
            return subprocess.run(['pkg-config'] + list(args) + PKGS, capture_output=True, text=True, check=True, env=PKG_ENV).stdout.split()
        except (OSError, subprocess.CalledProcessError):
            if not TARGET_ARM64:              # the x86-64 Linux build: exactly as before, pkg-config is required there
                raise
            sdl = os.environ.get('SDL3_ROOT') or (os.path.join(SYSROOT_DIR, 'opt', 'ssbu-deps', 'sdl3') if SYSROOT_DIR else
                                                  os.path.join(HERE, '..', 'reference', 'upstream', 'SDL3-3.4.18-VC', 'SDL3-3.4.18'))
            inc = os.path.join(SYSROOT_DIR, 'usr', 'include') if SYSROOT_DIR else '/usr/include'
            cflags = ['-I', os.path.join(sdl, 'include'), '-I', os.path.join(inc, 'freetype2'), '-I', os.path.join(inc, 'opus')]
            libs = ['-L', os.path.join(sdl, 'lib'), '-Wl,-rpath-link,' + os.path.join(sdl, 'lib'),
                    '-lSDL3', '-lfreetype', '-lssl', '-lcrypto', '-lavformat', '-lavcodec', '-lswscale', '-lswresample', '-lavutil']
            print('pkg-config gave nothing (%s): falling back to the library list of toolchain/env.sh' % (SYSROOT_DIR or 'the system'), flush=True)
            return cflags + libs if not args else (cflags if '--cflags' in args else libs)
    _sdl_inc = HOST_FLAGS.index('-I', HOST_FLAGS.index('-Ddyn_sym_count=(*game_dyn_sym_count_p)'))      # the SDL3 VC package: the system's SDL3 instead
    HOST_FLAGS = HOST_FLAGS[:_sdl_inc] + HOST_FLAGS[_sdl_inc + 2:] + PLAT_FLAGS + ['-I', os.path.join(HERE, 'plat')] + pkg('--cflags')
    # plat_mf.c: the Media Foundation subset nnmovie_mf.c uses, on FFmpeg. Not in the Linux build yet: online play
    # (net_relay.c: Winsock extensions, COM UPnP; net_off.c answers "off", as with SSBU_ONLINE unset)
    HOST_SOURCES = [n for n in HOST_SOURCES if n != 'net_relay.c'] + ['plat_posix.c', 'plat_mf.c', 'net_off.c'] + (['plat_heap.c'] if TARGET_ARM64 else [])      # plat_heap.c (C3): arm64 only, the C heap below 2^47 (needs the -no-pie link below); x86 builds are unchanged
    LIBS = pkg('--libs') + ['-lGL', '-lm', '-ldl', '-lpthread']      # libGL: the OpenGL front end is linked (not used)
    HAVE_IMGUI, CXX_SOURCES = False, []                  # the settings menu (menu.cpp) is Win32 + D3D: menu_stub.c on Linux for now


def debug_link_flags(out):
    """The exe's debug information (-g in HOST_FLAGS) and linker map, beside the exe: <exe>.pdb lets WinDbg / Visual Studio
    name the host functions of a minidump (hostrt.c), <exe>.map the freeze watchdog's report (nvn_vk_wd.c). /OPT and
    /INCREMENTAL as link.exe has them without /DEBUG, so the code is the same as before. /PDBALTPATH: the exe holds the
    file name of its .pdb, not the full path (which names the Windows user). Every link line of the tree should use this."""
    base = os.path.splitext(out)[0]
    return ['-Wl,/DEBUG', '-Wl,/PDB:' + base + '.pdb', '-Wl,/PDBALTPATH:%_PDB%', '-Wl,/MAP:' + base + '.map', '-Wl,/OPT:REF', '-Wl,/OPT:ICF', '-Wl,/INCREMENTAL:NO']


def lld_def_lib(def_path, lib):
    """An import library from a .def file, made by lld-link alone (no MSVC lib.exe, no Windows SDK)."""
    if os.path.exists(lib) and os.path.getmtime(lib) >= os.path.getmtime(def_path):
        return lib
    r = subprocess.run([LLD_LINK, '/machine:x64', '/def:' + def_path, '/out:' + lib], capture_output=True, text=True, errors='replace')
    if r.returncode:
        raise SystemExit('lld-link could not make %s from %s:\n%s' % (lib, def_path, (r.stdout + r.stderr)[-2000:]))
    return lib


def nocrt_fltused(libdir):
    """nocrt\\fltused.c -> an object defining _fltused, which clang references from every object that uses floating point
    (the C runtime defines it; the module DLLs have no C runtime). The game DLL has it in game_data.c."""
    src, obj = os.path.join(NOCRT_DIR, 'fltused.c'), os.path.join(libdir, 'fltused.obj')
    if not os.path.exists(obj) or os.path.getmtime(obj) < os.path.getmtime(src):
        r = subprocess.run([CLANG, '-c', OPT_BASE] + NOCRT_FLAGS + CODEGEN_FLAGS + [src, '-o', obj], capture_output=True, text=True, errors='replace')
        if r.returncode:
            raise SystemExit('clang could not compile %s:\n%s' % (src, (r.stdout + r.stderr)[-2000:]))
    return obj


def link_dll(objs, dll, rsp, implib=True, fltused=False, symbolic=False):
    """Objects -> DLL: with clang's default linker (MSVC link.exe and the static C runtime) or, under RECOMP_NOCRT, with
    lld-link alone against the import libraries of nocrt\\ (ucrtbase.dll, ntdll.dll). fltused: add the _fltused object
    (DLLs whose objects do not define it). Returns the CompletedProcess."""
    libdir = os.path.join(HERE, 'obj_nocrt')
    if NOCRT and fltused:
        os.makedirs(libdir, exist_ok=True)
        objs = list(objs) + [nocrt_fltused(libdir)]
    with open(rsp, 'w') as f:
        f.write('\n'.join('"%s"' % o.replace('\\', '/') for o in objs))
    if TARGET_LINUX:          # a .so: -z now resolves everything at load, as a DLL is
        # arm64: TARGET_FLAGS bring --target/--sysroot/-mcpu and ld.lld; --no-dependent-libraries because lld, unlike
        # GNU ld, acts on the #pragma comment(lib, ...) the lifted objects carry (from the Windows build). symbolic:
        # -Bsymbolic keeps every module .so bound to its own copies of call_indirect / host_import / guest_* / nro_catch_call.
        extra = CODEGEN_FLAGS + ['-Wl,--no-dependent-libraries'] + (['-Wl,-Bsymbolic'] if symbolic else []) if TARGET_ARM64 else []   # CODEGEN_FLAGS already carries TARGET_FLAGS (--target/--sysroot/-mcpu/-fuse-ld=lld)
        return subprocess.run([CLANG] + extra + ['-shared', '@' + rsp, '-o', dll] + PLAT_FLAGS + ['-lm', '-Wl,-soname,' + os.path.basename(dll), '-Wl,-z,now'], capture_output=True, text=True, errors='replace')
    if not NOCRT:
        return subprocess.run([CLANG, '-shared', '@' + rsp, '-o', dll], capture_output=True, text=True, errors='replace')
    os.makedirs(libdir, exist_ok=True)
    libs = [lld_def_lib(os.path.join(NOCRT_DIR, n + '.def'), os.path.join(libdir, n + '.lib')) for n in ('ucrtbase_min', 'ntdll_min')]
    argv = [LLD_LINK, '/dll', '/noentry', '/nodefaultlib', '/machine:x64', '/out:' + dll, '@' + rsp] + libs
    return subprocess.run(argv + (['/implib:' + dll[:-4] + '.lib'] if implib else []), capture_output=True, text=True, errors='replace')


def compile_game_data(objdir, shim):
    """The DLL's shim and GAME_DATA_SOURCES (the dump-derived host tables and the lifted SDK code) -> objects."""
    os.makedirs(objdir, exist_ok=True)
    newest_h = max([os.path.getmtime(os.path.join(HERE, h)) for h in os.listdir(HERE) if h.endswith('.h')] + [os.path.getmtime(os.path.abspath(__file__))])
    flags = ['-c', OPT_BASE, '-w', '-fno-strict-aliasing', '-DSDKLIFT_GEN', '-I', HERE, '-I', GEN] + NOCRT_FLAGS + PLAT_FLAGS + CODEGEN_FLAGS + os.environ.get('RECOMP_DATA_CFLAGS', '').split()
    jobs, objs = [], []
    for src in [shim] + [os.path.join(HERE, n) for n in GAME_DATA_SOURCES]:
        if not os.path.exists(src):
            print('%s is missing: an earlier step of scripts/setup.py writes it (tables, sdk, modules)' % src)
            return None
        obj = os.path.join(objdir, ('_shim' if src == shim else os.path.basename(src)[:-2]) + '.obj')
        objs.append(obj)
        if not os.path.exists(obj) or os.path.getmtime(obj) < max(os.path.getmtime(src), newest_h):
            jobs.append([CLANG] + flags + [src, '-o', obj])
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(min(8, os.cpu_count() or 8) if TARGET_ARM64 else 8) as pool:
        for argv, r in zip(jobs, pool.map(lambda a: subprocess.run(a, capture_output=True, text=True, errors='replace'), jobs)):
            if r.returncode:
                failed += 1
                print('FAILED %s\n%s' % (os.path.basename(argv[-3]), (r.stdout + r.stderr)[-3000:]))
    return None if failed else objs


def build_game_dll():
    dll = os.path.join(HERE, GAME_DLL)
    lib = dll if TARGET_LINUX else dll[:-4] + '.lib'          # Linux: the exe links against the .so itself
    shim = os.path.join(HERE, os.environ.get('RECOMP_SHIM', 'game_shim_eh.c'))
    data_dir = OBJ + '_data'                 # beside OBJ, not in it (cmd_compile removes what it does not know there)
    data_objs = compile_game_data(data_dir, shim)
    if data_objs is None:
        return 1
    objs = sorted(os.path.join(OBJ, n) for n in os.listdir(OBJ) if n.endswith('.obj')) + data_objs
    newest = max(os.path.getmtime(o) for o in objs)
    if os.path.exists(dll) and os.path.exists(lib) and os.path.getmtime(dll) >= newest:
        return 0
    t0 = time.time()
    r = link_dll(objs, dll, os.path.join(data_dir, 'link.rsp'), symbolic=True)
    print((r.stdout + r.stderr)[-4000:])
    if r.returncode == 0:
        print('linked %s: %.0f MB in %.0fs%s' % (dll, os.path.getsize(dll) / 1048576.0, time.time() - t0, ' (lld-link, no C runtime)' if NOCRT else ''))
    else:
        print('could not link %s (is a game exe still running and holding it open?)' % GAME_DLL)
    return r.returncode


def game_import_lib(objdir):
    """The exe's import library of the game DLL, made from nocrt\\ssbu_game.def (the DLL's exports), not by linking the
    DLL: the exe links without the game DLL, on a PC without any dump (scripts/make_release.py)."""
    import re
    with open(os.path.join(NOCRT_DIR, 'ssbu_game.def')) as f:
        text = re.sub(r'(?m)^LIBRARY\s+\S+', 'LIBRARY ' + GAME_DLL, f.read())
    d = os.path.join(objdir, 'ssbu_game_import.def')
    if not os.path.exists(d) or open(d).read() != text:
        with open(d, 'w') as f:
            f.write(text)
    return lld_def_lib(d, os.path.join(objdir, 'ssbu_game_import.lib'))


def compile_host(job):
    src, obj = job
    flags = CXX_FLAGS if src.endswith('.cpp') else HOST_FLAGS
    r = subprocess.run([CLANG, '-c'] + flags + [src, '-o', obj], capture_output=True, text=True)
    return src, r.returncode, (r.stdout + r.stderr)[-3000:]


def cmd_link(host_only=False):
    if not host_only and build_game_dll():
        return 1
    exe = os.environ.get('RECOMP_EXE', 'ssbu_native.exe')      # parallel work: one exe name per worker
    out = os.path.join(HERE, exe)
    objdir = os.path.join(HERE, 'obj_host', os.path.splitext(exe)[0])
    os.makedirs(objdir, exist_ok=True)
    t0 = time.time()
    newest_dep = max([os.path.getmtime(os.path.join(HERE, h)) for h in os.listdir(HERE) if h.endswith(('.h', '.inc'))] + [os.path.getmtime(os.path.join(HERE, 'build.py'))])
    # nvn_vk.c includes the other nvn_vk*.c files as one translation unit: an edit of one of them must rebuild nvn_vk.obj
    newest_vk = max(os.path.getmtime(os.path.join(HERE, n)) for n in os.listdir(HERE) if n.startswith('nvn_vk') and n.endswith('.c'))
    objs, jobs = [], []
    if not HAVE_IMGUI:
        print('settings menu: Dear ImGui not found in %s (IMGUI_ROOT): built without it (menu_stub.c; F1 does nothing)' % IMGUI)
    for n in HOST_SOURCES + ['gamedll.c'] + ([] if HAVE_IMGUI else ['menu_stub.c']):
        src, obj = os.path.join(HERE, n), os.path.join(objdir, n[:-2] + '.obj')
        objs.append(obj)
        if not os.path.exists(obj) or os.path.getmtime(obj) < max(os.path.getmtime(src), newest_dep, newest_vk if n == 'nvn_vk.c' else 0):
            jobs.append((src, obj))
    for src in CXX_SOURCES:                          # U1: the settings menu (C++); imgui's own files depend on their source and menu_imconfig.h only
        obj = os.path.join(objdir, os.path.splitext(os.path.basename(src))[0] + '_cxx.obj')
        objs.append(obj)
        dep = newest_dep if src.endswith('menu.cpp') else os.path.getmtime(os.path.join(HERE, 'menu_imconfig.h'))
        if not os.path.exists(obj) or os.path.getmtime(obj) < max(os.path.getmtime(src), dep):
            jobs.append((src, obj))
    failed = 0
    # 16 host-compile threads: the host files are small (nothing like the lifted ones), so they are not memory bound; on
    # the aarch64 target never more than the machine that runs the compilers has CPUs (the Frame natively, the PC when cross)
    with concurrent.futures.ThreadPoolExecutor(min(16, os.cpu_count() or 8) if TARGET_ARM64 else 16) as pool:
        for src, code, err in pool.map(compile_host, jobs):
            if code:
                failed += 1
                print('FAILED %s\n%s' % (os.path.basename(src), err))
    if failed:
        return 1
    if TARGET_LINUX:
        # arm64: TARGET_FLAGS = the target and lld; -no-pie keeps the brk heap low (the game was handed pointers below
        # 2^47 on Windows; a PIE + this kernel's mmap area puts them far above it, see vmem.c / OURS buildplat.py),
        # --no-dependent-libraries ignores the deplibs the lifted objects carry.
        extra = TARGET_FLAGS + ['-no-pie', '-Wl,--no-dependent-libraries'] if TARGET_ARM64 else []
        r = subprocess.run([CLANG] + extra + objs + ['-L', HERE, '-l:' + GAME_DLL, '-Wl,-rpath,$ORIGIN', '-Wl,--build-id=sha1'] + LIBS + ['-o', out], capture_output=True, text=True, errors='replace')
    else:
        r = subprocess.run([CLANG] + objs + [game_import_lib(objdir), os.path.join(SDL_DIR, 'SDL3.lib')] + LIBS + debug_link_flags(out) + ['-o', out], capture_output=True, text=True, errors='replace')
    if r.returncode == 0 and not TARGET_LINUX:
        import shutil
        try:
            shutil.copy2(os.path.join(SDL_DIR, 'SDL3.dll'), os.path.join(HERE, 'SDL3.dll'))
        except PermissionError:                  # a running game holds the copy beside it (the same file): keep it
            if not os.path.exists(os.path.join(HERE, 'SDL3.dll')):
                raise
    print((r.stdout + r.stderr)[-4000:])                 # link.exe reports its errors on stdout
    if r.returncode == 0:
        print('linked %s: %.1f MB in %.1fs (%d host files recompiled)' % (out, os.path.getsize(out) / 1048576.0, time.time() - t0, len(jobs)))
    return r.returncode


if __name__ == '__main__':
    if sys.argv[1] == 'compile':
        args = [a for a in sys.argv[2:] if not a.startswith('--')]
        if not any(a.startswith('--opt=') for a in sys.argv) and '--plain' not in sys.argv:
            # the default: the optimised build (clang 9 -O1, functions over 20,000 statements unoptimised, 180 s per file
            # with automatic fallback; plan\phases\P3_performance.md section 4). --opt=<level> or --plain = this file's own compiler loop.
            import build_opt
            sys.exit(build_opt.cmd_main(int(args[0]) if args else 28))
        sys.exit(cmd_compile(int(args[0]) if args else 28, '--force' in sys.argv))
    if sys.argv[1] == 'nro':                 # run-time modules, one DLL each: python build.py nro <module> ... (build_nro.py)
        # default: the optimised modules, i.e. the folder nnro.c loads by default (build_opt.py: gen_nro_eh_o2, obj_nro_eh_o2,
        # nro_bin_eh_o2). --plain = build_nro.py's own -O0 loop into nro_bin_eh (run those with SSBU_NRO_BIN=nro_bin_eh).
        rest = [a for a in sys.argv[2:] if a != '--plain']
        if '--plain' not in sys.argv:
            import build_opt
            jobs = int(rest.pop(0)) if rest and rest[0].isdigit() else 28
            sys.exit(build_opt.cmd_nro(jobs, rest))
        import build_nro
        sys.exit(build_nro.main(rest))
    if sys.argv[1] == 'host':                # the exe alone, nothing of the game in it or needed (scripts/make_release.py)
        sys.exit(cmd_link(host_only=True))
    if sys.argv[1] == 'gamedll':             # the game DLL alone, from the objects of compile (scripts/setup.py with a runtime package)
        sys.exit(build_game_dll())
    sys.exit(cmd_link())
