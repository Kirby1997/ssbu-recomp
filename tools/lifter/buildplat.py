"""Build-platform abstraction shared by build.py, build_opt.py, build_nro.py (and scripts/setup.py's child processes).

The target comes from RECOMP_TARGET (set by scripts/setup.py --target): `windows` (default, behaviour exactly as before) or
`linux-arm64` (clang + lld, ELF, aarch64; either cross-compiling with a sysroot or native on the Steam Frame).
Everything the children need arrives through environment variables, so every script also works when started by hand:

    RECOMP_TARGET        windows | linux-arm64
    RECOMP_CLANG         the compiler driver
    RECOMP_TARGET_FLAGS  extra driver flags for compile AND link, shell-quoted: "--target=aarch64-linux-gnu --sysroot=/x -fuse-ld=lld -march=armv8.2-a+lse"
    SDL3_ROOT            prefix with include/SDL3 and lib/libSDL3.so*   (windows: the VC package, lib/x64/SDL3.lib)
    VULKAN_HEADERS       prefix with include/vulkan/vulkan.h
    RECOMP_LIBS          extra link arguments for the executable (shell-quoted), appended to the default libraries
"""
import os
import shlex
import sys

TARGET = os.environ.get('RECOMP_TARGET', 'windows')
LINUX = TARGET != 'windows'
OBJ_EXT = '.o' if LINUX else '.obj'
EXE_SUFFIX = '' if LINUX else '.exe'
SHARED_SUFFIX = '.so' if LINUX else '.dll'
TARGET_FLAGS = shlex.split(os.environ.get('RECOMP_TARGET_FLAGS', '')) if LINUX else []

# Code-generation flags for every translation unit of the lifted code and the runtime on linux-arm64 (PORT_AUDIT Phase 1).
# -ffp-contract=off: clang on aarch64 otherwise fuses a*b+c into fmadd even at -O0. Never fast-math / -ffinite-math-only /
# -fno-signed-zeros / -mstrict-align.
LINUX_CODEGEN = ['-ffp-contract=off', '-fno-math-errno', '-fno-omit-frame-pointer']
# Code that ends up in a .so: PIC, nothing exported unless marked, no PLT indirection for calls inside the library.
LINUX_SHARED_CODE = ['-fPIC', '-fvisibility=hidden', '-fno-semantic-interposition']


def codegen_flags(shared):
    """Flags added to every compile on the target (empty for windows: its flag lists stay as they were)."""
    if not LINUX:
        return []
    return TARGET_FLAGS + LINUX_CODEGEN + (LINUX_SHARED_CODE if shared else [])


def host_only_flags():
    """Extra compile flags of the host runtime (the executable). Linux: _GNU_SOURCE for plat.h; position-dependent code, because
    the executable is linked -no-pie so its brk heap (and with it malloc, see vmem.c) sits low in the address space: Windows
    never handed the game a pointer above 2^47 and this kernel's mmap/PIE area is at 0xaaaa.../0xffff... ."""
    return ['-D_GNU_SOURCE', '-fno-pie'] if LINUX else []


def exe_link_flags():
    """Link flags of the executable (Linux only; see host_only_flags)."""
    return ['-no-pie', '-fno-omit-frame-pointer'] if LINUX else []


def default_shared_name(stem):
    """ssbu_game_eh_o1 -> ssbu_game_eh_o1.dll | libssbu_game_eh_o1.so"""
    return ('lib' + stem + '.so') if LINUX else (stem + '.dll')


def link_shared_flags(module=False):
    """Extra link flags for a shared library. NRO modules each re-define call_indirect, host_import, guest_*, stx_diag,
    nro_catch_call ...: -Bsymbolic keeps every module bound to its own copies (the game library too)."""
    if not LINUX:
        return []
    return TARGET_FLAGS + ['-shared', '-fPIC'] + (['-Wl,-Bsymbolic'] if module else [])


def sdl_include():
    return os.path.join(sdl_root(), 'include')


def sdl_root():
    default = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'reference', 'upstream', 'SDL3-3.4.18-VC', 'SDL3-3.4.18')
    return os.environ.get('SDL3_ROOT', default)


def sdl_libdir():
    return os.path.join(sdl_root(), 'lib') if LINUX else os.path.join(sdl_root(), 'lib', 'x64')


def extra_include_flags():
    """FreeType and Opus headers live in subfolders of the include dir on Linux; add them next to the sysroot ones."""
    if not LINUX:
        return []
    out = []
    for flag in TARGET_FLAGS:
        if flag.startswith('--sysroot='):
            inc = os.path.join(flag.split('=', 1)[1], 'usr', 'include')
            break
    else:
        inc = '/usr/include'
    for sub in ('freetype2', 'opus'):
        if os.path.isdir(os.path.join(inc, sub)):
            out += ['-I', os.path.join(inc, sub)]
    return out


def exe_libs():
    """Link arguments of the executable. Windows: as before. Linux: SDL3 from the bundle folder, FreeType, Opus, libc pieces;
    --as-needed so unused ones cost nothing; $ORIGIN rpath so the folder is relocatable."""
    if not LINUX:
        return ['-luser32', '-lgdi32', '-lwinmm', '-lmfplat', '-lmfreadwrite', '-lmfuuid', '-lole32', '-lshlwapi']
    return (['-L' + sdl_libdir(), '-Wl,--as-needed', '-lSDL3', '-lfreetype', '-lopus', '-lpthread', '-ldl', '-lm', '-lrt',
             "-Wl,-rpath,$ORIGIN", '-Wl,-rpath-link,' + sdl_libdir()] + shlex.split(os.environ.get('RECOMP_LIBS', '')))


def terminate_tree(proc):
    """Kill a compiler process and its -cc1 child (windows: taskkill /T; linux: the process group made by start_new_session)."""
    import subprocess
    if LINUX:
        import signal
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            proc.kill()
    else:
        subprocess.run(['taskkill', '/PID', str(proc.pid), '/T', '/F'], capture_output=True)      # this compiler and its -cc1 child only


def popen_group(cmd, **kw):
    """Popen that terminate_tree can kill as a tree."""
    import subprocess
    if LINUX:
        kw['start_new_session'] = True
    return subprocess.Popen(cmd, **kw)
